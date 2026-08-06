#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/native_view_host.hpp>
#include <pulp/view/ui_components.hpp>

#include "bridge_dispatch.hpp"
#include "value_widget_access.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace pulp::view {

namespace {

enum class BoundedRetirementBlocker {
    none,
    too_many_views,
    attached_native_view,
};

BoundedRetirementBlocker validate_bounded_retirement_tree(
    const View& root, const WidgetBridge::DeadlineCheck* deadline_check) {
    constexpr std::size_t kMaxDeadlineResetViews = 2048;
    std::size_t view_count = 0;
    const auto validate = [&](auto&& self, const View* node)
        -> BoundedRetirementBlocker {
        if (deadline_check) (*deadline_check)();
        if (node == nullptr)
            return BoundedRetirementBlocker::none;
        ++view_count;
        if (view_count > kMaxDeadlineResetViews)
            return BoundedRetirementBlocker::too_many_views;
        if (const auto* native = dynamic_cast<const NativeViewHost*>(node);
            native != nullptr && native->is_native_attached())
            return BoundedRetirementBlocker::attached_native_view;
        for (std::size_t child = 0; child < node->child_count(); ++child) {
            const auto blocker = self(self, node->child_at(child));
            if (blocker != BoundedRetirementBlocker::none)
                return blocker;
        }
        return BoundedRetirementBlocker::none;
    };
    for (std::size_t index = 0; index < root.child_count(); ++index) {
        const auto blocker = validate(validate, root.child_at(index));
        if (blocker != BoundedRetirementBlocker::none)
            return blocker;
    }
    return BoundedRetirementBlocker::none;
}

std::string_view bounded_retirement_denial(
    BoundedRetirementBlocker blocker) noexcept {
    switch (blocker) {
        case BoundedRetirementBlocker::too_many_views:
            return "realm reset tree exceeds bounded cleanup limit";
        case BoundedRetirementBlocker::attached_native_view:
            return "realm reset cannot replace an attached native view";
        case BoundedRetirementBlocker::none:
            return {};
    }
    return {};
}

} // namespace

void WidgetBridge::clear(const DeadlineCheck& deadline_check) {
    clear_realm(deadline_check);
}

void WidgetBridge::clear_for_realm_replacement(
    const DeadlineCheck& deadline_check) {
    retire_realm(deadline_check);
}

std::string_view WidgetBridge::bounded_realm_retirement_denial() const noexcept {
    return bounded_retirement_denial(
        validate_bounded_retirement_tree(root_, nullptr));
}

void WidgetBridge::clear_quarantined_realm() {
    pending_frame_ids_.clear();
    shortcuts_.clear();
    release_all_param_gesture_routes();
    param_bindings_.clear();

    const auto exact_owned_by_this = [&](const View* candidate) {
        return std::any_of(
            owned_widgets_.begin(), owned_widgets_.end(),
            [candidate](const auto& widget) {
                return widget.identifies(candidate);
            });
    };
    const auto foreign_owned = foreign_owned_widget_states();
    const auto exact_owned_by_foreign = [&](const View* candidate) {
        return std::any_of(
            foreign_owned.begin(), foreign_owned.end(),
            [candidate](const auto& widget) {
                return widget.identifies(candidate);
            });
    };

    // An unregistered native/custom descendant belongs to the bridge whose
    // registered ancestor contains it. A registered node from another bridge
    // starts a new ownership island; its unregistered descendants follow it.
    // Precompute this classification while the live tree is unchanged.
    std::vector<View*> realm_views;
    const auto classify = [&](auto&& self, View& node,
                              bool inherited_this_realm) -> void {
        bool this_realm = inherited_this_realm;
        if (exact_owned_by_foreign(&node))
            this_realm = false;
        if (exact_owned_by_this(&node))
            this_realm = true;
        if (this_realm)
            realm_views.push_back(&node);
        for (std::size_t index = 0; index < node.child_count(); ++index)
            self(self, *node.child_at(index), this_realm);
    };
    realm_views.reserve(owned_widgets_.size());
    for (std::size_t index = 0; index < root_.child_count(); ++index)
        classify(classify, *root_.child_at(index), false);
    const auto owns = [&](const View* candidate) {
        return std::find(realm_views.begin(), realm_views.end(), candidate)
               != realm_views.end();
    };

    // Complete every allocation before changing ownership. Once extraction
    // starts, no virtual detach hook or allocating operation may interrupt
    // transfer into retired_realm_. Each parent's final direct-child
    // count includes the foreign ownership islands lifted through a retired
    // child, so adopt_child_for_realm_reset() can stay noexcept.
    retired_realm_.children.reserve(
        retired_realm_.children.size() + realm_views.size());
    const auto reserve_final_children = [&](auto&& self, View& parent)
        -> std::size_t {
        std::size_t final_count = 0;
        for (std::size_t index = 0; index < parent.child_count(); ++index) {
            auto& child = *parent.child_at(index);
            const auto child_final_count = self(self, child);
            final_count += owns(&child) ? child_final_count : 1;
        }
        parent.children_.reserve(final_count);
        return final_count;
    };
    reserve_final_children(reserve_final_children, root_);

    const auto retire_owned = [&](auto&& self, View& parent) -> void {
        for (auto index = parent.child_count(); index > 0; --index) {
            auto* child = parent.child_at(index - 1);
            self(self, *child);
            if (!owns(child))
                continue;

            // Descendants owned by newer bridges were left in this node by the
            // recursive pass. Lift them into the live parent before retaining
            // this realm's exact creation identity.
            auto removed = parent.extract_child_for_realm_reset(child);
            while (!removed->children_.empty()) {
                auto survivor = removed->extract_child_for_realm_reset(
                    removed->children_.front().get());
                parent.adopt_child_for_realm_reset(std::move(survivor));
            }
            retired_realm_.children.push_back(std::move(removed));
        }
    };
    retire_owned(retire_owned, root_);

    // Foreign descendants have now been lifted out of the retired components,
    // so invalidating these roots cannot erase the newer bridge's live cache.
    if (!realm_views.empty())
        invalidate_cached_subtrees_everywhere(realm_views);

    owned_widgets_.clear();
    widgets_.clear();
    registrations_.clear();
}

void WidgetBridge::force_retire_root_for_owner_teardown() noexcept {
    unregister_global_dispatch();
    if (callback_alive_)
        callback_alive_->store(false, std::memory_order_release);
    if (root_.child_count() == 0)
        return;

    // This is deliberately broader than selective quarantine cleanup. It is
    // reached only after allocation failure, where preserving unrelated root
    // children would leave their realm-borrowing descendants unsafe. Every
    // destination slot was constructed with the bridge, so the transition does
    // not allocate, call user code, or destroy the detached graph.
    commit_realm_retirement(emergency_retired_realm_);
}

void WidgetBridge::commit_realm_retirement(
    RetiredRealmState& destination) noexcept {
    assert(destination.children.empty());
    assert(destination.drag == nullptr);
    assert(destination.interaction == nullptr);
    destination.root_click_callback = std::move(root_.on_global_click);
    ComboBox::abandon_active_popup(root_);
    root_.retire_interaction_state_for_realm_reset(
        destination.drag, destination.interaction);
    root_.retire_children_for_realm_reset(destination.children);
    realm_retired_ = true;
}

void WidgetBridge::clear_realm(const DeadlineCheck& deadline_check) {
    pending_frame_ids_.clear();
    shortcuts_.clear();
    release_all_param_gesture_routes();
    param_bindings_.clear();

    ComboBox::close_active_popup(root_);
    for (auto index = root_.child_count(); index > 0; --index) {
        auto* child = root_.child_at(index - 1);
        auto removed = root_.remove_child(child);
        forget_widget_subtree(
            removed.get(), false, deadline_check, true);
        retire_removed_widget(std::move(removed));
        if (deadline_check) deadline_check();
    }
    owned_widgets_.clear();
    widgets_.clear();
    registrations_.clear();
}

void WidgetBridge::retire_realm(const DeadlineCheck& deadline_check) {
    if (realm_retired_)
        return;
    std::vector<View*> roots_to_retire;
    roots_to_retire.reserve(root_.child_count());
    for (std::size_t index = 0; index < root_.child_count(); ++index)
        roots_to_retire.push_back(root_.child_at(index));

    // All fallible work precedes the ownership transition. A bounded reset
    // rejects trees whose validation cannot fit inside its fixed grace window;
    // an ordinary reload uses the same transition without imposing that cap.
    if (deadline_check) {
        const auto denial = bounded_retirement_denial(
            validate_bounded_retirement_tree(root_, &deadline_check));
        if (!denial.empty())
            throw std::runtime_error("Runtime.evaluate " + std::string(denial));
    }
    const auto removed_recognizers = root_.collect_realm_reset_gestures();
    defer_all_param_gesture_routes();
    if (!roots_to_retire.empty())
        invalidate_cached_subtrees_everywhere(
            roots_to_retire, deadline_check);
    unregister_global_dispatch(deadline_check);

    // No deadline callback, allocating operation, or virtual lifecycle hook is
    // permitted after the bridge leaves the global registry and before the
    // child-vector swap. Retirement is one non-throwing ownership transition.
    root_.prepare_children_for_realm_reset(removed_recognizers);
    if (callback_alive_)
        callback_alive_->store(false, std::memory_order_release);
    // This is the single commit point for every realm replacement. The old
    // bridge owns all maps and the detached tree until its deferred teardown;
    // no recursive widget cleanup or user callback runs after this swap.
    commit_realm_retirement(retired_realm_);

    // Ordinary reload preserves synchronous native detach notification, but
    // only after the bridge owns a stable retired vector. Deadline replacement
    // skips arbitrary hooks and defers destruction as before.
    if (!deadline_check) {
        for (auto& child : retired_realm_.children)
            child->finish_realm_reset_detach();
    }

    // A retired realm may remain owned while native accessibility providers
    // drain. Once ordinary detach hooks have observed their old parent/clock,
    // make the old graph inert before a replacement can become live.
    for (auto& child : retired_realm_.children)
        child->disconnect_frame_clock_for_realm_reset();
}

void WidgetBridge::quarantine_realm() noexcept {
    unregister_global_dispatch();
    if (callback_alive_)
        callback_alive_->store(false, std::memory_order_release);
    // The first quarantine detaches this realm's root callback. A later
    // teardown pass may run after a replacement realm has installed its own
    // callback on the shared root, so repeated quarantine must not erase it.
    if (!realm_quarantined_)
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
