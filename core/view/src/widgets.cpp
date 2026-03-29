#include <pulp/view/widgets.hpp>
#include <pulp/view/animation.hpp>
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <string>

namespace pulp::view {

// ── Schema renderer — interprets declarative JSON widget definitions ─────────

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
                if (s.back() == '%') return std::stof(s) / 100.0f * std::min(w, h) * 0.5f;
                return std::stof(s);
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

// ── Label ────────────────────────────────────────────────────────────────────

void Label::paint(canvas::Canvas& canvas) {
    if (text_.empty()) return;

    auto text_color = resolve_color("text.primary", canvas::Color::rgba(200, 200, 200));
    canvas.set_fill_color({text_color.r, text_color.g, text_color.b, text_color.a});
    canvas.set_font("Inter", font_size_);

    // Apply text-transform
    std::string display_text = text_;
    if (text_transform_ == TextTransform::uppercase) {
        for (auto& ch : display_text) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    } else if (text_transform_ == TextTransform::lowercase) {
        for (auto& ch : display_text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    } else if (text_transform_ == TextTransform::capitalize) {
        bool cap_next = true;
        for (auto& ch : display_text) {
            if (cap_next && std::isalpha(static_cast<unsigned char>(ch))) {
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                cap_next = false;
            }
            if (ch == ' ') cap_next = true;
        }
    }

    // Text alignment
    float lh = line_height_ > 0 ? line_height_ : font_size_ * 1.4f;
    float baseline_y = bounds().height * 0.5f + font_size_ * 0.35f;

    float x = 0;
    switch (text_align_) {
        case LabelAlign::left:
            canvas.set_text_align(canvas::TextAlign::left);
            break;
        case LabelAlign::center:
            canvas.set_text_align(canvas::TextAlign::center);
            x = bounds().width * 0.5f;
            break;
        case LabelAlign::right:
            canvas.set_text_align(canvas::TextAlign::right);
            x = bounds().width;
            break;
    }

    if (!multi_line_) {
        // Text overflow ellipsis
        if (text_overflow_ellipsis() && text_align_ == LabelAlign::left) {
            float avail = bounds().width;
            float text_w = canvas.measure_text(display_text);
            if (text_w > avail && display_text.size() > 3) {
                std::string truncated = display_text;
                while (truncated.size() > 1) {
                    truncated.pop_back();
                    float w = canvas.measure_text(truncated + "...");
                    if (w <= avail) {
                        canvas.fill_text(truncated + "...", x, baseline_y);
                        goto decoration;
                    }
                }
            }
        }
        canvas.fill_text(display_text, x, baseline_y);
    } else {
        float y = font_size_ * 0.85f;
        size_t pos = 0;
        while (pos < display_text.size()) {
            size_t nl = display_text.find('\n', pos);
            if (nl == std::string::npos) nl = display_text.size();
            canvas.fill_text(display_text.substr(pos, nl - pos), x, y);
            y += lh;
            pos = nl + 1;
        }
    }

    // Text decoration (underline, line-through, overline)
    decoration:
    if (text_decoration_ != TextDecoration::none) {
        auto dec_color = has_decoration_color_ ? decoration_color_ : text_color;
        canvas.set_stroke_color(dec_color);
        canvas.set_line_width(1.0f);
        float text_w = canvas.measure_text(display_text);
        float draw_x = x;
        if (text_align_ == LabelAlign::center) draw_x = x - text_w * 0.5f;
        else if (text_align_ == LabelAlign::right) draw_x = x - text_w;

        if (text_decoration_ == TextDecoration::underline)
            canvas.stroke_line(draw_x, baseline_y + 2, draw_x + text_w, baseline_y + 2);
        else if (text_decoration_ == TextDecoration::line_through)
            canvas.stroke_line(draw_x, baseline_y - font_size_ * 0.2f, draw_x + text_w, baseline_y - font_size_ * 0.2f);
        else if (text_decoration_ == TextDecoration::overline)
            canvas.stroke_line(draw_x, baseline_y - font_size_ * 0.7f, draw_x + text_w, baseline_y - font_size_ * 0.7f);
    }
}

// ── Knob ─────────────────────────────────────────────────────────────────────

void Knob::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    float cx = b.width * 0.5f;
    float cy = b.height * 0.5f;
    float radius = std::min(cx, cy) * 0.8f;

    // ── Declarative schema path: JSON defines appearance as data ──────────
    if (!widget_schema_.empty()) {
        render_schema(canvas, widget_schema_, b.width, b.height, value_, *this);
        // Fall through to draw labels on top
    }
    // ── Custom shader path: replaces body/track/fill, keeps labels/glow ──
    else if (!custom_sksl_.empty()) {
        canvas::Canvas::ShaderUniforms u;
        u.value = value_;
        u.time = 0; // TODO: wire to FrameClock elapsed
        u.accent_color = resolve_color("accent.primary", canvas::Color::rgba(100, 150, 255));
        u.bg_color = resolve_color("bg.primary", canvas::Color::rgba(30, 30, 46));
        u.track_color = resolve_color("control.track", canvas::Color::rgba(60, 60, 60));
        u.fill_color = resolve_color("control.fill", canvas::Color::rgba(100, 150, 255));
        u.thumb_color = resolve_color("control.thumb", canvas::Color::rgba(220, 220, 220));
        canvas.draw_with_sksl(custom_sksl_, 0, 0, b.width, b.height, u);
        // Fall through to draw labels and value text on top of the shader
    } else {
        // ── Default C++ paint path ──────────────────────────────────────

        // Hover glow ring (drawn behind everything)
        float glow = hover_glow_.value();
        if (glow > 0.01f) {
            auto accent = resolve_color("accent.primary", canvas::Color::rgba(100, 150, 255));
            canvas.set_stroke_color(canvas::Color::rgba(accent.r, accent.g, accent.b,
                                    static_cast<uint8_t>(40 * glow)));
            canvas.set_line_width(6.0f);
            canvas.stroke_arc(cx, cy, radius + 2.0f, start_angle, end_angle);
        }

        // Track (background arc)
        auto track_color = resolve_color("control.track", canvas::Color::rgba(60, 60, 60));
        canvas.set_stroke_color({track_color.r, track_color.g, track_color.b, track_color.a});
        canvas.set_line_width(3.0f);
        canvas.set_line_cap(canvas::LineCap::round);
        canvas.stroke_arc(cx, cy, radius, start_angle, end_angle);

        // Value arc
        float value_angle = start_angle + value_ * (end_angle - start_angle);
        auto fill_color = resolve_color("control.fill", canvas::Color::rgba(100, 150, 255));
        canvas.set_stroke_color({fill_color.r, fill_color.g, fill_color.b, fill_color.a});
        canvas.stroke_arc(cx, cy, radius, start_angle, value_angle);

        // Thumb indicator line
        float thumb_x = cx + radius * 0.6f * std::cos(value_angle);
        float thumb_y = cy + radius * 0.6f * std::sin(value_angle);
        float inner_x = cx + radius * 0.3f * std::cos(value_angle);
        float inner_y = cy + radius * 0.3f * std::sin(value_angle);
        auto thumb_color = resolve_color("control.thumb", canvas::Color::rgba(220, 220, 220));
        canvas.set_stroke_color({thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a});
        canvas.set_line_width(2.0f);
        canvas.stroke_line(inner_x, inner_y, thumb_x, thumb_y);
    }

    // Label below (always drawn, even with shader)
    if (!label_.empty()) {
        auto text_color = resolve_color("text.secondary", canvas::Color::rgba(150, 150, 150));
        canvas.set_fill_color({text_color.r, text_color.g, text_color.b, text_color.a});
        canvas.set_font("Inter", 10.0f);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text(label_, cx, b.height - 6);
    }

    // Value text in center (always drawn, even with shader)
    if (format_) {
        auto text_color = resolve_color("text.primary", canvas::Color::rgba(200, 200, 200));
        canvas.set_fill_color({text_color.r, text_color.g, text_color.b, text_color.a});
        canvas.set_font("Inter", 11.0f);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text(format_(value_), cx, cy + 4);
    }
}

// ── Fader ────────────────────────────────────────────────────────────────────

void Fader::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    bool vert = orientation_ == Orientation::vertical;
    float track_length = vert ? b.height : b.width;
    float track_width = vert ? b.width : b.height;

    // Track
    auto track_color = resolve_color("control.track", canvas::Color::rgba(60, 60, 60));
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
    auto fill_color = resolve_color("control.fill", canvas::Color::rgba(100, 150, 255));
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
    auto thumb_color = resolve_color("control.thumb", canvas::Color::rgba(220, 220, 220));
    canvas.set_fill_color({thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a});

    float thumb_radius = std::min(track_width * 0.35f, 8.0f) * hover_thumb_scale_.value();
    if (vert) {
        float thumb_y = track_length - value_ * track_length;
        canvas.fill_circle(b.width * 0.5f, thumb_y, thumb_radius);
    } else {
        float thumb_x = value_ * track_length;
        canvas.fill_circle(thumb_x, b.height * 0.5f, thumb_radius);
    }

    // Label
    if (!label_.empty()) {
        auto text_color = resolve_color("text.secondary", canvas::Color::rgba(150, 150, 150));
        canvas.set_fill_color({text_color.r, text_color.g, text_color.b, text_color.a});
        canvas.set_font("Inter", 10.0f);
        canvas.set_text_align(canvas::TextAlign::center);
        if (vert) {
            canvas.fill_text(label_, b.width * 0.5f, b.height - 6);
        } else {
            canvas.fill_text(label_, b.width * 0.5f, b.height - 6);
        }
    }
}

// ── Toggle ───────────────────────────────────────────────────────────────────

void Toggle::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    float switch_w = std::min(b.width, 40.0f);
    float switch_h = std::min(b.height * 0.6f, 20.0f);
    float sx = (b.width - switch_w) * 0.5f;
    float sy = (b.height - switch_h) * 0.5f;

    // Track — blend color based on animated thumb position
    float t = thumb_position_.value();
    auto on_color = resolve_color("accent.primary", canvas::Color::rgba(100, 150, 255));
    auto off_color = resolve_color("control.track", canvas::Color::rgba(60, 60, 60));
    auto bg_color = canvas::Color::rgba(
        static_cast<uint8_t>(off_color.r + (on_color.r - off_color.r) * t),
        static_cast<uint8_t>(off_color.g + (on_color.g - off_color.g) * t),
        static_cast<uint8_t>(off_color.b + (on_color.b - off_color.b) * t),
        255);
    canvas.set_fill_color(bg_color);
    canvas.fill_rounded_rect(sx, sy, switch_w, switch_h, switch_h * 0.5f);

    // Hover highlight on track
    float hov = hover_opacity_.value();
    if (hov > 0.01f) {
        canvas.set_fill_color(canvas::Color::rgba(255, 255, 255, static_cast<uint8_t>(15 * hov)));
        canvas.fill_rounded_rect(sx, sy, switch_w, switch_h, switch_h * 0.5f);
    }

    // Thumb circle — position animated between off and on
    auto thumb_color = resolve_color("control.thumb", canvas::Color::rgba(220, 220, 220));
    canvas.set_fill_color({thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a});
    float thumb_r = switch_h * 0.4f;
    float off_x = sx + switch_h * 0.5f;
    float on_x = sx + switch_w - switch_h * 0.5f;
    float thumb_x = off_x + (on_x - off_x) * t;
    canvas.fill_circle(thumb_x, sy + switch_h * 0.5f, thumb_r);

    // Label
    if (!label_.empty()) {
        auto text_color = resolve_color("text.secondary", canvas::Color::rgba(150, 150, 150));
        canvas.set_fill_color({text_color.r, text_color.g, text_color.b, text_color.a});
        canvas.set_font("Inter", 10.0f);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text(label_, b.width * 0.5f, b.height - 6);
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
        auto fill = resolve_color("control.fill", canvas::Color::rgba(100, 150, 255));
        canvas.set_fill_color(fill);
        canvas.fill_rounded_rect(cx - r, cy - r, r * 2, r * 2, r);
        // Check glyph (simple checkmark using lines)
        canvas.set_stroke_color(canvas::Color::rgba(30, 30, 40));
        canvas.set_line_width(2.0f);
        canvas.stroke_line(cx - r * 0.35f, cy, cx - r * 0.05f, cy + r * 0.3f);
        canvas.stroke_line(cx - r * 0.05f, cy + r * 0.3f, cx + r * 0.4f, cy - r * 0.3f);
    } else {
        // Stroked circle
        auto border = resolve_color("control.border", canvas::Color::rgba(80, 80, 100));
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

    auto bg = on_ ? resolve_color("accent.primary", canvas::Color::rgba(100, 150, 255))
                  : resolve_color("bg.surface", canvas::Color::rgba(50, 50, 60));
    auto border = resolve_color("control.border", canvas::Color::rgba(80, 80, 100));

    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 6);
    if (!on_) {
        canvas.set_stroke_color(border);
        canvas.set_line_width(1);
        canvas.stroke_rounded_rect(0, 0, b.width, b.height, 6);
    }

    if (!label_.empty()) {
        auto text_color = on_ ? canvas::Color::rgba(255, 255, 255)
                              : resolve_color("text.primary", canvas::Color::rgba(200, 200, 210));
        canvas.set_fill_color(text_color);
        canvas.set_font("Inter", 13);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text(label_, b.width * 0.5f, b.height * 0.5f + 4.5f);
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

    auto color = resolve_color("text.secondary", canvas::Color::rgba(160, 160, 180));
    canvas.set_stroke_color(color);
    canvas.set_line_width(1.5f);

    switch (type_) {
        case Type::image_upload: {
            // Landscape image icon: rounded rect with mountain + sun
            float r = s * 1.0f;
            canvas.stroke_rounded_rect(cx - r, cy - r, r * 2, r * 2, r * 0.25f);
            // Mountain (two triangles forming landscape)
            float base_y = cy + r * 0.55f;
            // Small mountain (left)
            canvas.stroke_line(cx - r * 0.5f, base_y, cx - r * 0.15f, cy + r * 0.05f);
            canvas.stroke_line(cx - r * 0.15f, cy + r * 0.05f, cx + r * 0.15f, base_y);
            // Large mountain (right, overlapping)
            canvas.stroke_line(cx - r * 0.1f, base_y, cx + r * 0.25f, cy - r * 0.25f);
            canvas.stroke_line(cx + r * 0.25f, cy - r * 0.25f, cx + r * 0.6f, base_y);
            // Sun circle (upper right)
            canvas.stroke_circle(cx + r * 0.4f, cy - r * 0.4f, r * 0.18f);
            break;
        }
        case Type::send: {
            // Paper plane icon pointing upper-right (matching original HTML)
            float ps = s * 1.0f;
            canvas.set_stroke_color(canvas::Color::rgba(255, 255, 255));
            canvas.set_line_width(1.5f);
            // Triangle body pointing upper-right
            float tx = cx + ps * 0.6f, ty = cy - ps * 0.6f;  // tip (upper right)
            float bl = cx - ps * 0.6f, bt = cy + ps * 0.1f;   // bottom left
            float br = cx - ps * 0.1f, bb = cy + ps * 0.6f;   // bottom right
            canvas.stroke_line(tx, ty, bl, bt);     // tip to bottom-left
            canvas.stroke_line(bl, bt, br, bb);     // bottom-left to bottom-right
            canvas.stroke_line(br, bb, tx, ty);     // bottom-right to tip
            // Inner fold
            canvas.stroke_line(bl, bt, cx + ps * 0.1f, cy + ps * 0.1f);
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

// ── ImageView ────────────────────────────────────────────────────────────────

void ImageView::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    if (path_.empty()) {
        // Placeholder: gray rect with "IMG" text
        canvas.set_fill_color(resolve_color("bg.surface", canvas::Color::rgba(50, 50, 60)));
        canvas.fill_rounded_rect(0, 0, b.width, b.height, 4);
        canvas.set_fill_color(resolve_color("text.secondary", canvas::Color::rgba(120, 120, 140)));
        canvas.set_font("Inter", 10);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text("IMG", b.width * 0.5f, b.height * 0.5f + 3);
        return;
    }

    // TODO: Load image via Skia SkData::MakeFromFileName + SkImages::DeferredFromEncodedData
    // For now render path as text placeholder
    canvas.set_fill_color(resolve_color("bg.surface", canvas::Color::rgba(50, 50, 60)));
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 4);
    canvas.set_fill_color(resolve_color("text.secondary", canvas::Color::rgba(120, 120, 140)));
    canvas.set_font("Inter", 9);
    canvas.set_text_align(canvas::TextAlign::center);

    // Show filename
    auto name = path_;
    auto slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    canvas.fill_text(name, b.width * 0.5f, b.height * 0.5f + 3);
}

// ── Meter ────────────────────────────────────────────────────────────────────

void Meter::set_level(float rms, float peak) {
    current_rms_ = std::clamp(rms, 0.0f, 1.0f);
    current_peak_ = std::clamp(peak, 0.0f, 1.0f);
    ballistics_.display_rms = current_rms_;
    ballistics_.display_peak = current_peak_;
}

void Meter::update(float raw_peak, float raw_rms, float dt) {
    ballistics_.update(raw_peak, raw_rms, dt);
}

void Meter::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    bool vert = orientation_ == Orientation::vertical;

    // Background
    auto bg = resolve_color("control.track", canvas::Color::rgba(30, 30, 30));
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 2.0f);

    float meter_length = vert ? b.height : b.width;

    // RMS fill (main body)
    auto rms_color = resolve_color("accent.success", canvas::Color::rgba(80, 200, 80));
    float rms_level = ballistics_.display_rms;

    // Color changes at different levels
    if (rms_level > 0.9f)
        rms_color = resolve_color("accent.error", canvas::Color::rgba(240, 60, 60));
    else if (rms_level > 0.7f)
        rms_color = resolve_color("accent.warning", canvas::Color::rgba(240, 180, 60));

    canvas.set_fill_color(rms_color);
    float fill = rms_level * meter_length;

    if (vert) {
        canvas.fill_rect(1, b.height - fill, b.width - 2, fill);
    } else {
        canvas.fill_rect(0, 1, fill, b.height - 2);
    }

    // Peak indicator line
    float peak_level = ballistics_.display_peak;
    if (peak_level > 0.01f) {
        auto peak_color = resolve_color("control.thumb", canvas::Color::rgba(255, 255, 255));
        canvas.set_stroke_color(peak_color);
        canvas.set_line_width(1.0f);

        float peak_pos = peak_level * meter_length;
        if (vert) {
            float y = b.height - peak_pos;
            canvas.stroke_line(1, y, b.width - 1, y);
        } else {
            canvas.stroke_line(peak_pos, 1, peak_pos, b.height - 1);
        }
    }

    // Held peak indicator
    float held = ballistics_.held_peak;
    if (held > 0.01f) {
        auto held_color = canvas::Color::rgba(255, 100, 100);
        if (held > 0.9f)
            held_color = resolve_color("accent.error", canvas::Color::rgba(255, 50, 50));

        canvas.set_stroke_color(held_color);
        canvas.set_line_width(2.0f);

        float held_pos = held * meter_length;
        if (vert) {
            float y = b.height - held_pos;
            canvas.stroke_line(0, y, b.width, y);
        } else {
            canvas.stroke_line(held_pos, 0, held_pos, b.height);
        }
    }
}

// ── XYPad ────────────────────────────────────────────────────────────────────

void XYPad::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Background
    auto bg = resolve_color("bg.surface", canvas::Color::rgba(40, 40, 55));
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 4.0f);

    // Grid lines
    auto grid = resolve_color("control.border", canvas::Color::rgba(60, 60, 75));
    canvas.set_stroke_color(grid);
    canvas.set_line_width(0.5f);
    canvas.stroke_line(b.width * 0.5f, 0, b.width * 0.5f, b.height);
    canvas.stroke_line(0, b.height * 0.5f, b.width, b.height * 0.5f);

    // Crosshair position
    float cx = x_ * b.width;
    float cy = (1.0f - y_) * b.height; // Y is inverted (0=bottom, 1=top)

    // Crosshair lines
    auto hair_color = resolve_color("control.fill", canvas::Color::rgba(100, 150, 255));
    canvas.set_stroke_color(hair_color);
    canvas.set_line_width(1.0f);
    canvas.stroke_line(cx, 0, cx, b.height);
    canvas.stroke_line(0, cy, b.width, cy);

    // Thumb dot
    auto thumb = resolve_color("control.thumb", canvas::Color::rgba(220, 220, 220));
    canvas.set_fill_color(thumb);
    canvas.fill_circle(cx, cy, 5.0f);

    // Labels
    auto text_color = resolve_color("text.secondary", canvas::Color::rgba(150, 150, 150));
    canvas.set_fill_color(text_color);
    canvas.set_font("Inter", 9.0f);

    if (!x_label_.empty()) {
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text(x_label_, b.width * 0.5f, b.height - 6);
    }
    if (!y_label_.empty()) {
        canvas.set_text_align(canvas::TextAlign::left);
        canvas.fill_text(y_label_, 2, 10);
    }
}

// ── WaveformView ─────────────────────────────────────────────────────────────

void WaveformView::set_data(const float* samples, size_t count) {
    samples_.assign(samples, samples + count);
}

void WaveformView::set_data(std::vector<float> samples) {
    samples_ = std::move(samples);
}

void WaveformView::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Background
    auto bg = resolve_color("bg.surface", canvas::Color::rgba(30, 30, 40));
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 2.0f);

    if (samples_.empty()) return;

    // Center line
    auto center_color = resolve_color("control.border", canvas::Color::rgba(50, 50, 60));
    canvas.set_stroke_color(center_color);
    canvas.set_line_width(0.5f);
    float cy = b.height * 0.5f;
    canvas.stroke_line(0, cy, b.width, cy);

    // GPU-accelerated waveform via draw_waveform (SDF anti-aliased)
    auto wave_color = resolve_color("accent.primary", canvas::Color::rgba(100, 180, 250));
    canvas::Canvas::WaveformStyle style;
    style.line_color = wave_color;
    style.fill_color = {wave_color.r, wave_color.g, wave_color.b, 40};
    style.line_thickness = 2.0f;
    style.show_fill = true;
    style.fill_center = 0.5f;

    canvas.draw_waveform(samples_.data(), samples_.size(),
                         0, 0, b.width, b.height, style);
}

// ── SpectrumView ─────────────────────────────────────────────────────────────

void SpectrumView::set_spectrum(const float* magnitudes_db, size_t bin_count) {
    bins_.assign(magnitudes_db, magnitudes_db + bin_count);
}

void SpectrumView::set_spectrum(std::vector<float> magnitudes_db) {
    bins_ = std::move(magnitudes_db);
}

void SpectrumView::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Background
    auto bg = resolve_color("bg.surface", canvas::Color::rgba(25, 25, 35));
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 2.0f);

    if (bins_.empty()) return;

    float db_range = max_db_ - min_db_;
    if (db_range <= 0) return;

    auto spectrum_color = resolve_color("accent.primary", canvas::Color::rgba(100, 180, 250));

    if (style_ == Style::bars) {
        canvas.set_fill_color(spectrum_color);
        float bar_width = b.width / static_cast<float>(bins_.size());

        for (size_t i = 0; i < bins_.size(); ++i) {
            float norm = std::clamp((bins_[i] - min_db_) / db_range, 0.0f, 1.0f);
            float bar_h = norm * b.height;
            float x = i * bar_width;
            canvas.fill_rect(x, b.height - bar_h, std::max(bar_width - 1, 1.0f), bar_h);
        }
    } else {
        // Line or filled style
        canvas.set_stroke_color(spectrum_color);
        canvas.set_line_width(1.5f);

        float step = b.width / static_cast<float>(bins_.size() - 1);

        // Draw as connected line segments
        for (size_t i = 0; i + 1 < bins_.size(); ++i) {
            float norm0 = std::clamp((bins_[i] - min_db_) / db_range, 0.0f, 1.0f);
            float norm1 = std::clamp((bins_[i + 1] - min_db_) / db_range, 0.0f, 1.0f);
            float x0 = i * step;
            float x1 = (i + 1) * step;
            float y0 = b.height - norm0 * b.height;
            float y1 = b.height - norm1 * b.height;
            canvas.stroke_line(x0, y0, x1, y1);
        }

        // For filled style, also fill below the line
        if (style_ == Style::filled) {
            auto fill = spectrum_color;
            fill.a = 60;
            canvas.set_fill_color(fill);

            for (size_t i = 0; i < bins_.size(); ++i) {
                float norm = std::clamp((bins_[i] - min_db_) / db_range, 0.0f, 1.0f);
                float x = i * step;
                float y = b.height - norm * b.height;
                float bar_w = step > 1 ? step : 1;
                canvas.fill_rect(x, y, bar_w, b.height - y);
            }
        }
    }

    // Frequency grid lines (approximate positions)
    auto grid = resolve_color("control.border", canvas::Color::rgba(50, 50, 65));
    canvas.set_stroke_color(grid);
    canvas.set_line_width(0.5f);

    // Horizontal dB grid lines at -20, -40, -60
    for (float db : {-20.0f, -40.0f, -60.0f}) {
        float norm = std::clamp((db - min_db_) / db_range, 0.0f, 1.0f);
        float y = b.height - norm * b.height;
        canvas.stroke_line(0, y, b.width, y);
    }
}

// ── Panel ────────────────────────────────────────────────────────────────────

void Panel::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    auto bg = resolve_color(bg_token_, canvas::Color::rgba(45, 45, 60));
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, corner_radius_);

    if (border_width_ > 0) {
        auto border = resolve_color(border_token_, canvas::Color::rgba(80, 80, 100));
        canvas.set_stroke_color(border);
        canvas.set_line_width(border_width_);
        canvas.stroke_rounded_rect(0, 0, b.width, b.height, corner_radius_);
    }
}

} // namespace pulp::view
