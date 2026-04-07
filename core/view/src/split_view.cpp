#include <pulp/view/split_view.hpp>
#include <algorithm>

namespace pulp::view {

SplitView::SplitView() {
    set_focusable(false);
}

void SplitView::set_first(std::unique_ptr<View> view) {
    if (first_) remove_child(first_);
    first_ = view.get();
    if (view) add_child(std::move(view));
}

void SplitView::set_second(std::unique_ptr<View> view) {
    if (second_) remove_child(second_);
    second_ = view.get();
    if (view) add_child(std::move(view));
}

void SplitView::set_split_fraction(float f) {
    split_fraction_ = std::clamp(f, 0.0f, 1.0f);
}

Rect SplitView::divider_rect() const {
    auto b = local_bounds();
    if (orientation_ == Orientation::horizontal) {
        float x = b.x + split_fraction_ * b.width - divider_width_ / 2;
        return {x, b.y, divider_width_, b.height};
    } else {
        float y = b.y + split_fraction_ * b.height - divider_width_ / 2;
        return {b.x, y, b.width, divider_width_};
    }
}

bool SplitView::is_over_divider(Point pos) const {
    auto dr = divider_rect();
    float expand = 4.0f;
    if (orientation_ == Orientation::horizontal)
        return pos.x >= dr.x - expand && pos.x <= dr.x + dr.width + expand &&
               pos.y >= dr.y && pos.y <= dr.y + dr.height;
    else
        return pos.y >= dr.y - expand && pos.y <= dr.y + dr.height + expand &&
               pos.x >= dr.x && pos.x <= dr.x + dr.width;
}

void SplitView::layout_children() {
    auto b = local_bounds();
    auto dr = divider_rect();

    if (orientation_ == Orientation::horizontal) {
        float first_w = dr.x - b.x;
        float second_x = dr.x + dr.width;
        float second_w = b.x + b.width - second_x;
        if (first_) first_->set_bounds({b.x, b.y, first_w, b.height});
        if (second_) second_->set_bounds({second_x, b.y, second_w, b.height});
    } else {
        float first_h = dr.y - b.y;
        float second_y = dr.y + dr.height;
        float second_h = b.y + b.height - second_y;
        if (first_) first_->set_bounds({b.x, b.y, b.width, first_h});
        if (second_) second_->set_bounds({b.x, second_y, b.width, second_h});
    }
}

void SplitView::paint(canvas::Canvas& canvas) {
    auto dr = divider_rect();
    auto divider_color = resolve_color("control.border", Color::rgba8(80, 80, 90));
    canvas.set_fill_color(divider_color);
    canvas.fill_rect(dr.x, dr.y, dr.width, dr.height);

    // Grip dots
    float cx = dr.x + dr.width / 2;
    float cy = dr.y + dr.height / 2;
    auto grip = resolve_color("text.disabled", Color::rgba8(100, 100, 110));
    canvas.set_fill_color(grip);

    if (orientation_ == Orientation::horizontal) {
        for (float dy : {-8.0f, 0.0f, 8.0f})
            canvas.fill_circle(cx, cy + dy, 1.5f);
    } else {
        for (float dx : {-8.0f, 0.0f, 8.0f})
            canvas.fill_circle(cx + dx, cy, 1.5f);
    }
}

void SplitView::on_mouse_down(Point pos) {
    if (is_over_divider(pos))
        dragging_ = true;
}

void SplitView::on_mouse_drag(Point pos) {
    if (!dragging_) return;
    auto b = local_bounds();
    if (orientation_ == Orientation::horizontal) {
        float new_f = (pos.x - b.x) / b.width;
        float min_f = min_first_ / b.width;
        float max_f = 1.0f - min_second_ / b.width;
        split_fraction_ = std::clamp(new_f, min_f, max_f);
    } else {
        float new_f = (pos.y - b.y) / b.height;
        float min_f = min_first_ / b.height;
        float max_f = 1.0f - min_second_ / b.height;
        split_fraction_ = std::clamp(new_f, min_f, max_f);
    }
    layout_children();
    if (on_split_changed) on_split_changed(split_fraction_);
}

void SplitView::on_mouse_up(Point pos) {
    (void)pos;
    dragging_ = false;
}

void SplitView::on_mouse_event(const MouseEvent& event) {
    View::on_mouse_event(event);
}

} // namespace pulp::view
