#include <pulp/view/scroll_bar.hpp>

#include <algorithm>

namespace pulp::view {

namespace {
// Visual reservations at the two endcaps for arrow buttons. The standalone
// ScrollBar doesn't draw OS-native arrow chevrons (those vary per platform),
// but it does reserve the space so the thumb math matches what users expect
// from a scrollbar and so a future paint override can drop in arrows without
// reworking the layout.
constexpr float kArrowReserve = 16.0f;
} // namespace

float ScrollBar::primary_axis_length() const {
    auto b = local_bounds();
    float full = orientation_ == Orientation::vertical ? b.height : b.width;
    return std::max(0.0f, full - 2.0f * kArrowReserve);
}

float ScrollBar::thumb_pixel_length() const {
    const float axis = primary_axis_length();
    if (axis <= 0.0f) return 0.0f;
    const float range = max_ - min_;
    if (range <= 0.0f) return axis;  // degenerate → full thumb
    const float total = range + page_size_;
    if (total <= 0.0f) return axis;
    const float ratio = std::clamp(page_size_ / total, 0.0f, 1.0f);
    // Floor at 16px so the thumb stays grabbable even on long ranges.
    return std::max(16.0f, axis * ratio);
}

float ScrollBar::thumb_pixel_offset() const {
    const float axis = primary_axis_length();
    if (axis <= 0.0f) return 0.0f;
    const float range = max_ - min_;
    if (range <= 0.0f) return 0.0f;
    const float thumb = thumb_pixel_length();
    const float t = std::clamp((value_ - min_) / range, 0.0f, 1.0f);
    return t * std::max(0.0f, axis - thumb);
}

bool ScrollBar::point_in_thumb(Point local) const {
    const float thumb = thumb_pixel_length();
    const float offset = thumb_pixel_offset() + kArrowReserve;
    if (orientation_ == Orientation::vertical) {
        return local.y >= offset && local.y <= offset + thumb;
    }
    return local.x >= offset && local.x <= offset + thumb;
}

void ScrollBar::apply_drag(Point local) {
    const float axis = primary_axis_length();
    if (axis <= 0.0f) return;
    const float thumb = thumb_pixel_length();
    const float usable = std::max(1.0f, axis - thumb);
    const float p = (orientation_ == Orientation::vertical ? local.y : local.x)
                    - kArrowReserve - drag_grab_offset_;
    const float t = std::clamp(p / usable, 0.0f, 1.0f);
    set_value(min_ + t * (max_ - min_));
}

void ScrollBar::apply_track_click(Point local) {
    // Click above/left of the thumb → page back; below/right → page forward.
    const float pos = orientation_ == Orientation::vertical ? local.y : local.x;
    const float thumb_lead = thumb_pixel_offset() + kArrowReserve;
    const float thumb_trail = thumb_lead + thumb_pixel_length();
    if (pos < thumb_lead) {
        set_value(value_ - page_step_);
    } else if (pos > thumb_trail) {
        set_value(value_ + page_step_);
    }
}

void ScrollBar::on_mouse_event(const MouseEvent& event) {
    if (!event.is_down) {
        dragging_ = false;
        return;
    }
    if (point_in_thumb(event.position)) {
        // Capture the grab offset so the thumb doesn't jump under the cursor
        // on the first drag tick.
        const float pos = orientation_ == Orientation::vertical
                              ? event.position.y
                              : event.position.x;
        drag_grab_offset_ = pos - kArrowReserve - thumb_pixel_offset();
        dragging_ = true;
    } else {
        dragging_ = false;
        apply_track_click(event.position);
    }
}

void ScrollBar::on_mouse_drag(Point pos) {
    if (!dragging_) return;
    apply_drag(pos);
}

bool ScrollBar::on_key_event(const KeyEvent& event) {
    if (!event.is_down) return false;
    const bool vert = orientation_ == Orientation::vertical;

    switch (event.key) {
        case KeyCode::up:
        case KeyCode::left:
            // up / left = decrease for both orientations (matches AppKit +
            // GTK scrollbars: a scrollbar's leading direction means smaller
            // value regardless of axis).
            return set_value(value_ - arrow_step_) || true;
        case KeyCode::down:
        case KeyCode::right:
            return set_value(value_ + arrow_step_) || true;
        case KeyCode::page_up:
            return set_value(value_ - page_step_) || true;
        case KeyCode::page_down:
            return set_value(value_ + page_step_) || true;
        case KeyCode::home:
            return set_value(min_) || true;
        case KeyCode::end_:
            return set_value(max_) || true;
        default:
            (void)vert;
            return false;
    }
}

void ScrollBar::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    if (b.width <= 0 || b.height <= 0) return;

    const auto track_color =
        resolve_color("control.track", canvas::Color::rgba8(60, 60, 60));
    const auto thumb_color =
        resolve_color("control.thumb", canvas::Color::rgba8(180, 180, 180));

    canvas.set_fill_color(track_color);
    canvas.fill_rounded_rect(0, 0, b.width, b.height,
                             std::min(b.width, b.height) * 0.4f);

    const float thumb = thumb_pixel_length();
    if (thumb <= 0.0f) return;
    const float lead = thumb_pixel_offset() + kArrowReserve;

    canvas.set_fill_color(thumb_color);
    if (orientation_ == Orientation::vertical) {
        canvas.fill_rounded_rect(2.0f, lead, std::max(2.0f, b.width - 4.0f),
                                 thumb, std::max(2.0f, (b.width - 4.0f) * 0.5f));
    } else {
        canvas.fill_rounded_rect(lead, 2.0f, thumb,
                                 std::max(2.0f, b.height - 4.0f),
                                 std::max(2.0f, (b.height - 4.0f) * 0.5f));
    }
}

} // namespace pulp::view
