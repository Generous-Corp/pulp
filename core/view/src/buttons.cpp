#include <pulp/view/buttons.hpp>
#include <pulp/canvas/canvas.hpp>

namespace pulp::view {

// ── TextButton ──────────────────────────────────────────────────────────

void TextButton::paint(canvas::Canvas& canvas) {
    float w = bounds().width, h = bounds().height;
    float r = 6.0f;

    // Background
    auto bg = hovered_ ? canvas::Color::rgba(80, 80, 90) : canvas::Color::rgba(60, 60, 70);
    if (!enabled_) bg = canvas::Color::rgba(50, 50, 55);
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, w, h, r);

    // Border
    canvas.set_stroke_color(canvas::Color::rgba(100, 100, 110));
    canvas.set_line_width(1.0f);
    canvas.stroke_rect(0, 0, w, h);

    // Label
    auto text_color = enabled_ ? canvas::Color::rgba(220, 220, 230) : canvas::Color::rgba(120, 120, 130);
    canvas.set_fill_color(text_color);
    canvas.set_font("system", 14.0f);
    float text_w = canvas.measure_text(label_);
    canvas.fill_text(label_, (w - text_w) / 2.0f, h * 0.65f);
}

void TextButton::on_mouse_down(Point) {
    if (enabled_ && on_click) on_click();
}

void TextButton::on_mouse_enter() { hovered_ = true; }
void TextButton::on_mouse_leave() { hovered_ = false; }

// ── HyperlinkButton ─────────────────────────────────────────────────────

void HyperlinkButton::paint(canvas::Canvas& canvas) {
    float w = bounds().width, h = bounds().height;
    auto color = hovered_ ? canvas::Color::rgba(120, 160, 255) : canvas::Color::rgba(80, 130, 230);
    canvas.set_fill_color(color);
    canvas.set_font("system", 14.0f);
    canvas.fill_text(text_, 0, h * 0.7f);

    if (hovered_) {
        float text_w = canvas.measure_text(text_);
        canvas.set_stroke_color(color);
        canvas.set_line_width(1.0f);
        canvas.stroke_line(0, h * 0.75f, text_w, h * 0.75f);
    }
}

void HyperlinkButton::on_mouse_down(Point) {
    // URL opening is platform-specific — delegate to platform::open_url if available
}

void HyperlinkButton::on_mouse_enter() { hovered_ = true; }
void HyperlinkButton::on_mouse_leave() { hovered_ = false; }

// ── ArrowButton ─────────────────────────────────────────────────────────

void ArrowButton::paint(canvas::Canvas& canvas) {
    float w = bounds().width, h = bounds().height;
    float cx = w / 2.0f, cy = h / 2.0f;
    float s = std::min(w, h) * 0.3f;

    canvas.set_fill_color(canvas::Color::rgba(180, 180, 190));

    canvas::Canvas::Point2D pts[3];
    switch (direction_) {
        case ArrowDirection::up:
            pts[0] = {cx, cy - s}; pts[1] = {cx + s, cy + s}; pts[2] = {cx - s, cy + s};
            break;
        case ArrowDirection::down:
            pts[0] = {cx, cy + s}; pts[1] = {cx + s, cy - s}; pts[2] = {cx - s, cy - s};
            break;
        case ArrowDirection::left:
            pts[0] = {cx - s, cy}; pts[1] = {cx + s, cy - s}; pts[2] = {cx + s, cy + s};
            break;
        case ArrowDirection::right:
            pts[0] = {cx + s, cy}; pts[1] = {cx - s, cy - s}; pts[2] = {cx - s, cy + s};
            break;
    }
    canvas.fill_path(pts, 3);
}

void ArrowButton::on_mouse_down(Point) {
    if (on_click) on_click();
}

// ── ShapeButton ─────────────────────────────────────────────────────────

void ShapeButton::paint(canvas::Canvas& canvas) {
    float w = bounds().width, h = bounds().height;
    if (draw_fn_)
        draw_fn_(canvas, w, h, hovered_, pressed_);
}

void ShapeButton::on_mouse_down(Point) {
    pressed_ = true;
    if (on_click) on_click();
}
void ShapeButton::on_mouse_enter() { hovered_ = true; }
void ShapeButton::on_mouse_leave() { hovered_ = false; pressed_ = false; }

// ── ImageButton ─────────────────────────────────────────────────────────

void ImageButton::paint(canvas::Canvas& canvas) {
    float w = bounds().width, h = bounds().height;
    const auto& path = pressed_ ? (pressed_path_.empty() ? normal_path_ : pressed_path_)
                     : hovered_ ? (hover_path_.empty() ? normal_path_ : hover_path_)
                     : normal_path_;

    if (!path.empty())
        canvas.draw_image_from_file(path, 0, 0, w, h);
}

void ImageButton::on_mouse_down(Point) {
    pressed_ = true;
    if (on_click) on_click();
}
void ImageButton::on_mouse_enter() { hovered_ = true; }
void ImageButton::on_mouse_leave() { hovered_ = false; pressed_ = false; }

// ── ResizableCorner ─────────────────────────────────────────────────────

void ResizableCorner::paint(canvas::Canvas& canvas) {
    float w = bounds().width, h = bounds().height;
    canvas.set_stroke_color(canvas::Color::rgba(120, 120, 130));
    canvas.set_line_width(1.0f);

    // Draw resize grip lines (diagonal)
    for (int i = 0; i < 3; ++i) {
        float offset = static_cast<float>(i) * 4.0f;
        canvas.stroke_line(w - 2.0f - offset, h, w, h - 2.0f - offset);
    }
}

void ResizableCorner::on_mouse_down(Point pos) {
    drag_start_x_ = pos.x;
    drag_start_y_ = pos.y;
}

void ResizableCorner::on_mouse_drag(Point pos) {
    if (on_resize)
        on_resize(pos.x - drag_start_x_, pos.y - drag_start_y_);
}

}  // namespace pulp::view
