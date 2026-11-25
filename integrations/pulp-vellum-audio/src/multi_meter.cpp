#include <pulp/vellum_audio/multi_meter.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::vellum_audio {

void MultiMeter::set_channel_count(int count) {
    ballistics_.num_channels = std::clamp(count, 0, pulp::signal::kMaxMeterChannels);
    request_repaint();
}

bool MultiMeter::update(const pulp::signal::MultiChannelMeterData& data, float dt) {
    return ballistics_.update(data, dt);
}

void MultiMeter::paint(::vellum::canvas::Canvas& canvas) {
    auto b = local_bounds();
    int num_ch = ballistics_.num_channels;
    if (num_ch <= 0)
        return;

    if (display_style_ == DisplayStyle::segmented && layout_ == Layout::horizontal) {
        constexpr float kMinDb = -60.0f;
        constexpr float kMaxDb = 6.0f;
        auto level_to_position = [&](float level) {
            if (level <= 0.0f)
                return 0.0f;
            const float db = 20.0f * std::log10(std::max(level, 1.0e-6f));
            return std::clamp((db - kMinDb) / (kMaxDb - kMinDb), 0.0f, 1.0f);
        };
        auto position_to_db = [&](float position) {
            return kMinDb + std::clamp(position, 0.0f, 1.0f) * (kMaxDb - kMinDb);
        };
        auto active_color_for_db = [](float db) {
            if (db >= 0.0f)
                return ::vellum::canvas::Color::rgba8(255, 48, 48);
            if (db >= -6.0f)
                return ::vellum::canvas::Color::rgba8(246, 218, 61);
            return ::vellum::canvas::Color::rgba8(134, 232, 68);
        };
        auto inactive_color_for_db = [](float db) {
            if (db >= 0.0f)
                return ::vellum::canvas::Color::rgba8(66, 16, 18);
            if (db >= -6.0f)
                return ::vellum::canvas::Color::rgba8(62, 48, 17);
            return ::vellum::canvas::Color::rgba8(23, 43, 18);
        };
        auto label_for_channel = [](int ch) -> const char* {
            if (ch == 0)
                return "L";
            if (ch == 1)
                return "R";
            if (ch == 2)
                return "C";
            if (ch == 3)
                return "Ls";
            if (ch == 4)
                return "Rs";
            return "";
        };

        canvas.set_fill_color(
            resolve_color("bg.primary", ::vellum::canvas::Color::rgba8(7, 8, 11)));
        canvas.fill_rounded_rect(0, 0, b.width, b.height, 3.0f);

        const float pad = 4.0f;
        const float label_w = 18.0f;
        const float scale_h = b.height >= 36.0f ? 12.0f : 0.0f;
        const float meter_x = label_w + pad;
        const float meter_w = std::max(1.0f, b.width - meter_x - pad);
        const float row_gap = 3.0f;
        const float row_area_h = std::max(1.0f, b.height - pad * 2.0f - scale_h -
                                                    row_gap * static_cast<float>(num_ch - 1));
        const float row_h = std::max(3.0f, row_area_h / static_cast<float>(num_ch));
        const int segments = meter_w >= 180.0f ? 36 : 24;
        const float segment_gap = 2.0f;
        const float segment_w =
            std::max(2.0f, (meter_w - segment_gap * static_cast<float>(segments - 1)) /
                               static_cast<float>(segments));

        canvas.set_font("Inter", 9.0f);
        for (int ch = 0; ch < num_ch; ++ch) {
            const auto& bc = ballistics_.channels[ch];
            const float y0 = pad + static_cast<float>(ch) * (row_h + row_gap);
            const float y_mid = y0 + row_h * 0.5f;
            const float peak_pos = level_to_position(bc.display_peak);
            const float rms_pos = level_to_position(bc.display_rms);

            canvas.set_fill_color(
                resolve_color("accent.success", ::vellum::canvas::Color::rgba8(134, 232, 68)));
            const char* label = label_for_channel(ch);
            if (label[0] != '\0')
                canvas.fill_text(label, pad, y_mid + 3.0f);

            for (int i = 0; i < segments; ++i) {
                const float segment_pos =
                    (static_cast<float>(i) + 0.5f) / static_cast<float>(segments);
                const float db = position_to_db(segment_pos);
                const bool active = segment_pos <= peak_pos;
                auto color = active ? active_color_for_db(db) : inactive_color_for_db(db);
                if (active && segment_pos > rms_pos)
                    color.a = 175.0f / 255.0f;
                canvas.set_fill_color(color);
                const float x = meter_x + static_cast<float>(i) * (segment_w + segment_gap);
                canvas.fill_rect(x, y0, segment_w, row_h);
            }

            const float held_pos = level_to_position(bc.held_peak);
            if (held_pos > 0.0f) {
                canvas.set_stroke_color(
                    bc.held_peak >= 1.0f
                        ? resolve_color("accent.error", ::vellum::canvas::Color::rgba8(255, 48, 48))
                        : ::vellum::canvas::Color::rgba8(255, 255, 255));
                canvas.set_line_width(1.0f);
                const float x = meter_x + held_pos * meter_w;
                canvas.stroke_line(x, y0 - 1.0f, x, y0 + row_h + 1.0f);
            }

            if (bc.clip_indicator) {
                canvas.set_fill_color(
                    resolve_color("accent.error", ::vellum::canvas::Color::rgba8(255, 48, 48)));
                canvas.fill_rect(b.width - pad - 3.0f, y0, 3.0f, row_h);
            }
        }

        if (scale_h > 0.0f) {
            struct Tick {
                float db;
                const char* label;
            };
            constexpr Tick ticks[] = {
                {-20.0f, "-20"}, {-15.0f, "-15"}, {-10.0f, "-10"}, {-6.0f, "-6"},
                {-3.0f, "-3"},   {0.0f, "0"},     {2.0f, "+2"},    {4.0f, "+4"},
            };
            canvas.set_font("Inter", 8.0f);
            canvas.set_fill_color(
                resolve_color("text.secondary", ::vellum::canvas::Color::rgba8(160, 166, 185)));
            for (const auto& tick : ticks) {
                const float pos = std::clamp((tick.db - kMinDb) / (kMaxDb - kMinDb), 0.0f, 1.0f);
                const float x = meter_x + pos * meter_w;
                canvas.set_stroke_color(::vellum::canvas::Color::rgba8(58, 64, 73));
                canvas.set_line_width(1.0f);
                canvas.stroke_line(x, pad, x, b.height - scale_h - 1.0f);
                canvas.set_fill_color(
                    resolve_color("text.secondary", ::vellum::canvas::Color::rgba8(160, 166, 185)));
                canvas.fill_text(tick.label, x - 6.0f, b.height - 3.0f);
            }
            canvas.fill_text("dB", meter_x - 14.0f, b.height - 3.0f);
        }
        return;
    }

    // Background
    auto bg = resolve_color("control.track", ::vellum::canvas::Color::rgba8(30, 30, 30));
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 2.0f);

    bool vert = (layout_ == Layout::vertical);
    float gap = 2.0f;

    // Calculate per-channel dimensions
    float ch_size;
    if (vert)
        ch_size = (b.width - gap * (num_ch - 1)) / num_ch;
    else
        ch_size = (b.height - gap * (num_ch - 1)) / num_ch;

    for (int ch = 0; ch < num_ch; ++ch) {
        auto& bc = ballistics_.channels[ch];
        float meter_length = vert ? b.height : b.width;

        // Channel position
        float x0, y0, cw, ch_h;
        if (vert) {
            x0 = ch * (ch_size + gap);
            y0 = 0;
            cw = ch_size;
            ch_h = b.height;
        } else {
            x0 = 0;
            y0 = ch * (ch_size + gap);
            cw = b.width;
            ch_h = ch_size;
        }

        // RMS fill
        auto rms_color =
            resolve_color("accent.success", ::vellum::canvas::Color::rgba8(80, 200, 80));
        float rms_level = bc.display_rms;
        if (rms_level > 0.9f)
            rms_color = resolve_color("accent.error", ::vellum::canvas::Color::rgba8(240, 60, 60));
        else if (rms_level > 0.7f)
            rms_color =
                resolve_color("accent.warning", ::vellum::canvas::Color::rgba8(240, 180, 60));

        canvas.set_fill_color(rms_color);
        float fill = rms_level * meter_length;
        if (vert)
            canvas.fill_rect(x0, ch_h - fill, cw, fill);
        else
            canvas.fill_rect(x0, y0, fill, ch_h);

        // Peak indicator
        float peak_level = bc.display_peak;
        if (peak_level > 0.01f) {
            canvas.set_stroke_color(::vellum::canvas::Color::rgba8(255, 255, 255));
            canvas.set_line_width(1.0f);
            float peak_pos = peak_level * meter_length;
            if (vert) {
                float y = ch_h - peak_pos;
                canvas.stroke_line(x0, y, x0 + cw, y);
            } else {
                canvas.stroke_line(peak_pos, y0, peak_pos, y0 + ch_h);
            }
        }

        // Held peak
        float held = bc.held_peak;
        if (held > 0.01f) {
            auto held_color =
                held > 0.9f
                    ? resolve_color("accent.error", ::vellum::canvas::Color::rgba8(255, 50, 50))
                    : ::vellum::canvas::Color::rgba8(255, 100, 100);
            canvas.set_stroke_color(held_color);
            canvas.set_line_width(2.0f);
            float held_pos = held * meter_length;
            if (vert) {
                float y = ch_h - held_pos;
                canvas.stroke_line(x0, y, x0 + cw, y);
            } else {
                canvas.stroke_line(held_pos, y0, held_pos, y0 + ch_h);
            }
        }

        // Clip indicator
        if (bc.clip_indicator) {
            canvas.set_fill_color(
                resolve_color("accent.error", ::vellum::canvas::Color::rgba8(255, 0, 0)));
            if (vert)
                canvas.fill_rect(x0, 0, cw, 3.0f);
            else
                canvas.fill_rect(cw - 3.0f, y0, 3.0f, ch_h);
        }
    }
}

bool CorrelationMeter::update(float correlation, float dt) {
    // A non-finite input would poison the display value and, because NaN
    // compares unequal to itself, report movement on every frame forever.
    const float target = std::isfinite(correlation) ? std::clamp(correlation, -1.0f, 1.0f) : 0.0f;
    if (!std::isfinite(display_correlation_))
        display_correlation_ = 0.0f;

    const float prev = display_correlation_;
    const float coeff = 1.0f - std::exp(-dt / 0.05f); // 50ms smoothing
    display_correlation_ += (target - display_correlation_) * coeff;

    // SNAP TOWARD THE TARGET, NOT TOWARD ZERO. This is an exponential approach,
    // so it closes on its target without ever arriving — unsnapped it moves by a
    // fraction of a bit every frame forever, and a caller gating a repaint on
    // "did it move" would repaint forever with nothing to show.
    //
    // The meters next door snap with a floor test (`if (v < 1e-6f) v = 0`)
    // because a silent meter rests AT zero. Correlation rests wherever the
    // signal is — a steady mono source settles at +1 — so a floor test would
    // never fire and copying it here would do nothing at all. The general form
    // is the one that works for both.
    if (std::abs(display_correlation_ - target) < 1e-6f)
        display_correlation_ = target;

    return display_correlation_ != prev;
}

void CorrelationMeter::paint(::vellum::canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Background
    auto bg = resolve_color("control.track", ::vellum::canvas::Color::rgba8(30, 30, 30));
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 2.0f);

    // Center line (0.0 correlation)
    float center_x = b.width * 0.5f;
    canvas.set_stroke_color(::vellum::canvas::Color::rgba8(80, 80, 80));
    canvas.set_line_width(1.0f);
    canvas.stroke_line(center_x, 0, center_x, b.height);

    // -1 and +1 labels position markers
    float quarter = b.width * 0.25f;
    canvas.set_stroke_color(::vellum::canvas::Color::rgba8(50, 50, 50));
    canvas.stroke_line(quarter, 0, quarter, b.height);
    canvas.stroke_line(b.width - quarter, 0, b.width - quarter, b.height);

    // Correlation indicator
    // Map -1..+1 to 0..width, inset by half bar width to prevent edge clipping
    float norm = (display_correlation_ + 1.0f) * 0.5f; // 0..1
    float bar_width = std::max(4.0f, b.width * 0.02f);
    float usable = b.width - bar_width;
    float indicator_x = bar_width * 0.5f + norm * usable;

    // Color: green at +1, yellow at 0, red at -1
    ::vellum::canvas::Color indicator_color;
    if (display_correlation_ > 0.0f) {
        // Green to yellow
        float t = display_correlation_;
        indicator_color = ::vellum::canvas::Color::rgba8(static_cast<uint8_t>(240 * (1.0f - t)),
                                                         static_cast<uint8_t>(200 + 55 * t),
                                                         static_cast<uint8_t>(60 * (1.0f - t)));
    } else {
        // Yellow to red
        float t = -display_correlation_;
        indicator_color = ::vellum::canvas::Color::rgba8(static_cast<uint8_t>(240),
                                                         static_cast<uint8_t>(200 * (1.0f - t)),
                                                         static_cast<uint8_t>(60 * (1.0f - t)));
    }

    // Draw indicator bar
    canvas.set_fill_color(indicator_color);
    canvas.fill_rounded_rect(indicator_x - bar_width * 0.5f, 1, bar_width, b.height - 2, 2.0f);

    // Fill from center to indicator
    auto fill_color = indicator_color;
    fill_color.a = 80.0f / 255.0f;
    canvas.set_fill_color(fill_color);
    if (indicator_x > center_x) {
        canvas.fill_rect(center_x, 2, indicator_x - center_x, b.height - 4);
    } else {
        canvas.fill_rect(indicator_x, 2, center_x - indicator_x, b.height - 4);
    }
}

} // namespace pulp::vellum_audio
