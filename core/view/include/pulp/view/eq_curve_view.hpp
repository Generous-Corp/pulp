#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/graph_scale.hpp>
#include <pulp/signal/biquad.hpp>
#include <vector>
#include <functional>

namespace pulp::view {

// ── EqCurveView ─────────────────────────────────────────────────────────────
// Parametric EQ curve visualization with draggable band handles.
// Each band has frequency, gain, Q, and filter type.
//
// The drawn curve is the true magnitude response of the bands: each band is
// designed into biquad coefficients (FilterDesign) and evaluated via
// signal::response_curve_db. It is the same math the audio path runs, so the
// picture matches what a listener hears — a low-pass rolls off, a notch cuts a
// null, a shelf plateaus.

class EqCurveView : public View {
public:
    enum class FilterType { low_pass, high_pass, band_pass, notch, peak, low_shelf, high_shelf };

    struct Band {
        float frequency = 1000.0f;  // Hz
        float gain_db = 0.0f;       // dB (-24 to +24)
        float q = 1.0f;             // Q factor (0.1 to 30)
        FilterType type = FilterType::peak;
        bool enabled = true;
        Color color{};               // Per-band color (if empty, uses accent)
    };

    EqCurveView();

    // Band management
    void set_bands(std::vector<Band> bands);
    const std::vector<Band>& bands() const { return bands_; }
    void set_band(size_t index, const Band& band);
    void add_band(Band band);
    void remove_band(size_t index);
    size_t band_count() const { return bands_.size(); }

    // Display range
    void set_frequency_range(float min_hz, float max_hz);
    void set_gain_range(float min_db, float max_db);
    float min_frequency() const { return min_freq_; }
    float max_frequency() const { return max_freq_; }
    float min_gain() const { return min_db_; }
    float max_gain() const { return max_db_; }

    // Sample rate the bands are designed against. A digital filter's response
    // depends on it — the same 8 kHz shelf is a gentle tilt at 96 kHz and a
    // near-cliff at 44.1 kHz — so a curve drawn at the wrong rate is the wrong
    // curve. Set it from Processor::prepare().
    void set_sample_rate(float sample_rate);
    float sample_rate() const { return sample_rate_; }

    // Magnitude of the combined band cascade at a frequency, in dB. The value
    // the curve is drawn from; exposed so callers can label a readout or probe
    // the response without re-deriving it.
    float magnitude_db_at(float freq_hz) const;

    // Axes this view plots against — shared with any widget that wants to
    // overlay on it (a spectrum, a crosshair readout) so both agree on where a
    // given frequency and dB value land.
    LogFrequencyScale frequency_scale() const;
    DecibelScale gain_scale() const;

    // Grid lines
    void set_show_grid(bool show) { show_grid_ = show; }
    bool show_grid() const { return show_grid_; }

    // Spectrum analyzer overlay (optional)
    void set_spectrum(const float* magnitudes_db, size_t bin_count);
    void clear_spectrum();

    // Interaction callbacks
    std::function<void(size_t band_index, Band band)> on_band_changed;
    std::function<void(size_t band_index)> on_band_selected;

    // Selected band
    int selected_band() const { return selected_band_; }
    void set_selected_band(int index) { selected_band_ = index; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_mouse_drag(Point pos) override;
    bool wants_mouse_input() const override { return true; }

private:
    std::vector<Band> bands_;
    float min_freq_ = 20.0f;
    float max_freq_ = 20000.0f;
    float min_db_ = -24.0f;
    float max_db_ = 24.0f;
    float sample_rate_ = 48000.0f;
    bool show_grid_ = true;
    std::vector<float> spectrum_;
    int selected_band_ = -1;
    int dragging_band_ = -1;
    Point drag_start_{};

    // Coefficients of the enabled bands, rebuilt only when a band or the
    // sample rate changes — not per frame, and never per curve point.
    std::vector<signal::BiquadCoefficients> coefficients_;
    // Scratch buffers for the drawn curves. Members so paint() reuses the
    // allocation across frames instead of allocating on every repaint.
    mutable std::vector<float> curve_db_;
    mutable std::vector<float> spectrum_db_;

    void rebuild_coefficients();
    float freq_to_x(float freq) const;
    float x_to_freq(float x) const;
    float db_to_y(float db) const;
    float y_to_db(float y) const;
    int hit_test_band(Point pos) const;
};

} // namespace pulp::view
