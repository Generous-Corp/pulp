#include <algorithm>
#include <pulp/view/widgets.hpp>
#include <algorithm>
#include <pulp/view/animation.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/image_cache.hpp>
#include <pulp/view/text_overflow.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/canvas/text_shaper.hpp>
#include <choc/text/choc_JSON.h>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>

namespace pulp::view {

// ── Schema renderer — interprets declarative JSON widget definitions ─────────

static bool parse_schema_float_token(const std::string& token, float& out) {
    if (token.empty()) return false;
    char* end = nullptr;
    float value = std::strtof(token.c_str(), &end);
    if (end == token.c_str() || !std::isfinite(value)) return false;
    while (*end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end))) return false;
        ++end;
    }
    out = value;
    return true;
}

static int parse_schema_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::optional<canvas::Color> parse_schema_hex_color(std::string_view value) {
    if (value.empty() || value.front() != '#') return std::nullopt;
    auto pair = [](char high, char low) -> std::optional<uint8_t> {
        const int h = parse_schema_hex_digit(high);
        const int l = parse_schema_hex_digit(low);
        if (h < 0 || l < 0) return std::nullopt;
        return static_cast<uint8_t>((h << 4) | l);
    };
    if (value.size() == 7 || value.size() == 9) {
        auto r = pair(value[1], value[2]);
        auto g = pair(value[3], value[4]);
        auto b = pair(value[5], value[6]);
        auto a = value.size() == 9 ? pair(value[7], value[8]) : std::optional<uint8_t>(255);
        if (!r || !g || !b || !a) return std::nullopt;
        return canvas::Color::rgba8(*r, *g, *b, *a);
    }
    return std::nullopt;
}

static void render_schema(canvas::Canvas& canvas, const std::string& json,
                          float w, float h, float value, View& view) {
    try {
        auto schema = choc::json::parse(json);
        if (!schema.isObject() || !schema.hasObjectMember("elements")) return;

        auto elements = schema["elements"];
        float cx = w * 0.5f, cy = h * 0.5f;
        float r = std::min(cx, cy) * 0.9f;

        for (uint32_t i = 0; i < elements.size(); ++i) {
            auto el = elements[i];
            auto type = el["type"].getWithDefault(std::string(""));

            // Resolve color: token name → theme color
            auto resolveColor = [&](const std::string& key, canvas::Color fallback) -> canvas::Color {
                if (!el.hasObjectMember(key)) return fallback;
                auto tok = el[key].getWithDefault(std::string(""));
                if (auto color = parse_schema_hex_color(tok))
                    return *color;
                return view.resolve_color(tok, fallback);
            };

            // Resolve dimension: percentage of widget size or absolute px
            auto resolveDim = [&](const std::string& key, float fallback) -> float {
                if (!el.hasObjectMember(key)) return fallback;
                auto s = el[key].getWithDefault(std::string(""));
                if (s.empty()) return fallback;

                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
                if (s.empty()) return fallback;

                float parsed = 0.0f;
                if (s.back() == '%') {
                    auto pct = s.substr(0, s.size() - 1);
                    return parse_schema_float_token(pct, parsed)
                        ? parsed / 100.0f * std::min(w, h) * 0.5f
                        : fallback;
                }
                return parse_schema_float_token(s, parsed) ? parsed : fallback;
            };

            // Resolve angle with optional value binding
            auto resolveAngle = [&](const std::string& key, float fallback) -> float {
                if (!el.hasObjectMember(key)) return fallback;
                auto v = el[key];
                if (v.isFloat() || v.isInt32() || v.isInt64())
                    return static_cast<float>(v.getWithDefault(0.0));
                if (v.isObject() && v.hasObjectMember("bind")) {
                    auto bind = v["bind"].getWithDefault(std::string(""));
                    if (bind == "value") {
                        auto range = v["range"];
                        float lo = range.size() > 0 ? static_cast<float>(range[0].getWithDefault(0.0)) : 0;
                        float hi = range.size() > 1 ? static_cast<float>(range[1].getWithDefault(270.0)) : 270;
                        return lo + value * (hi - lo);
                    }
                }
                return fallback;
            };

            auto lineW = static_cast<float>(el.hasObjectMember("width")
                ? el["width"].getWithDefault(3.0) : 3.0);

            if (type == "arc") {
                auto color = resolveColor("color", {100, 150, 255, 255});
                auto radius = resolveDim("radius", r);
                auto start = resolveAngle("startAngle", -135.0f);
                auto sweep = resolveAngle("sweepAngle", 270.0f);
                float startRad = start * 3.14159f / 180.0f;
                float endRad = (start + sweep) * 3.14159f / 180.0f;
                canvas.set_stroke_color(color);
                canvas.set_line_width(lineW);
                canvas.set_line_cap(canvas::LineCap::round);
                canvas.stroke_arc(cx, cy, radius, startRad, endRad);
            } else if (type == "circle") {
                auto color = resolveColor("color", {100, 150, 255, 255});
                auto radius = resolveDim("radius", r * 0.3f);
                canvas.set_fill_color(color);
                canvas.fill_circle(cx, cy, radius);
                if (el.hasObjectMember("strokeColor")) {
                    auto stroke = resolveColor("strokeColor", {60, 60, 80, 255});
                    auto stroke_width = static_cast<float>(el.hasObjectMember("strokeWidth")
                        ? el["strokeWidth"].getWithDefault(1.0) : 1.0);
                    canvas.set_stroke_color(stroke);
                    canvas.set_line_width(stroke_width);
                    canvas.stroke_circle(cx, cy, radius);
                }
            } else if (type == "line") {
                auto color = resolveColor("color", {220, 220, 220, 255});
                // Line from inner to outer at value angle
                float angle = resolveAngle("angle", -135.0f + value * 270.0f);
                float angleRad = angle * 3.14159f / 180.0f;
                float innerR = resolveDim("innerRadius", r * 0.3f);
                float outerR = resolveDim("outerRadius", r);
                canvas.set_stroke_color(color);
                canvas.set_line_width(lineW);
                canvas.set_line_cap(canvas::LineCap::round);
                canvas.stroke_line(cx + innerR * std::cos(angleRad), cy + innerR * std::sin(angleRad),
                                   cx + outerR * std::cos(angleRad), cy + outerR * std::sin(angleRad));
            } else if (type == "rect") {
                auto color = resolveColor("color", {60, 60, 80, 255});
                auto rr = resolveDim("cornerRadius", 0);
                canvas.set_fill_color(color);
                if (rr > 0) canvas.fill_rounded_rect(0, 0, w, h, rr);
                else canvas.fill_rect(0, 0, w, h);
            } else if (type == "text") {
                auto color = resolveColor("color", {200, 200, 200, 255});
                auto text = el.hasObjectMember("text") ? el["text"].getWithDefault(std::string("")) : "";
                auto size = static_cast<float>(el.hasObjectMember("fontSize")
                    ? el["fontSize"].getWithDefault(11.0) : 11.0);
                canvas.set_fill_color(color);
                canvas.set_font("Inter", size);
                canvas.set_text_align(canvas::TextAlign::center);
                canvas.fill_text(text, cx, cy);
            }
        }
    } catch (...) {
        // Invalid JSON — draw error indicator
        canvas.set_fill_color({200, 50, 50, 200});
        canvas.fill_rect(0, 0, w, h);
    }
}

// ── Knob animation ──────────────────────────────────────────────────────────

namespace {
// Default-path knob geometry shared by paint() and modulation hit-testing so
// the drawn handles and the draggable hit zones never diverge.
struct KnobModGeom { float cx, cy, ring_r, arc_w, mod_w, mod_r_base; };
KnobModGeom knob_mod_geom(const Rect& b, size_t ring_count) {
    float cx = b.width * 0.5f, cy = b.height * 0.5f;
    float full_r = std::min(cx, cy) - 3.0f;
    float arc_w = std::max(3.0f, full_r * 0.13f);
    float ring_r = ring_count == 0 ? (full_r - arc_w * 0.5f) : (full_r * 0.64f);
    float mod_w = std::max(2.0f, full_r * 0.05f);
    float mod_r_base = ring_r + arc_w * 0.5f + mod_w * 0.5f + 2.0f;
    return {cx, cy, ring_r, arc_w, mod_w, mod_r_base};
}
float knob_value_to_angle(float v) {
    return Knob::start_angle + std::clamp(v, 0.0f, 1.0f) * (Knob::end_angle - Knob::start_angle);
}
}  // namespace

void Knob::on_mouse_enter() {
    float dur = resolve_dimension("motion.duration.fast", 0.08f);
    hover_glow_.animate_to(1.0f, dur, easing::ease_out_quad);
}

void Knob::on_mouse_leave() {
    float dur = resolve_dimension("motion.duration.fast", 0.08f);
    hover_glow_.animate_to(0.0f, dur, easing::ease_out_quad);
}

void Knob::on_mouse_event(const MouseEvent& event) {
    if (!event.is_down) return;
    // Double-click returns the knob to its default position.
    if (event.click_count >= 2) store_normalized(default_value_, Notify::sync);
}

void Knob::on_mouse_down(Point pos) {
    // Modulation-arc handle hit-test FIRST: if the press lands on a ring's
    // start/end handle, drag adjusts that ring's depth (not the base value),
    // and we stay in absolute-coordinate mode (no relative-mouse capture).
    if (!mod_rings_.empty()) {
        auto g = knob_mod_geom(local_bounds(), mod_rings_.size());
        float hit_r = std::max(3.0f, g.mod_w * 0.95f) + 4.0f;  // handle + slop
        for (size_t i = 0; i < mod_rings_.size(); ++i) {
            float mod_r = g.mod_r_base + static_cast<float>(i) * (g.mod_w + 2.0f);
            // Use the RAW per-end offsets (not the sorted range) so the grabbed
            // handle maps to the endpoint it actually is — that endpoint then
            // moves independently of the other on drag.
            const auto& m = mod_rings_[i];
            float lo_v = std::clamp(value_ + m.lo, 0.0f, 1.0f);
            float hi_v = std::clamp(value_ + m.hi, 0.0f, 1.0f);
            float lo_a = knob_value_to_angle(lo_v), hi_a = knob_value_to_angle(hi_v);
            float lx = g.cx + mod_r * std::cos(lo_a), ly = g.cy + mod_r * std::sin(lo_a);
            float hx = g.cx + mod_r * std::cos(hi_a), hy = g.cy + mod_r * std::sin(hi_a);
            float dl = std::hypot(pos.x - lx, pos.y - ly);
            float dh = std::hypot(pos.x - hx, pos.y - hy);
            if (dl <= hit_r || dh <= hit_r) {
                mod_drag_ring_ = static_cast<int>(i);
                mod_drag_is_high_ = dh <= dl;  // which END was grabbed
                // Seed the continuous-angle tracker at the grabbed handle's
                // current position so the drag can't cross the bottom gap.
                mod_drag_last_angle_ = knob_value_to_angle(mod_drag_is_high_ ? hi_v : lo_v);
                return;
            }
        }
    }
    drag_start_y_ = pos.y;
    drag_last_y_ = pos.y;
    drag_start_proportion_ = position_for_value();
    if (!gesture_active_) {
        gesture_active_ = true;
        if (on_gesture_begin) on_gesture_begin();
    }
    if (window_host())
        window_host()->set_mouse_relative_mode(true);
}

void Knob::on_mouse_up(Point) {
    mod_drag_ring_ = -1;
    if (window_host())
        window_host()->set_mouse_relative_mode(false);
    if (gesture_active_) {
        gesture_active_ = false;
        if (on_gesture_end) on_gesture_end();
    }
}

void Knob::on_mouse_drag(Point pos) {
    // Dragging a modulation handle moves ONLY the grabbed endpoint to the
    // pointer's angle (offset = pointerValue − base, clipped to [−1,1]). The
    // other endpoint stays fixed — no symmetric grow/shrink. The two ends may
    // cross; modulation_range() sorts them, so the arc still draws correctly.
    if (mod_drag_ring_ >= 0) {
        constexpr float two_pi = 6.2831853f, pi = 3.14159265f;
        auto g = knob_mod_geom(local_bounds(), mod_rings_.size());
        // Track the drag angle CONTINUOUSLY (unwrapped relative to the last
        // angle), then clamp to the live arc [start, end]. The bottom gap is a
        // hard wall: pushing the pointer into or across it sticks the handle at
        // the boundary on its current side instead of teleporting to the far
        // end. To reach the other end the user must drag the long way (over the
        // top), which never crosses the gap.
        float raw = std::atan2(pos.y - g.cy, pos.x - g.cx);
        while (raw - mod_drag_last_angle_ < -pi) raw += two_pi;
        while (raw - mod_drag_last_angle_ >  pi) raw -= two_pi;
        float clamped = std::clamp(raw, Knob::start_angle, Knob::end_angle);
        mod_drag_last_angle_ = clamped;
        float pv = (clamped - Knob::start_angle) / (Knob::end_angle - Knob::start_angle);
        auto& m = mod_rings_[static_cast<size_t>(mod_drag_ring_)];
        float& end = mod_drag_is_high_ ? m.hi : m.lo;
        float off = std::clamp(pv - value_, -1.0f, 1.0f);
        if (off != end) {
            end = off;
            request_repaint();
            if (on_modulation_change) on_modulation_change(mod_drag_ring_, m.lo, m.hi);
        }
        return;
    }
    // Drag up to increase, down to decrease. The travel is applied in the core's
    // PROPORTION space (post-curve), so a skewed knob drags perceptually
    // linearly; the core maps it back to a real value, then clamps and snaps it.
    // Sensitivity, the fine-drag divisor and the velocity law all live in the
    // core — the widget only supplies the pixels.
    const double delta_total = static_cast<double>(drag_start_y_ - pos.y);
    const double delta_tick = static_cast<double>(drag_last_y_ - pos.y);
    drag_last_y_ = pos.y;
    const bool moved = core_.velocity_mode()
        ? core_.drag_step(delta_tick, /*fine*/ false, Notify::none)
        : core_.drag_to(delta_total, drag_start_proportion_, /*fine*/ false, Notify::none);
    if (!moved) return;
    const float snapped = static_cast<float>(core_.normalized());
    if (snapped == value_) return;
    value_ = snapped;
    request_repaint();
    if (on_change) on_change(value_);
}

void Knob::advance_animations(float dt) {
    hover_glow_.advance(dt);
}

// ── Fader animation ─────────────────────────────────────────────────────────

void Fader::on_mouse_enter() {
    float dur = resolve_dimension("motion.duration.fast", 0.08f);
    hover_thumb_scale_.animate_to(1.3f, dur, easing::ease_out_quad);
}

void Fader::on_mouse_leave() {
    float dur = resolve_dimension("motion.duration.fast", 0.08f);
    hover_thumb_scale_.animate_to(1.0f, dur, easing::ease_out_quad);
}

void Fader::on_mouse_event(const MouseEvent& event) {
    if (!event.is_down) {
        on_mouse_up(event.position);
        return;
    }
    on_mouse_down(event.position);
}

void Fader::on_mouse_down(Point pos) {
    if (!dragging_ && on_gesture_begin) on_gesture_begin();
    dragging_ = true;
    auto b = local_bounds();
    float p = orientation_ == Orientation::horizontal
                  ? (b.width > 0 ? pos.x / b.width : 0.0f)
                  : (b.height > 0 ? 1.0f - pos.y / b.height : 0.0f);
    float new_val = value_for_position(p);
    if (new_val != value_) {
        value_ = new_val;
        if (on_change) on_change(value_);
    }
}

void Fader::on_mouse_up(Point) {
    if (!dragging_) return;
    dragging_ = false;
    if (on_gesture_end) on_gesture_end();
}

void Fader::on_mouse_drag(Point pos) {
    if (!dragging_) return;
    auto b = local_bounds();
    float p = orientation_ == Orientation::horizontal
                  ? (b.width > 0 ? pos.x / b.width : 0.0f)
                  : (b.height > 0 ? 1.0f - pos.y / b.height : 0.0f);
    float new_val = value_for_position(p);
    if (new_val != value_) {
        value_ = new_val;
        if (on_change) on_change(value_);
    }
}

void Fader::advance_animations(float dt) {
    hover_thumb_scale_.advance(dt);
}

// ── Toggle animation ────────────────────────────────────────────────────────

void Toggle::set_on(bool v, bool animate) {
    if (on_ == v) {
        // Already in the requested logical state. A non-animated caller still
        // wants the thumb to match exactly — reconcile it if a prior animation
        // left it mid-flight (a re-seed or screenshot after an interactive
        // toggle). The animated path keeps the old idempotent early-out.
        if (!animate) {
            const float want = v ? 1.0f : 0.0f;
            if (thumb_position_.value() != want) {
                thumb_position_.set(want);
                request_repaint();
            }
        }
        return;
    }
    on_ = v;
    // Keep the ARIA state slot in lockstep with the logical state — it is what
    // NSAccessibility (accessibilityValue), UIA (ToggleState) and AT-SPI
    // (CHECKED) read for a `toggle`/switch. Header ctor seeds "false".
    set_access_checked(v ? "true" : "false");
    if (animate) {
        float dur = resolve_dimension("motion.duration.normal", 0.15f);
        thumb_position_.animate_to(v ? 1.0f : 0.0f, dur, easing::ease_out_cubic);
    } else {
        // Snap: the initial seed from stored/preset state has nothing to animate
        // from, and a single headless paint never ticks the animation clock — an
        // animated seed would render stuck in the off position regardless of the
        // logical state.
        thumb_position_.set(v ? 1.0f : 0.0f);
    }
    // Programmatic toggle changes (preset apply, JS bridge setValue) must reach
    // the screen on their own; user-input toggles also repaint via the host
    // per-event path.
    request_repaint();
}

void Toggle::on_mouse_down(Point) {
    bool new_state = !on_;
    set_on(new_state);
    if (on_toggle) on_toggle(new_state);
}

void Toggle::on_mouse_enter() {
    float dur = resolve_dimension("motion.duration.fast", 0.08f);
    hover_opacity_.animate_to(1.0f, dur, easing::ease_out_quad);
}

void Toggle::on_mouse_leave() {
    float dur = resolve_dimension("motion.duration.fast", 0.08f);
    hover_opacity_.animate_to(0.0f, dur, easing::ease_out_quad);
}

void Toggle::advance_animations(float dt) {
    thumb_position_.advance(dt);
    hover_opacity_.advance(dt);
}

// Label implementation lives in core/view/src/widgets/label.cpp.

// ── Knob ─────────────────────────────────────────────────────────────────────

// Rotating indicator notch, shared by the silver vector knob and the
// single-frame sprite-body knob. Draws a short radial line at the value's
// angle (value 0..1 → [-135°, +135°], the analog-synth convention), centered
// at (cx, cy): a dark backing stroke for contrast plus the bright pointer on
// top. `notch_r` is the extent — the line runs from 35% to 95% of it; the two
// stroke widths scale from `width_ref`. Factored out of the silver path so an
// imported sprite knob (a captured static disc + a separate pointer) can still
// show a turning pointer drawn natively over the disc.
static void draw_knob_indicator_notch(canvas::Canvas& canvas,
                                      float cx, float cy,
                                      float notch_r, float width_ref,
                                      float value) {
    const canvas::Color kBacking   = canvas::Color::rgba(0.10f, 0.11f, 0.13f, 0.85f);
    const canvas::Color kIndicator = canvas::Color::rgba(0.97f, 0.97f, 0.97f, 1.0f);
    float angle = -1.5707963f /* -90° */
                  + (value - 0.5f) * 4.7123890f /* 270° total range */;
    float outer_x = cx + notch_r * 0.95f * std::cos(angle);
    float outer_y = cy + notch_r * 0.95f * std::sin(angle);
    float inner_x = cx + notch_r * 0.35f * std::cos(angle);
    float inner_y = cy + notch_r * 0.35f * std::sin(angle);
    // Subtle dark backing line for contrast on the bright top arc.
    canvas.set_stroke_color(kBacking);
    canvas.set_line_width(std::max(2.5f, width_ref * 0.10f));
    canvas.stroke_line(inner_x, inner_y, outer_x, outer_y);
    // Bright top line — the actual indicator.
    canvas.set_stroke_color(kIndicator);
    canvas.set_line_width(std::max(1.5f, width_ref * 0.07f));
    canvas.stroke_line(inner_x, inner_y, outer_x, outer_y);
}

// Pointer reproduced from the design's OWN indicator node (set_captured_indicator).
// Same [-135°,+135°] value→angle arc as the synthetic notch, but the radii, width
// and color come from the imported art, and it pivots at the disc core center
// (cx, cy) — so the line rides the disc's baked min/center/max reference ticks
// instead of a guessed sweep. A faint dark backing keeps a hairline legible on a
// bright metallic face without reading as a second line.
static void draw_knob_captured_pointer(canvas::Canvas& canvas,
                                       float cx, float cy,
                                       float r_in, float r_out, float width,
                                       canvas::Color color, float value) {
    float angle = -1.5707963f + (value - 0.5f) * 4.7123890f;
    float ox = cx + r_out * std::cos(angle);
    float oy = cy + r_out * std::sin(angle);
    float ix = cx + r_in  * std::cos(angle);
    float iy = cy + r_in  * std::sin(angle);
    canvas.set_stroke_color(canvas::Color::rgba(0.10f, 0.11f, 0.13f, 0.45f));
    canvas.set_line_width(width + 1.25f);
    canvas.stroke_line(ix, iy, ox, oy);
    canvas.set_stroke_color(color);
    canvas.set_line_width(width);
    canvas.stroke_line(ix, iy, ox, oy);
}

std::pair<float, float> Knob::modulation_range(size_t ring) const {
    if (ring >= mod_rings_.size()) return {value_, value_};
    const auto& m = mod_rings_[ring];
    // Independent endpoints: [base+lo, base+hi], sorted so .first ≤ .second.
    float a = std::clamp(value_ + m.lo, 0.0f, 1.0f);
    float b = std::clamp(value_ + m.hi, 0.0f, 1.0f);
    return {std::min(a, b), std::max(a, b)};
}

float Knob::modulated_value(size_t ring) const {
    if (ring >= mod_rings_.size()) return value_;
    const auto& m = mod_rings_[ring];
    // The indicator rides STRICTLY between the two current endpoints: phase
    // [-1,1] → t [0,1] interpolated from the low-offset end to the high-offset
    // end. Because it's a convex blend of the two clamped endpoints, the dot
    // always stays on the arc between them — even when the range is unipolar
    // (base sits at one end) or the ends have been dragged past each other
    // (lo > hi); it simply sweeps the other direction.
    float lo_v = std::clamp(value_ + m.lo, 0.0f, 1.0f);
    float hi_v = std::clamp(value_ + m.hi, 0.0f, 1.0f);
    float t = (std::clamp(mod_phase_, -1.0f, 1.0f) + 1.0f) * 0.5f;
    return lo_v + t * (hi_v - lo_v);
}

void Knob::paint(canvas::Canvas& canvas) {
    // A paint delegate installed on this knob or on any ancestor gets first
    // refusal (widget_painter.hpp). It declines by default, in which case the
    // stock rendering below runs unchanged.
    if (auto* p = effective_painter()) {
        RotaryPaintState s;
        s.bounds = local_bounds();
        s.enabled = enabled();
        s.hovered = is_hovered();
        s.pressed = gesture_active_;
        s.focused = has_focus();
        s.position = position_for_value();
        s.start_angle = Knob::start_angle;
        s.end_angle = Knob::end_angle;
        s.value = core_.value();
        s.value_min = core_.minimum();
        s.value_max = core_.maximum();
        if (p->paint_rotary(canvas, s, *this)) return;
    }

    auto b = local_bounds();
    float cx = b.width * 0.5f;
    float cy = b.height * 0.5f;
    float radius = std::min(cx, cy) * 0.8f;
    float shader_time = frame_clock() ? frame_clock()->time() : 0.0f;

    // ── Sprite strip path: designer-created filmstrip ─────────────────────
    if (sprite_strip_ && sprite_strip_->loaded()) {
        int frame = sprite_strip_->frame_for_value(value_);
        int fx, fy;
        sprite_strip_->frame_offset(frame, fx, fy);
        float fw = static_cast<float>(sprite_strip_->frame_width());
        float fh = static_cast<float>(sprite_strip_->frame_height());
        // Radius the rotating-indicator overlay (single-frame strips only)
        // sweeps within. Defaults to the layout box; the core-fit branch
        // below tightens it to the disc's actual rendered radius.
        float notch_r = std::min(b.width, b.height) * 0.5f;
        if (sprite_strip_->source() == SpriteStrip::Source::image_file) {
            if (has_sprite_core()) {
                // Core-fit: scale the WHOLE frame uniformly so its opaque core
                // fills the layout box (the soft drop-shadow bleed extends
                // beyond), then center the core in the box. Mirrors the
                // importer's have_core image sizing (design_codegen.cpp) so an
                // imported sprite knob lands at the knob's logical size, not the
                // PNG's natural shadow-inflated size (which oversized it and
                // overlapped neighbours). The core dims are in the frame's own
                // pixel space, so `s` also folds in the export scale.
                float s = std::min(b.width / sprite_core_w_, b.height / sprite_core_h_);
                float dst_w = fw * s;
                float dst_h = fh * s;
                float core_box_w = sprite_core_w_ * s;
                float core_box_h = sprite_core_h_ * s;
                float pad_x = (b.width  - core_box_w) * 0.5f;
                float pad_y = (b.height - core_box_h) * 0.5f;
                float dst_x = -sprite_core_x_ * s + pad_x;
                float dst_y = -sprite_core_y_ * s + pad_y;
                notch_r = std::min(core_box_w, core_box_h) * 0.5f;
                canvas.draw_image_from_file_rect(
                    sprite_strip_->path(),
                    static_cast<float>(fx), static_cast<float>(fy), fw, fh,
                    dst_x, dst_y, dst_w, dst_h);
            } else {
                // Legacy fallback (no recovered core): render the frame at its
                // NATURAL logical size centered on the layout box. Figma's PNG
                // export captures the node's absoluteRenderBounds (bounding box
                // + drop-shadow bleed) at a known scale (2× via the
                // figma-plugin extractor), so the PNG's intended logical size
                // is pixel_size / kExportScale. Centering on the layout box
                // lets the shadow halo naturally extend over neighboring
                // widgets — matching Figma's own behavior where overlapping
                // shadows reinforce each other.
                constexpr float kExportScale = 2.0f;
                float dst_w = fw / kExportScale;
                float dst_h = fh / kExportScale;
                float dst_x = (b.width  - dst_w) * 0.5f;
                float dst_y = (b.height - dst_h) * 0.5f;
                canvas.draw_image_from_file_rect(
                    sprite_strip_->path(),
                    static_cast<float>(fx), static_cast<float>(fy), fw, fh,
                    dst_x, dst_y, dst_w, dst_h);
            }
        } else {
            // Legacy raw-RGBA path. NOTE: SkiaCanvas::draw_image_from_data
            // expects ENCODED bytes (PNG/JPEG), so callers that fed decoded
            // pixels here will draw nothing. Kept for backward compatibility
            // and any future raw-pixmap canvas API.
            size_t frame_bytes = static_cast<size_t>(fw * fh * 4);
            size_t offset = static_cast<size_t>(fy * sprite_strip_->total_width() * 4 +
                                                 fx * 4);
            if (offset + frame_bytes <= sprite_strip_->data_size()) {
                canvas.save();
                canvas.clip_rect(0, 0, b.width, b.height);
                float sx = b.width / fw;
                float sy = b.height / fh;
                canvas.scale(sx, sy);
                canvas.translate(static_cast<float>(-fx), static_cast<float>(-fy));
                canvas.draw_image_from_data(sprite_strip_->data(),
                                             sprite_strip_->data_size(),
                                             0, 0,
                                             static_cast<float>(sprite_strip_->total_width()),
                                             static_cast<float>(sprite_strip_->total_height()));
                canvas.restore();
            }
        }
        // A single-frame strip is a static body (the captured disc): overlay
        // the native rotating indicator notch so the imported sprite knob
        // still TURNS with value. Multi-frame strips encode the rotation in
        // the frames themselves (frame_for_value picks the angle), so they
        // get no overlay.
        if (sprite_strip_->frame_count() == 1) {
            if (has_captured_indicator_) {
                float r_out = ind_r_out_ * notch_r;
                float r_in  = ind_r_in_  * notch_r;
                float w = std::max(1.5f, ind_width_ * notch_r);
                // The disc art's BAKED indicator (a stuck second line at the rest
                // angle) is erased at import — clean_baked_knob_indicator rewrites
                // the disc PNG — so here we just draw the design's own rotating
                // pointer over the now-clean disc.
                draw_knob_captured_pointer(canvas, cx, cy, r_in, r_out, w,
                                           ind_color_, value_);
            } else {
                draw_knob_indicator_notch(canvas, cx, cy, notch_r, notch_r, value_);
            }
        }
        // Fall through to draw labels on top
    }
    // ── Declarative schema path: JSON defines appearance as data ──────────
    else if (!widget_schema_.empty()) {
        render_schema(canvas, widget_schema_, b.width, b.height, value_, *this);
        // Fall through to draw labels on top
    }
    // ── Custom shader path: replaces body/track/fill, keeps labels/glow ──
    else if (has_custom_shader()) {
        canvas::Canvas::ShaderUniforms u;
        u.value = value_;
        u.time = shader_time;
        u.accent_color = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));
        u.bg_color = resolve_color("bg.primary", canvas::Color::rgba8(30, 30, 46));
        u.track_color = resolve_color("control.track", canvas::Color::rgba8(60, 60, 60));
        u.fill_color = resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
        u.thumb_color = resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220));
        canvas.draw_with_sksl(custom_shader(), 0, 0, b.width, b.height, u);
        // Fall through to draw labels and value text on top of the shader
    } else if (render_style_ == WidgetRenderStyle::minimal) {
        // ── Minimal/design-preview: simple circle outline (matches design tools) ──
        float full_r = std::min(cx, cy) - 2.0f;
        auto fill_bg = resolve_color("bg.surface", canvas::Color::rgba8(49, 50, 68));
        canvas.set_fill_color(fill_bg);
        canvas.fill_circle(cx, cy, full_r);

        // Use per-widget border color if set via setBorder, otherwise theme color
        auto stroke = has_border()
            ? border_color() : resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
        canvas.set_stroke_color(stroke);
        canvas.set_line_width(2.5f);
        canvas.stroke_circle(cx, cy, full_r);
    } else if (render_style_ == WidgetRenderStyle::silver) {
        // ── Silver/chrome skeuomorphic knob (native vector) ────────────────
        // Mimics the ELYSIUM-style brushed-metal knob from Figma without
        // PNG sprites. All canvas primitives — works on the CPU raster
        // path (pulp-screenshot) and the GPU Graphite path (live window).
        //
        // Layered top-to-bottom:
        //   1. Soft drop shadow below the body
        //   2. Outer dark rim (the knob's edge bezel)
        //   3. Chrome body — two-circle radial gradient (off-centered light
        //      source from top-left, fading to mid-grey at bottom-right)
        //   4. Specular highlight arc at the top of the body
        //   5. Indicator notch — short white line at the value's rotation
        //
        // Rotation: value 0..1 maps to angle range [-135°, +135°] which
        // matches the classic analog-synth knob convention (Reaktor / U-He
        // / Ableton stock knobs). Indicator notch sits at that angle from
        // 35% inner radius to 95% outer radius — long enough to read,
        // short enough not to dominate.
        //
        // Named brushed-metal palette tuned to the ELYSIUM Figma reference:
        // body highlight ~RGB 200, mid
        // ~150, shadow side ~95; the rim/bevel/indicator tones frame it.
        const canvas::Color kSilverRim              = canvas::Color::rgba(0.22f, 0.23f, 0.26f, 1.0f);
        const canvas::Color kSilverBodyLight        = canvas::Color::rgba(0.82f, 0.84f, 0.88f, 1.0f);
        const canvas::Color kSilverBodyMid          = canvas::Color::rgba(0.62f, 0.65f, 0.70f, 1.0f);
        const canvas::Color kSilverBodyDim          = canvas::Color::rgba(0.40f, 0.43f, 0.49f, 1.0f);
        const canvas::Color kSilverInnerBevel       = canvas::Color::rgba(0.18f, 0.20f, 0.23f, 0.55f);

        float full_r = std::min(cx, cy) - 2.0f;
        float body_r = full_r - 1.5f;         // chrome body radius
        float inner_r = body_r - 1.0f;

        // 1. Drop shadow — soft offset disc below the body
        for (int i = 4; i >= 1; --i) {
            float a = 0.06f * static_cast<float>(i);
            canvas.set_fill_color(canvas::Color::rgba(0, 0, 0, a));
            canvas.fill_circle(cx, cy + 1.5f + static_cast<float>(i),
                               full_r + static_cast<float>(i));
        }

        // 2. Outer dark rim (the edge bezel) — slightly darker than body
        canvas.set_fill_color(kSilverRim);
        canvas.fill_circle(cx, cy, full_r);

        // 3. Chrome body — two-circle radial gradient from light source
        //    at the upper-left to mid-tone toward lower-right. Palette
        //    tuned to match the Figma reference: highlight ~RGB 200, mid
        //    body ~150, shadow side ~95. Without the darker mid-body the
        //    knob looks washed-out and the indicator loses contrast.
        canvas::Color grads[3] = { kSilverBodyLight, kSilverBodyMid, kSilverBodyDim };
        float stops[3] = { 0.0f, 0.50f, 1.0f };
        canvas.set_fill_gradient_radial_two_circles(
            cx - body_r * 0.30f, cy - body_r * 0.40f, body_r * 0.05f,
            cx + body_r * 0.25f, cy + body_r * 0.35f, body_r * 1.20f,
            grads, stops, 3);
        canvas.fill_circle(cx, cy, body_r);
        canvas.clear_fill_gradient();

        // 4a. Inner bevel ring — a thin darker stroke just inside the
        //     body edge creates a "bezel-inside-bezel" effect that reads
        //     as a precision-machined control. Subtle (alpha 0.35) so it
        //     doesn't compete with the highlight.
        canvas.set_stroke_color(kSilverInnerBevel);
        canvas.set_line_width(std::max(1.0f, body_r * 0.04f));
        canvas.stroke_circle(cx, cy, body_r - body_r * 0.05f);

        // 4b. Specular highlight arc near the top — soft white rim
        //     suggesting reflected light from above.
        canvas.set_stroke_color(canvas::Color::rgba(1.0f, 1.0f, 1.0f, 0.55f));
        canvas.set_line_width(std::max(1.0f, body_r * 0.08f));
        canvas.set_line_cap(canvas::LineCap::round);
        // Top half of the body (~225° → 315° in canvas radians; top = 270°).
        canvas.stroke_arc(cx, cy, inner_r * 0.94f,
                          3.926990f /* 225° */, 5.497787f /* 315° */);

        // 4c. Lower shadow arc — counterweight to the specular at the
        //     bottom edge, sells the "rounded body" illusion.
        canvas.set_stroke_color(canvas::Color::rgba(0.0f, 0.0f, 0.0f, 0.30f));
        canvas.set_line_width(std::max(1.0f, body_r * 0.07f));
        canvas.stroke_arc(cx, cy, inner_r * 0.94f,
                          0.785398f /* 45° */, 2.356194f /* 135° */);

        // 5. Indicator notch — the rotating pointer (shared with the
        //    single-frame sprite-body path). Extent rides the inner radius;
        //    stroke widths scale from the chrome body radius.
        draw_knob_indicator_notch(canvas, cx, cy, inner_r, body_r, value_);
    } else {
        // ── Default C++ paint path — Ink & Signal knob ───────────────────
        // Solid raised body disc + full track ring + thick value arc + a
        // white dot pointer on the dial face, matching the Figma design
        // language (not a thin needle on a bare arc).
        float full_r = std::min(cx, cy) - 3.0f;
        float arc_w  = std::max(3.0f, full_r * 0.13f);   // thick ring
        // With modulation rings present, shrink the main arc so the thin
        // per-source rings fit in the band outside it (within full_r).
        float ring_r = mod_rings_.empty() ? (full_r - arc_w * 0.5f) : (full_r * 0.64f);
        float body_r = ring_r - arc_w * 0.5f - 2.0f;     // disc inside the ring
        float value_angle = start_angle + position_for_value() * (end_angle - start_angle);

        // Hover glow ring (drawn behind everything)
        float glow = hover_glow_.value();
        if (glow > 0.01f) {
            auto accent = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));
            canvas.set_stroke_color(canvas::Color::rgba(accent.r, accent.g, accent.b,
                                    0.16f * glow));
            canvas.set_line_width(arc_w + 6.0f);
            canvas.stroke_arc(cx, cy, ring_r, start_angle, end_angle);
        }

        // Raised body disc
        auto body_color = resolve_color("bg.elevated", canvas::Color::rgba8(38, 44, 54));
        canvas.set_fill_color(body_color);
        canvas.fill_circle(cx, cy, body_r);

        // Subtle bevel ring framing the disc edge (matches the Figma knob's
        // lighter rim between the body and the arc).
        auto bevel = resolve_color("control.border", canvas::Color::rgba8(70, 78, 92));
        canvas.set_stroke_color(canvas::Color::rgba(bevel.r, bevel.g, bevel.b, 0.6f));  // token-lint:allow (alpha blend of resolved token)
        canvas.set_line_width(1.0f);
        canvas.stroke_circle(cx, cy, body_r - 0.5f);

        // Full track ring (background arc)
        auto track_color = resolve_color("knob.arc.bg", canvas::Color::rgba8(60, 66, 78));
        canvas.set_stroke_color(track_color);
        canvas.set_line_width(arc_w);
        canvas.set_line_cap(canvas::LineCap::round);
        canvas.stroke_arc(cx, cy, ring_r, start_angle, end_angle);

        // Value arc (accent)
        auto fill_color = resolve_color("knob.arc", canvas::Color::rgba8(100, 150, 255));
        canvas.set_stroke_color(fill_color);
        canvas.stroke_arc(cx, cy, ring_r, start_angle, value_angle);

        // White dot pointer on the dial face near the rim
        float dot_r = std::max(2.0f, full_r * 0.06f);
        float dot_rad = body_r - dot_r - 2.0f;
        float dot_x = cx + dot_rad * std::cos(value_angle);
        float dot_y = cy + dot_rad * std::sin(value_angle);
        auto thumb_color = resolve_color("knob.thumb", canvas::Color::rgba8(230, 230, 230));
        canvas.set_fill_color(thumb_color);
        canvas.fill_circle(dot_x, dot_y, dot_r);

        // Modulation rings (Saturn) — one thin per-source arc outside the main
        // arc. The arc shows the modulation RANGE centered on the base value
        // ([value-|depth|, value+|depth|], clipped at the parameter limits). A
        // dot at each end is a draggable handle (drag to set depth). A bright
        // indicator dot rides the arc at the live modulated value (base +
        // depth·phase), so you can see where the parameter actually is in real
        // time as the source moves.
        if (!mod_rings_.empty()) {
            auto g = knob_mod_geom(b, mod_rings_.size());
            float handle_r = std::max(3.0f, g.mod_w * 0.95f);
            canvas.set_line_cap(canvas::LineCap::round);
            for (size_t i = 0; i < mod_rings_.size(); ++i) {
                const auto& m = mod_rings_[i];
                float mod_r = g.mod_r_base + static_cast<float>(i) * (g.mod_w + 2.0f);
                auto [lo, hi] = modulation_range(i);
                float lo_a = knob_value_to_angle(lo);
                float hi_a = knob_value_to_angle(hi);

                // Faint full track + the colored range arc.
                canvas.set_stroke_color(canvas::Color::rgba(m.color.r, m.color.g, m.color.b, 0.22f));  // token-lint:allow (per-source mod colour)
                canvas.set_line_width(g.mod_w);
                canvas.stroke_arc(g.cx, g.cy, mod_r, start_angle, end_angle);
                canvas.set_stroke_color(m.color);
                canvas.stroke_arc(g.cx, g.cy, mod_r, lo_a, hi_a);
                // No end-cap handle dots by design — the arc's ends are still
                // draggable (hit-tested in on_mouse_down), but only the live
                // indicator dot below is drawn, riding between the two ends.

                // Live modulated-value indicator: bright dot ringed in the
                // source colour, riding the arc at base + depth·phase.
                float la = knob_value_to_angle(modulated_value(i));
                float lx = g.cx + mod_r * std::cos(la), ly = g.cy + mod_r * std::sin(la);
                auto ind = resolve_color("knob.thumb", canvas::Color::rgba8(235, 235, 235));
                canvas.set_fill_color(ind);
                canvas.fill_circle(lx, ly, handle_r * 0.78f);
                canvas.set_stroke_color(m.color);
                canvas.set_line_width(2.0f);
                canvas.stroke_circle(lx, ly, handle_r * 0.78f + 1.0f);
            }
        }
    }

    // Label below (always drawn, even with shader)
    if (show_label_ && !label_.empty()) {
        auto text_color = resolve_color("text.secondary", canvas::Color::rgba8(150, 150, 150));
        canvas.set_fill_color({text_color.r, text_color.g, text_color.b, text_color.a});
        canvas.set_font("Inter", 10.0f);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text_anchored(label_, cx, b.height - 6, canvas::Canvas::TextAnchor::Baseline);
    }

    // Value text in center (always drawn, even with shader)
    if (format_) {
        auto text_color = resolve_color("text.primary", canvas::Color::rgba8(200, 200, 200));
        canvas.set_fill_color({text_color.r, text_color.g, text_color.b, text_color.a});
        canvas.set_font("Inter", 11.0f);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text(format_(value_), cx, cy + 4);
    }
}

// ── Fader ────────────────────────────────────────────────────────────────────

void Fader::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    float shader_time = frame_clock() ? frame_clock()->time() : 0.0f;

    bool vert = orientation_ == Orientation::vertical;
    float track_length = vert ? b.height : b.width;
    float track_width = vert ? b.width : b.height;

    // Sprite strip path
    if (sprite_strip_ && sprite_strip_->loaded()) {
        int frame = sprite_strip_->frame_for_value(value_);
        int fx, fy;
        sprite_strip_->frame_offset(frame, fx, fy);
        size_t frame_bytes = static_cast<size_t>(sprite_strip_->frame_width() *
                                                  sprite_strip_->frame_height() * 4);
        size_t offset = static_cast<size_t>(fy * sprite_strip_->total_width() * 4 + fx * 4);
        if (offset + frame_bytes <= sprite_strip_->data_size()) {
            canvas.save();
            canvas.clip_rect(0, 0, b.width, b.height);
            float sx = b.width / static_cast<float>(sprite_strip_->frame_width());
            float sy = b.height / static_cast<float>(sprite_strip_->frame_height());
            canvas.scale(sx, sy);
            canvas.translate(static_cast<float>(-fx), static_cast<float>(-fy));
            canvas.draw_image_from_data(sprite_strip_->data(), sprite_strip_->data_size(),
                                         0, 0,
                                         static_cast<float>(sprite_strip_->total_width()),
                                         static_cast<float>(sprite_strip_->total_height()));
            canvas.restore();
        }
    } else if (!widget_schema_.empty()) {
        render_schema(canvas, widget_schema_, b.width, b.height, value_, *this);
    } else if (has_custom_shader()) {
        canvas::Canvas::ShaderUniforms u;
        u.value = value_;
        u.time = shader_time;
        u.accent_color = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));
        u.bg_color = resolve_color("bg.primary", canvas::Color::rgba8(30, 30, 46));
        u.track_color = resolve_color("control.track", canvas::Color::rgba8(60, 60, 60));
        u.fill_color = resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
        u.thumb_color = resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220));
        canvas.draw_with_sksl(custom_shader(), 0, 0, b.width, b.height, u);
    } else if (render_style_ == WidgetRenderStyle::minimal) {
        // ── Minimal: thin track only, no fill, no thumb (matches design tools) ──
        auto track_color = resolve_color("control.track", canvas::Color::rgba8(69, 71, 90));
        canvas.set_fill_color(track_color);
        float track_thick = vert ? std::min(b.width * 0.4f, 6.0f) : std::min(b.height * 0.4f, 6.0f);
        if (vert) {
            float tx = (b.width - track_thick) * 0.5f;
            canvas.fill_rounded_rect(tx, 0, track_thick, track_length, track_thick * 0.5f);
        } else {
            float ty = (b.height - track_thick) * 0.5f;
            canvas.fill_rounded_rect(0, ty, track_length, track_thick, track_thick * 0.5f);
        }
    } else {
        // Skew-mapped track position for the current value (== value_ when
        // skew is linear, so the default look is unchanged).
        const float pos = position_for_value();

        // ── Per-widget skin overrides (figma-plugin import) ────────────────
        // When the importer derived track / fill / thumb colours from the
        // captured design, honour them here and force a rounded-rect thumb so
        // the look matches the captured art — but keep the position
        // value-driven (the thumb still moves with value_), unlike a baked
        // full-image sprite. Falls back to theme tokens when unset.
        const bool skinned = has_skin();
        const bool skin_rect_thumb = skinned;  // captured faders use a slab thumb

        // Track
        auto track_color = has_skin_track_
            ? track_color_
            : resolve_color("control.track", canvas::Color::rgba8(60, 60, 60));
        canvas.set_fill_color({track_color.r, track_color.g, track_color.b, track_color.a});

        // Track thickness. When the importer derived the captured track width,
        // honour it exactly (clamped to the widget box) so the track is the
        // narrow line the art shows — not a fraction of the box, which
        // over-wide widget bounds would balloon. Otherwise fall back to the
        // default heuristic (skinned: ~18% of box; default: 4px line).
        float track_thick =
            has_skin_track_width_ ? std::min(skin_track_width_, track_width)
            : skinned             ? std::max(6.0f, std::min(track_width * 0.18f, 12.0f))
                                  : 4.0f;
        float track_radius = track_thick * 0.5f;
        float track_x = 0, track_y = 0, track_w = 0, track_h = 0;
        if (vert) {
            track_x = (b.width - track_thick) * 0.5f;
            track_y = 0; track_w = track_thick; track_h = track_length;
        } else {
            track_y = (b.height - track_thick) * 0.5f;
            track_x = 0; track_w = track_length; track_h = track_thick;
        }
        canvas.fill_rounded_rect(track_x, track_y, track_w, track_h, track_radius);

        // Track outline. The captured empty track has a visible lighter edge
        // around the dark channel. When the importer derived that edge colour
        // from the art, stroke the track rect so the empty portion above the
        // thumb doesn't read as a flat dark slab. Drawn before the fill/thumb
        // so they sit on top, matching the captured layering.
        if (has_skin_track_border_) {
            canvas.set_stroke_color({track_border_color_.r, track_border_color_.g,
                                     track_border_color_.b, track_border_color_.a});
            canvas.set_line_width(1.0f);
            canvas.stroke_rounded_rect(track_x, track_y, track_w, track_h, track_radius);
        }

        // Fill (value portion)
        auto fill_color = has_skin_fill_
            ? fill_color_
            : resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
        canvas.set_fill_color({fill_color.r, fill_color.g, fill_color.b, fill_color.a});

        if (vert) {
            float fill_height = pos * track_length;
            float tx = (b.width - track_thick) * 0.5f;
            canvas.fill_rounded_rect(tx, track_length - fill_height, track_thick, fill_height, track_radius);
        } else {
            float fill_width = pos * track_length;
            float ty = (b.height - track_thick) * 0.5f;
            canvas.fill_rounded_rect(0, ty, fill_width, track_thick, track_radius);
        }

        // Thumb (with hover scale animation)
        auto thumb_color = has_skin_thumb_
            ? thumb_color_
            : resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220));
        canvas.set_fill_color({thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a});

        if (thumb_shape_ == ThumbShape::rectangle || skin_rect_thumb) {
            const float scale = hover_thumb_scale_.value();
            // Skinned faders default to a wide rounded slab (matching the
            // captured Figma thumb) when explicit dimensions weren't given.
            // When the importer derived a thin track width, it also sized the
            // widget box to the captured thumb width — so the thumb fills the
            // box (no 0.62 shrink), and the track is the thin line.
            const float skin_thumb_frac = has_skin_track_width_ ? 1.0f : 0.62f;
            const float skin_default_w = vert ? std::min(b.width, track_width) * skin_thumb_frac
                                              : std::max(10.0f, track_length * 0.10f);
            // A captured fader thumb is a flat slab — wider than tall (~2:1).
            // Default the cross-axis extent to ~half the long-axis so the thumb
            // reads as a slab, not the near-square/circle that
            // track_length*0.10 produced on tall faders. (Vertical fader: thumb
            // is wide & short, so its height ≈ half its width.)
            const float skin_default_h = vert ? std::max(8.0f, skin_default_w * 0.5f)
                                              : std::min(b.height, track_width) * 0.62f;
            const float default_w = skinned ? skin_default_w : (vert ? std::min(b.width, track_width) : 10.0f);
            const float default_h = skinned ? skin_default_h : (vert ? 9.0f : std::min(b.height, track_width));
            const float thumb_w = std::max(1.0f, (thumb_width_ > 0.0f ? thumb_width_ : default_w) * scale);
            const float thumb_h = std::max(1.0f, (thumb_height_ > 0.0f ? thumb_height_ : default_h) * scale);
            const float axis_half = (vert ? thumb_h : thumb_w) * 0.5f;
            const float usable = std::max(0.0f, track_length - 2.0f * axis_half);
            const float axis_center = axis_half + (vert ? (1.0f - pos) : pos) * usable;
            // Skinned slab defaults to a generously rounded corner when none
            // was provided (the captured thumb is a pill).
            const float skin_radius = std::min(thumb_w, thumb_h) * 0.5f;
            const float radius = thumb_corner_radius_ > 0.0f
                ? std::min(thumb_corner_radius_, std::min(thumb_w, thumb_h) * 0.5f)
                : skin_radius;  // pill ends by default (matches the Figma fader handle)
            float thumb_x, thumb_y;
            if (vert) {
                thumb_x = (b.width - thumb_w) * 0.5f;
                thumb_y = axis_center - thumb_h * 0.5f;
            } else {
                thumb_x = axis_center - thumb_w * 0.5f;
                thumb_y = (b.height - thumb_h) * 0.5f;
            }
            canvas.fill_rounded_rect(thumb_x, thumb_y, thumb_w, thumb_h, radius);
            // Optional thumb border (the captured silver thumb has a darker
            // bevel edge). Drawn as a stroke around the slab.
            if (has_skin_thumb_border_) {
                canvas.set_stroke_color(thumb_border_color_);
                canvas.set_line_width(1.5f);
                canvas.stroke_rounded_rect(thumb_x, thumb_y, thumb_w, thumb_h, radius);
            }
        } else {
            float thumb_radius = std::min(track_width * 0.35f, 8.0f) * hover_thumb_scale_.value();
            // Convention: when mapping a 0..1 value to a position with a circular/rect
            // indicator, inset by the indicator's radius so it stays fully within bounds:
            //   usable = length - 2 * radius;  pos = radius + value * usable;
            if (vert) {
                float usable = track_length - 2.0f * thumb_radius;
                float thumb_y = thumb_radius + usable - pos * usable;
                canvas.fill_circle(b.width * 0.5f, thumb_y, thumb_radius);
            } else {
                float usable = track_length - 2.0f * thumb_radius;
                float thumb_x = thumb_radius + pos * usable;
                canvas.fill_circle(thumb_x, b.height * 0.5f, thumb_radius);
            }
        }
    }

    // Label
    if (!label_.empty()) {
        auto text_color = resolve_color("text.secondary", canvas::Color::rgba8(150, 150, 150));
        canvas.set_fill_color({text_color.r, text_color.g, text_color.b, text_color.a});
        canvas.set_font("Inter", 10.0f);
        canvas.set_text_align(canvas::TextAlign::center);
        if (vert) {
            canvas.fill_text_anchored(label_, b.width * 0.5f, b.height - 6, canvas::Canvas::TextAnchor::Baseline);
        } else {
            canvas.fill_text_anchored(label_, b.width * 0.5f, b.height - 6, canvas::Canvas::TextAnchor::Baseline);
        }
    }
}

// ── RangeSlider ──────────────────────────────────────────────────────────────
// HTML <input type="range"> equivalent. Track + handle, no decorative
// fader chrome. Min/max/step quantisation lives here so the painted
// position and the value seen by JS callers always agree.
void RangeSlider::clamp_and_quantize_() {
    // Defensive: if max < min, treat the range as collapsed at min.
    float lo = min_;
    float hi = std::max(min_, max_);

    float v = std::clamp(value_, lo, hi);

    // Snap to step relative to min_, but only when step is positive.
    // Step <= 0 means "continuous" — matches HTML <input step="any">.
    if (step_ > 0.0f) {
        float steps = std::round((v - lo) / step_);
        v = lo + steps * step_;
        // Re-clamp because rounding can push us past hi (e.g. min=0 max=1
        // step=0.3 — last reachable step is 0.9, not 1.2).
        v = std::clamp(v, lo, hi);
    }

    if (v != value_) {
        value_ = v;
        if (on_change) on_change(value_);
    }
}

void RangeSlider::set_skew_from_midpoint(float mid_value) {
    float lo = min_, hi = std::max(min_, max_);
    if (hi <= lo) { skew_ = 1.0f; return; }
    float prop = std::clamp((mid_value - lo) / (hi - lo), 1e-4f, 1.0f - 1e-4f);
    skew_ = std::log(0.5f) / std::log(prop);
    skew_ = std::max(0.0001f, skew_);
}

float RangeSlider::value_to_position_() const {
    float lo = min_, hi = std::max(min_, max_);
    float span = hi - lo;
    float prop = span > 0.0f ? std::clamp((value_ - lo) / span, 0.0f, 1.0f) : 0.0f;
    return skew_ == 1.0f ? prop : std::pow(prop, skew_);
}

float RangeSlider::position_to_value_(float t) const {
    float lo = min_;
    float hi = std::max(min_, max_);
    float clamped_t = std::clamp(t, 0.0f, 1.0f);
    // Linear drag position → value proportion via the inverse skew curve.
    float prop = skew_ == 1.0f ? clamped_t : std::pow(clamped_t, 1.0f / skew_);
    float v = lo + prop * (hi - lo);

    if (step_ > 0.0f) {
        float steps = std::round((v - lo) / step_);
        v = lo + steps * step_;
        v = std::clamp(v, lo, hi);
    }
    return v;
}

void RangeSlider::update_from_position_(Point pos) {
    auto b = local_bounds();
    float t;
    if (orientation_ == Orientation::horizontal) {
        t = b.width > 0 ? pos.x / b.width : 0;
    } else {
        // Vertical: y=0 at top → t=1 (max); y=height → t=0 (min).
        t = b.height > 0 ? 1.0f - pos.y / b.height : 0;
    }
    float new_val = position_to_value_(t);
    if (new_val != value_) {
        value_ = new_val;
        if (on_change) on_change(value_);
    }
}

void RangeSlider::on_mouse_event(const MouseEvent& event) {
    if (!event.is_down) {
        if (dragging_ && on_gesture_end) on_gesture_end();
        dragging_ = false;
        return;
    }
    if (!dragging_ && on_gesture_begin) on_gesture_begin();
    dragging_ = true;
    update_from_position_(event.position);
}

void RangeSlider::on_mouse_drag(Point pos) {
    if (!dragging_) return;
    update_from_position_(pos);
}

void RangeSlider::on_mouse_enter() {
    float dur = resolve_dimension("motion.duration.fast", 0.08f);
    hover_scale_.animate_to(1.3f, dur, easing::ease_out_quad);
}

void RangeSlider::on_mouse_leave() {
    float dur = resolve_dimension("motion.duration.fast", 0.08f);
    hover_scale_.animate_to(1.0f, dur, easing::ease_out_quad);
}

void RangeSlider::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    bool horiz = orientation_ == Orientation::horizontal;

    // Resolve theme colors. Caller-supplied accent overrides theme for
    // both the active fill and the handle.
    auto track_color = resolve_color("control.track", canvas::Color::rgba8(60, 60, 60));
    auto theme_fill  = resolve_color("control.fill",  canvas::Color::rgba8(100, 150, 255));
    auto theme_thumb = resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220));
    auto fill_color  = has_accent_color_ ? accent_color_ : theme_fill;
    auto thumb_color = has_accent_color_ ? accent_color_ : theme_thumb;

    // Track thickness — typical HTML range visuals sit in 4..6px.
    float track_thick = std::min(track_thickness_,
                                 horiz ? std::max(b.height, 1.0f)
                                       : std::max(b.width,  1.0f));

    // Normalised position along the track, taking the (possibly-collapsed)
    // [min,max] range and the skew curve into account.
    float lo = min_;
    float hi = std::max(min_, max_);
    float t = value_to_position_();

    // Background track.
    canvas.set_fill_color(track_color);
    if (horiz) {
        float ty = (b.height - track_thick) * 0.5f;
        canvas.fill_rounded_rect(0, ty, b.width, track_thick, track_thick * 0.5f);
    } else {
        float tx = (b.width - track_thick) * 0.5f;
        canvas.fill_rounded_rect(tx, 0, track_thick, b.height, track_thick * 0.5f);
    }

    // Active fill — only the portion between min and current value.
    canvas.set_fill_color(fill_color);
    if (horiz) {
        float fill_w = t * b.width;
        float ty = (b.height - track_thick) * 0.5f;
        if (fill_w > 0)
            canvas.fill_rounded_rect(0, ty, fill_w, track_thick, track_thick * 0.5f);
    } else {
        float fill_h = t * b.height;
        float tx = (b.width - track_thick) * 0.5f;
        // Vertical: fill the lower portion (closer to min at the bottom).
        if (fill_h > 0)
            canvas.fill_rounded_rect(tx, b.height - fill_h, track_thick, fill_h, track_thick * 0.5f);
    }

    // Handle. Inset by handle radius so it never overshoots the bounds.
    canvas.set_fill_color(thumb_color);
    float handle_radius = (horiz ? std::min(b.height, 16.0f) * 0.5f
                                 : std::min(b.width,  16.0f) * 0.5f)
                          * hover_scale_.value();
    if (horiz) {
        float usable = std::max(0.0f, b.width - 2.0f * handle_radius);
        float hx = handle_radius + t * usable;
        canvas.fill_circle(hx, b.height * 0.5f, handle_radius);
    } else {
        float usable = std::max(0.0f, b.height - 2.0f * handle_radius);
        // Vertical: t=0 → bottom (handle near max-y), t=1 → top.
        float hy = handle_radius + (1.0f - t) * usable;
        canvas.fill_circle(b.width * 0.5f, hy, handle_radius);
    }
}

// ── Toggle ───────────────────────────────────────────────────────────────────

void Toggle::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    float shader_time = frame_clock() ? frame_clock()->time() : 0.0f;

    float switch_w = std::min(b.width, 40.0f);
    float switch_h = std::min(b.height * 0.6f, 20.0f);
    float sx = (b.width - switch_w) * 0.5f;
    float sy = (b.height - switch_h) * 0.5f;

    if (!widget_schema_.empty()) {
        render_schema(canvas, widget_schema_, b.width, b.height, on_ ? 1.0f : 0.0f, *this);
    } else if (has_custom_shader()) {
        canvas::Canvas::ShaderUniforms u;
        u.value = on_ ? 1.0f : 0.0f;
        u.time = shader_time;
        u.accent_color = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));
        u.bg_color = resolve_color("bg.primary", canvas::Color::rgba8(30, 30, 46));
        u.track_color = resolve_color("control.track", canvas::Color::rgba8(60, 60, 60));
        u.fill_color = resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
        u.thumb_color = resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220));
        canvas.draw_with_sksl(custom_shader(), 0, 0, b.width, b.height, u);
    } else {

        // Track — blend color based on animated thumb position
        float t = thumb_position_.value();
        auto on_color = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));
        auto off_color = resolve_color("control.track", canvas::Color::rgba8(60, 60, 60));
        auto bg_color = off_color.interpolate(on_color, t);
        canvas.set_fill_color(bg_color);
        canvas.fill_rounded_rect(sx, sy, switch_w, switch_h, switch_h * 0.5f);

        // Hover highlight on track
        float hov = hover_opacity_.value();
        if (hov > 0.01f) {
            canvas.set_fill_color(canvas::Color::rgba8(255, 255, 255, static_cast<uint8_t>(15 * hov)));
            canvas.fill_rounded_rect(sx, sy, switch_w, switch_h, switch_h * 0.5f);
        }

        // Thumb circle — position animated between off and on. The Ink & Signal
        // toggle uses a dark ink knob on the teal track (Figma), not a white
        // thumb; interpolate from the neutral off-thumb to dark ink as it turns on.
        auto off_thumb = resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220));
        auto on_thumb = resolve_color("accent.text", canvas::Color::rgba8(12, 20, 24));
        auto thumb_color = off_thumb.interpolate(on_thumb, t);
        canvas.set_fill_color({thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a});
        float thumb_r = switch_h * 0.4f;
        float off_x = sx + switch_h * 0.5f;
        float on_x = sx + switch_w - switch_h * 0.5f;
        float thumb_x = off_x + (on_x - off_x) * t;
        canvas.fill_circle(thumb_x, sy + switch_h * 0.5f, thumb_r);
    }

    // Label
    if (!label_.empty()) {
        auto text_color = resolve_color("text.secondary", canvas::Color::rgba8(150, 150, 150));
        canvas.set_fill_color({text_color.r, text_color.g, text_color.b, text_color.a});
        canvas.set_font("Inter", 10.0f);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text_anchored(label_, b.width * 0.5f, b.height - 6, canvas::Canvas::TextAnchor::Baseline);
    }
}

// ── Checkbox ────────────────────────────────────────────────────────────────

void Checkbox::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    float size = std::min(b.width, b.height);
    float cx = b.width * 0.5f;
    float cy = b.height * 0.5f;
    float r = size * 0.35f;  // smaller radius to avoid clipping at bounds edge

    // Rounded SQUARE (matches the Figma design language — not a circle).
    float corner = r * 0.35f;
    if (checked_) {
        // Filled rounded square
        auto fill = resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
        canvas.set_fill_color(fill);
        canvas.fill_rounded_rect(cx - r, cy - r, r * 2, r * 2, corner);
        // Check glyph (simple checkmark using lines)
        canvas.set_stroke_color(canvas::Color::rgba8(30, 30, 40));
        canvas.set_line_width(2.0f);
        canvas.stroke_line(cx - r * 0.35f, cy, cx - r * 0.05f, cy + r * 0.3f);
        canvas.stroke_line(cx - r * 0.05f, cy + r * 0.3f, cx + r * 0.4f, cy - r * 0.3f);
    } else {
        // Stroked rounded square
        auto border = resolve_color("control.border", canvas::Color::rgba8(80, 80, 100));
        canvas.set_stroke_color(border);
        canvas.set_line_width(1.5f);
        canvas.stroke_rounded_rect(cx - r, cy - r, r * 2, r * 2, corner);
    }
}

void Checkbox::on_mouse_down(Point) {
    checked_ = !checked_;
    if (on_change) on_change(checked_);
}

// ── ToggleButton ────────────────────────────────────────────────────────────

void ToggleButton::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    auto bg = on_
        ? on_background_color_.value_or(resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255)))
        : off_background_color_.value_or(resolve_color("bg.surface", canvas::Color::rgba8(50, 50, 60)));
    auto border = on_
        ? on_border_color_.value_or(resolve_color("control.border", canvas::Color::rgba8(80, 80, 100)))
        : off_border_color_.value_or(resolve_color("control.border", canvas::Color::rgba8(80, 80, 100)));
    const bool has_custom_border = on_ ? on_border_color_.has_value() : off_border_color_.has_value();
    const float radius = corner_radius_.value_or(6.0f);

    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, radius);
    if (!on_ || has_custom_border) {
        canvas.set_stroke_color(border);
        canvas.set_line_width(1);
        canvas.stroke_rounded_rect(0, 0, b.width, b.height, radius);
    }

    if (!label_.empty()) {
        auto text_color = on_
            ? on_text_color_.value_or(canvas::Color::rgba8(255, 255, 255))
            : off_text_color_.value_or(resolve_color("text.primary", canvas::Color::rgba8(200, 200, 210)));
        canvas.set_fill_color(text_color);
        canvas.set_font("Inter", font_size_.value_or(13.0f));
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text_anchored(label_, b.width * 0.5f, b.height * 0.5f, canvas::Canvas::TextAnchor::GlyphCenter);
    }
}

void ToggleButton::on_mouse_down(Point) {
    if (radio_group_ != 0) {
        if (on_) return;            // clicking the active radio keeps it selected
        set_on(true);
        if (auto* p = parent()) {   // deselect siblings in the same group
            for (size_t i = 0; i < p->child_count(); ++i) {
                auto* sib = dynamic_cast<ToggleButton*>(p->child_at(i));
                if (sib && sib != this && sib->radio_group_ == radio_group_ && sib->is_on()) {
                    sib->set_on(false);
                    if (sib->on_toggle) sib->on_toggle(false);
                }
            }
        }
        if (on_toggle) on_toggle(true);
        return;
    }
    set_on(!on_);
    if (on_toggle) on_toggle(on_);
}

// ── Icon ────────────────────────────────────────────────────────────────────

void Icon::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    float cx = b.width * 0.5f;
    float cy = b.height * 0.5f;
    float s = std::min(b.width, b.height) * 0.35f; // icon scale

    auto color = resolve_color("text.secondary", canvas::Color::rgba8(160, 160, 180));
    canvas.set_stroke_color(color);
    canvas.set_line_width(1.5f);
    canvas.set_line_cap(canvas::LineCap::round);
    canvas.set_line_join(canvas::LineJoin::round);

    switch (type_) {
        case Type::image_upload: {
            float frame_w = std::min(b.width, b.height) * 0.82f;
            float frame_h = frame_w * 0.76f;
            float frame_x = cx - frame_w * 0.5f;
            float frame_y = cy - frame_h * 0.46f;
            float radius = frame_w * 0.13f;
            canvas.set_line_width(1.4f);
            canvas.stroke_rounded_rect(frame_x, frame_y, frame_w, frame_h, radius);

            float base_y = frame_y + frame_h * 0.74f;
            canvas.begin_path();
            canvas.move_to(frame_x + frame_w * 0.16f, base_y);
            canvas.line_to(frame_x + frame_w * 0.36f, frame_y + frame_h * 0.50f);
            canvas.line_to(frame_x + frame_w * 0.50f, base_y);
            canvas.move_to(frame_x + frame_w * 0.42f, base_y);
            canvas.line_to(frame_x + frame_w * 0.62f, frame_y + frame_h * 0.36f);
            canvas.line_to(frame_x + frame_w * 0.84f, base_y);
            canvas.stroke_current_path();

            canvas.set_fill_color(color);
            canvas.fill_circle(frame_x + frame_w * 0.73f, frame_y + frame_h * 0.27f, frame_w * 0.07f);
            break;
        }
        case Type::send: {
            float plane = std::min(b.width, b.height) * 0.82f;
            auto paper = canvas::Color::rgba8(255, 255, 255, 244);
            canvas.set_fill_color(paper);
            canvas.begin_path();
            canvas.move_to(cx - plane * 0.42f, cy - plane * 0.10f);
            canvas.line_to(cx + plane * 0.44f, cy - plane * 0.34f);
            canvas.line_to(cx + plane * 0.06f, cy + plane * 0.42f);
            canvas.line_to(cx - plane * 0.04f, cy + plane * 0.14f);
            canvas.close_path();
            canvas.fill_current_path();

            canvas.set_stroke_color(canvas::Color::rgba8(255, 255, 255, 210));
            canvas.set_line_width(1.15f);
            canvas.stroke_line(cx - plane * 0.02f, cy + plane * 0.10f,
                               cx + plane * 0.14f, cy - plane * 0.02f);
            break;
        }
        case Type::search: {
            // Magnifying glass
            float r = s * 0.6f;
            canvas.stroke_circle(cx - s * 0.15f, cy - s * 0.15f, r);
            canvas.stroke_line(cx + r * 0.5f, cy + r * 0.5f, cx + s, cy + s);
            break;
        }
        case Type::close: {
            // X mark
            canvas.stroke_line(cx - s * 0.5f, cy - s * 0.5f, cx + s * 0.5f, cy + s * 0.5f);
            canvas.stroke_line(cx + s * 0.5f, cy - s * 0.5f, cx - s * 0.5f, cy + s * 0.5f);
            break;
        }
    }
}

// ── InlineValueEditor ────────────────────────────────────────────────────────

std::string InlineValueEditor::display_() const {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", std::max(0, decimals_), value_);
    return std::string(buf) + suffix_;
}

InlineValueEditor::~InlineValueEditor() {
    // By destruction the widget may already be detached, and frame_clock() walks
    // parent_ — it would return null and leak a subscription holding a dangling
    // `this`. The cached clock pointer is what makes the unsubscribe reliable.
    unsubscribe_caret_blink_();
}

void InlineValueEditor::subscribe_caret_blink_() {
    if (caret_blink_sub_ >= 0) return;
    if (auto* fc = frame_clock()) {
        caret_blink_clock_ = fc;
        caret_blink_sub_ = fc->subscribe([this](float dt) {
            caret_blink_.advance(dt);
            request_repaint();
            return true;
        });
    }
}

void InlineValueEditor::on_frame_clock_changed() {
    // Identical hazard to TextEditor::on_frame_clock_changed: a host that owns its
    // FrameClock clears the tree's clock from its destructor and frees the clock
    // right after, so this is the last moment the cached `caret_blink_clock_` is
    // safe to touch. Drop the subscription now; re-home it on the new clock if we
    // are still editing.
    unsubscribe_caret_blink_();
    if (editing_) subscribe_caret_blink_();
}

void InlineValueEditor::unsubscribe_caret_blink_() {
    if (caret_blink_sub_ >= 0 && caret_blink_clock_)
        caret_blink_clock_->unsubscribe(caret_blink_sub_);
    caret_blink_sub_ = -1;
    caret_blink_clock_ = nullptr;
}

void InlineValueEditor::begin_edit() {
    if (!enabled_ || editing_) return;
    editing_ = true;
    invalid_ = false;
    caret_blink_.reset();
    subscribe_caret_blink_();
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", std::max(0, decimals_), value_);
    edit_buffer_ = buf;
    request_repaint();
}

void InlineValueEditor::commit_edit() {
    if (!editing_) return;
    editing_ = false;
    unsubscribe_caret_blink_();
    if (!edit_buffer_.empty()) {
        try {
            double v = std::stod(edit_buffer_);
            if (v < min_ || v > max_) {
                invalid_ = true;   // out of range: flag, keep the prior value
            } else {
                invalid_ = false;
                value_ = v;
                if (on_change) on_change(value_);
            }
        } catch (...) {
            // unparseable → leave the value unchanged
        }
    }
    request_repaint();
}

void InlineValueEditor::cancel_edit() {
    editing_ = false;
    invalid_ = false;
    edit_buffer_.clear();
    unsubscribe_caret_blink_();
    request_repaint();
}

void InlineValueEditor::on_mouse_down(Point) {
    if (enabled_ && !editing_) begin_edit();
}

void InlineValueEditor::on_text_input(const TextInputEvent& event) {
    if (!editing_) return;
    for (char c : event.text)
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+')
            edit_buffer_ += c;
    invalid_ = false;
    caret_blink_.keep_solid();  // typing holds the caret visible
    request_repaint();
}

bool InlineValueEditor::on_key_event(const KeyEvent& event) {
    if (!editing_ || !event.is_down) return false;
    if (event.key == KeyCode::enter) { commit_edit(); return true; }
    if (event.key == KeyCode::escape) { cancel_edit(); return true; }
    if (event.key == KeyCode::backspace) {
        if (!edit_buffer_.empty()) edit_buffer_.pop_back();
        caret_blink_.keep_solid();  // an edit holds the caret visible
        request_repaint();
        return true;
    }
    return false;
}

void InlineValueEditor::on_focus_changed(bool gained) {
    if (!gained && editing_) commit_edit();
}

void InlineValueEditor::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    const float radius = 6.0f;
    auto bg = resolve_color("bg.elevated", canvas::Color::rgba8(20, 24, 30));
    auto fg = resolve_color("text.primary", canvas::Color::rgba8(220, 224, 230));
    auto accent = resolve_color("accent.primary", canvas::Color::rgba8(22, 218, 194));
    auto danger = resolve_color("accent.error", canvas::Color::rgba8(255, 92, 77));
    auto border = resolve_color("control.border", canvas::Color::rgba8(60, 66, 78));
    if (!enabled_) fg.a = 120;

    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, radius);

    auto ring = invalid_ ? danger : (editing_ ? accent : border);
    canvas.set_stroke_color(ring);
    canvas.set_line_width((editing_ || invalid_) ? 1.6f : 1.0f);
    canvas.stroke_rounded_rect(0.8f, 0.8f, b.width - 1.6f, b.height - 1.6f, radius - 0.8f);

    canvas.set_font("Inter", std::min(14.0f, b.height * 0.55f));
    canvas.set_text_align(canvas::TextAlign::center);
    const std::string txt = editing_ ? edit_buffer_ : display_();
    canvas.set_fill_color(editing_ ? accent : fg);
    canvas.fill_text_anchored(txt, b.width * 0.5f, b.height * 0.5f,
                              canvas::Canvas::TextAnchor::GlyphCenter);
    canvas.set_text_align(canvas::TextAlign::left);
    // Caret is a SEPARATE shape just right of the (stable, centered) text —
    // never part of the measured string, so the value doesn't jump or shift
    // left/right as the caret blinks. The blink phase advances on the frame
    // clock, not here: advancing it per paint would tie the rate to the
    // display's refresh.
    if (editing_ && caret_blink_.visible()) {
        const float tw = canvas.measure_text(txt);
        const float font_size = std::min(14.0f, b.height * 0.55f);

        CaretMetrics m;
        m.x = b.width * 0.5f + tw * 0.5f + 3.0f;
        m.cell_top = b.height * 0.28f;
        m.cell_height = b.height * 0.44f;
        m.baseline = b.height * 0.5f + font_size * 0.36f;  // GlyphCenter → baseline
        m.advance = 0.0f;  // the caret always sits past the last glyph here
        m.nominal_advance = canvas.measure_text("0");
        m.stroke = 1.5f;
        // The text is centered, so a block caret's glyph redraw would need the
        // same anchored placement; there is no glyph under it, so nothing to invert.
        paint_caret(canvas, caret_style_, m, accent);
    }
}

// ── DualRangeSlider ──────────────────────────────────────────────────────────

void DualRangeSlider::clamp_() {
    low_ = std::clamp(low_, min_, max_);
    high_ = std::clamp(high_, min_, max_);
    request_repaint();
}

float DualRangeSlider::pos_for_(float v) const {
    if (max_ <= min_) return 0.0f;
    return std::clamp((v - min_) / (max_ - min_), 0.0f, 1.0f);
}

float DualRangeSlider::value_for_pos_(float t) const {
    return min_ + std::clamp(t, 0.0f, 1.0f) * (max_ - min_);
}

void DualRangeSlider::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    const bool horiz = orientation_ == Orientation::horizontal;
    const float track = 4.0f, r = 7.0f;
    auto track_color = resolve_color("control.track", canvas::Color::rgba8(58, 64, 74));
    auto fill_color = resolve_color("control.fill", canvas::Color::rgba8(22, 218, 194));
    auto thumb_color = resolve_color("control.thumb", canvas::Color::rgba8(232, 236, 242));
    if (!enabled_) {
        fill_color.a = 90; thumb_color.a = 130; track_color.a = 140;
    }
    const float tlo = pos_for_(std::min(low_, high_));
    const float thi = pos_for_(std::max(low_, high_));

    if (horiz) {
        const float cy = b.height * 0.5f;
        const float x0 = r, x1 = b.width - r, span = std::max(1.0f, x1 - x0);
        canvas.set_fill_color(track_color);
        canvas.fill_rounded_rect(x0, cy - track * 0.5f, span, track, track * 0.5f);
        canvas.set_fill_color(fill_color);
        canvas.fill_rounded_rect(x0 + tlo * span, cy - track * 0.5f, (thi - tlo) * span, track, track * 0.5f);
        for (float t : {pos_for_(low_), pos_for_(high_)}) {
            float cx = x0 + t * span;
            canvas.set_fill_color(thumb_color);
            canvas.fill_circle(cx, cy, r);
            canvas.set_stroke_color(fill_color);
            canvas.set_line_width(2.0f);
            canvas.stroke_circle(cx, cy, r);
        }
    } else {
        const float cx = b.width * 0.5f;
        const float y0 = r, y1 = b.height - r, span = std::max(1.0f, y1 - y0);
        // Vertical: min at the bottom, so invert the screen-y.
        canvas.set_fill_color(track_color);
        canvas.fill_rounded_rect(cx - track * 0.5f, y0, track, span, track * 0.5f);
        canvas.set_fill_color(fill_color);
        canvas.fill_rounded_rect(cx - track * 0.5f, y0 + (1.0f - thi) * span, track, (thi - tlo) * span, track * 0.5f);
        for (float t : {pos_for_(low_), pos_for_(high_)}) {
            float cy = y0 + (1.0f - t) * span;
            canvas.set_fill_color(thumb_color);
            canvas.fill_circle(cx, cy, r);
            canvas.set_stroke_color(fill_color);
            canvas.set_line_width(2.0f);
            canvas.stroke_circle(cx, cy, r);
        }
    }
}

float DualRangeSlider::pointer_t_(Point pos) const {
    auto b = local_bounds();
    const float r = 7.0f;
    if (orientation_ == Orientation::horizontal) {
        float x0 = r, span = std::max(1.0f, b.width - 2 * r);
        return std::clamp((pos.x - x0) / span, 0.0f, 1.0f);
    }
    float y0 = r, span = std::max(1.0f, b.height - 2 * r);
    return std::clamp(1.0f - (pos.y - y0) / span, 0.0f, 1.0f);  // min at bottom
}

void DualRangeSlider::apply_(Point pos) {
    float v = value_for_pos_(pointer_t_(pos));
    // no_cross: a dragged thumb stops at the other (low ≤ high preserved).
    if (drag_ == 0) low_ = no_cross_ ? std::min(v, high_) : v;
    else if (drag_ == 1) high_ = no_cross_ ? std::max(v, low_) : v;
    clamp_();
    if (on_change) on_change(low_, high_);
}

void DualRangeSlider::on_mouse_down(Point pos) {
    if (!enabled_) return;
    float t = pointer_t_(pos);
    // Grab the nearer thumb; tie favours the low thumb.
    drag_ = (std::fabs(t - pos_for_(high_)) < std::fabs(t - pos_for_(low_))) ? 1 : 0;
    apply_(pos);
}

void DualRangeSlider::on_mouse_drag(Point pos) {
    if (drag_ >= 0) apply_(pos);
}

void DualRangeSlider::on_mouse_up(Point) {
    drag_ = -1;
}

// ── GroupBox ───────────────────────────────────────────────────────────────

void GroupBox::set_collapsed(bool c) {
    if (c == collapsed_) return;
    collapsed_ = c;
    apply_child_visibility();
    if (on_toggle) on_toggle(collapsed_);
    request_repaint();
}

void GroupBox::apply_child_visibility() {
    for (std::size_t i = 0; i < child_count(); ++i)
        child_at(i)->set_visible(!collapsed_);
}

void GroupBox::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    const float radius = 8.0f;
    const float frame_h = collapsed_ ? GroupBox::header_height : b.height;

    auto bg = resolve_color("bg.surface", canvas::Color::rgba8(30, 36, 46));
    auto border = resolve_color("control.border", canvas::Color::rgba8(60, 66, 78));
    auto fg = resolve_color("text.secondary", canvas::Color::rgba8(150, 156, 168));

    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, frame_h, radius);
    canvas.set_stroke_color(border);
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(0.5f, 0.5f, b.width - 1.0f, frame_h - 1.0f, radius - 0.5f);

    // Title chip (top-left): a filled pill with the uppercase title.
    if (!title_.empty()) {
        std::string up = title_;
        for (auto& ch : up) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
        canvas.set_font("Inter", 11.0f);
        canvas.set_text_align(canvas::TextAlign::left);
        float tw = canvas.measure_text(up);
        canvas.set_fill_color(resolve_color("bg.elevated", canvas::Color::rgba8(42, 48, 60)));
        canvas.fill_rounded_rect(10.0f, 7.0f, tw + 18.0f, 18.0f, 5.0f);
        canvas.set_fill_color(fg);
        canvas.fill_text(up, 19.0f, 20.0f);
    }

    // Collapse chevron (top-right), drawn as strokes so it never tofus:
    // ▾ when expanded, ▸ when collapsed.
    if (collapsible_) {
        const float cx = b.width - 18.0f, cy = GroupBox::header_height * 0.5f;
        canvas.set_stroke_color(fg);
        canvas.set_line_width(1.6f);
        if (collapsed_) {                 // ▸ pointing right
            canvas.stroke_line(cx - 2.0f, cy - 4.0f, cx + 2.0f, cy);
            canvas.stroke_line(cx + 2.0f, cy, cx - 2.0f, cy + 4.0f);
        } else {                          // ▾ pointing down
            canvas.stroke_line(cx - 4.0f, cy - 2.0f, cx, cy + 2.0f);
            canvas.stroke_line(cx, cy + 2.0f, cx + 4.0f, cy - 2.0f);
        }
    }
}

void GroupBox::on_mouse_event(const MouseEvent& event) {
    // A click anywhere in the header band toggles collapse (when collapsible).
    if (event.is_down && collapsible_ && event.position.y <= GroupBox::header_height) {
        set_collapsed(!collapsed_);
        return;
    }
    View::on_mouse_event(event);
}

// Visualizer implementations (ImageView / Meter / XYPad / WaveformView /
// SpectrumView / Panel / SpectrogramView / MultiMeter / CorrelationMeter)
// live in core/view/src/widgets/visualizers.cpp.

// ── WaveformRecorder ─────────────────────────────────────────────────────────

namespace {
constexpr float kRecorderPad = 16.0f;   // outer panel padding
constexpr float kRecorderMeterH = 22.0f;  // bottom level-meter strip height
constexpr float kRecorderBadgeRow = 34.0f;  // space reserved for the badge row
}  // namespace

Rect WaveformRecorder::meter_rect_() const {
    auto b = local_bounds();
    return {kRecorderPad,
            b.height - kRecorderPad - kRecorderMeterH,
            std::max(0.0f, b.width - 2 * kRecorderPad),
            kRecorderMeterH};
}

Rect WaveformRecorder::waveform_rect_() const {
    auto b = local_bounds();
    float top = kRecorderPad + kRecorderBadgeRow;
    float bottom = meter_rect_().y - 12.0f;
    return {kRecorderPad, top,
            std::max(0.0f, b.width - 2 * kRecorderPad),
            std::max(0.0f, bottom - top)};
}

Point WaveformRecorder::transport_center_() const {
    auto wf = waveform_rect_();
    return {wf.x + wf.width * 0.5f, wf.y + wf.height * 0.5f};
}

float WaveformRecorder::transport_radius_() const {
    auto wf = waveform_rect_();
    return std::clamp(std::min(wf.width, wf.height) * 0.18f, 18.0f, 44.0f);
}

void WaveformRecorder::set_state(State s) {
    if (s == state_) return;
    state_ = s;
    dragging_threshold_ = false;
    request_repaint();
    if (on_state_change) on_state_change(state_);
}

void WaveformRecorder::advance_state_() {
    switch (state_) {
        case State::armed:
            if (on_record) on_record();
            set_state(State::recording);
            break;
        case State::recording:
            if (on_stop) on_stop();
            set_state(State::captured);
            break;
        case State::captured:
            if (on_play) on_play();
            set_state(State::armed);
            break;
    }
}

void WaveformRecorder::update_threshold_from_x_(float x) {
    auto m = meter_rect_();
    if (m.width <= 0.0f) return;
    float t = std::clamp((x - m.x) / m.width, 0.0f, 1.0f);
    if (t == threshold_) return;
    threshold_ = t;
    request_repaint();
    if (on_threshold_change) on_threshold_change(threshold_);
}

void WaveformRecorder::on_mouse_down(Point pos) {
    // The center transport button takes priority over the meter.
    Point c = transport_center_();
    float r = transport_radius_();
    float dx = pos.x - c.x;
    float dy = pos.y - c.y;
    if (dx * dx + dy * dy <= r * r) {
        advance_state_();
        return;
    }
    // The threshold thumb is only draggable while armed.
    if (state_ == State::armed) {
        auto m = meter_rect_();
        if (pos.x >= m.x - 8.0f && pos.x <= m.x + m.width + 8.0f &&
            pos.y >= m.y - 8.0f && pos.y <= m.y + m.height + 8.0f) {
            dragging_threshold_ = true;
            update_threshold_from_x_(pos.x);
        }
    }
}

void WaveformRecorder::on_mouse_drag(Point pos) {
    if (dragging_threshold_ && state_ == State::armed)
        update_threshold_from_x_(pos.x);
}

void WaveformRecorder::on_mouse_up(Point) {
    dragging_threshold_ = false;
}

void WaveformRecorder::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Theme tokens (Ink & Signal dark theme supplies the teal/coral/amber).
    auto bg     = resolve_color("bg.surface",     canvas::Color::rgba8(36, 36, 36));
    auto border = resolve_color("control.border", canvas::Color::rgba8(58, 58, 58));
    auto text2  = resolve_color("text.secondary", canvas::Color::rgba8(138, 138, 138));
    auto teal   = resolve_color("accent.primary", canvas::Color::rgba8(59, 130, 246));
    auto coral  = resolve_color("accent.error",   canvas::Color::rgba8(239, 68, 68));
    auto amber  = resolve_color("accent.warning", canvas::Color::rgba8(245, 158, 11));

    // Panel background.
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 14.0f);

    // ── Waveform area ────────────────────────────────────────────────────
    auto wf = waveform_rect_();
    canvas.set_fill_color(canvas::Color::rgba(0, 0, 0, 0.18f));
    canvas.fill_rounded_rect(wf.x, wf.y, wf.width, wf.height, 10.0f);

    canvas::Color wave_color;
    switch (state_) {
        case State::armed:
            wave_color = canvas::Color::rgba(text2.r, text2.g, text2.b, 0.30f);
            break;
        case State::recording: wave_color = coral; break;
        case State::captured:  wave_color = teal;  break;
    }

    if (!waveform_.empty() && wf.width > 0.0f && wf.height > 0.0f) {
        canvas.set_fill_color(wave_color);
        float mid_y = wf.y + wf.height * 0.5f;
        int cols = std::max(1, static_cast<int>(wf.width));
        float bar_w = wf.width / static_cast<float>(cols);
        size_t n = waveform_.size();
        for (int i = 0; i < cols; ++i) {
            size_t s0 = static_cast<size_t>((static_cast<float>(i) / cols) * n);
            size_t s1 = static_cast<size_t>((static_cast<float>(i + 1) / cols) * n);
            if (s1 <= s0) s1 = s0 + 1;
            float peak = 0.0f;
            for (size_t s = s0; s < s1 && s < n; ++s)
                peak = std::max(peak, std::fabs(waveform_[s]));
            float half = std::clamp(peak, 0.0f, 1.0f) * (wf.height * 0.45f);
            float x = wf.x + static_cast<float>(i) * bar_w;
            canvas.fill_rect(x, mid_y - half, std::max(1.0f, bar_w - 0.5f), half * 2.0f);
        }
    } else {
        // Empty/armed: faint center baseline.
        canvas.set_fill_color(canvas::Color::rgba(text2.r, text2.g, text2.b, 0.18f));
        canvas.fill_rect(wf.x + 6.0f, wf.y + wf.height * 0.5f - 0.5f,
                         std::max(0.0f, wf.width - 12.0f), 1.0f);
    }

    // ── Level meter + threshold thumb ────────────────────────────────────
    auto m = meter_rect_();
    float meter_r = m.height * 0.5f;
    canvas.set_fill_color(canvas::Color::rgba(0, 0, 0, 0.30f));
    canvas.fill_rounded_rect(m.x, m.y, m.width, m.height, meter_r);
    if (level_ > 0.0f && m.width > 0.0f) {
        canvas::Color fill = (state_ == State::recording) ? coral : teal;
        if (state_ == State::captured)
            fill = canvas::Color::rgba(teal.r, teal.g, teal.b, 0.35f);
        canvas.set_fill_color(fill);
        float fw = std::clamp(m.width * level_, meter_r * 2.0f, m.width);
        canvas.fill_rounded_rect(m.x, m.y, fw, m.height, meter_r);
    }
    {
        float tx = m.x + std::clamp(threshold_, 0.0f, 1.0f) * m.width;
        float thumb_w = 6.0f;
        float thumb_h = m.height + 10.0f;
        float thumb_y = m.y - 5.0f;
        canvas::Color thumb = (state_ == State::armed)
            ? amber
            : canvas::Color::rgba(text2.r, text2.g, text2.b, 0.5f);
        canvas.set_fill_color(thumb);
        canvas.fill_rounded_rect(tx - thumb_w * 0.5f, thumb_y, thumb_w, thumb_h, 3.0f);
    }

    // ── Transport button ─────────────────────────────────────────────────
    Point c = transport_center_();
    float r = transport_radius_();
    canvas.set_fill_color(canvas::Color::rgba(0, 0, 0, 0.25f));
    canvas.fill_circle(c.x, c.y, r + 3.0f);
    canvas.set_fill_color(resolve_color("bg.primary", canvas::Color::rgba8(20, 20, 20)));
    canvas.fill_circle(c.x, c.y, r);
    canvas.set_stroke_color(border);
    canvas.set_line_width(1.5f);
    canvas.stroke_circle(c.x, c.y, r);

    if (state_ == State::armed) {
        // Red record dot.
        canvas.set_fill_color(coral);
        canvas.fill_circle(c.x, c.y, r * 0.42f);
    } else if (state_ == State::recording) {
        // Red stop square.
        canvas.set_fill_color(coral);
        float sq = r * 0.62f;
        canvas.fill_rounded_rect(c.x - sq * 0.5f, c.y - sq * 0.5f, sq, sq, 3.0f);
    } else {
        // Teal play triangle.
        canvas.set_fill_color(teal);
        float tr = r * 0.5f;
        canvas::Canvas::Point2D tri[3] = {
            {c.x - tr * 0.6f, c.y - tr},
            {c.x - tr * 0.6f, c.y + tr},
            {c.x + tr,        c.y     },
        };
        canvas.fill_path(tri, 3);
    }

    // ── Status badge (top-right) ─────────────────────────────────────────
    {
        std::string label;
        canvas::Color badge;
        switch (state_) {
            case State::armed:     label = "READY";    badge = amber; break;
            case State::recording: label = "REC";      badge = coral; break;
            case State::captured:  label = "CAPTURED"; badge = teal;  break;
        }
        canvas.set_font("Inter", 11.0f);
        float badge_h = 22.0f;
        float text_w = static_cast<float>(label.size()) * 7.0f;  // heuristic advance
        float dot_w = (state_ == State::recording) ? 14.0f : 0.0f;
        float check_w = (state_ == State::captured) ? 16.0f : 0.0f;
        float badge_w = text_w + dot_w + check_w + 20.0f;
        float bx = b.width - kRecorderPad - badge_w;
        float by = kRecorderPad - 2.0f;
        canvas.set_fill_color(canvas::Color::rgba(badge.r, badge.g, badge.b, 0.18f));
        canvas.fill_rounded_rect(bx, by, badge_w, badge_h, badge_h * 0.5f);

        float content_x = bx + 10.0f;
        float badge_cy = by + badge_h * 0.5f;
        if (state_ == State::recording) {
            canvas.set_fill_color(badge);
            canvas.fill_circle(content_x + 4.0f, badge_cy, 4.0f);
            content_x += dot_w;
        }
        if (state_ == State::captured) {
            canvas.set_stroke_color(badge);
            canvas.set_line_width(2.0f);
            canvas.stroke_line(content_x + 1.0f, badge_cy,
                               content_x + 5.0f, badge_cy + 4.0f);
            canvas.stroke_line(content_x + 5.0f, badge_cy + 4.0f,
                               content_x + 12.0f, badge_cy - 4.0f);
            content_x += check_w;
        }
        canvas.set_fill_color(badge);
        canvas.fill_text_anchored(label, content_x, badge_cy,
                                  canvas::Canvas::TextAnchor::GlyphCenter);
    }
}

} // namespace pulp::view
