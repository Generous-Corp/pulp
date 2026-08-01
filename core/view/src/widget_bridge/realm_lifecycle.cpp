#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/native_view_host.hpp>
#include <pulp/view/ui_components.hpp>

#include "value_widget_access.hpp"

#include <algorithm>
#include <cmath>

namespace pulp::view {

void WidgetBridge::clear(const DeadlineCheck& deadline_check) {
    clear_realm(RealmClearPolicy::NotifyCurrentRealm, deadline_check);
}

void WidgetBridge::clear_for_realm_replacement(
    const DeadlineCheck& deadline_check) {
    clear_realm(RealmClearPolicy::SkipRealmCallbacks, deadline_check);
    if (deadline_check)
        realm_retired_ = true;
}

void WidgetBridge::clear_quarantined_realm() {
    clear_realm(RealmClearPolicy::SkipRealmCallbacks, {}, true);
}

void WidgetBridge::clear_realm(
    RealmClearPolicy policy, const DeadlineCheck& deadline_check,
    bool owned_root_children_only) {
    if (deadline_check && !owned_root_children_only) {
        constexpr std::size_t kMaxDeadlineResetViews = 2048;
        std::vector<View*> roots_to_remove;
        roots_to_remove.reserve(root_.child_count());
        std::size_t view_count = 0;
        const auto count_views = [&](auto&& self, View* node) -> void {
            deadline_check();
            if (node == nullptr)
                return;
            if (++view_count > kMaxDeadlineResetViews)
                throw std::runtime_error(
                    "Runtime.evaluate realm reset tree exceeds bounded cleanup limit");
            if (const auto* native = dynamic_cast<const NativeViewHost*>(node);
                native != nullptr && native->is_native_attached())
                throw std::runtime_error(
                    "Runtime.evaluate realm reset cannot replace an attached native view");
            for (std::size_t child = 0; child < node->child_count(); ++child)
                self(self, node->child_at(child));
        };
        for (std::size_t index = 0; index < root_.child_count(); ++index) {
            deadline_check();
            auto* child = root_.child_at(index);
            count_views(count_views, child);
            roots_to_remove.push_back(child);
        }
        root_.prepare_children_for_realm_reset();
        deadline_check();
        if (callback_alive_)
            callback_alive_->store(false, std::memory_order_release);
        defer_all_param_gesture_routes();
        deadline_check();
        unregister_global_dispatch(deadline_check);
        deadline_check();
        retired_root_click_callback_ = std::move(root_.on_global_click);
        invalidate_cached_subtrees_everywhere(
            roots_to_remove, deadline_check);
        // Everything below is constant-work ownership transfer. The retired
        // bridge retains all maps, callbacks, drag state, interaction slots,
        // and widgets until the next UI pump outside this response fence.
        ComboBox::abandon_active_popup(root_);
        root_.retire_interaction_state_for_realm_reset(
            retired_root_drag_, retired_root_interaction_);
        root_.retire_children_for_realm_reset(retired_root_children_);
        return;
    }

    pending_frame_ids_.clear();
    shortcuts_.clear();
    release_all_param_gesture_routes();
    param_bindings_.clear();  // bindings reference widgets torn down below
    // Scoped quarantine teardown may run after a replacement realm has opened
    // its own ComboBox. Removing an old owned ComboBox destroys it and clears
    // its popup slots; the process-global close would target the replacement.
    if (!owned_root_children_only)
        ComboBox::close_active_popup(root_);

    const bool notify_js = policy == RealmClearPolicy::NotifyCurrentRealm;
    const auto owns = [&](const View* candidate) {
        return std::any_of(
            owned_widgets_.begin(), owned_widgets_.end(),
            [candidate](const auto& widget) {
                return widget.identifies(candidate);
            });
    };
    const auto remove_owned_children =
        [&](auto&& self, View& parent) -> void {
            for (auto index = parent.child_count(); index > 0; --index) {
                auto* child = parent.child_at(index - 1);
                self(self, *child);
                if (owns(child)) {
                    std::vector<std::unique_ptr<View>> preserved_children;
                    while (child->child_count() > 0) {
                        preserved_children.push_back(
                            child->remove_child(
                                child->child_at(child->child_count() - 1)));
                    }
                    auto removed = parent.remove_child(child);
                    forget_widget_subtree(
                        removed.get(), false, deadline_check, notify_js);
                    for (auto preserved = preserved_children.rbegin();
                         preserved != preserved_children.rend(); ++preserved) {
                        parent.add_child(std::move(*preserved));
                    }
                }
                if (deadline_check) deadline_check();
            }
        };
    if (owned_root_children_only) {
        remove_owned_children(remove_owned_children, root_);
    } else {
        for (auto index = root_.child_count(); index > 0; --index) {
            auto* child = root_.child_at(index - 1);
            auto removed = root_.remove_child(child);
            forget_widget_subtree(
                removed.get(), false, deadline_check, notify_js);
            if (deadline_check) deadline_check();
        }
    }
    owned_widgets_.clear();
    widgets_.clear();
    registrations_.clear();
}

void WidgetBridge::quarantine_realm() noexcept {
    unregister_global_dispatch();
    if (callback_alive_)
        callback_alive_->store(false, std::memory_order_release);
    root_.on_global_click = {};
    begin_root_quarantine();
    try {
        // set_visible() invalidates retained scene caches but does not schedule
        // a frame. Route through the bridge invalidator so composite hosts that
        // replaced the default root repaint callback also erase this realm.
        request_repaint();
    } catch (...) {
        // Quarantine is fail-close cleanup and must preserve its noexcept
        // contract even when a host-supplied invalidator throws.
    }
}

void WidgetBridge::snapshot_values(
    std::unordered_map<std::string, float>& out) const {
    for (auto& [id, view] : widgets_) {
        float value = 0.0f;
        if (try_get_scalar_value(view, value)) out[id] = value;
    }
}

void WidgetBridge::restore_values(
    const std::unordered_map<std::string, float>& snapshot) {
    for (auto& [id, val] : snapshot) {
        auto it = widgets_.find(id);
        if (it == widgets_.end()) continue;
        try_set_scalar_value(it->second, val);
    }
}

void WidgetBridge::snapshot_values(
    WidgetReloadSnapshot& out, const DeadlineCheck& deadline_check,
    bool include_custom_state) const {
    for (auto& [id, state] : widgets_) {
        if (deadline_check) deadline_check();
        View* view = state.view;
        float value = 0.0f;
        // Selection controls preserve their index. Restore uses the silent
        // setter so reload does not impersonate a user edit.
        if (try_get_scalar_value(view, value)) {
            out.scalar_values[id] = value;
        } else if (auto* combo = dynamic_cast<ComboBox*>(view)) {
            out.scalar_values[id] = static_cast<float>(combo->selected());
        } else if (auto* seg = dynamic_cast<SegmentedControl*>(view)) {
            out.scalar_values[id] = static_cast<float>(seg->selected());
        } else if (auto* xy = dynamic_cast<XYPad*>(view)) {
            out.xy_values[id] = {.x = xy->x_value(), .y = xy->y_value()};
        }

        // A custom widget may preserve an opaque blob independently of its
        // built-in scalar/selection state.
        if (include_custom_state) {
            std::string blob;
            if (view->save_reload_state(blob))
                out.custom_state[id] = std::move(blob);
        }
    }
    if (deadline_check) deadline_check();
}

void WidgetBridge::restore_values(
    const WidgetReloadSnapshot& snapshot, const DeadlineCheck& deadline_check,
    bool include_custom_state) {
    for (auto& [id, val] : snapshot.scalar_values) {
        if (deadline_check) deadline_check();
        auto it = widgets_.find(id);
        if (it == widgets_.end()) continue;
        if (try_set_scalar_value(it->second, val)) continue;
        if (auto* combo = dynamic_cast<ComboBox*>(it->second.view)) {
            combo->set_selected_silent(static_cast<int>(std::lround(val)));
        } else if (auto* seg = dynamic_cast<SegmentedControl*>(it->second.view)) {
            seg->set_selected_silent(static_cast<int>(std::lround(val)));
        }
    }
    for (auto& [id, val] : snapshot.xy_values) {
        if (deadline_check) deadline_check();
        auto it = widgets_.find(id);
        if (it == widgets_.end()) continue;
        if (auto* xy = dynamic_cast<XYPad*>(it->second.view)) {
            xy->set_x(val.x);
            xy->set_y(val.y);
        }
    }
    if (include_custom_state) {
        for (auto& [id, blob] : snapshot.custom_state) {
            if (deadline_check) deadline_check();
            auto it = widgets_.find(id);
            if (it == widgets_.end()) continue;
            (void)it->second->restore_reload_state(blob);
        }
    }
    if (deadline_check) deadline_check();
}

} // namespace pulp::view
