// Asynchronous file dialogs: routing (async backend, then native panel) and
// one-shot delivery through the caller's MainThreadScope. The panels
// themselves live in platform/mac/file_dialog_async_mac.mm.

#include <pulp/platform/file_dialog.hpp>

#include "dialog_internal.hpp"

#include <filesystem>
#include <mutex>
#include <system_error>
#include <utility>

namespace pulp::platform {

namespace {

std::mutex g_async_mutex;
FileDialog::AsyncBackend g_async_backend;
bool g_async_installed = false;

bool async_backend(FileDialog::AsyncBackend& out) {
    std::lock_guard<std::mutex> lock(g_async_mutex);
    if (!g_async_installed)
        return false;
    out = g_async_backend;
    return true;
}

// Finish `state` with `empty` when nothing could be shown; otherwise keep the
// closer and tie the dialog to the scope.
template <typename Result>
DialogSession settle(MainThreadScope& scope,
                     const std::shared_ptr<detail::DialogSessionState>& state,
                     const std::function<void(Result)>& finish, Result empty) {
    if (!state->close) {
        finish(std::move(empty));
        return DialogSession(state);
    }
    detail::bind_to_scope(scope, state);
    return DialogSession(state);
}

} // namespace

void FileDialog::set_async_backend(AsyncBackend backend) {
    std::lock_guard<std::mutex> lock(g_async_mutex);
    g_async_backend = std::move(backend);
    g_async_installed = true;
}

void FileDialog::clear_async_backend() {
    std::lock_guard<std::mutex> lock(g_async_mutex);
    g_async_backend = {};
    g_async_installed = false;
}

bool FileDialog::has_async_backend() {
    std::lock_guard<std::mutex> lock(g_async_mutex);
    return g_async_installed;
}

std::vector<std::string> FileDialog::content_type_ids_for(const std::vector<FileFilter>& filters) {
    return detail::native_content_type_ids(filters);
}

DialogSession FileDialog::open_async(MainThreadScope& scope, NativeViewHandle parent,
                                     OpenRequest request,
                                     std::function<void(std::vector<std::string>)> done) {
    using Result = std::vector<std::string>;
    auto state = std::make_shared<detail::DialogSessionState>();
    auto finish = detail::make_finisher<Result>(scope, state, std::move(done));

    AsyncBackend backend;
    if (async_backend(backend)) {
        if (backend.open) {
            backend.open(request, [finish](Result paths) { finish(std::move(paths)); });
            auto cancel = backend.cancel;
            state->close = [cancel, finish] {
                if (cancel)
                    cancel();
                finish({});
            };
        }
        return settle<Result>(scope, state, finish, {});
    }
    state->close = detail::present_native_open(
        parent, request, [finish](Result paths) { finish(std::move(paths)); });
    return settle<Result>(scope, state, finish, {});
}

DialogSession FileDialog::save_async(MainThreadScope& scope, NativeViewHandle parent,
                                     SaveRequest request,
                                     std::function<void(std::optional<std::string>)> done) {
    using Result = std::optional<std::string>;
    auto state = std::make_shared<detail::DialogSessionState>();
    auto finish = detail::make_finisher<Result>(scope, state, std::move(done));

    AsyncBackend backend;
    if (async_backend(backend)) {
        if (backend.save) {
            backend.save(request, [finish](Result path) { finish(std::move(path)); });
            auto cancel = backend.cancel;
            state->close = [cancel, finish] {
                if (cancel)
                    cancel();
                finish(std::nullopt);
            };
        }
        return settle<Result>(scope, state, finish, std::nullopt);
    }
    state->close = detail::present_native_save(
        parent, request, std::nullopt,
        [finish](Result path, std::string) { finish(std::move(path)); });
    return settle<Result>(scope, state, finish, std::nullopt);
}

DialogSession
FileDialog::save_copy_async(MainThreadScope& scope, NativeViewHandle parent,
                            std::string source_path, SaveRequest request,
                            std::function<void(std::optional<std::string>, std::string)> done) {
    using Result = std::pair<std::optional<std::string>, std::string>;
    auto state = std::make_shared<detail::DialogSessionState>();
    std::function<void(Result)> unpack = [done = std::move(done)](Result r) {
        if (done)
            done(std::move(r.first), std::move(r.second));
    };
    auto finish = detail::make_finisher<Result>(scope, state, std::move(unpack));

    AsyncBackend backend;
    if (async_backend(backend)) {
        // A host backend chooses the destination; the copy is ours.
        if (backend.save) {
            backend.save(request, [finish, source_path](std::optional<std::string> path) {
                if (!path) {
                    finish({std::nullopt, {}});
                    return;
                }
                std::error_code ec;
                std::filesystem::copy_file(source_path, *path,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                if (ec)
                    finish({std::nullopt, ec.message()});
                else
                    finish({std::move(path), {}});
            });
            auto cancel = backend.cancel;
            state->close = [cancel, finish] {
                if (cancel)
                    cancel();
                finish({std::nullopt, {}});
            };
        }
        return settle<Result>(scope, state, finish, {std::nullopt, {}});
    }
    state->close =
        detail::present_native_save(parent, request, std::move(source_path),
                                    [finish](std::optional<std::string> path, std::string error) {
                                        finish({std::move(path), std::move(error)});
                                    });
    return settle<Result>(scope, state, finish, {std::nullopt, {}});
}

} // namespace pulp::platform
