#pragma once

// LassoComponent — rubber-band rectangle selection overlay.
// Draw a selection rectangle by clicking and dragging on an empty area.
// Items within the rectangle are added to the selection set.
// Inspired by PlunderTube's VisageMarqueeOverlay pattern.

#include <pulp/view/view.hpp>
#include <functional>
#include <vector>
#include <cmath>
#include <algorithm>

namespace pulp::view {

/// Rectangle in parent coordinates
struct SelectionRect {
    float x = 0, y = 0, width = 0, height = 0;

    bool contains(float px, float py) const {
        return px >= x && px < x + width && py >= y && py < y + height;
    }

    bool intersects(float rx, float ry, float rw, float rh) const {
        return x < rx + rw && x + width > rx && y < ry + rh && y + height > ry;
    }
};

/// Lasso (rubber-band) selection overlay.
/// Attach to a container view. Handles mouse drag to create a selection rectangle.
/// Calls back with the selection rect so the parent can determine which items are selected.
class LassoComponent : public View {
public:
    LassoComponent() {
        set_focusable(false);
    }

    /// Called during drag with the current selection rectangle (in parent coords).
    /// The parent should use this to highlight items that intersect the rect.
    std::function<void(const SelectionRect&)> on_selection_changed;

    /// Called when the drag ends with the final selection rectangle.
    std::function<void(const SelectionRect&)> on_selection_complete;

    /// Start a new lasso selection at the given point.
    void begin_selection(float x, float y) {
        active_ = true;
        start_x_ = x;
        start_y_ = y;
        current_x_ = x;
        current_y_ = y;
        set_visible(true);
    }

    /// Update the selection rectangle during drag.
    void update_selection(float x, float y) {
        if (!active_) return;
        current_x_ = x;
        current_y_ = y;

        if (on_selection_changed)
            on_selection_changed(selection_rect());
    }

    /// End the selection.
    void end_selection() {
        if (!active_) return;

        if (on_selection_complete)
            on_selection_complete(selection_rect());

        active_ = false;
        set_visible(false);
    }

    /// Whether a lasso selection is currently active.
    bool is_active() const { return active_; }

    /// Get the current selection rectangle.
    SelectionRect selection_rect() const {
        float x = std::min(start_x_, current_x_);
        float y = std::min(start_y_, current_y_);
        float w = std::abs(current_x_ - start_x_);
        float h = std::abs(current_y_ - start_y_);
        return {x, y, w, h};
    }

    void paint(canvas::Canvas& canvas) override {
        if (!active_) return;

        auto rect = selection_rect();

        // Draw selection rectangle with semi-transparent fill and border
        canvas.set_fill_color(canvas::Color(100, 150, 255, 40));
        canvas.fill_rect(rect.x, rect.y, rect.width, rect.height);

        canvas.set_stroke_color(canvas::Color(100, 150, 255, 180));
        canvas.set_line_width(1.0f);
        canvas.stroke_rect(rect.x, rect.y, rect.width, rect.height);
    }

    void on_mouse_down(Point pos) override {
        begin_selection(pos.x, pos.y);
    }

    void on_mouse_drag(Point pos) override {
        update_selection(pos.x, pos.y);
    }

    void on_mouse_up(Point /*pos*/) override {
        end_selection();
    }

private:
    bool active_ = false;
    float start_x_ = 0, start_y_ = 0;
    float current_x_ = 0, current_y_ = 0;
};

}  // namespace pulp::view
