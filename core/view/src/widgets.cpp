#include <pulp/view/widgets.hpp>
#include <cmath>

namespace pulp::view {

// ── Label ────────────────────────────────────────────────────────────────────

void Label::paint(canvas::Canvas& canvas) {
    auto text_color = resolve_color("text.primary", canvas::Color::rgba(200, 200, 200));
    auto font = resolve_color("font.family"); // won't resolve but that's ok
    (void)font;

    canvas.set_fill_color({text_color.r, text_color.g, text_color.b, text_color.a});
    canvas.set_font("Inter", font_size_);
    canvas.set_text_align(canvas::TextAlign::left);
    canvas.fill_text(text_, 0, bounds().height * 0.5f);
}

// ── Knob ─────────────────────────────────────────────────────────────────────

void Knob::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    float cx = b.width * 0.5f;
    float cy = b.height * 0.5f;
    float radius = std::min(cx, cy) * 0.8f;

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

    // Label below
    if (!label_.empty()) {
        auto text_color = resolve_color("text.secondary", canvas::Color::rgba(150, 150, 150));
        canvas.set_fill_color({text_color.r, text_color.g, text_color.b, text_color.a});
        canvas.set_font("Inter", 10.0f);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text(label_, cx, b.height - 2);
    }

    // Value text in center
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

    // Thumb
    auto thumb_color = resolve_color("control.thumb", canvas::Color::rgba(220, 220, 220));
    canvas.set_fill_color({thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a});

    float thumb_radius = std::min(track_width * 0.35f, 8.0f);
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
            canvas.fill_text(label_, b.width * 0.5f, b.height - 2);
        } else {
            canvas.fill_text(label_, b.width * 0.5f, b.height - 2);
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

    // Track
    auto bg_color = on_
        ? resolve_color("accent.primary", canvas::Color::rgba(100, 150, 255))
        : resolve_color("control.track", canvas::Color::rgba(60, 60, 60));
    canvas.set_fill_color({bg_color.r, bg_color.g, bg_color.b, bg_color.a});
    canvas.fill_rounded_rect(sx, sy, switch_w, switch_h, switch_h * 0.5f);

    // Thumb circle
    auto thumb_color = resolve_color("control.thumb", canvas::Color::rgba(220, 220, 220));
    canvas.set_fill_color({thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a});
    float thumb_r = switch_h * 0.4f;
    float thumb_x = on_ ? sx + switch_w - switch_h * 0.5f : sx + switch_h * 0.5f;
    canvas.fill_circle(thumb_x, sy + switch_h * 0.5f, thumb_r);

    // Label
    if (!label_.empty()) {
        auto text_color = resolve_color("text.secondary", canvas::Color::rgba(150, 150, 150));
        canvas.set_fill_color({text_color.r, text_color.g, text_color.b, text_color.a});
        canvas.set_font("Inter", 10.0f);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text(label_, b.width * 0.5f, b.height - 2);
    }
}

} // namespace pulp::view
