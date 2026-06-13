#include <pulp/view/gap_widgets.hpp>
#include <pulp/canvas/canvas.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::view {

namespace {
using canvas::Color;

// Tone → semantic colour token (derive_theme family), with a sane literal
// fallback for the no-theme case. accent.text resolves the "on bright fill"
// colour (ink-signal provides it; others fall back to a dark ink).
const char* tone_token(Tone t) {
    switch (t) {
        case Tone::info:    return "accent.secondary";
        case Tone::success: return "accent.success";
        case Tone::warning: return "accent.warning";
        case Tone::danger:  return "accent.error";
        case Tone::neutral: default: return "text.secondary";
    }
}

constexpr float kPad = 12.0f;
}  // namespace

// ── Badge ───────────────────────────────────────────────────────────────
void Badge::paint(canvas::Canvas& canvas) {
    const float w = bounds().width, h = bounds().height, r = h / 2.0f;
    const bool neutral = tone_ == Tone::neutral;
    Color fill = neutral ? resolve_color("bg.elevated", Color::rgba8(60, 60, 70))
                         : resolve_color(tone_token(tone_), Color::rgba8(100, 150, 255));
    Color text = neutral ? resolve_color("text.primary", Color::rgba8(220, 220, 230))
                         : resolve_color("accent.text", Color::rgba8(5, 35, 32));
    canvas.set_fill_color(fill);
    canvas.fill_rounded_rect(0, 0, w, h, r);
    canvas.set_fill_color(text);
    canvas.set_font("system", 12.0f);
    const float tw = canvas.measure_text(text_);
    canvas.fill_text(text_, (w - tw) / 2.0f, h * 0.68f);
}

// ── InlineBanner ──────────────────────────────────────────────────────────
void InlineBanner::paint(canvas::Canvas& canvas) {
    const float w = bounds().width, h = bounds().height;
    Color accent = resolve_color(tone_token(tone_), Color::rgba8(94, 120, 255));
    canvas.set_fill_color(resolve_color("bg.elevated", Color::rgba8(30, 37, 48)));
    canvas.fill_rounded_rect(0, 0, w, h, 10.0f);
    canvas.set_stroke_color(resolve_color("control.border", Color::rgba8(80, 80, 100)));
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(0, 0, w, h, 10.0f);
    canvas.set_fill_color(accent);                       // left accent bar
    canvas.fill_rect(0, 0, 4.0f, h);
    canvas.set_font("system", 13.0f);
    canvas.set_fill_color(resolve_color("text.primary", Color::rgba8(230, 230, 240)));
    const float lw = canvas.measure_text(label_);
    canvas.fill_text(label_, 16.0f, h * 0.62f);
    if (!message_.empty()) {
        canvas.set_fill_color(accent);
        canvas.fill_text(message_, 16.0f + lw + 8.0f, h * 0.62f);
    }
}

// ── Toast ─────────────────────────────────────────────────────────────────
void Toast::paint(canvas::Canvas& canvas) {
    const float w = bounds().width, h = bounds().height;
    canvas.set_fill_color(resolve_color("bg.elevated", Color::rgba8(40, 48, 60)));
    canvas.fill_rounded_rect(0, 0, w, h, 14.0f);
    canvas.set_stroke_color(resolve_color("control.border", Color::rgba8(80, 80, 100)));
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(0, 0, w, h, 14.0f);
    canvas.set_fill_color(resolve_color("accent.primary", Color::rgba8(22, 218, 194)));
    canvas.fill_rect(0, 0, 4.0f, h);
    canvas.set_font("system", 14.0f);
    canvas.set_fill_color(resolve_color("text.primary", Color::rgba8(240, 240, 245)));
    canvas.fill_text(title_, 18.0f, subtitle_.empty() ? h * 0.6f : h * 0.42f);
    if (!subtitle_.empty()) {
        canvas.set_font("system", 12.0f);
        canvas.set_fill_color(resolve_color("text.secondary", Color::rgba8(150, 150, 160)));
        canvas.fill_text(subtitle_, 18.0f, h * 0.72f);
    }
    if (!action_.empty()) {
        canvas.set_font("system", 13.0f);
        canvas.set_fill_color(resolve_color("accent.primary", Color::rgba8(22, 218, 194)));
        const float aw = canvas.measure_text(action_);
        canvas.fill_text(action_, w - aw - 18.0f, h * 0.6f);
    }
}
void Toast::on_mouse_down(Point pos) {
    if (action_.empty() || !on_action) return;
    if (pos.x > bounds().width - 80.0f) on_action();
}

// ── EmptyState ──────────────────────────────────────────────────────────
void EmptyState::paint(canvas::Canvas& canvas) {
    const float w = bounds().width, h = bounds().height;
    // Dashed placeholder border (matches the Figma empty-state).
    canvas.set_stroke_color(resolve_color("control.border", Color::rgba8(80, 80, 100)));
    canvas.set_line_width(1.5f);
    const float dashes[] = {5.0f, 4.0f};
    canvas.set_line_dash(dashes, 2, 0.0f);
    canvas.stroke_rounded_rect(1, 1, w - 2, h - 2, 14.0f);
    canvas.set_line_dash(nullptr, 0, 0.0f);  // revert to solid

    // Folder glyph above the text — a simple outlined folder.
    auto icon = resolve_color("text.secondary", Color::rgba8(150, 150, 160));
    const float fw = 22.0f, fh = 16.0f;
    const float fx = (w - fw) * 0.5f, fy = h * 0.26f;
    canvas.set_stroke_color(icon);
    canvas.set_line_width(1.5f);
    canvas.stroke_rounded_rect(fx, fy + 3.0f, fw, fh - 3.0f, 2.0f);   // body
    canvas.stroke_line(fx + 2.0f, fy + 3.0f, fx + 2.0f + 7.0f, fy + 3.0f);  // tab top
    canvas.stroke_line(fx + 2.0f, fy + 3.0f, fx + 3.0f, fy);
    canvas.stroke_line(fx + 9.0f, fy + 3.0f, fx + 8.0f, fy);

    canvas.set_font("system", 14.0f);
    const float mw = canvas.measure_text(message_);
    const float aw = action_.empty() ? 0.0f : canvas.measure_text(action_) + 8.0f;
    float x = (w - (mw + aw)) / 2.0f;
    canvas.set_fill_color(icon);
    canvas.fill_text(message_, x, h * 0.66f);
    if (!action_.empty()) {
        canvas.set_fill_color(resolve_color("accent.primary", Color::rgba8(22, 218, 194)));
        canvas.fill_text(action_, x + mw + 8.0f, h * 0.66f);
    }
}
void EmptyState::on_mouse_down(Point) { if (!action_.empty() && on_action) on_action(); }

// ── Stepper ───────────────────────────────────────────────────────────────
void Stepper::set_value(double v) {
    value_ = std::clamp(v, min_, max_);
    if (on_change) on_change(value_);
}
void Stepper::paint(canvas::Canvas& canvas) {
    const float w = bounds().width, h = bounds().height, btn = h;
    canvas.set_fill_color(resolve_color("bg.elevated", Color::rgba8(30, 37, 48)));
    canvas.fill_rounded_rect(0, 0, w, h, 10.0f);
    // Darker center value cell + segment dividers (matches the Figma stepper's
    // segmented [-] value [+] look).
    canvas.set_fill_color(resolve_color("bg.surface", Color::rgba8(20, 25, 33)));
    canvas.fill_rect(btn, 1.0f, std::max(0.0f, w - 2.0f * btn), h - 2.0f);
    canvas.set_stroke_color(resolve_color("control.border", Color::rgba8(80, 80, 100)));
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(0, 0, w, h, 10.0f);
    canvas.stroke_line(btn, 4.0f, btn, h - 4.0f);
    canvas.stroke_line(w - btn, 4.0f, w - btn, h - 4.0f);
    canvas.set_font("system", 16.0f);
    canvas.set_fill_color(resolve_color("text.secondary", Color::rgba8(150, 150, 160)));
    canvas.fill_text("-", btn * 0.4f, h * 0.62f);
    canvas.fill_text("+", w - btn * 0.55f, h * 0.62f);
    char buf[32];
    const char* sign = value_ > 0 ? "+" : "";
    std::snprintf(buf, sizeof(buf), "%s%g%s%s", sign, value_,
                  suffix_.empty() ? "" : " ", suffix_.c_str());
    canvas.set_font("system", 14.0f);
    canvas.set_fill_color(resolve_color("text.primary", Color::rgba8(220, 220, 230)));
    const std::string s(buf);
    const float sw = canvas.measure_text(s);
    canvas.fill_text(s, (w - sw) / 2.0f, h * 0.62f);
}
void Stepper::on_mouse_down(Point pos) {
    const float w = bounds().width, btn = bounds().height;
    if (pos.x < btn) set_value(value_ - step_);
    else if (pos.x > w - btn) set_value(value_ + step_);
}

// ── PanControl (1-D) ──────────────────────────────────────────────────────
void PanControl::set_value(float v) {
    value_ = std::clamp(v, -1.0f, 1.0f);
    if (on_change) on_change(value_);
}
void PanControl::set_from_x(float x) {
    const float w = bounds().width;
    set_value(w > 0 ? (x / w) * 2.0f - 1.0f : 0.0f);
}
void PanControl::paint(canvas::Canvas& canvas) {
    const float w = bounds().width, h = bounds().height, th = 6.0f, cy = h / 2.0f, cx = w / 2.0f;
    canvas.set_fill_color(resolve_color("slider.track", Color::rgba8(60, 60, 60)));
    canvas.fill_rounded_rect(0, cy - th / 2, w, th, th / 2);
    const float tx = (value_ + 1.0f) * 0.5f * w;
    const float x0 = std::min(cx, tx), fw = std::fabs(tx - cx);
    if (fw > 1.0f) {
        canvas.set_fill_color(resolve_color("slider.fill", Color::rgba8(22, 218, 194)));
        canvas.fill_rect(x0, cy - th / 2, fw, th);
    }
    canvas.set_fill_color(resolve_color("control.border", Color::rgba8(120, 130, 140)));
    canvas.fill_rect(cx - 1.0f, cy - th / 2 - 2.0f, 2.0f, th + 4.0f);   // centre detent
    canvas.set_fill_color(resolve_color("slider.thumb", Color::rgba8(220, 220, 220)));
    canvas.fill_circle(tx, cy, h / 2.0f);
}
void PanControl::on_mouse_down(Point pos) { set_from_x(pos.x); }
void PanControl::on_mouse_drag(Point pos) { set_from_x(pos.x); }

// ── Popover ───────────────────────────────────────────────────────────────
void Popover::paint(canvas::Canvas& canvas) {
    const float w = bounds().width, h = bounds().height;
    canvas.set_fill_color(resolve_color("modal.bg", Color::rgba8(47, 55, 67)));
    canvas.fill_rounded_rect(0, 8, w, h - 8, 14.0f);
    canvas.set_stroke_color(resolve_color("modal.border", Color::rgba8(80, 80, 100)));
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(0, 8, w, h - 8, 14.0f);
    // upward tail
    canvas::Canvas::Point2D tail[3] = {{28, 0}, {44, 9}, {28 - 8, 9}};
    canvas.set_fill_color(resolve_color("modal.bg", Color::rgba8(47, 55, 67)));
    canvas.fill_path(tail, 3);
    if (!title_.empty()) {
        canvas.set_font("system", 15.0f);
        canvas.set_fill_color(resolve_color("text.primary", Color::rgba8(240, 240, 245)));
        canvas.fill_text(title_, 16.0f, 36.0f);
    }
}

// ── InCanvasDialog ──────────────────────────────────────────────────────
void InCanvasDialog::paint(canvas::Canvas& canvas) {
    const float w = bounds().width, h = bounds().height;
    canvas.set_fill_color(resolve_color("overlay.bg", Color::rgba8(0, 0, 0, 180)));
    canvas.fill_rect(0, 0, w, h);                       // scrim
    const float pw = std::min(380.0f, w - 40.0f), ph = 150.0f;
    const float px = (w - pw) / 2.0f, py = (h - ph) / 2.0f;
    canvas.set_fill_color(resolve_color("bg.elevated", Color::rgba8(40, 48, 60)));
    canvas.fill_rounded_rect(px, py, pw, ph, 20.0f);
    canvas.set_stroke_color(resolve_color("modal.border", Color::rgba8(80, 80, 100)));
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(px, py, pw, ph, 20.0f);
    canvas.set_font("system", 18.0f);
    canvas.set_fill_color(resolve_color("text.primary", Color::rgba8(240, 240, 245)));
    canvas.fill_text(title_, px + 24.0f, py + 36.0f);
    if (!message_.empty()) {
        canvas.set_font("system", 14.0f);
        canvas.set_fill_color(resolve_color("text.secondary", Color::rgba8(150, 150, 160)));
        canvas.fill_text(message_, px + 24.0f, py + 64.0f);
    }
    // buttons (cancel ghost, confirm filled)
    canvas.set_font("system", 14.0f);
    const float by = py + ph - 24.0f;
    Color confirmFill = destructive_ ? resolve_color("accent.error", Color::rgba8(255, 92, 77))
                                     : resolve_color("accent.primary", Color::rgba8(22, 218, 194));
    const float cw = canvas.measure_text(confirm_) + 28.0f;
    canvas.set_fill_color(confirmFill);
    canvas.fill_rounded_rect(px + pw - cw - 24.0f, by - 18.0f, cw, 30.0f, 8.0f);
    canvas.set_fill_color(resolve_color("accent.text", Color::rgba8(5, 35, 32)));
    canvas.fill_text(confirm_, px + pw - cw - 24.0f + 14.0f, by);
    canvas.set_fill_color(resolve_color("text.primary", Color::rgba8(220, 220, 230)));
    const float cancw = canvas.measure_text(cancel_);
    canvas.fill_text(cancel_, px + pw - cw - 24.0f - cancw - 20.0f, by);
}
void InCanvasDialog::on_mouse_down(Point pos) {
    // Right portion of the panel button row → confirm; just left of it → cancel.
    if (pos.x > bounds().width / 2.0f) { if (on_confirm) on_confirm(); }
    else { if (on_cancel) on_cancel(); }
}

// ── ChannelStrip ──────────────────────────────────────────────────────────
void ChannelStrip::paint(canvas::Canvas& canvas) {
    const float w = bounds().width, h = bounds().height;
    canvas.set_fill_color(resolve_color("bg.elevated", Color::rgba8(30, 37, 48)));
    canvas.fill_rounded_rect(0, 0, w, h, 12.0f);
    canvas.set_stroke_color(resolve_color("control.border", Color::rgba8(80, 80, 100)));
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(0, 0, w, h, 12.0f);
    const float topPad = 12.0f, botPad = 36.0f, faderH = h - topPad - botPad;
    // meter (left) + fader (right of meter)
    const float meterX = w * 0.32f, faderX = w * 0.6f, tw = 6.0f;
    canvas.set_fill_color(resolve_color("control.track", Color::rgba8(60, 60, 60)));
    canvas.fill_rounded_rect(meterX, topPad, tw, faderH, tw / 2);
    canvas.fill_rounded_rect(faderX, topPad, tw, faderH, tw / 2);
    const float lvl = std::clamp(level_, 0.0f, 1.0f);
    // meter fill (bottom-up)
    canvas.set_fill_color(resolve_color("meter.green", Color::rgba8(63, 207, 119)));
    canvas.fill_rect(meterX, topPad + faderH * (1 - lvl), tw, faderH * lvl);
    // fader fill + handle
    canvas.set_fill_color(resolve_color("slider.fill", Color::rgba8(22, 218, 194)));
    canvas.fill_rect(faderX, topPad + faderH * (1 - lvl), tw, faderH * lvl);
    const float hy = topPad + faderH * (1 - lvl);
    canvas.set_fill_color(resolve_color("slider.thumb", Color::rgba8(220, 220, 220)));
    canvas.fill_rounded_rect(faderX - 7.0f, hy - 6.0f, 20.0f, 12.0f, 6.0f);
    // pan dot row
    const float py = h - botPad + 8.0f, cx = w / 2.0f, panW = w * 0.6f;
    canvas.set_fill_color(resolve_color("control.track", Color::rgba8(60, 60, 60)));
    canvas.fill_rounded_rect(cx - panW / 2, py, panW, 4.0f, 2.0f);
    canvas.set_fill_color(resolve_color("accent.primary", Color::rgba8(22, 218, 194)));
    canvas.fill_circle(cx + (pan_ * panW / 2), py + 2.0f, 5.0f);
    // label
    canvas.set_font("system", 12.0f);
    canvas.set_fill_color(resolve_color("text.secondary", Color::rgba8(150, 150, 160)));
    const float lw = canvas.measure_text(label_);
    canvas.fill_text(label_, (w - lw) / 2.0f, h - 8.0f);
}

}  // namespace pulp::view
