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

// ── Left dB-gutter drag-to-zoom (analyzer range) ─────────────────────────────
// Width of the interactive gutter on the LEFT edge, over the dB axis labels.
// Narrow enough that the plot interior (and the band dots) are never captured.
constexpr float kAnalyzerGutterWidth = 28.0f;
// Vertical drag sensitivity: dBFS of floor movement per pixel. ~0.35 sweeps the
// full −40…−100 band in a comfortable ~170 px pull.
constexpr float kAnalyzerDragGain = 0.35f;
// Range clamps enforced during the drag so it can never invert or collapse.
constexpr float kAnalyzerTopMax = 20.0f;      // top_db ceiling (+20 dBFS)
constexpr float kAnalyzerBottomMin = -100.0f; // bottom_db floor (−100 dBFS)
constexpr float kAnalyzerMinSpan = 20.0f;     // minimum visible span (dB)

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
    // Reserve content_top_pad_ pixels at the top: max_db moves down by the
    // padding, the bottom (min_db / frequency labels) stays put.
    const float pad = std::min(content_top_pad_, b.height);
    return {min_db_, max_db_, b.y + pad, b.height - pad};
}

void EqCurveView::set_spectrum(const float* magnitudes_db, size_t bin_count) {
    spectrum_.assign(magnitudes_db, magnitudes_db + bin_count);
    // Temporal smoothing: ease a persistent display buffer toward the incoming
    // frame per-bin — FAST attack (rise) so transients register, SLOW release
    // (fall) so the display flows and stops flickering. The paint path draws
    // from this smoothed buffer, not the raw frame. Reset on a bin-count change.
    constexpr float kSpectrumAttack  = 0.5f;   // rise toward a louder bin
    constexpr float kSpectrumRelease = 0.15f;  // fall toward a quieter bin
    if (spectrum_smoothed_.size() != bin_count) {
        spectrum_smoothed_.assign(magnitudes_db, magnitudes_db + bin_count);
        return;
    }
    for (size_t i = 0; i < bin_count; ++i) {
        const float target = magnitudes_db[i];
        const float a = target > spectrum_smoothed_[i] ? kSpectrumAttack : kSpectrumRelease;
        spectrum_smoothed_[i] += (target - spectrum_smoothed_[i]) * a;
    }
}

void EqCurveView::clear_spectrum() {
    spectrum_.clear();
    spectrum_smoothed_.clear();
}

void EqCurveView::set_analyzer_enabled(bool on) {
    if (analyzer_enabled_ == on) return;
    analyzer_enabled_ = on;
    // Do not clear the spectrum here — a caller may toggle the overlay off and
    // back on without re-feeding. The paint path gates on analyzer_enabled_, and
    // analyzer_animating() (which needs_continuous_frames consults) reads the
    // same flag, so a disabled analyzer neither draws nor spins frames.
    request_repaint();
}

bool EqCurveView::point_in_analyzer_gutter(Point pos) const {
    const auto b = local_bounds();
    const float plot_top = b.y + std::min(content_top_pad_, b.height);
    return pos.x >= b.x && pos.x <= b.x + kAnalyzerGutterWidth &&
           pos.y >= plot_top && pos.y <= b.y + b.height;
}

// ── Analyzer dBFS scale ─────────────────────────────────────────────────────
// A dedicated scale spanning the full plotting height (top_db at the top,
// bottom_db at the bottom), decoupled from the ±gain axis, so the spectrum
// envelope fills the whole graph rather than being squeezed onto the gain axis.
float EqCurveView::analyzer_db_to_y(float dbfs) const {
    const auto b = local_bounds();
    const float plot_top = b.y + std::min(content_top_pad_, b.height);
    const float plot_bottom = b.y + b.height;
    const float span = analyzer_bottom_db_ - analyzer_top_db_;  // e.g. -60
    const float t = span != 0.0f ? (dbfs - analyzer_top_db_) / span : 0.0f;
    return plot_top + std::clamp(t, 0.0f, 1.0f) * (plot_bottom - plot_top);
}

// ── StateStore binding ──────────────────────────────────────────────────────
void EqCurveView::store_begin(state::ParamID id) {
    if (store_ && id != 0) store_->begin_gesture(id);
}

void EqCurveView::store_end(state::ParamID id) {
    if (store_ && id != 0) store_->end_gesture(id);
}

void EqCurveView::store_set(state::ParamID id, float value) {
    if (!store_ || id == 0) return;
    // Guard: a set_value fired inline (no host EventLoop) would re-enter our own
    // Direction-B listener; the flag makes that a no-op so we don't re-apply our
    // own edit. It is NOT the loop guard against host writes — the listener path
    // never writes back to the store regardless (see apply_param_from_host).
    writing_to_store_ = true;
    store_->set_value(id, value);
    writing_to_store_ = false;
}

void EqCurveView::apply_param_from_host(state::ParamID id, float value) {
    // Pure widget update: map id → (band, field), write the value into the band,
    // rebuild the drawn curve, and repaint. Never opens a gesture or writes back
    // to the store, so a host-driven change cannot feed back into the record path.
    for (size_t bi = 0; bi < band_params_.size() && bi < bands_.size(); ++bi) {
        const BandParamIds& p = band_params_[bi];
        if (id != 0 && id == p.freq)      bands_[bi].frequency = value;
        else if (id != 0 && id == p.gain) bands_[bi].gain_db = value;
        else if (id != 0 && id == p.q)    bands_[bi].q = value;
        else continue;
        rebuild_coefficients();
        request_repaint();
        return;
    }
}

void EqCurveView::bind_bands(state::StateStore& store, std::span<const BandParamIds> ids) {
    store_ = &store;
    band_params_.assign(ids.begin(), ids.end());
    param_listener_tokens_.clear();
    // One Main-thread listener dispatches every bound param. Main so it is fed by
    // the UI-thread pump (pump_listeners) draining host RT writes, never inline
    // on the audio thread.
    param_listener_tokens_.push_back(store.add_listener(
        [this](state::ParamID id, float value) {
            if (writing_to_store_) return;  // our own Direction-A echo
            apply_param_from_host(id, value);
        },
        state::ListenerThread::Main));
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
        // Clamp to the plotting area (respecting the reserved top padding) so a
        // dot pushed past the top/bottom stops at the edge (half-visible) and
        // stays grabbable, rather than vanishing.
        const float top = b.y + std::min(content_top_pad_, b.height);
        float by = std::clamp(db_to_y(band_handle_db(bands_[i])), top, b.y + b.height);
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

    // Spectrum analyzer overlay — a smooth translucent envelope in the accent
    // ("spec") color, drawn UNDER the EQ curve so the white composite line and
    // colored bands stay the hero. The FFT is (a) temporally smoothed in
    // set_spectrum(); here it is (b) resampled from linear bins onto the log
    // axis, (c) frequency-smoothed (~1/12-octave box average) into a clean
    // contour instead of a picket fence, then rendered as (d) a soft vertical
    // gradient fill (dense at the top, fading to nothing at 0 dB) beneath a thin
    // brighter top stroke.
    if (analyzer_enabled_ && !spectrum_smoothed_.empty() && b.width >= 4.0f) {
        const auto columns = static_cast<size_t>(b.width);
        spectrum_db_.resize(columns);
        resample_spectrum_log(spectrum_smoothed_, sample_rate_, freq_axis, spectrum_db_);

        // (c) Frequency smoothing: a box moving average over the log-spaced
        // columns. The 10-octave span maps across b.width px, so ~1/12 octave is
        // about width/120 px; a half-window near that reads as a smooth energy
        // curve. band_db_ is reused as the read-only source (its per-band-curve
        // use above is finished by now).
        const int hw = std::max(2, static_cast<int>(b.width / 120.0f));
        band_db_.assign(spectrum_db_.begin(), spectrum_db_.end());
        for (size_t i = 0; i < columns; ++i) {
            const int lo = std::max(0, static_cast<int>(i) - hw);
            const int hi = std::min(static_cast<int>(columns) - 1, static_cast<int>(i) + hw);
            float acc = 0.0f;
            for (int k = lo; k <= hi; ++k) acc += band_db_[static_cast<size_t>(k)];
            spectrum_db_[i] = acc / static_cast<float>(hi - lo + 1);
        }

        // (d) Build a top-edge polyline sampled every ~3 px, plus baseline anchors
        // at the bottom of the plot, as one closed fill polygon. Points [1 .. n-2]
        // are the envelope; [0] and [n-1] are the baseline anchors. The envelope
        // is mapped through the DEDICATED analyzer dBFS scale (analyzer_db_to_y),
        // which spans the FULL plot height (0 dBFS top → −60 dBFS bottom) — NOT
        // the ±gain axis — so a broadband envelope fills the whole graph like
        // Logic / Pro-Q instead of being crushed onto the gain axis.
        const Color spec = Color::rgba8(0x64, 0xaf, 0xeb);
        const float plot_top = b.y + std::min(content_top_pad_, b.height);
        const float y_base = b.y + b.height;  // bottom of plot = analyzer floor

        fill_poly_.clear();
        fill_poly_.push_back({b.x, y_base});
        constexpr float step = 3.0f;
        for (float fx = 0.0f; fx <= b.width; fx += step) {
            const size_t ci = std::min(columns - 1, static_cast<size_t>(fx));
            const float y = analyzer_db_to_y(spectrum_db_[ci]);
            fill_poly_.push_back({b.x + fx, y});
        }
        fill_poly_.push_back({b.x + b.width, y_base});

        // Fill: vertical gradient, ~0x44 alpha at the top (loud) fading to ~0x06
        // at the bottom (quiet), across the full analyzer height.
        const Color stops[2] = {with_alpha(spec, 0x44 / 255.0f),
                                with_alpha(spec, 0x06 / 255.0f)};
        const float positions[2] = {0.0f, 1.0f};
        canvas.set_fill_gradient_linear(b.x, plot_top, b.x, y_base, stops, positions, 2);
        canvas.fill_path(fill_poly_.data(), fill_poly_.size());

        // Top stroke: a thin brighter accent line along the envelope only (skip
        // the two baseline anchor points), round joins for a smooth contour.
        if (fill_poly_.size() > 3) {
            canvas.set_stroke_color(with_alpha(spec, 0x99 / 255.0f));
            canvas.set_line_width(1.3f);
            canvas.set_line_cap(canvas::LineCap::round);
            canvas.set_line_join(canvas::LineJoin::round);
            canvas.stroke_path(&fill_poly_[1], fill_poly_.size() - 2);
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

    // Left dB-scale drag handle — a subtle vertical grip at the right edge of the
    // gutter, hinting that the dB scale can be dragged to zoom the analyzer range
    // (Logic-style). Drawn faint at rest and brighter while the pointer hovers the
    // gutter or a zoom drag is in flight. Only shown when the analyzer is used and
    // the gutter drag is enabled; a plugin that disables either gets no handle.
    if (analyzer_scale_draggable_ && analyzer_enabled_) {
        const float plot_top = b.y + std::min(content_top_pad_, b.height);
        const float plot_bottom = b.y + b.height;
        const float gx = b.x + kAnalyzerGutterWidth - 3.0f;
        const float gcy = (plot_top + plot_bottom) * 0.5f;
        const float half = 14.0f;
        const Color grip = Color::rgba8(0x64, 0xaf, 0xeb);  // analyzer accent
        const float a = (gutter_hover_ || analyzer_drag_active_) ? 0.55f : 0.20f;
        canvas.set_stroke_color(with_alpha(grip, a));
        canvas.set_line_width(2.0f);
        canvas.set_line_cap(canvas::LineCap::round);
        canvas.stroke_line(gx, gcy - half, gx, gcy + half);
        // Two short grip ticks for texture.
        canvas.set_line_width(1.5f);
        canvas.stroke_line(gx - 4.0f, gcy - 5.0f, gx + 4.0f, gcy - 5.0f);
        canvas.stroke_line(gx - 4.0f, gcy + 5.0f, gx + 4.0f, gcy + 5.0f);
    }

    // Band handles — filled in the band's color, white ring, drawn last so they
    // sit above every curve. The active band (dragged, or hovered) grows.
    const int active_band = dragging_band_ >= 0 ? dragging_band_ : hovered_band_;
    // Per-band eased radius (opt-in). Resize (and mark uninitialized with -1) if
    // the band count changed so the next frame snaps rather than animating from 0.
    if (hover_animation_ && handle_radius_.size() != bands_.size())
        handle_radius_.assign(bands_.size(), -1.0f);
    hover_animating_ = false;
    for (size_t i = 0; i < bands_.size(); ++i) {
        auto& band = bands_[i];
        // A disabled (bypassed) band drops out of the composite; its handle is
        // hidden unless show_disabled_handles_ opts in, in which case it is
        // drawn dimmed and slightly smaller so it stays re-grabbable.
        const bool disabled = !band.enabled;
        if (disabled && !show_disabled_handles_) continue;

        float hx = freq_to_x(band.frequency);
        const float handle_top = b.y + std::min(content_top_pad_, b.height);
        float hy = std::clamp(db_to_y(band_handle_db(band)), handle_top, b.y + b.height);
        const bool is_active = static_cast<int>(i) == active_band;
        const float target_radius = (is_active ? 8.5f : 6.0f) * (disabled ? 0.8f : 1.0f);
        // Ease-out lerp toward the target (never overshoots). ~0.3/frame settles
        // to within ~0.06px in ~10 frames (~160ms @ 60fps) — a subtle settle, not
        // a bounce. Snaps on the first frame (cur == -1) so a fresh band or a
        // band-count change doesn't animate up from nothing.
        float radius = target_radius;
        if (hover_animation_ && i < handle_radius_.size()) {
            float& cur = handle_radius_[i];
            if (cur < 0.0f) {
                cur = target_radius;
            } else {
                cur += (target_radius - cur) * 0.3f;
                if (std::abs(cur - target_radius) < 0.06f) cur = target_radius;
                else hover_animating_ = true;
            }
            radius = cur;
        }

        const float dim = disabled ? 0.32f : 1.0f;
        canvas.set_fill_color(band_color(band, i).with_alpha(dim));
        canvas.fill_circle(hx, hy, radius);
        canvas.set_stroke_color(Color::rgba8(255, 255, 255, disabled ? 90 : 255));
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
    // Left dB-gutter drag zooms the analyzer range. A grabbed dot ALWAYS wins
    // (hit >= 0), so a low-frequency handle sitting over the gutter still drags
    // the band; only an empty press inside the narrow gutter starts a zoom drag.
    if (hit < 0 && analyzer_scale_draggable_ && analyzer_enabled_ &&
        point_in_analyzer_gutter(pos)) {
        analyzer_drag_active_ = true;
        analyzer_drag_start_y_ = pos.y;
        analyzer_drag_start_top_ = analyzer_top_db_;
        analyzer_drag_start_bottom_ = analyzer_bottom_db_;
        request_repaint();
        return;
    }
    selected_band_ = hit;
    if (hit >= 0) {
        dragging_band_ = hit;
        drag_start_ = pos;
        // Direction A: open a gesture for the param(s) this dot drag will change
        // — freq always, gain only for a band that has one — so the whole drag
        // is one grouped, host-recordable edit.
        if (record_gestures_ && store_ &&
            hit < static_cast<int>(band_params_.size())) {
            const BandParamIds& p = band_params_[static_cast<size_t>(hit)];
            store_begin(p.freq);
            drag_freq_gesture_ = p.freq;
            if (band_has_gain(bands_[static_cast<size_t>(hit)].type)) {
                store_begin(p.gain);
                drag_gain_gesture_ = p.gain;
            }
        }
        if (on_band_selected) on_band_selected(static_cast<size_t>(hit));
    }
}

void EqCurveView::on_mouse_drag(Point pos) {
    // Left dB-gutter zoom drag: rescale the analyzer dBFS range. Dragging DOWN
    // (dy > 0) lowers/expands the floor toward −100 dBFS (zoom OUT, more range);
    // dragging UP raises the floor toward −40 dBFS (zoom IN, spectrum fills more).
    // top_db is pinned at its press value (0 by default). Clamped so the range
    // can never invert (top ≤ +20), fall past the floor (bottom ≥ −100), or
    // collapse below the minimum span (≥ 20 dB).
    if (analyzer_drag_active_) {
        const float dy = pos.y - analyzer_drag_start_y_;
        float new_top = std::min(analyzer_drag_start_top_, kAnalyzerTopMax);
        float new_bottom = analyzer_drag_start_bottom_ - dy * kAnalyzerDragGain;
        new_bottom = std::clamp(new_bottom, kAnalyzerBottomMin, new_top - kAnalyzerMinSpan);
        set_analyzer_range(new_top, new_bottom);
        request_repaint();
        return;
    }
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
        // Direction A: publish the changed value(s) inside the open gesture.
        if (record_gestures_ && store_ &&
            dragging_band_ < static_cast<int>(band_params_.size())) {
            const BandParamIds& p = band_params_[static_cast<size_t>(dragging_band_)];
            store_set(p.freq, band.frequency);
            if (band_has_gain(band.type)) store_set(p.gain, band.gain_db);
        }
        if (on_band_changed) on_band_changed(static_cast<size_t>(dragging_band_), band);
    }
}

void EqCurveView::on_mouse_event(const MouseEvent& event) {
    // Scroll wheel over a band adjusts its Q — the width of a peak, the slope of
    // a shelf. Multiplicative so it feels even across the range, and clamped to
    // the same bounds a drag respects. The step scales with the wheel-delta
    // MAGNITUDE (not just its sign): a slow notch nudges Q, a fast trackpad flick
    // moves it further — but the per-event exponent is capped so momentum can
    // never slam Q onto a rail in a single event. scroll_delta_y > 0 (down)
    // narrows toward lower Q, preserving the prior sign convention.
    if (event.is_wheel) {
        const int band = hit_test_band(event.position);
        if (band >= 0) {
            auto& b = bands_[static_cast<size_t>(band)];
            constexpr float kQWheelGain = 0.02f;  // exponent per delta unit
            const float exponent =
                std::clamp(event.scroll_delta_y * kQWheelGain, -0.3f, 0.3f);
            const float factor = std::exp(-exponent);
            b.q = std::clamp(b.q * factor, 0.1f, 12.0f);
            rebuild_coefficients();
            selected_band_ = band;
            hovered_band_ = band;
            // Direction A: a scroll adjusts Q — wrap the single write in a
            // begin/set/end so the host records it as one gesture.
            if (record_gestures_ && store_ &&
                band < static_cast<int>(band_params_.size())) {
                const state::ParamID qid = band_params_[static_cast<size_t>(band)].q;
                store_begin(qid);
                store_set(qid, b.q);
                store_end(qid);
            }
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
            // Direction A: the reset write is one gesture-bracketed edit.
            if (record_gestures_ && store_ &&
                band < static_cast<int>(band_params_.size())) {
                const state::ParamID gid = band_params_[static_cast<size_t>(band)].gain;
                store_begin(gid);
                store_set(gid, 0.0f);
                store_end(gid);
            }
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

void EqCurveView::on_gesture_event(const GestureEvent& event) {
    // Only the continuous (changed) phase carries a scale delta worth applying;
    // begin/end/cancel are boundaries.
    if (event.phase != GesturePhase::changed) return;
    int band = hovered_band_ >= 0 ? hovered_band_ : selected_band_;
    if (band < 0 || band >= static_cast<int>(bands_.size())) return;
    auto& b = bands_[static_cast<size_t>(band)];
    // macOS reports pinch-OUT (fingers apart) as a POSITIVE magnification and
    // pinch-IN as NEGATIVE. Pinch-in should narrow (raise Q), pinch-out widen
    // (lower Q): factor = exp(-delta_scale*k) maps delta<0 → factor>1 (narrower)
    // and delta>0 → factor<1 (wider). Multiplicative for an even feel; clamped to
    // the same bounds a drag/scroll respects.
    constexpr float kPinchGain = 3.0f;
    const float factor = std::exp(-event.delta_scale * kPinchGain);
    b.q = std::clamp(b.q * factor, 0.1f, 12.0f);
    rebuild_coefficients();
    selected_band_ = band;
    hovered_band_ = band;
    // Direction A: a pinch adjusts Q — bracket the write as one gesture.
    if (record_gestures_ && store_ &&
        band < static_cast<int>(band_params_.size())) {
        const state::ParamID qid = band_params_[static_cast<size_t>(band)].q;
        store_begin(qid);
        store_set(qid, b.q);
        store_end(qid);
    }
    if (on_band_changed) on_band_changed(static_cast<size_t>(band), b);
    request_repaint();
}

void EqCurveView::on_hover_move(Point local_pos) {
    if (dragging_band_ < 0) hovered_band_ = hit_test_band(local_pos);
    // Left dB-gutter affordance: an ns-resize cursor + a brighter grip when the
    // pointer is over the drag zone AND not over a dot (a dot's own gesture wins).
    const bool in_gutter = analyzer_scale_draggable_ && analyzer_enabled_ &&
                           hovered_band_ < 0 && point_in_analyzer_gutter(local_pos);
    if (in_gutter != gutter_hover_) {
        gutter_hover_ = in_gutter;
        request_repaint();
    }
    set_cursor(in_gutter ? CursorStyle::vertical_resize : CursorStyle::default_);
}

void EqCurveView::on_mouse_leave() {
    hovered_band_ = -1;
    if (gutter_hover_) {
        gutter_hover_ = false;
        request_repaint();
    }
}

void EqCurveView::on_mouse_up(Point pos) {
    (void)pos;
    // End any in-flight left-gutter zoom drag (harmless when none is active).
    analyzer_drag_active_ = false;
    // Direction A: close whatever gesture(s) the drag opened. store_end no-ops on
    // a zero id, so a non-drag release (no open gesture) is harmless.
    store_end(drag_freq_gesture_);
    store_end(drag_gain_gesture_);
    drag_freq_gesture_ = 0;
    drag_gain_gesture_ = 0;
    dragging_band_ = -1;
}

} // namespace pulp::view
