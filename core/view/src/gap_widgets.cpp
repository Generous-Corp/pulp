#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/frame_clock.hpp>
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
        case Tone::info:    return "accent.info";
        case Tone::success: return "accent.success";
        case Tone::warning: return "accent.warning";
        case Tone::danger:  return "accent.error";
        case Tone::neutral: default: return "text.secondary";
    }
}

constexpr float kPad = 12.0f;

// Drag-to-scrub tuning, shared by Stepper and NumberBox: how many vertical
// pixels of drag equal one `step_`, and the pixel threshold below which a press
// counts as a click (type / nudge) rather than the start of a scrub.
constexpr float kScrubPxPerStep = 6.0f;
constexpr float kScrubThreshold = 3.0f;
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
    const float r = 10.0f, bar = 4.0f;
    // Accent left edge as part of the rounded panel (matches Figma): fill the
    // whole panel with the accent, then overlay the surface inset by the bar
    // width — the strip that shows carries the panel's rounded corners, so the
    // bar never pokes past them and isn't a floating inset rect.
    canvas.set_fill_color(accent);
    canvas.fill_rounded_rect(0, 0, w, h, r);
    canvas.set_fill_color(resolve_color("bg.elevated", Color::rgba8(30, 37, 48)));
    canvas.fill_rounded_rect(bar, 0, w - bar, h, r);
    canvas.set_stroke_color(resolve_color("control.border", Color::rgba8(80, 80, 100)));
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(0, 0, w, h, r);
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
    const float r = 14.0f, bar = 4.0f;
    // Accent left edge as part of the rounded panel (see InlineBanner).
    canvas.set_fill_color(resolve_color("accent.primary", Color::rgba8(22, 218, 194)));
    canvas.fill_rounded_rect(0, 0, w, h, r);
    canvas.set_fill_color(resolve_color("bg.elevated", Color::rgba8(40, 48, 60)));
    canvas.fill_rounded_rect(bar, 0, w - bar, h, r);
    canvas.set_stroke_color(resolve_color("control.border", Color::rgba8(80, 80, 100)));
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(0, 0, w, h, r);
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

    // Folder glyph stacked cleanly ABOVE the text. Lay the icon and the
    // message out as one vertically-centered block (icon, gap, text) so the
    // two never crowd or overlap regardless of the box height — the previous
    // fixed fractions (icon at 0.22h, text at 0.66h) collided on short boxes.
    auto icon = resolve_color("text.secondary", Color::rgba8(150, 150, 160));
    const float bw = 26.0f, bh = 19.0f;
    const float gap = 12.0f;        // clear space between icon and text
    const float msg_font = 14.0f;
    const float stack_h = bh + gap + msg_font;
    const float by = std::max(8.0f, (h - stack_h) * 0.5f);  // icon top
    const float bx = (w - bw) * 0.5f;
    const float t = 4.0f;  // tab height
    canvas.set_stroke_color(icon);
    canvas.set_line_width(1.5f);
    canvas.set_line_join(canvas::LineJoin::round);
    canvas.begin_path();
    canvas.move_to(bx, by + bh);                 // bottom-left
    canvas.line_to(bx, by + 1.0f);               // up the left edge
    canvas.line_to(bx + 1.0f, by);               // tab corner
    canvas.line_to(bx + 9.0f, by);               // tab top
    canvas.line_to(bx + 11.0f, by + t);          // tab slope into body top
    canvas.line_to(bx + bw, by + t);             // body top edge
    canvas.line_to(bx + bw, by + bh);            // right edge
    canvas.line_to(bx, by + bh);                 // bottom edge (close)
    canvas.stroke_current_path();

    canvas.set_font("system", msg_font);
    const float mw = canvas.measure_text(message_);
    const float aw = action_.empty() ? 0.0f : canvas.measure_text(action_) + 8.0f;
    float x = (w - (mw + aw)) / 2.0f;
    // Baseline sits a gap below the folder body, with room for the glyph ascent.
    const float text_baseline = by + bh + gap + msg_font * 0.78f;
    canvas.set_fill_color(icon);
    canvas.fill_text(message_, x, text_baseline);
    if (!action_.empty()) {
        canvas.set_fill_color(resolve_color("accent.primary", Color::rgba8(22, 218, 194)));
        canvas.fill_text(action_, x + mw + 8.0f, text_baseline);
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
    // Hover / press affordance: a soft disc behind the −/+ glyph under the
    // pointer, so the buttons feel pressable instead of static.
    {
        Color accent = resolve_color("accent.primary", Color::rgba8(22, 218, 194));
        auto tint = [&](int zone, float cx) {
            const bool press = pressed_zone_ == zone, hov = hover_zone_ == zone;
            if (!press && !hov) return;
            canvas.set_fill_color(Color::rgba(accent.r, accent.g, accent.b,
                                              press ? 0.24f : 0.12f));  // token-lint:allow (zone state tint)
            canvas.fill_circle(cx, h * 0.5f, h * 0.5f - 4.0f);
        };
        tint(0, btn * 0.5f);          // minus
        tint(1, w - btn * 0.5f);      // plus
    }
    canvas.set_stroke_color(resolve_color("control.border", Color::rgba8(80, 80, 100)));
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(0, 0, w, h, 10.0f);
    canvas.stroke_line(btn, 4.0f, btn, h - 4.0f);
    canvas.stroke_line(w - btn, 4.0f, w - btn, h - 4.0f);
    // −/+ glyphs, vertically centered in their h×h zones (GlyphCenter anchors
    // on the glyph's optical center, not the baseline).
    canvas.set_font("system", 18.0f);
    canvas.set_fill_color(resolve_color("text.secondary", Color::rgba8(150, 150, 160)));
    canvas.set_text_align(canvas::TextAlign::center);
    canvas.fill_text_anchored("−", btn * 0.5f, h * 0.5f, canvas::Canvas::TextAnchor::GlyphCenter);
    canvas.fill_text_anchored("+", w - btn * 0.5f, h * 0.5f, canvas::Canvas::TextAnchor::GlyphCenter);
    canvas.set_text_align(canvas::TextAlign::left);
    // Center value — the edit buffer + a blinking caret while typing, else the value.
    std::string s;
    if (editing_) {
        canvas.set_stroke_color(resolve_color("focus.ring", Color::rgba8(22, 218, 194)));
        canvas.set_line_width(1.5f);
        canvas.stroke_rounded_rect(btn + 1.0f, 2.0f,
                                   std::max(0.0f, w - 2.0f * btn - 2.0f), h - 4.0f, 4.0f);
        auto* fc = frame_clock();
        const bool caret_on = !fc || std::fmod(fc->time(), 1.0) < 0.5;  // ~1 Hz blink
        s = edit_buffer_ + (caret_on ? "|" : " ");
    } else {
        char buf[32];
        const char* sign = value_ > 0 ? "+" : "";
        std::snprintf(buf, sizeof(buf), "%s%g%s%s", sign, value_,
                      suffix_.empty() ? "" : " ", suffix_.c_str());
        s = buf;
    }
    canvas.set_font("system", 14.0f);
    canvas.set_fill_color(resolve_color("text.primary", Color::rgba8(220, 220, 230)));
    canvas.set_text_align(canvas::TextAlign::center);
    canvas.fill_text_anchored(s, w * 0.5f, h * 0.5f, canvas::Canvas::TextAnchor::GlyphCenter);
    canvas.set_text_align(canvas::TextAlign::left);
}
int Stepper::zone_at_(float x) const {
    const float w = bounds().width, btn = bounds().height;
    if (x < btn) return 0;          // minus
    if (x > w - btn) return 1;      // plus
    return -1;                      // center value cell
}
void Stepper::on_mouse_down(Point pos) {
    press_y_ = pos.y;
    scrubbing_ = false;
    const int z = zone_at_(pos.x);
    if (z == 0) { commit_edit_(); set_value(value_ - step_); pressed_zone_ = 0; }
    else if (z == 1) { commit_edit_(); set_value(value_ + step_); pressed_zone_ = 1; }
    else {
        pressed_zone_ = -1;
        // Center cell: start typing a value (the host focuses us on click).
        editing_ = true;
        char buf[32]; std::snprintf(buf, sizeof(buf), "%g", value_);
        edit_buffer_ = buf;
    }
    drag_start_value_ = value_;
    request_repaint();
}
void Stepper::on_mouse_drag(Point pos) {
    // A vertical drag scrubs the value (up = increase). Below the threshold a
    // center press stays a click-to-type; once it becomes a drag we cancel any
    // just-started edit and scrub instead.
    if (!scrubbing_ && std::abs(press_y_ - pos.y) < kScrubThreshold) return;
    scrubbing_ = true;
    if (editing_) { editing_ = false; edit_buffer_.clear(); }
    const double raw = drag_start_value_
        + static_cast<double>(press_y_ - pos.y) / kScrubPxPerStep * step_;
    set_value(std::round(raw / step_) * step_);   // snap to the step grid
    request_repaint();
}
void Stepper::on_mouse_up(Point) {
    pressed_zone_ = -1;
    scrubbing_ = false;
    request_repaint();
}
void Stepper::on_hover_move(Point local_pos) {
    const int z = zone_at_(local_pos.x);
    if (z != hover_zone_) { hover_zone_ = z; request_repaint(); }
}
void Stepper::on_mouse_leave() {
    if (hover_zone_ != -1) { hover_zone_ = -1; request_repaint(); }
}
void Stepper::on_text_input(const TextInputEvent& event) {
    if (!editing_) return;
    for (char c : event.text) {
        if ((c >= '0' && c <= '9') || c == '.' || (c == '-' && edit_buffer_.empty()))
            edit_buffer_ += c;
    }
    request_repaint();
}
bool Stepper::on_key_event(const KeyEvent& event) {
    if (!editing_ || !event.is_down) return false;
    if (event.key == KeyCode::enter) { commit_edit_(); return true; }
    if (event.key == KeyCode::escape) {
        editing_ = false; edit_buffer_.clear(); request_repaint(); return true;
    }
    if (event.key == KeyCode::backspace) {
        if (!edit_buffer_.empty()) edit_buffer_.pop_back();
        request_repaint();
        return true;
    }
    return false;
}
void Stepper::on_focus_changed(bool gained) {
    View::on_focus_changed(gained);
    if (!gained && editing_) commit_edit_();
}
void Stepper::commit_edit_() {
    if (!editing_) return;
    editing_ = false;
    try {
        if (!edit_buffer_.empty()) {
            set_value(std::stod(edit_buffer_));
            // Remember the typed decimal precision so the scroll-wheel nudges
            // by that least-significant digit (type "7.1" → wheel steps 0.1).
            auto dot = edit_buffer_.find('.');
            if (dot != std::string::npos) {
                int decimals = static_cast<int>(edit_buffer_.size() - dot - 1);
                wheel_step_ = decimals > 0 ? std::pow(10.0, -decimals) : 0.0;
            } else {
                wheel_step_ = 0.0;
            }
        }
    } catch (...) {}
    edit_buffer_.clear();
    request_repaint();
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
    canvas.fill_rect(cx - 1.0f, cy - th / 2 - 2.0f, 2.0f, th + 4.0f);   // center detent
    canvas.set_fill_color(resolve_color("slider.thumb", Color::rgba8(220, 220, 220)));
    canvas.fill_circle(tx, cy, (h / 2.0f) * hover_scale_.value());
}
void PanControl::on_mouse_down(Point pos) { set_from_x(pos.x); }
void PanControl::on_mouse_drag(Point pos) { set_from_x(pos.x); }
void PanControl::on_mouse_enter() {
    hover_scale_.animate_to(1.3f, resolve_dimension("motion.duration.fast", 0.08f), easing::ease_out_quad);
}
void PanControl::on_mouse_leave() {
    hover_scale_.animate_to(1.0f, resolve_dimension("motion.duration.fast", 0.08f), easing::ease_out_quad);
}

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

void ChannelStrip::handle_pointer_(Point pos) {
    // Geometry mirrors paint(): the fader column spans [topPad, topPad+faderH];
    // the pan row sits in the bottom padding band. Pick the zone by y.
    const float w = bounds().width, h = bounds().height;
    const float topPad = 12.0f, botPad = 36.0f, faderH = h - topPad - botPad;
    if (faderH <= 0.0f) return;
    if (pos.y >= h - botPad) {
        // Pan row: map x across the centered pan track to [-1, +1].
        const float cx = w / 2.0f, panW = w * 0.6f;
        if (panW <= 0.0f) return;
        float v = std::clamp((pos.x - cx) / (panW / 2.0f), -1.0f, 1.0f);
        if (v != pan_) { pan_ = v; request_repaint(); if (on_pan_change) on_pan_change(pan_); }
    } else {
        // Fader column: top = 1.0, bottom = 0.0.
        float v = std::clamp(1.0f - (pos.y - topPad) / faderH, 0.0f, 1.0f);
        if (v != level_) { level_ = v; request_repaint(); if (on_level_change) on_level_change(level_); }
    }
}

void ChannelStrip::on_mouse_down(Point pos) { handle_pointer_(pos); }
void ChannelStrip::on_mouse_drag(Point pos) { handle_pointer_(pos); }

void ChannelStrip::on_wheel(float delta_y) {
    float v = std::clamp(level_ + (delta_y > 0 ? -0.02f : 0.02f), 0.0f, 1.0f);
    if (v != level_) { level_ = v; request_repaint(); if (on_level_change) on_level_change(level_); }
}

// ── Spinner ─────────────────────────────────────────────────────────────
void Spinner::paint(canvas::Canvas& canvas) {
    const float w = bounds().width, h = bounds().height;
    const float cx = w * 0.5f, cy = h * 0.5f;
    const float thick = std::max(2.0f, std::min(w, h) * 0.12f);
    const float r = std::min(w, h) * 0.5f - thick;
    if (r <= 0.0f) return;
    constexpr float kTwoPi = 6.2831853f;
    canvas.set_line_cap(canvas::LineCap::round);
    canvas.set_line_width(thick);
    // Faint full-circle track.
    canvas.set_stroke_color(resolve_color("control.track", Color::rgba8(60, 60, 60)));
    canvas.stroke_arc(cx, cy, r, 0.0f, kTwoPi);
    // Accent arc — determinate fraction, or a sweeping 270° arc when < 0.
    canvas.set_stroke_color(resolve_color("accent.primary", Color::rgba8(22, 218, 194)));
    if (progress_ >= 0.0f) {
        const float sweep = std::clamp(progress_, 0.0f, 1.0f) * kTwoPi;
        canvas.stroke_arc(cx, cy, r, -1.5708f, -1.5708f + sweep);  // from 12 o'clock
    } else {
        const float start = std::fmod(phase_ * 3.0f, kTwoPi);     // ~0.5 rev/s
        canvas.stroke_arc(cx, cy, r, start, start + 4.712f);       // 270°
    }
}

// ── NumberBox ───────────────────────────────────────────────────────────
void NumberBox::set_value(double v) {
    const double nv = std::clamp(v, min_, max_);
    if (nv != value_) { value_ = nv; request_repaint(); if (on_change) on_change(value_); }
}
void NumberBox::paint(canvas::Canvas& canvas) {
    const float w = bounds().width, h = bounds().height;
    const float r = h * 0.5f;
    canvas.set_fill_color(resolve_color("bg.surface", Color::rgba8(30, 30, 46)));
    canvas.fill_rounded_rect(0, 0, w, h, r);

    // Hover / press affordance: a soft disc behind the chevron under the
    // pointer, so the ‹ › buttons feel pressable instead of static.
    {
        Color accent = resolve_color("accent.primary", Color::rgba8(22, 218, 194));
        auto tint = [&](int zone, float cx) {
            const bool press = pressed_zone_ == zone, hov = hover_zone_ == zone;
            if (!press && !hov) return;
            canvas.set_fill_color(Color::rgba(accent.r, accent.g, accent.b,
                                              press ? 0.24f : 0.12f));  // token-lint:allow (zone state tint)
            canvas.fill_circle(cx, h * 0.5f, r - 4.0f);
        };
        tint(0, r);          // ‹
        tint(1, w - r);      // ›
    }

    canvas.set_stroke_color(resolve_color("control.border", Color::rgba8(80, 80, 100)));
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(0, 0, w, h, r);
    // Chevrons in the end zones.
    canvas.set_font("Inter", 13.0f);
    canvas.set_fill_color(resolve_color("text.secondary", Color::rgba8(150, 150, 170)));
    canvas.set_text_align(canvas::TextAlign::center);
    canvas.fill_text_anchored("\xe2\x80\xb9", r, h * 0.5f, canvas::Canvas::TextAnchor::GlyphCenter);        // ‹
    canvas.fill_text_anchored("\xe2\x80\xba", w - r, h * 0.5f, canvas::Canvas::TextAnchor::GlyphCenter);    // ›
    // Center: the edit buffer + blinking caret while typing, else the value.
    std::string s;
    if (editing_) {
        canvas.set_stroke_color(resolve_color("focus.ring", Color::rgba8(22, 218, 194)));
        canvas.set_line_width(1.5f);
        canvas.stroke_rounded_rect(h, 2.0f, std::max(0.0f, w - 2.0f * h),
                                   h - 4.0f, (h - 4.0f) * 0.5f);
        auto* fc = frame_clock();
        const bool caret_on = !fc || std::fmod(fc->time(), 1.0) < 0.5;  // ~1 Hz blink
        s = edit_buffer_ + (caret_on ? "|" : " ");
    } else {
        char buf[32];
        const char* sign = value_ > 0 ? "+" : "";
        std::snprintf(buf, sizeof(buf), "%s%g%s%s", sign, value_,
                      suffix_.empty() ? "" : " ", suffix_.c_str());
        s = buf;
    }
    canvas.set_font("Inter", 12.0f);
    canvas.set_fill_color(resolve_color("text.primary", Color::rgba8(220, 220, 230)));
    canvas.set_text_align(canvas::TextAlign::center);
    canvas.fill_text_anchored(s, w * 0.5f, h * 0.5f, canvas::Canvas::TextAnchor::GlyphCenter);
    canvas.set_text_align(canvas::TextAlign::left);
}
int NumberBox::zone_at_(float x) const {
    const float w = bounds().width, edge = bounds().height;  // square-ish end zones
    if (x < edge) return 0;          // ‹ decrement
    if (x > w - edge) return 1;      // › increment
    return -1;                       // center value cell
}
void NumberBox::on_mouse_down(Point pos) {
    press_y_ = pos.y;
    scrubbing_ = false;
    const int z = zone_at_(pos.x);
    if (z == 0) { commit_edit_(); set_value(value_ - step_); pressed_zone_ = 0; }       // ‹
    else if (z == 1) { commit_edit_(); set_value(value_ + step_); pressed_zone_ = 1; }  // ›
    else {
        pressed_zone_ = -1;
        // Center cell: start typing a value (the host focuses us on click).
        editing_ = true;
        char buf[32]; std::snprintf(buf, sizeof(buf), "%g", value_);
        edit_buffer_ = buf;
    }
    drag_start_value_ = value_;
    request_repaint();
}
void NumberBox::on_mouse_drag(Point pos) {
    // Vertical drag scrubs (up = increase); a sub-threshold center press stays
    // a click-to-type. Once it becomes a drag, cancel any edit and scrub.
    if (!scrubbing_ && std::abs(press_y_ - pos.y) < kScrubThreshold) return;
    scrubbing_ = true;
    if (editing_) { editing_ = false; edit_buffer_.clear(); }
    const double raw = drag_start_value_
        + static_cast<double>(press_y_ - pos.y) / kScrubPxPerStep * step_;
    set_value(std::round(raw / step_) * step_);   // snap to the step grid
    request_repaint();
}
void NumberBox::on_mouse_up(Point) {
    pressed_zone_ = -1;
    scrubbing_ = false;
    request_repaint();
}
void NumberBox::on_hover_move(Point local_pos) {
    const int z = zone_at_(local_pos.x);
    if (z != hover_zone_) { hover_zone_ = z; request_repaint(); }
}
void NumberBox::on_mouse_leave() {
    if (hover_zone_ != -1) { hover_zone_ = -1; request_repaint(); }
}
void NumberBox::on_text_input(const TextInputEvent& event) {
    if (!editing_) return;
    for (char c : event.text) {
        if ((c >= '0' && c <= '9') || c == '.' || (c == '-' && edit_buffer_.empty()))
            edit_buffer_ += c;
    }
    request_repaint();
}
bool NumberBox::on_key_event(const KeyEvent& event) {
    if (!editing_ || !event.is_down) return false;
    if (event.key == KeyCode::enter) { commit_edit_(); return true; }
    if (event.key == KeyCode::escape) {
        editing_ = false; edit_buffer_.clear(); request_repaint(); return true;
    }
    if (event.key == KeyCode::backspace) {
        if (!edit_buffer_.empty()) edit_buffer_.pop_back();
        request_repaint();
        return true;
    }
    return false;
}
void NumberBox::on_focus_changed(bool gained) {
    View::on_focus_changed(gained);
    if (!gained && editing_) commit_edit_();
}
void NumberBox::commit_edit_() {
    if (!editing_) return;
    editing_ = false;
    try {
        if (!edit_buffer_.empty()) set_value(std::stod(edit_buffer_));
    } catch (...) {}
    edit_buffer_.clear();
    request_repaint();
}

}  // namespace pulp::view
