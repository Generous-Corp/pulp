#pragma once

// Shared plumbing for the asynchronous dialogs: one-shot delivery through a
// MainThreadScope, and the hooks each platform backend implements.

#include <pulp/platform/alert.hpp>
#include <pulp/platform/dialog_session.hpp>
#include <pulp/platform/file_dialog.hpp>
#include <pulp/platform/main_thread.hpp>
#include <pulp/platform/native_handle.hpp>
#include <pulp/platform/popup_menu.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pulp::platform::detail {

struct DialogSessionState {
    std::atomic<bool> open{true};
    std::atomic<bool> finished{false};
    // Closes what is showing so it finishes with its dismissed result.
    // Main thread only.
    std::function<void()> close;
    // Whatever the platform backend must keep alive while the dialog is up.
    // Released once the dialog has finished or its scope is gone.
    std::shared_ptr<void> resources;
    std::uint64_t teardown_id = 0;
};

// How one completion reached its caller.
enum class Delivery {
    posted,            // queued on the main thread through the scope
    inline_no_backend, // the scope exists but has no main-thread backend
    dropped,           // the scope is gone
};

// Hand `task` to `post`. When the post is refused while `alive()` still holds,
// the platform has no main-thread backend (off Apple), so the task runs here,
// inline, rather than never; when the scope is gone it is dropped.
template <typename Post, typename Alive>
Delivery deliver_or_inline(const std::function<void()>& task, Post&& post, Alive&& alive) {
    if (post(task))
        return Delivery::posted;
    if (alive()) {
        task();
        return Delivery::inline_no_backend;
    }
    return Delivery::dropped;
}

// A completion that runs `done` once: the first call wins, from any thread.
// With a main-thread backend the result is posted through the scope, so it
// reaches `done` on the main thread, after the opening call has returned, and
// never after the scope is gone. Without one (off Apple) `done` runs inline,
// in the call that finished the dialog.
template <typename Result>
std::function<void(Result)> make_finisher(MainThreadScope& scope,
                                          std::shared_ptr<DialogSessionState> state,
                                          std::function<void(Result)> done) {
    MainThreadScope::Handle handle = scope.handle();
    MainThreadScope* owner = &scope;
    return [handle, owner, state, done = std::move(done)](Result result) {
        if (state->finished.exchange(true))
            return;
        auto box = std::make_shared<Result>(std::move(result));
        std::function<void()> task = [owner, state, done, box]() {
            // Runs only while the scope is alive, so `owner` is valid.
            if (state->teardown_id != 0)
                owner->remove_teardown(state->teardown_id);
            state->teardown_id = 0;
            state->open = false;
            state->close = nullptr;
            state->resources = nullptr;
            if (done)
                done(std::move(*box));
        };
        deliver_or_inline(
            task, [&](std::function<void()> t) { return handle.post(std::move(t)); },
            [&] { return handle.alive(); });
    };
}

// Closes the dialog when the scope dies; the finisher's post is then refused,
// so the completion is dropped.
inline void bind_to_scope(MainThreadScope& scope,
                          const std::shared_ptr<DialogSessionState>& state) {
    std::weak_ptr<DialogSessionState> weak = state;
    state->teardown_id = scope.add_teardown([weak] {
        if (auto s = weak.lock()) {
            s->open = false;
            if (s->close) {
                auto close = std::move(s->close);
                s->close = nullptr;
                close();
            }
            s->resources = nullptr;
        }
    });
}

// ── Native backends (platform/mac/*.mm; stubs elsewhere) ─────────────────
// Each returns a closer, or an empty function when it cannot show anything.

std::function<void()> present_native_alert(NativeViewHandle parent, const AlertRequest& request,
                                           const AlertButtonPlan& plan,
                                           std::function<void(long button_index)> respond);

std::function<void()> present_native_open(NativeViewHandle parent,
                                          const FileDialog::OpenRequest& request,
                                          std::function<void(std::vector<std::string>)> respond);

// With `copy_source`, the file is copied to the chosen destination while the
// grant is held; `respond` gets the destination or an error.
std::function<void()>
present_native_save(NativeViewHandle parent, const FileDialog::SaveRequest& request,
                    std::optional<std::string> copy_source,
                    std::function<void(std::optional<std::string>, std::string)> respond);

// Opens the menu on a later run-loop pass from a timer bound to `scope`.
// `resources` receives what must stay alive until the menu has finished.
std::function<void()> present_native_menu(MainThreadScope& scope, const MenuAnchor& anchor,
                                          MenuModel model, MenuPlacement placement,
                                          std::function<void(std::optional<int>)> respond,
                                          std::shared_ptr<void>& resources);

bool native_open_url(const std::string& url);

std::vector<std::string> native_content_type_ids(const std::vector<FileFilter>& filters);

} // namespace pulp::platform::detail
