#include <pulp/view/caret.hpp>

#include <pulp/view/motion_preferences.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::view {
namespace {

// A period this short reads as a flicker rather than a blink, and a zero
// period would divide by zero in `visible()`.
constexpr float kMinBlinkPeriod = 0.05f;

CaretStyle g_default_style = CaretStyle::ibeam;
CaretBlinkConfig g_default_blink{};

CaretBlinkConfig sanitize(const CaretBlinkConfig& in) noexcept {
    CaretBlinkConfig out = in;
    out.period_seconds = std::max(kMinBlinkPeriod, out.period_seconds);
    out.duty = std::clamp(out.duty, 0.0f, 1.0f);
    out.solid_hold_seconds = std::max(0.0f, out.solid_hold_seconds);
    return out;
}

}  // namespace

// ── Style names ──────────────────────────────────────────────────────────

const char* caret_style_to_string(CaretStyle style) noexcept {
    switch (style) {
        case CaretStyle::underline: return "underline";
        case CaretStyle::block:     return "block";
        case CaretStyle::ibeam:
        default:                    return "ibeam";
    }
}

CaretStyle caret_style_from_string(std::string_view name) noexcept {
    if (name == "underline") return CaretStyle::underline;
    if (name == "block")     return CaretStyle::block;
    return CaretStyle::ibeam;
}

// ── Process-wide defaults ────────────────────────────────────────────────

CaretStyle default_caret_style() noexcept { return g_default_style; }

void set_default_caret_style(CaretStyle style) noexcept { g_default_style = style; }

const CaretBlinkConfig& default_caret_blink() noexcept { return g_default_blink; }

void set_default_caret_blink(const CaretBlinkConfig& config) noexcept {
    g_default_blink = sanitize(config);
}

// ── Blink state machine ──────────────────────────────────────────────────

CaretBlink::CaretBlink() noexcept : config_(g_default_blink) {}

void CaretBlink::set_config(const CaretBlinkConfig& config) noexcept {
    config_ = sanitize(config);
    // A period change must not leave the phase past the new period, which
    // would make `visible()` read from a wrapped-around cycle for one frame.
    phase_ = std::fmod(phase_, config_.period_seconds);
}

void CaretBlink::keep_solid() noexcept {
    phase_ = 0.0f;
    solid_remaining_ = config_.solid_hold_seconds;
}

void CaretBlink::reset() noexcept {
    phase_ = 0.0f;
    solid_remaining_ = 0.0f;
}

void CaretBlink::advance(float dt) noexcept {
    if (!(dt > 0.0f)) return;  // also rejects NaN

    // Spend the solid hold first. A frame longer than the whole hold (a
    // stalled render, a debugger pause) must consume it and carry the
    // remainder into the blink phase rather than dropping either.
    if (solid_remaining_ > 0.0f) {
        if (dt < solid_remaining_) {
            solid_remaining_ -= dt;
            return;
        }
        dt -= solid_remaining_;
        solid_remaining_ = 0.0f;
        phase_ = 0.0f;
    }
    phase_ = std::fmod(phase_ + dt, config_.period_seconds);
}

bool CaretBlink::visible() const noexcept {
    if (!config_.enabled) return true;
    if (solid_remaining_ > 0.0f) return true;
    // A blinking caret is animated content; reduced-motion "off" means show
    // it and hold still.
    if (motion_policy_is_off()) return true;
    return phase_ < config_.period_seconds * config_.duty;
}

// ── Geometry ─────────────────────────────────────────────────────────────

float caret_advance(const CaretMetrics& m) noexcept {
    return m.advance > 0.0f ? m.advance : std::max(0.0f, m.nominal_advance);
}

Rect caret_rect_for_style(CaretStyle style, const CaretMetrics& m) noexcept {
    switch (style) {
        case CaretStyle::underline: {
            // Sits on the baseline the way the `_` glyph does — a hair below
            // it, never at the bottom of the widget's box.
            const float thickness = std::max(1.0f, m.stroke);
            const float gap = std::max(1.0f, m.cell_height * 0.08f);
            return {m.x, m.baseline + gap, caret_advance(m), thickness};
        }
        case CaretStyle::block:
            return {m.x, m.cell_top, caret_advance(m), m.cell_height};
        case CaretStyle::ibeam:
        default: {
            // Straddles the anchor rather than starting at it, so the bar marks
            // the glyph boundary itself instead of overlapping the glyph after
            // it. The underline and block, which cover a whole cell, start at
            // the anchor and extend right.
            const float w = std::max(1.0f, m.stroke);
            return {m.x - w * 0.5f, m.cell_top, w, m.cell_height};
        }
    }
}

void paint_caret(canvas::Canvas& canvas, CaretStyle style, const CaretMetrics& m,
                 canvas::Color color) {
    const Rect r = caret_rect_for_style(style, m);
    if (r.is_empty()) return;
    if (style == CaretStyle::ibeam) {
        // A centered stroke, not a filled rect: the two differ by half a stroke
        // width, and every existing editor pixel is placed by this line.
        const float cx = r.x + r.width * 0.5f;
        canvas.set_stroke_color(color);
        canvas.set_line_width(r.width);
        canvas.stroke_line(cx, r.y, cx, r.bottom());
        return;
    }
    canvas.set_fill_color(color);
    canvas.fill_rect(r.x, r.y, r.width, r.height);
}

void paint_caret_over_text(canvas::Canvas& canvas, CaretStyle style,
                           const CaretMetrics& m, canvas::Color color,
                           canvas::Color text_bg, const std::string& text,
                           float text_x) {
    paint_caret(canvas, style, m, color);
    if (style != CaretStyle::block) return;

    const Rect r = caret_rect_for_style(style, m);
    if (r.is_empty()) return;

    // Redraw only the covered glyph, in the background color, so the block
    // reads as an inverted cell rather than an eraser. Clipping to the caret
    // rect keeps this to one glyph without having to slice the string on a
    // UTF-8 boundary.
    canvas.save();
    canvas.clip_rect(r.x, r.y, r.width, r.height);
    canvas.set_fill_color(text_bg);
    canvas.fill_text(text, text_x, m.baseline);
    canvas.restore();
}

}  // namespace pulp::view
