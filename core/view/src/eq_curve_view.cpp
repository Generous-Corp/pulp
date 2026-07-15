#include <pulp/view/eq_curve_view.hpp>
#include <pulp/view/theme_contrast.hpp>
#include <pulp/signal/frequency_response.hpp>
#include <array>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <algorithm>

namespace pulp::view {

namespace {

// Number of points sampled along the curve. One per ~1px column at typical
// widget widths; the curve is redrawn as a polyline through them.
constexpr size_t curve_resolution = 256;

// Bands whose gain parameter does nothing. A low-pass has no gain to set — its
// handle rides the 0 dB line and only frequency (and Q) are draggable.
bool band_has_gain(EqCurveView::FilterType type) {
    using FT = EqCurveView::FilterType;
    return type == FT::peak || type == FT::low_shelf || type == FT::high_shelf;
}

bool band_is_shelf(EqCurveView::FilterType type) {
    using FT = EqCurveView::FilterType;
    return type == FT::low_shelf || type == FT::high_shelf;
}

// Vertical position of a band's handle, in dB — chosen so the dot sits ON the
// band's own curve. A peaking band's curve passes through (freq, gain), so the
// dot is at its gain. A shelf's corner frequency is the half-gain point of the
// transition, so its dot rides the curve at gain/2. Gain-less bands ride 0 dB.
float band_handle_db(const EqCurveView::Band& band) {
    if (!band_has_gain(band.type)) return 0.0f;
    return band_is_shelf(band.type) ? band.gain_db * 0.5f : band.gain_db;
}

// Default band palette — our own hues, spaced around the wheel so adjacent
// bands stay distinct. Used when a Band leaves its color unset.
constexpr std::array<Color, 8> kBandPalette = {{
    Color::rgba8(235, 110, 120),  // rose
    Color::rgba8(240, 165, 90),   // amber
    Color::rgba8(200, 205, 105),  // lime
    Color::rgba8(95, 200, 175),   // teal
    Color::rgba8(100, 175, 235),  // sky
    Color::rgba8(140, 140, 235),  // indigo
    Color::rgba8(190, 130, 225),  // violet
    Color::rgba8(225, 120, 190),  // magenta
}};

Color band_color(const EqCurveView::Band& band, size_t index) {
    return band.color.a > 0 ? band.color : kBandPalette[index % kBandPalette.size()];
}

// Format a frequency tick label the way an audio engineer reads it: bare Hz
// below 1 kHz, "kHz" above, trimming a trailing ".0".
std::string freq_label(float hz) {
    if (hz >= 1000.0f) {
        float k = hz / 1000.0f;
        if (k == std::floor(k)) return std::to_string(static_cast<int>(k)) + "k";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1fk", k);
        return buf;
    }
    return std::to_string(static_cast<int>(hz));
}

// The Biquad filter type a band draws as.
signal::Biquad::Type biquad_type(EqCurveView::FilterType type) {
    using FT = EqCurveView::FilterType;
    using BT = signal::Biquad::Type;
    switch (type) {
        case FT::low_pass:   return BT::lowpass;
        case FT::high_pass:  return BT::highpass;
        case FT::band_pass:  return BT::bandpass;
        case FT::notch:      return BT::notch;
        case FT::low_shelf:  return BT::low_shelf;
        case FT::high_shelf: return BT::high_shelf;
        case FT::peak:       break;
    }
    return BT::peaking;
}

// Design one band into biquad coefficients through the EXACT same call the
// audio path uses (Biquad::set_coefficients), so the drawn curve and the
// processed sound never disagree — including shelves, whose Q the older
// FilterDesign shelf helpers silently dropped (they take a slope S, not Q).
signal::BiquadCoefficients design_band(const EqCurveView::Band& band, float sample_rate) {
    // A filter's response is only defined below Nyquist; the cookbook formulas
    // degenerate at and above it. Clamp so a band dragged to the top of the
    // axis on a 44.1 kHz session still designs to something finite.
    const float f = std::clamp(band.frequency, 1.0f, sample_rate * 0.49f);
    const float q = std::max(band.q, 0.01f);
    signal::Biquad bq;
    bq.set_coefficients(biquad_type(band.type), f, q, sample_rate, band.gain_db);
    return bq.coefficients();
}

} // namespace

EqCurveView::EqCurveView() {
    set_focusable(true);
}

void EqCurveView::rebuild_coefficients() {
    coefficients_.clear();
    coefficients_.reserve(bands_.size());
    for (const auto& band : bands_) {
        if (!band.enabled) continue;
        coefficients_.push_back(design_band(band, sample_rate_));
    }
}

void EqCurveView::set_bands(std::vector<Band> bands) {
    bands_ = std::move(bands);
    rebuild_coefficients();
}

void EqCurveView::set_band(size_t index, const Band& band) {
    if (index < bands_.size()) {
        bands_[index] = band;
        rebuild_coefficients();
    }
}

void EqCurveView::add_band(Band band) {
    bands_.push_back(std::move(band));
    rebuild_coefficients();
}

void EqCurveView::remove_band(size_t index) {
    if (index < bands_.size()) {
        bands_.erase(bands_.begin() + static_cast<ptrdiff_t>(index));
        rebuild_coefficients();
    }
}

void EqCurveView::set_frequency_range(float min_hz, float max_hz) {
    min_freq_ = std::max(1.0f, min_hz);
    max_freq_ = std::max(min_freq_ + 1.0f, max_hz);
}

void EqCurveView::set_gain_range(float min_db, float max_db) {
    min_db_ = min_db;
    max_db_ = std::max(min_db_ + 1.0f, max_db);
}

void EqCurveView::set_sample_rate(float sample_rate) {
    if (!(sample_rate > 0.0f)) return;
    sample_rate_ = sample_rate;
    rebuild_coefficients();
}

float EqCurveView::magnitude_db_at(float freq_hz) const {
    return signal::cascade_magnitude_db(coefficients_, freq_hz, sample_rate_);
}

Color EqCurveView::band_palette_color(size_t index) {
    return kBandPalette[index % kBandPalette.size()];
}

LogFrequencyScale EqCurveView::frequency_scale() const {
    auto b = local_bounds();
    return {min_freq_, max_freq_, b.x, b.width};
}

DecibelScale EqCurveView::gain_scale() const {
    auto b = local_bounds();
    return {min_db_, max_db_, b.y, b.height};
}

void EqCurveView::set_spectrum(const float* magnitudes_db, size_t bin_count) {
    spectrum_.assign(magnitudes_db, magnitudes_db + bin_count);
}

void EqCurveView::clear_spectrum() {
    spectrum_.clear();
}

// The four axis conversions all defer to the shared scales, so anything
// overlaid on this view (a spectrum, a crosshair) lands on the same pixels.
float EqCurveView::freq_to_x(float freq) const { return frequency_scale().to_x(freq); }
float EqCurveView::x_to_freq(float x) const { return frequency_scale().to_frequency(x); }
float EqCurveView::db_to_y(float db) const { return gain_scale().to_y(db); }
float EqCurveView::y_to_db(float y) const { return gain_scale().to_decibels(y); }

int EqCurveView::hit_test_band(Point pos) const {
    constexpr float hit_radius = 10.0f;
    auto b = local_bounds();
    for (size_t i = 0; i < bands_.size(); ++i) {
        float bx = freq_to_x(bands_[i].frequency);
        // Clamp to the viewport so a dot pushed past the top/bottom stops at the
        // edge (half-visible) and stays grabbable, rather than vanishing.
        float by = std::clamp(db_to_y(band_handle_db(bands_[i])), b.y, b.y + b.height);
        float dx = pos.x - bx;
        float dy = pos.y - by;
        if (dx * dx + dy * dy <= hit_radius * hit_radius)
            return static_cast<int>(i);
    }
    return -1;
}

void EqCurveView::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    auto bg = resolve_color("bg.surface", Color::rgba8(30, 30, 40));
    auto grid_color = resolve_color("waveform.grid", Color::rgba8(60, 60, 80));
    auto curve_color = resolve_color("accent.primary", Color::rgba8(100, 180, 255));

    // Background
    canvas.set_fill_color(bg);
    canvas.fill_rect(b.x, b.y, b.width, b.height);

    const auto freq_axis = frequency_scale();
    const auto gain_axis = gain_scale();

    // Grid lines — 1-2-5 per decade, decade boundaries drawn heavier.
    if (show_grid_) {
        canvas.set_stroke_color(grid_color);
        for (float f : freq_axis.ticks()) {
            canvas.set_line_width(LogFrequencyScale::is_major_tick(f) ? 1.0f : 0.5f);
            float x = freq_axis.to_x(f);
            canvas.stroke_line(x, b.y, x, b.y + b.height);
        }
        for (float db : gain_axis.ticks(12.0f)) {
            float y = gain_axis.to_y(db);
            canvas.set_line_width(db == 0.0f ? 1.0f : 0.5f);
            canvas.stroke_line(b.x, y, b.x + b.width, y);
        }
    }

    const float center_y = gain_axis.to_y(0.0f);

    // Per-band curves + translucent fills, each in the band's color. Drawn
    // under the composite so a band's handle sits on its own curve rather than
    // floating off the summed line. Each band's curve is that single filter's
    // response — a peak's bell, a shelf's step — evaluated the same way the
    // composite and the audio are.
    if (show_band_curves_ && b.width > 0.0f) {
        band_db_.resize(curve_resolution);
        for (size_t bi = 0; bi < bands_.size(); ++bi) {
            const auto& band = bands_[bi];
            if (!band.enabled) continue;
            const signal::BiquadCoefficients c = design_band(band, sample_rate_);
            signal::response_curve_db(std::span<const signal::BiquadCoefficients>(&c, 1),
                                      min_freq_, max_freq_, sample_rate_, band_db_);
            const Color col = band_color(band, bi);

            // The band's peak deviation from 0 dB — the anchor for the fill's
            // vertical gradient and how far its color reaches.
            float extreme_db = 0.0f;
            for (size_t i = 0; i < curve_resolution; ++i)
                if (std::abs(band_db_[i]) > std::abs(extreme_db)) extreme_db = band_db_[i];

            // Fill between the curve and the 0 dB line as one polygon.
            fill_poly_.clear();
            fill_poly_.reserve(curve_resolution + 2);
            fill_poly_.push_back({freq_axis.to_x(freq_axis.frequency_at(0, curve_resolution)), center_y});
            for (size_t i = 0; i < curve_resolution; ++i)
                fill_poly_.push_back({freq_axis.to_x(freq_axis.frequency_at(i, curve_resolution)),
                                      gain_axis.to_y(band_db_[i])});
            fill_poly_.push_back({freq_axis.to_x(freq_axis.frequency_at(curve_resolution - 1, curve_resolution)),
                                  center_y});

            // Vertical gradient: transparent at the 0 dB line, the band color at
            // its peak — the depth cue dsssp uses. Skip a flat band (nothing to
            // shade, and a zero-length gradient axis is degenerate).
            if (std::abs(extreme_db) > 0.1f) {
                const float extreme_y = gain_axis.to_y(extreme_db);
                const Color stops[2] = {with_alpha(col, 0.0f), with_alpha(col, 0.34f)};
                const float positions[2] = {0.0f, 1.0f};
                canvas.set_fill_gradient_linear(b.x, center_y, b.x, extreme_y, stops, positions, 2);
                canvas.fill_path(fill_poly_.data(), fill_poly_.size());
            }

            // Stroke the band's own curve.
            canvas.set_stroke_color(with_alpha(col, 0.75f));
            canvas.set_line_width(1.25f);
            float px = freq_axis.to_x(freq_axis.frequency_at(0, curve_resolution));
            float py = gain_axis.to_y(band_db_[0]);
            for (size_t i = 1; i < curve_resolution; ++i) {
                const float x = freq_axis.to_x(freq_axis.frequency_at(i, curve_resolution));
                const float y = gain_axis.to_y(band_db_[i]);
                canvas.stroke_line(px, py, x, y);
                px = x;
                py = y;
            }
        }
    }

    // Spectrum overlay, resampled from linear FFT bins onto the log axis so it
    // sits in the same x-spacing as the curve below and has a value in every
    // pixel column (raw bins leave the bottom decade nearly empty).
    if (!spectrum_.empty() && b.width >= 1.0f) {
        const auto columns = static_cast<size_t>(b.width);
        spectrum_db_.resize(columns);
        resample_spectrum_log(spectrum_, sample_rate_, freq_axis, spectrum_db_);

        canvas.set_fill_color(with_alpha(curve_color, 0.16f));
        const float baseline = gain_axis.to_y(min_db_);
        for (size_t i = 0; i < columns; ++i) {
            const float x = b.x + static_cast<float>(i);
            const float y = gain_axis.to_y(spectrum_db_[i]);
            if (y < baseline) canvas.fill_rect(x, y, 1.0f, baseline - y);
        }
    }

    // Combined response curve — the true magnitude of the band cascade. Drawn
    // even with no enabled bands: an empty cascade is unity gain, so the curve
    // is a flat line at 0 dB. That IS the response, and it is the unity
    // reference a reader looks for — not an empty panel.
    if (b.width > 0.0f) {
        curve_db_.resize(curve_resolution);
        signal::response_curve_db(coefficients_, min_freq_, max_freq_, sample_rate_, curve_db_);

        // Bright, near-white so the summed response reads clearly over the
        // colored per-band curves — the one line that shows the actual output.
        const Color composite_color =
            show_band_curves_ ? Color::rgba8(238, 240, 248) : curve_color;
        canvas.set_stroke_color(composite_color);
        canvas.set_line_width(2.0f);
        float prev_x = freq_axis.to_x(freq_axis.frequency_at(0, curve_resolution));
        float prev_y = gain_axis.to_y(curve_db_[0]);
        for (size_t i = 1; i < curve_resolution; ++i) {
            const float x = freq_axis.to_x(freq_axis.frequency_at(i, curve_resolution));
            const float y = gain_axis.to_y(curve_db_[i]);
            canvas.stroke_line(prev_x, prev_y, x, y);
            prev_x = x;
            prev_y = y;
        }
    }

    // Axis labels — dB up the left edge, frequency along the bottom. Kept
    // subtle so they read as reference, not chrome.
    if (show_labels_) {
        const auto label_color = resolve_color("text.secondary", Color::rgba8(150, 150, 165));
        canvas.set_fill_color(label_color);
        canvas.set_font("Inter", 10.0f);
        for (float db : gain_axis.ticks(12.0f)) {
            const float y = gain_axis.to_y(db);
            const std::string txt = (db > 0 ? "+" : "") + std::to_string(static_cast<int>(db));
            canvas.fill_text_anchored(txt, b.x + 4.0f, y - 2.0f,
                                      canvas::Canvas::TextAnchor::Baseline);
        }
        for (float f : freq_axis.ticks()) {
            if (!LogFrequencyScale::is_major_tick(f)) continue;
            const float x = freq_axis.to_x(f);
            canvas.fill_text_anchored(freq_label(f), x, b.y + b.height - 4.0f,
                                      canvas::Canvas::TextAnchor::GlyphCenter);
        }
    }

    // Band handles — filled in the band's color, white ring, drawn last so they
    // sit above every curve. The active band (dragged, or hovered) grows.
    const int active_band = dragging_band_ >= 0 ? dragging_band_ : hovered_band_;
    for (size_t i = 0; i < bands_.size(); ++i) {
        auto& band = bands_[i];
        if (!band.enabled) continue;

        float hx = freq_to_x(band.frequency);
        float hy = std::clamp(db_to_y(band_handle_db(band)), b.y, b.y + b.height);
        const bool is_active = static_cast<int>(i) == active_band;
        float radius = is_active ? 8.5f : 6.0f;

        canvas.set_fill_color(band_color(band, i));
        canvas.fill_circle(hx, hy, radius);
        canvas.set_stroke_color(Color::rgba8(255, 255, 255));
        canvas.set_line_width(is_active ? 2.0f : 1.5f);
        canvas.stroke_circle(hx, hy, radius);
    }

    // Readout of the active band — the values you're actually setting. Pinned
    // top-left so it never chases the pointer or overlaps a curve.
    if (show_readout_ && active_band >= 0 &&
        active_band < static_cast<int>(bands_.size())) {
        const auto& band = bands_[static_cast<size_t>(active_band)];

        // Fixed-width fields so the line never reflows as values change — each
        // value is padded to its widest form (20.0 kHz / -24.0 dB / Q 10.00) and
        // drawn in a monospace face, so only the digits move. (Monospace gives
        // tabular figures on every backend; the "tnum" font feature does not —
        // it is a no-op on the CoreGraphics path.)
        char fbuf[16], gbuf[16], qbuf[16];
        if (band.frequency >= 1000.0f)
            std::snprintf(fbuf, sizeof(fbuf), "%5.1f kHz", band.frequency / 1000.0f);
        else
            std::snprintf(fbuf, sizeof(fbuf), "%5.0f Hz ", band.frequency);
        if (band_has_gain(band.type))
            std::snprintf(gbuf, sizeof(gbuf), "%+5.1f dB", band.gain_db);
        else
            std::snprintf(gbuf, sizeof(gbuf), "        ");  // reserve the cell
        std::snprintf(qbuf, sizeof(qbuf), "Q %5.2f", band.q);
        const std::string text = std::string(fbuf) + "   " + gbuf + "   " + qbuf;

        canvas.set_font("Menlo", 12.0f);
        canvas.set_fill_color(resolve_color("text.primary", Color::rgba8(235, 238, 245)));
        canvas.fill_text_anchored(text, b.x + 10.0f, b.y + 18.0f,
                                  canvas::Canvas::TextAnchor::Baseline);
    }
}

void EqCurveView::on_mouse_down(Point pos) {
    // Swallow the drag-start press that the platform pairs with a double-click
    // (handled already in on_mouse_event) so it doesn't miss the just-moved
    // handle and clear the selection.
    if (suppress_next_down_) {
        suppress_next_down_ = false;
        return;
    }
    int hit = hit_test_band(pos);
    selected_band_ = hit;
    if (hit >= 0) {
        dragging_band_ = hit;
        drag_start_ = pos;
        if (on_band_selected) on_band_selected(static_cast<size_t>(hit));
    }
}

void EqCurveView::on_mouse_drag(Point pos) {
    if (dragging_band_ >= 0 && dragging_band_ < static_cast<int>(bands_.size())) {
        auto& band = bands_[static_cast<size_t>(dragging_band_)];
        band.frequency = std::clamp(x_to_freq(pos.x), min_freq_, max_freq_);
        // Only bands that have a gain follow the pointer vertically. Dragging a
        // low-pass up and down would otherwise write a gain that changes neither
        // the audio nor the curve, leaving the handle stranded off its own line.
        if (band_has_gain(band.type)) {
            // The handle rides the curve: for a shelf it sits at gain/2, so to
            // keep the dot under the pointer the gain moves at twice the rate.
            float target_db = y_to_db(pos.y);
            if (band_is_shelf(band.type)) target_db *= 2.0f;
            band.gain_db = std::clamp(target_db, min_db_, max_db_);
        }
        rebuild_coefficients();
        if (on_band_changed) on_band_changed(static_cast<size_t>(dragging_band_), band);
    }
}

void EqCurveView::on_mouse_event(const MouseEvent& event) {
    // Scroll wheel over a band adjusts its Q — the width of a peak, the slope of
    // a shelf. Multiplicative so it feels even across the range, and clamped to
    // the same bounds a drag respects.
    if (event.is_wheel) {
        const int band = hit_test_band(event.position);
        if (band >= 0) {
            auto& b = bands_[static_cast<size_t>(band)];
            const float factor = event.scroll_delta_y > 0 ? 1.0f / 1.1f : 1.1f;
            b.q = std::clamp(b.q * factor, 0.1f, 12.0f);
            rebuild_coefficients();
            selected_band_ = band;
            hovered_band_ = band;
            if (on_band_changed) on_band_changed(static_cast<size_t>(band), b);
        }
        return;
    }

    // Double-click a handle to reset it to flat (0 dB), the quick "undo my
    // move" every EQ has. Q is left as-is — the width is usually intentional.
    // The platform dispatches a press through on_mouse_event AND on_mouse_down
    // for the same click, so suppress the drag-start that would otherwise fire
    // right after (and, because the handle just moved to 0 dB, miss and
    // deselect the band).
    if (event.is_down && event.click_count >= 2) {
        const int band = hit_test_band(event.position);
        if (band >= 0 && band_has_gain(bands_[static_cast<size_t>(band)].type)) {
            bands_[static_cast<size_t>(band)].gain_db = 0.0f;
            rebuild_coefficients();
            selected_band_ = band;
            suppress_next_down_ = true;
            if (on_band_changed)
                on_band_changed(static_cast<size_t>(band), bands_[static_cast<size_t>(band)]);
            return;
        }
    }

    // Track the band under the pointer so paint() can highlight it and show its
    // readout even before a drag starts.
    if (!event.is_down && dragging_band_ < 0)
        hovered_band_ = hit_test_band(event.position);

    View::on_mouse_event(event);
}

void EqCurveView::on_hover_move(Point local_pos) {
    if (dragging_band_ < 0) hovered_band_ = hit_test_band(local_pos);
}

void EqCurveView::on_mouse_leave() { hovered_band_ = -1; }

void EqCurveView::on_mouse_up(Point pos) {
    (void)pos;
    dragging_band_ = -1;
}

} // namespace pulp::view
