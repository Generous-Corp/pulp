#pragma once

/// @file component_dragger.hpp
/// ComponentDragger — drag-to-move helper for any View
/// (closes the gap-doc P2 row "Drag & drop ComponentDragger utility").
///
/// A `ComponentDragger` adds "click and drag to move me within my parent"
/// behavior to any `View`. It is intentionally a free helper (not a mixin)
/// so that owners stay in control of which mouse phases get forwarded.
///
/// Usage:
/// @code
///   ComponentDragger dragger;
///   class DraggableBox : public View {
///       ComponentDragger dragger;
///   public:
///       void on_mouse_event(const MouseEvent& e) override {
///           if (e.isPress())   dragger.start_dragging(*this, e.position);
///           else if (e.isDrag()) dragger.drag_view(*this, e.position);
///       }
///   };
/// @endcode
///
/// `start_dragging()` snapshots the drag origin (in the dragged view's
/// local coordinates) plus the view's bounds at gesture start.
/// `drag_view()` translates the bounds by the delta between the current
/// pointer position and that snapshot, optionally constraining the new
/// origin so the view stays inside its parent's local bounds.
///
/// Headless-friendly: no window host, no scheduler. Pure arithmetic over
/// the dragged view's bounds; tests can call `start_dragging` /
/// `drag_view` directly without simulating real mouse events.
///
/// License-lineage note: the name is Pulp-native per the gap-doc rule —
/// the behavior mirrors a well-trodden idiom across UI toolkits, but the
/// implementation here is independent.

#include <algorithm>
#include <pulp/view/geometry.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {

class ComponentDragger {
public:
    ComponentDragger() = default;

    /// Begin a drag gesture. `mouse_local` is the press position in the
    /// dragged view's LOCAL coordinate space (i.e. the same `pos` value
    /// you'd hand to `View::on_mouse_down`).
    void start_dragging(View& view, Point mouse_local) {
        mouse_start_ = mouse_local;
        bounds_start_ = view.bounds();
        active_ = true;
    }

    /// Continue a drag gesture. Moves the view's bounds so the press
    /// position stays under the cursor. Honors `constrain_to_parent()`.
    /// No-op when not currently dragging.
    void drag_view(View& view, Point mouse_local) {
        if (!active_) return;
        const Point delta{mouse_local.x - mouse_start_.x,
                          mouse_local.y - mouse_start_.y};
        Rect target = bounds_start_;
        target.x += delta.x;
        target.y += delta.y;

        if (constrain_to_parent_) {
            if (auto* p = view.parent()) {
                const Rect parent_local = p->local_bounds();
                target.x = std::clamp(target.x,
                                      parent_local.x,
                                      std::max(parent_local.x,
                                               parent_local.right() - target.width));
                target.y = std::clamp(target.y,
                                      parent_local.y,
                                      std::max(parent_local.y,
                                               parent_local.bottom() - target.height));
            }
        }
        view.set_bounds(target);
        if (on_drag) on_drag(target);
    }

    /// End the gesture. Subsequent `drag_view` calls become no-ops until
    /// the next `start_dragging`.
    void end_dragging() {
        active_ = false;
        if (on_end) on_end();
    }

    /// When true (the default), `drag_view` clamps the dragged view's
    /// origin so it stays fully inside the parent's local bounds.
    void set_constrain_to_parent(bool yes) { constrain_to_parent_ = yes; }
    bool constrain_to_parent() const { return constrain_to_parent_; }

    bool is_dragging() const { return active_; }

    /// Mouse-local press position captured at `start_dragging`.
    Point mouse_start() const { return mouse_start_; }
    /// Dragged view's bounds captured at `start_dragging`.
    Rect bounds_start() const { return bounds_start_; }

    /// Optional callbacks for tests / app glue.
    std::function<void(Rect)> on_drag;
    std::function<void()> on_end;

private:
    bool active_ = false;
    bool constrain_to_parent_ = true;
    Point mouse_start_{};
    Rect bounds_start_{};
};

} // namespace pulp::view
