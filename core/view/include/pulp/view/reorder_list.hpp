#pragma once

/// @file reorder_list.hpp
/// Drag-to-reorder container.
///
/// A faithful port re-rolls the same interaction every time it has a rack of
/// effect slots, a modulation list, or a preset order: pick a row up, slide the
/// neighbours aside to open a gap, drop it into a new slot, and commit the new
/// order. ReorderList packages that once — a single-axis (vertical or horizontal)
/// container of fixed-pitch items where the user drags an item to a new index; on
/// drop it permutes the display order, settles the lifted item with a drop tween
/// (reusing View::animate), flashes a landing glow, and fires on_reorder(from, to).
///
/// The index arithmetic (`reorder_target_index`) is a pure function of the drag
/// offset, the item pitch, and the count, so the reorder decision is unit-testable
/// headlessly without a window or a frame clock.

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/geometry.hpp>
#include <pulp/view/view.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace pulp::view {

/// Target index for a drag that started on item `from`, moved `offset` along the
/// main axis, over a list of `count` items spaced `pitch` apart. Rounds the offset
/// to the nearest whole item and clamps into [0, count-1]. A non-positive pitch or
/// empty list returns `from` (no move). Pure — the reorder brain, no view state.
int reorder_target_index(int from, float offset, float pitch, int count);

/// Single-axis drag-to-reorder container. Items are laid out at a fixed pitch
/// (item extent + gap) along the main axis; dragging one moves it and slides the
/// neighbours, and dropping commits the new order.
class ReorderList : public View {
public:
    enum class Orientation { vertical, horizontal };

    ReorderList() = default;

    /// Append an item (ownership moves in). The item is made non-hit-testable so
    /// the whole row acts as a drag handle and the drag routes to the container
    /// (a consumer that needs interactive item content can re-enable hit-testing
    /// on inner children and drive reorder through the exposed handlers).
    void add_item(std::unique_ptr<View> item);

    int item_count() const { return static_cast<int>(items_.size()); }
    /// Item at DISPLAY index `i` (reflects reorders), or nullptr out of range.
    View* item_at(int i) const {
        return (i >= 0 && i < item_count()) ? items_[i] : nullptr;
    }
    /// Current display index of `v`, or -1 if not an item.
    int index_of(const View* v) const;

    void set_orientation(Orientation o) { orientation_ = o; }
    Orientation orientation() const { return orientation_; }
    void set_item_extent(float px) { item_extent_ = px; }  ///< height (vertical) / width (horizontal)
    void set_gap(float px) { gap_ = px; }
    /// Distance between adjacent item origins along the main axis.
    float pitch() const { return item_extent_ + gap_; }

    /// The item currently being dragged (display index), or -1 when idle.
    int dragging_index() const { return dragging_; }
    /// The index the dragged item would land on right now, or -1 when idle.
    int drop_target_index() const { return target_; }

    /// Fired once on a committed reorder (drop that changed the index). `from` and
    /// `to` are display indices before the move. Not fired for a no-op drop.
    std::function<void(int from, int to)> on_reorder;

    void layout_children() override;
    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_up(Point pos) override;

private:
    // Main-axis coordinate of a point in this view's local space.
    float main_of(Point p) const {
        return orientation_ == Orientation::vertical ? p.y : p.x;
    }
    // Position item `display_index` at its resting slot (accounting for an
    // in-progress drag that opened a gap), writing its bounds.
    void position_item(int display_index) const;

    std::vector<View*> items_;   ///< display order; ownership in the View child list
    Orientation orientation_ = Orientation::vertical;
    float item_extent_ = 40.0f;
    float gap_ = 6.0f;

    int dragging_ = -1;          ///< display index being dragged, or -1
    int target_ = -1;            ///< live drop target, or -1
    float drag_start_main_ = 0;  ///< main-axis coord where the drag began
    float drag_offset_ = 0;      ///< live main-axis offset from the start
    ValueAnimation settle_{0.0f};   ///< drop tween: residual offset → 0
    ValueAnimation glow_{0.0f};     ///< landing glow: 1 → 0 after a drop
    int glow_index_ = -1;           ///< item the landing glow plays on
};

}  // namespace pulp::view
