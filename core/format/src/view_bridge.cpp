#include <pulp/format/view_bridge.hpp>
#include <pulp/format/editor_ui.hpp>
#include <pulp/format/remote_view_session.hpp>
#include <pulp/runtime/exceptions.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/view.hpp>

namespace pulp::format {
namespace {

std::unique_ptr<view::View> safe_create_view(Processor& processor) noexcept {
    PULP_TRY { return processor.create_view(); }
    PULP_CATCH_ALL { return nullptr; }
}

ViewSize safe_view_size(Processor& processor) noexcept {
    PULP_TRY { return processor.view_size(); }
    PULP_CATCH_ALL { return {}; }
}

} // namespace

ViewBridge::ViewBridge(Processor& processor, state::StateStore& store)
    : ViewBridge(processor, store, Options{}) {}

ViewBridge::ViewBridge(Processor& processor, state::StateStore& store, Options options)
    : processor_(processor),
      store_(store),
      options_(options),
      size_hints_(safe_view_size(processor)) {
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
    auto custom = safe_create_view(processor_);
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

    size_hints_ = safe_view_size(processor_);
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
    last_reload_generation_ = processor_.editor_reload_generation();
    return true;
}

bool ViewBridge::poll_editor_reload() {
    if (!view_raw_ || !processor_.supports_editor_reload()) return false;
    const uint64_t gen = processor_.editor_reload_generation();
    if (gen == last_reload_generation_) return false;
    // Consume the generation only on a SUCCESSFUL rebuild. If create_view()
    // returns null transiently, leave last_reload_generation_ so the next tick
    // retries instead of stranding the stale UI until another swap.
    if (rebuild_primary_view()) {
        last_reload_generation_ = gen;
        return true;
    }
    return false;
}

bool ViewBridge::rebuild_primary_view() {
    if (!view_raw_) return false;
    // Re-run create_view() on the (hot-swapped) processor to get the new editor.
    auto fresh = safe_create_view(processor_);
    if (!fresh) return false;

    // Transplant the fresh content into the SAME root View object the host holds
    // by reference — so the open editor updates in place (no re-attach, works for
    // both the CoreGraphics and GPU hosts). Move children over and mirror the
    // root's background; a P0.2 "hold last frame" is implicit — the host keeps
    // painting the stable root, which now carries the new content.
    while (view_raw_->child_count() > 0) {
        view_raw_->remove_child(view_raw_->child_at(view_raw_->child_count() - 1));
    }
    std::vector<view::View*> order;
    order.reserve(fresh->child_count());
    for (size_t i = 0; i < fresh->child_count(); ++i) order.push_back(fresh->child_at(i));
    for (view::View* child : order) {
        if (auto owned = fresh->remove_child(child)) view_raw_->add_child(std::move(owned));
    }
    if (fresh->has_background_color()) {
        view_raw_->set_background_color(fresh->background_color());
    } else {
        view_raw_->clear_background_color();
    }
    // Carry the new editor's ROOT layout too (not just children + background) — a
    // reloaded editor may change root flex / layout-mode (review 3.1). NOTE: a
    // root-level event handler set on the root inside create_view() is NOT carried
    // (std::function handlers can't be generically transplanted) — attach handlers
    // to child views, or re-instantiate the plugin, if the root must own one.
    view_raw_->flex() = fresh->flex();
    view_raw_->set_layout_mode(fresh->layout_mode());
    view_raw_->invalidate_layout();

    // Refresh size hints (the new editor may prefer a different size). NOTE: the
    // host owns the window; a preferred-size CHANGE across reload is not renotified
    // to the host here (review 3.2, LOW) — the editor lays out to the host's current
    // size, and a size change needs a re-instantiate for now.
    size_hints_ = safe_view_size(processor_);
    width_ = size_hints_.preferred_width;
    height_ = size_hints_.preferred_height;
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
    PULP_TRY { processor_.on_view_opened(*view_raw_); }
    PULP_CATCH_ALL {}
}

std::unique_ptr<view::View> ViewBridge::release_view() {
    if (!view_ || released_) return nullptr;
    released_ = true;
    return std::move(view_);  // view_raw_ stays valid so lifecycle keeps dispatching
}

void ViewBridge::close() {
    if (!view_raw_) return;
    if (attached_) {
        PULP_TRY { processor_.on_view_closed(*view_raw_); }
        PULP_CATCH_ALL {}
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
        PULP_TRY { processor_.on_view_resized(*view_raw_, width, height); }
        PULP_CATCH_ALL {}
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
