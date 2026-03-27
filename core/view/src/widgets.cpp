#include <pulp/view/widgets.hpp>
#include <pulp/view/animation.hpp>
#include <cmath>

namespace pulp::view {

// ── Knob animation ──────────────────────────────────────────────────────────

void Knob::on_mouse_enter() {
    float dur = resolve_dimension("motion.duration.fast", 0.08f);
    hover_glow_.animate_to(1.0f, dur, easing::ease_out_quad);
}

void Knob::on_mouse_leave() {
    float dur = resolve_dimension("motion.duration.fast", 0.08f);
    hover_glow_.animate_to(0.0f, dur, easing::ease_out_quad);
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
        canvas.fill_text(label_, b.width * 0.5f, b.height - 2);
    }
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
        canvas.fill_text(x_label_, b.width * 0.5f, b.height - 2);
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

    // Waveform
    auto wave_color = resolve_color("accent.primary", canvas::Color::rgba(100, 180, 250));
    canvas.set_stroke_color(wave_color);
    canvas.set_line_width(1.0f);

    float samples_per_pixel = static_cast<float>(samples_.size()) / b.width;

    for (float px = 0; px < b.width - 1; px += 1.0f) {
        size_t idx0 = static_cast<size_t>(px * samples_per_pixel);
        size_t idx1 = static_cast<size_t>((px + 1) * samples_per_pixel);
        if (idx0 >= samples_.size()) break;
        if (idx1 >= samples_.size()) idx1 = samples_.size() - 1;

        // Find min/max in this pixel's range for accurate display
        float min_val = samples_[idx0], max_val = samples_[idx0];
        for (size_t i = idx0; i <= idx1; ++i) {
            min_val = std::min(min_val, samples_[i]);
            max_val = std::max(max_val, samples_[i]);
        }

        float y0 = cy - min_val * cy;
        float y1 = cy - max_val * cy;
        canvas.stroke_line(px, y0, px, y1);
    }
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
