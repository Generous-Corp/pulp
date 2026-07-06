#include <pulp/format/view_bridge.hpp>
#include <pulp/format/editor_ui.hpp>
#include <pulp/format/remote_view_session.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/view.hpp>

namespace pulp::format {

ViewBridge::ViewBridge(Processor& processor, state::StateStore& store)
    : ViewBridge(processor, store, Options{}) {}

ViewBridge::ViewBridge(Processor& processor, state::StateStore& store, Options options)
    : processor_(processor),
      store_(store),
      options_(options),
      size_hints_(processor.view_size()) {
    width_ = size_hints_.preferred_width;
    height_ = size_hints_.preferred_height;
}

ViewBridge::~ViewBridge() {
    // Flip liveness FIRST so a display-link idle pump that races this teardown
    // (or a stale pump left on a host after a bridge replacement) reads false and
    // no-ops before we tear down the store / scripted session in close(). Guard
    // against a moved-from bridge (alive_ would be null). NOTE: the pump's
    // check-then-use is only safe because both the pump and this teardown run on
    // the host main thread — the token makes the no-op DECISION safe, not a
    // cross-thread deref of bridge_ptr.
    if (alive_) alive_->store(false, std::memory_order_release);
    close();
}

bool ViewBridge::open(std::string* error) {
    if (view_raw_) return true;
    last_error_.clear();

    // First chance: let the processor supply a fully custom view.
    auto custom = processor_.create_view();
    if (custom) {
        view_ = std::move(custom);
        if (processor_.active_scripted_ui()) {
            uses_script_ui_ = true;
        }
    } else {
        // Fall back to the scripted-UI or AutoUi default.
        auto instance = build_editor_ui(store_, options_.enable_hot_reload, &last_error_);
        if (!instance.root) {
            if (error) *error = last_error_.empty() ? "ViewBridge: failed to build editor UI" : last_error_;
            return false;
        }
        view_ = std::move(instance.root);
        scripted_ui_ = std::move(instance.scripted_ui);
        uses_script_ui_ = instance.uses_script_ui;
    }
    view_raw_ = view_.get();

    size_hints_ = processor_.view_size();
    // A NATIVE create_view() already computed its own layout bounds; make the
    // host window match them exactly so the editor's own edge padding is never
    // clipped (otherwise a plugin that doesn't declare DESIGN_WIDTH/HEIGHT gets
    // the default window size, which can be narrower than the laid-out editor —
    // the right column + padding then fall off the right/bottom edge). Scripted
    // UIs keep the processor-declared view_size(). This is SDK-level: every
    // native editor is sized correctly without per-plugin hardcoding.
    if (view_ && !uses_script_ui_) {
        const auto b = view_->bounds();
        if (b.width > 0.0f && b.height > 0.0f) {
            size_hints_.preferred_width = static_cast<uint32_t>(b.width + 0.5f);
            size_hints_.preferred_height = static_cast<uint32_t>(b.height + 0.5f);
        }
    }
    width_ = size_hints_.preferred_width;
    height_ = size_hints_.preferred_height;
    attached_ = false;
    released_ = false;
    return true;
}

view::ScriptedUiSession* ViewBridge::scripted_ui() {
    if (scripted_ui_) return scripted_ui_.get();
    return processor_.active_scripted_ui();
}

const view::ScriptedUiSession* ViewBridge::scripted_ui() const {
    if (scripted_ui_) return scripted_ui_.get();
    return processor_.active_scripted_ui();
}

void ViewBridge::notify_attached() {
    if (!view_raw_ || attached_) return;
    attached_ = true;
    processor_.on_view_opened(*view_raw_);
}

std::unique_ptr<view::View> ViewBridge::release_view() {
    if (!view_ || released_) return nullptr;
    released_ = true;
    return std::move(view_);  // view_raw_ stays valid so lifecycle keeps dispatching
}

void ViewBridge::close() {
    if (!view_raw_) return;
    if (attached_) {
        processor_.on_view_closed(*view_raw_);
        attached_ = false;
    }
    scripted_ui_.reset();
    view_.reset();          // no-op if already released
    view_raw_ = nullptr;
    uses_script_ui_ = false;
    released_ = false;
    secondaries_.clear();
}

void ViewBridge::resize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    if (view_raw_ && attached_) {
        processor_.on_view_resized(*view_raw_, width, height);
    }
}

view::View* ViewBridge::attach_secondary_view(std::unique_ptr<view::View> v, ViewRole role) {
    if (!v) return nullptr;
    auto* raw = v.get();
    secondaries_.push_back({std::move(v), role});
    return raw;
}

bool ViewBridge::detach_secondary_view(view::View* target) {
    for (auto it = secondaries_.begin(); it != secondaries_.end(); ++it) {
        if (it->view.get() == target) {
            secondaries_.erase(it);
            return true;
        }
    }
    return false;
}

size_t ViewBridge::view_count() const {
    return (view_raw_ ? 1u : 0u) + secondaries_.size();
}

view::View* ViewBridge::view_at(size_t index) {
    if (view_raw_) {
        if (index == 0) return view_raw_;
        --index;
    }
    if (index < secondaries_.size()) return secondaries_[index].view.get();
    return nullptr;
}

ViewRole ViewBridge::role_at(size_t index) const {
    if (view_raw_) {
        if (index == 0) return options_.role;
        --index;
    }
    if (index < secondaries_.size()) return secondaries_[index].role;
    return ViewRole::Editor;
}

RemoteViewSession* ViewBridge::attach_remote_channel(
    std::unique_ptr<runtime::MessageChannel> channel,
    std::string label)
{
    if (!channel) {
        last_error_ = "attach_remote_channel: null channel";
        return nullptr;
    }
    auto session = std::unique_ptr<RemoteViewSession>(
        new RemoteViewSession(std::move(label), store_, std::move(channel)));
    if (!session->handshake_(processor_)) {
        last_error_ = session->last_error();
        return nullptr;
    }
    auto* raw = session.get();
    remotes_.push_back(std::move(session));
    return raw;
}

bool ViewBridge::detach_remote(RemoteViewSession* session) {
    for (auto it = remotes_.begin(); it != remotes_.end(); ++it) {
        if (it->get() == session) {
            (*it)->close();
            remotes_.erase(it);
            return true;
        }
    }
    return false;
}

} // namespace pulp::format
