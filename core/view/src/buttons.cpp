#include <pulp/view/buttons.hpp>
#include <pulp/view/widget_painter.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/text_overflow.hpp>
#include <pulp/view/theme_contrast.hpp>
#include <algorithm>

namespace pulp::view {

// ── TextButton ──────────────────────────────────────────────────────────

void TextButton::paint(canvas::Canvas& canvas) {
    float w = bounds().width, h = bounds().height;
    float r = 6.0f;

    // A paint delegate (widget_painter.hpp) may replace the face, the label, or
    // both — the two hooks are independent, so a skin can restyle the background
    // and keep the stock text, or the reverse. Both decline by default.
    WidgetPainter* delegate = effective_painter();
    ButtonPaintState ps;
    if (delegate != nullptr) {
        ps.bounds = local_bounds();
        ps.enabled = enabled_;
        ps.hovered = hovered_;
        ps.pressed = pressed_;
        ps.focused = has_focus();
        ps.text = label_;
        ps.background = (style_ == Style::primary)
            ? resolve_color("accent.primary", canvas::Color::rgba8(20, 184, 166))
            : resolve_color("bg.elevated", canvas::Color::rgba8(60, 60, 70));
    }
    const bool skin_bg = delegate != nullptr && delegate->paint_button_background(canvas, ps, *this);
    const bool skin_text = delegate != nullptr && delegate->paint_button_text(canvas, ps, *this);
    if (skin_bg) {
        if (!skin_text) paint_label(canvas);
        return;
    }

    // Background. NOTE: Color::rgba() takes 0–1 floats; these are 0–255 channel values and
    // must use rgba8() — rgba() clamps every channel to 1.0 and paints the button solid
    // white (the standalone Settings/Done buttons regressed to white because of this).
    // Theme-driven button face. The literal fallbacks reproduce the legacy
    // neutral appearance when no theme tokens are present; hover/pressed/
    // disabled are derived from the resolved base so state feedback tracks the
    // active theme instead of being frozen to hardcoded greys (#3.3 reskin gap).
    // Base face per variant. `primary` is accent-filled; `secondary` is the
    // neutral elevated face + border; `ghost` is transparent.
    bool filled = style_ != Style::ghost;
    auto base = (style_ == Style::primary)
        ? resolve_color("accent.primary", canvas::Color::rgba8(20, 184, 166))
        : resolve_color("bg.elevated", canvas::Color::rgba8(60, 60, 70));
    auto bg = base;
    if (hovered_) bg = adjust_lightness(base, 0.06f);
    if (pressed_) bg = adjust_lightness(base, 0.12f);
    if (!enabled_) bg = adjust_lightness(base, -0.04f);
    if (filled) {
        canvas.set_fill_color(bg);
        canvas.fill_rounded_rect(0, 0, w, h, r);
    }

    // Border — secondary only (primary is borderless accent fill; ghost is
    // bare). Rounded to match the filled background.
    if (style_ == Style::secondary) {
        canvas.set_stroke_color(resolve_color("control.border", canvas::Color::rgba8(100, 100, 110)));
        canvas.set_line_width(1.0f);
        canvas.stroke_rounded_rect(0, 0, w, h, r);
    }

    if (!skin_text) paint_label(canvas);
}

void TextButton::paint_label(canvas::Canvas& canvas) {
    const float w = bounds().width, h = bounds().height;
    // Label colour per variant: primary uses on-accent (ink) text; ghost uses
    // accent text; secondary uses the standard primary text.
    auto text_color =
        !enabled_ ? resolve_color("text.disabled", canvas::Color::rgba8(120, 120, 130))
        : style_ == Style::primary ? resolve_color("accent.text", canvas::Color::rgba8(18, 22, 28))
        : style_ == Style::ghost ? resolve_color("accent.primary", canvas::Color::rgba8(20, 184, 166))
        : resolve_color("text.primary", canvas::Color::rgba8(220, 220, 230));
    canvas.set_fill_color(text_color);
    canvas.set_font("system", 14.0f);
    constexpr float kButtonHPad = 8.0f;
    std::string draw_label = text_overflow_ellipsis()
        ? truncate_to_width(canvas, label_, std::max(0.0f, w - kButtonHPad * 2.0f))
        : label_;
    float text_w = canvas.measure_text(draw_label);
    canvas.fill_text(draw_label, (w - text_w) / 2.0f, h * 0.65f);
}

void TextButton::on_mouse_down(Point) {
    if (!enabled_) return;
    pressed_ = true;
    request_repaint();
    if (on_click) on_click();
}

// The pressed face was unreachable: `pressed_` was read by paint() but nothing
// ever wrote it, so a button never rendered its down state. Latch it on press
// and clear it on release or on leaving the button with the pointer still down.
void TextButton::on_mouse_up(Point) {
    if (!pressed_) return;
    pressed_ = false;
    request_repaint();
}

void TextButton::on_mouse_enter() { hovered_ = true; }
void TextButton::on_mouse_leave() {
    hovered_ = false;
    pressed_ = false;
}

// ── HyperlinkButton ─────────────────────────────────────────────────────

void HyperlinkButton::paint(canvas::Canvas& canvas) {
    float w = bounds().width, h = bounds().height;
    // Bug fix: the previous literals passed 0–255 ints to Color::rgba(), which
    // takes 0–1 floats and clamps — so the link rendered solid white. Resolve
    // from the theme's link token with the intended blue as an rgba8 fallback.
    auto link = resolve_color("text.link", canvas::Color::rgba8(80, 130, 230));
    auto color = hovered_ ? adjust_lightness(link, 0.12f) : link;
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

    // Bug fix (same rgba() 0–1-float clamp as HyperlinkButton): use rgba8 and
    // resolve from the theme so the glyph isn't a clamped solid white.
    canvas.set_fill_color(resolve_color("text.secondary", canvas::Color::rgba8(180, 180, 190)));

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
    // Bug fix (rgba() 0–1-float clamp): use rgba8 + theme token so the grip
    // lines aren't a clamped solid white.
    canvas.set_stroke_color(resolve_color("control.border", canvas::Color::rgba8(120, 120, 130)));
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
