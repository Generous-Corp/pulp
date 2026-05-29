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
            } else if (type == "line") {
                auto color = resolveColor("color", {220, 220, 220, 255});
                // Line from inner to outer at value angle
                float angle = resolveAngle("angle", -135.0f + value * 270.0f);
                float angleRad = angle * 3.14159f / 180.0f;
                float innerR = resolveDim("innerRadius", r * 0.3f);
                float outerR = resolveDim("outerRadius", r);
                canvas.set_stroke_color(color);
                canvas.set_line_width(lineW);
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
    // Double-click: reset to default (0.5)
    if (event.click_count >= 2) {
        value_ = default_value_;
        if (on_change) on_change(value_);
    }
}

void Knob::on_mouse_down(Point pos) {
    drag_start_y_ = pos.y;
    drag_start_value_ = value_;
    if (window_host())
        window_host()->set_mouse_relative_mode(true);
}

void Knob::on_mouse_up(Point) {
    if (window_host())
        window_host()->set_mouse_relative_mode(false);
}

void Knob::on_mouse_drag(Point pos) {
    // Drag up to increase, down to decrease. 150px = full range.
    float delta = (drag_start_y_ - pos.y) / 150.0f;
    float new_val = std::clamp(drag_start_value_ + delta, 0.0f, 1.0f);
    if (new_val != value_) {
        value_ = new_val;
        if (on_change) on_change(value_);
    }
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
    if (!event.is_down) { dragging_ = false; return; }
    dragging_ = true;
    auto b = local_bounds();
    float new_val;
    if (orientation_ == Orientation::horizontal) {
        new_val = std::clamp(event.position.x / b.width, 0.0f, 1.0f);
    } else {
        new_val = std::clamp(1.0f - event.position.y / b.height, 0.0f, 1.0f);
    }
    if (new_val != value_) {
        value_ = new_val;
        if (on_change) on_change(value_);
    }
}

void Fader::on_mouse_drag(Point pos) {
    if (!dragging_) return;
    auto b = local_bounds();
    float new_val;
    if (orientation_ == Orientation::horizontal) {
        new_val = std::clamp(pos.x / b.width, 0.0f, 1.0f);
    } else {
        new_val = std::clamp(1.0f - pos.y / b.height, 0.0f, 1.0f);
    }
    if (new_val != value_) {
        value_ = new_val;
        if (on_change) on_change(value_);
    }
}

void Fader::advance_animations(float dt) {
    hover_thumb_scale_.advance(dt);
}

// ── Toggle animation ────────────────────────────────────────────────────────

void Toggle::set_on(bool v) {
    if (on_ == v) return;
    on_ = v;
    float dur = resolve_dimension("motion.duration.normal", 0.15f);
    thumb_position_.animate_to(v ? 1.0f : 0.0f, dur, easing::ease_out_cubic);
    // pulp #73 — programmatic toggle (preset apply, JS bridge setValue)
    // must reach the screen on its own. User-input toggles already
    // repaint via the host's per-event setNeedsDisplay path.
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

// Label moved to core/view/src/widgets/label.cpp
// in the 2026-05 Phase 2 (R2-6) batch.

// ── Knob ─────────────────────────────────────────────────────────────────────

void Knob::paint(canvas::Canvas& canvas) {
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
        if (sprite_strip_->source() == SpriteStrip::Source::image_file) {
            // Render the frame at its NATURAL logical size centered on the
            // layout box. Figma's PNG export captures the node's
            // absoluteRenderBounds (bounding box + drop-shadow bleed) at a
            // known scale (2× via the figma-plugin extractor), so the PNG's
            // intended logical size is pixel_size / kExportScale. Centering
            // on the layout box lets the shadow halo naturally extend over
            // neighboring widgets — matching Figma's own behavior where
            // overlapping shadows reinforce each other. Aspect-cover into
            // the layout box would clamp the smaller knobs to the box
            // height and visibly under-scale them relative to a larger
            // sibling that shares the same shadow ratio.
            constexpr float kExportScale = 2.0f;
            float dst_w = fw / kExportScale;
            float dst_h = fh / kExportScale;
            float dst_x = (b.width  - dst_w) * 0.5f;
            float dst_y = (b.height - dst_h) * 0.5f;
            canvas.draw_image_from_file_rect(
                sprite_strip_->path(),
                static_cast<float>(fx), static_cast<float>(fy), fw, fh,
                dst_x, dst_y, dst_w, dst_h);
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
        // Fall through to draw labels on top
    }
    // ── Declarative schema path: JSON defines appearance as data ──────────
    else if (!widget_schema_.empty()) {
        render_schema(canvas, widget_schema_, b.width, b.height, value_, *this);
        // Fall through to draw labels on top
    }
    // ── Custom shader path: replaces body/track/fill, keeps labels/glow ──
    else if (!custom_sksl_.empty()) {
        canvas::Canvas::ShaderUniforms u;
        u.value = value_;
        u.time = shader_time;
        u.accent_color = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));
        u.bg_color = resolve_color("bg.primary", canvas::Color::rgba8(30, 30, 46));
        u.track_color = resolve_color("control.track", canvas::Color::rgba8(60, 60, 60));
        u.fill_color = resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
        u.thumb_color = resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220));
        canvas.draw_with_sksl(custom_sksl_, 0, 0, b.width, b.height, u);
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
    } else {
        // ── Default C++ paint path ──────────────────────────────────────

        // Hover glow ring (drawn behind everything)
        float glow = hover_glow_.value();
        if (glow > 0.01f) {
            auto accent = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));
            canvas.set_stroke_color(canvas::Color::rgba(accent.r, accent.g, accent.b,
                                    static_cast<uint8_t>(40 * glow)));
            canvas.set_line_width(6.0f);
            canvas.stroke_arc(cx, cy, radius + 2.0f, start_angle, end_angle);
        }

        // Track (background arc)
        auto track_color = resolve_color("control.track", canvas::Color::rgba8(60, 60, 60));
        canvas.set_stroke_color({track_color.r, track_color.g, track_color.b, track_color.a});
        canvas.set_line_width(3.0f);
        canvas.set_line_cap(canvas::LineCap::round);
        canvas.stroke_arc(cx, cy, radius, start_angle, end_angle);

        // Value arc
        float value_angle = start_angle + value_ * (end_angle - start_angle);
        auto fill_color = resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
        canvas.set_stroke_color({fill_color.r, fill_color.g, fill_color.b, fill_color.a});
        canvas.stroke_arc(cx, cy, radius, start_angle, value_angle);

        // Thumb indicator line
        float thumb_x = cx + radius * 0.6f * std::cos(value_angle);
        float thumb_y = cy + radius * 0.6f * std::sin(value_angle);
        float inner_x = cx + radius * 0.3f * std::cos(value_angle);
        float inner_y = cy + radius * 0.3f * std::sin(value_angle);
        auto thumb_color = resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220));
        canvas.set_stroke_color({thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a});
        canvas.set_line_width(2.0f);
        canvas.stroke_line(inner_x, inner_y, thumb_x, thumb_y);
    }

    // Label below (always drawn, even with shader)
    if (!label_.empty()) {
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
    } else if (!custom_sksl_.empty()) {
        canvas::Canvas::ShaderUniforms u;
        u.value = value_;
        u.time = shader_time;
        u.accent_color = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));
        u.bg_color = resolve_color("bg.primary", canvas::Color::rgba8(30, 30, 46));
        u.track_color = resolve_color("control.track", canvas::Color::rgba8(60, 60, 60));
        u.fill_color = resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
        u.thumb_color = resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220));
        canvas.draw_with_sksl(custom_sksl_, 0, 0, b.width, b.height, u);
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

        // Track
        auto track_color = resolve_color("control.track", canvas::Color::rgba8(60, 60, 60));
        canvas.set_fill_color({track_color.r, track_color.g, track_color.b, track_color.a});

        float track_thick = 4.0f;
        if (vert) {
            float tx = (b.width - track_thick) * 0.5f;
            canvas.fill_rounded_rect(tx, 0, track_thick, track_length, 2.0f);
        } else {
            float ty = (b.height - track_thick) * 0.5f;
            canvas.fill_rounded_rect(0, ty, track_length, track_thick, 2.0f);
        }

        // Fill (value portion)
        auto fill_color = resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
        canvas.set_fill_color({fill_color.r, fill_color.g, fill_color.b, fill_color.a});

        if (vert) {
            float fill_height = value_ * track_length;
            float tx = (b.width - track_thick) * 0.5f;
            canvas.fill_rounded_rect(tx, track_length - fill_height, track_thick, fill_height, 2.0f);
        } else {
            float fill_width = value_ * track_length;
            float ty = (b.height - track_thick) * 0.5f;
            canvas.fill_rounded_rect(0, ty, fill_width, track_thick, 2.0f);
        }

        // Thumb (with hover scale animation)
        auto thumb_color = resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220));
        canvas.set_fill_color({thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a});

        float thumb_radius = std::min(track_width * 0.35f, 8.0f) * hover_thumb_scale_.value();
        // Convention: when mapping a 0..1 value to a position with a circular/rect
        // indicator, inset by the indicator's radius so it stays fully within bounds:
        //   usable = length - 2 * radius;  pos = radius + value * usable;
        if (vert) {
            float usable = track_length - 2.0f * thumb_radius;
            float thumb_y = thumb_radius + usable - value_ * usable;
            canvas.fill_circle(b.width * 0.5f, thumb_y, thumb_radius);
        } else {
            float usable = track_length - 2.0f * thumb_radius;
            float thumb_x = thumb_radius + value_ * usable;
            canvas.fill_circle(thumb_x, b.height * 0.5f, thumb_radius);
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
//
// pulp issue-966.

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

float RangeSlider::position_to_value_(float t) const {
    float lo = min_;
    float hi = std::max(min_, max_);
    float clamped_t = std::clamp(t, 0.0f, 1.0f);
    float v = lo + clamped_t * (hi - lo);

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
        dragging_ = false;
        return;
    }
    dragging_ = true;
    update_from_position_(event.position);
}

void RangeSlider::on_mouse_drag(Point pos) {
    if (!dragging_) return;
    update_from_position_(pos);
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
    // [min,max] range into account.
    float lo = min_;
    float hi = std::max(min_, max_);
    float span = hi - lo;
    float t = span > 0 ? std::clamp((value_ - lo) / span, 0.0f, 1.0f) : 0.0f;

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
    float handle_radius = horiz ? std::min(b.height, 16.0f) * 0.5f
                                : std::min(b.width,  16.0f) * 0.5f;
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
    } else if (!custom_sksl_.empty()) {
        canvas::Canvas::ShaderUniforms u;
        u.value = on_ ? 1.0f : 0.0f;
        u.time = shader_time;
        u.accent_color = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));
        u.bg_color = resolve_color("bg.primary", canvas::Color::rgba8(30, 30, 46));
        u.track_color = resolve_color("control.track", canvas::Color::rgba8(60, 60, 60));
        u.fill_color = resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
        u.thumb_color = resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220));
        canvas.draw_with_sksl(custom_sksl_, 0, 0, b.width, b.height, u);
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

        // Thumb circle — position animated between off and on
        auto thumb_color = resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220));
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

    if (checked_) {
        // Filled circle
        auto fill = resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
        canvas.set_fill_color(fill);
        canvas.fill_rounded_rect(cx - r, cy - r, r * 2, r * 2, r);
        // Check glyph (simple checkmark using lines)
        canvas.set_stroke_color(canvas::Color::rgba8(30, 30, 40));
        canvas.set_line_width(2.0f);
        canvas.stroke_line(cx - r * 0.35f, cy, cx - r * 0.05f, cy + r * 0.3f);
        canvas.stroke_line(cx - r * 0.05f, cy + r * 0.3f, cx + r * 0.4f, cy - r * 0.3f);
    } else {
        // Stroked circle
        auto border = resolve_color("control.border", canvas::Color::rgba8(80, 80, 100));
        canvas.set_stroke_color(border);
        canvas.set_line_width(1.5f);
        canvas.stroke_rounded_rect(cx - r, cy - r, r * 2, r * 2, r);
    }
}

void Checkbox::on_mouse_down(Point) {
    checked_ = !checked_;
    if (on_change) on_change(checked_);
}

// ── ToggleButton ────────────────────────────────────────────────────────────

void ToggleButton::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    auto bg = on_ ? resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255))
                  : resolve_color("bg.surface", canvas::Color::rgba8(50, 50, 60));
    auto border = resolve_color("control.border", canvas::Color::rgba8(80, 80, 100));

    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 6);
    if (!on_) {
        canvas.set_stroke_color(border);
        canvas.set_line_width(1);
        canvas.stroke_rounded_rect(0, 0, b.width, b.height, 6);
    }

    if (!label_.empty()) {
        auto text_color = on_ ? canvas::Color::rgba8(255, 255, 255)
                              : resolve_color("text.primary", canvas::Color::rgba8(200, 200, 210));
        canvas.set_fill_color(text_color);
        canvas.set_font("Inter", 13);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text_anchored(label_, b.width * 0.5f, b.height * 0.5f, canvas::Canvas::TextAnchor::GlyphCenter);
    }
}

void ToggleButton::on_mouse_down(Point) {
    on_ = !on_;
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

// Visualizers (ImageView / Meter / XYPad / WaveformView /
// SpectrumView / Panel / SpectrogramView / MultiMeter /
// CorrelationMeter) moved to core/view/src/widgets/visualizers.cpp
// in the 2026-05 Phase 2 (R2-6) batch.

} // namespace pulp::view
