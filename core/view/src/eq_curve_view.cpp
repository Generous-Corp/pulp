#include <pulp/view/eq_curve_view.hpp>
#include <pulp/view/theme_contrast.hpp>
#include <pulp/signal/filter_design.hpp>
#include <pulp/signal/frequency_response.hpp>
#include <cmath>
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

// Vertical position of a band's handle, in dB. Bands with no gain parameter
// ride the 0 dB line rather than a meaningless stored value.
float band_handle_db(const EqCurveView::Band& band) {
    return band_has_gain(band.type) ? band.gain_db : 0.0f;
}

// Design one band into biquad coefficients — the same coefficients the audio
// path would run for this band.
signal::BiquadCoefficients design_band(const EqCurveView::Band& band, float sample_rate) {
    using FT = EqCurveView::FilterType;
    using FD = signal::FilterDesign;

    // A filter's response is only defined below Nyquist; the cookbook formulas
    // degenerate at and above it. Clamp so a band dragged to the top of the
    // axis on a 44.1 kHz session still designs to something finite.
    const float f = std::clamp(band.frequency, 1.0f, sample_rate * 0.49f);
    const float q = std::max(band.q, 0.01f);
    const float g = band.gain_db;

    switch (band.type) {
        case FT::low_pass:   return FD::lowpass(f, q, sample_rate);
        case FT::high_pass:  return FD::highpass(f, q, sample_rate);
        case FT::band_pass:  return FD::bandpass(f, q, sample_rate);
        case FT::notch:      return FD::notch(f, q, sample_rate);
        case FT::low_shelf:  return FD::low_shelf(f, g, sample_rate);
        case FT::high_shelf: return FD::high_shelf(f, g, sample_rate);
        case FT::peak:       break;
    }
    return FD::peaking_eq(f, q, g, sample_rate);
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
    for (size_t i = 0; i < bands_.size(); ++i) {
        float bx = freq_to_x(bands_[i].frequency);
        float by = db_to_y(band_handle_db(bands_[i]));
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

    // Spectrum overlay, resampled from linear FFT bins onto the log axis so it
    // sits in the same x-spacing as the curve below and has a value in every
    // pixel column (raw bins leave the bottom decade nearly empty).
    if (!spectrum_.empty() && b.width >= 1.0f) {
        const auto columns = static_cast<size_t>(b.width);
        spectrum_db_.resize(columns);
        resample_spectrum_log(spectrum_, sample_rate_, freq_axis, spectrum_db_);

        canvas.set_fill_color(with_alpha(curve_color, 40));
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

        canvas.set_stroke_color(curve_color);
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

    // Band handles
    for (size_t i = 0; i < bands_.size(); ++i) {
        auto& band = bands_[i];
        if (!band.enabled) continue;

        float hx = freq_to_x(band.frequency);
        float hy = db_to_y(band_handle_db(band));
        float radius = (static_cast<int>(i) == selected_band_) ? 8.0f : 6.0f;

        auto handle_color = band.color.a > 0 ? band.color : curve_color;
        canvas.set_fill_color(handle_color);
        canvas.fill_circle(hx, hy, radius);
        canvas.set_stroke_color(Color::rgba8(255, 255, 255));
        canvas.set_line_width(1.5f);
        canvas.stroke_circle(hx, hy, radius);
    }
}

void EqCurveView::on_mouse_down(Point pos) {
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
        if (band_has_gain(band.type))
            band.gain_db = std::clamp(y_to_db(pos.y), min_db_, max_db_);
        rebuild_coefficients();
        if (on_band_changed) on_band_changed(static_cast<size_t>(dragging_band_), band);
    }
}

void EqCurveView::on_mouse_event(const MouseEvent& event) {
    View::on_mouse_event(event);
}

void EqCurveView::on_mouse_up(Point pos) {
    (void)pos;
    dragging_band_ = -1;
}

} // namespace pulp::view
