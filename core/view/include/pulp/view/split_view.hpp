#pragma once

#include <pulp/view/view.hpp>
#include <functional>

namespace pulp::view {

// ── SplitView ───────────────────────────────────────────────────────────────
// Resizable split pane (horizontal/vertical).

class SplitView : public View {
public:
    enum class Orientation { horizontal, vertical };

    SplitView();

    /// Set the two child views.
    void set_first(std::unique_ptr<View> view);
    void set_second(std::unique_ptr<View> view);
    View* first() const { return first_; }
    View* second() const { return second_; }

    /// Split orientation.
    void set_orientation(Orientation o) { orientation_ = o; }
    Orientation orientation() const { return orientation_; }

    /// Split position as a fraction [0, 1]. Default 0.5.
    void set_split_fraction(float f);
    float split_fraction() const { return split_fraction_; }

    /// Minimum size for each pane (pixels).
    void set_min_first_size(float px) { min_first_ = px; }
    void set_min_second_size(float px) { min_second_ = px; }

    /// Divider width in pixels.
    void set_divider_width(float w) { divider_width_ = w; }
    float divider_width() const { return divider_width_; }

    /// Callback when split position changes.
    std::function<void(float fraction)> on_split_changed;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_event(const MouseEvent& event) override;
    bool wants_mouse_input() const override { return true; }

    void layout_children() override;

private:
    Orientation orientation_ = Orientation::horizontal;
    float split_fraction_ = 0.5f;
    float min_first_ = 50.0f;
    float min_second_ = 50.0f;
    float divider_width_ = 4.0f;
    View* first_ = nullptr;
    View* second_ = nullptr;
    bool dragging_ = false;

    Rect divider_rect() const;
    bool is_over_divider(Point pos) const;
};

} // namespace pulp::view
