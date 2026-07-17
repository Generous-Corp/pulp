#pragma once

// Spectral and multi-channel meter visualizers. These are the widgets whose
// declarations need pulp/signal types (SpectrogramBuffer, ColorMapper,
// MultiChannelBallistics), so they live here rather than in widgets.hpp — only
// a TU that actually paints one pays for the core/signal headers. Their
// implementations are in core/view/src/widgets/visualizers.cpp.
//
// widgets.hpp includes this header, so existing consumers keep compiling
// unchanged; new code that only needs a visualizer should include this header
// directly.

#include <pulp/view/view.hpp>
#include <pulp/signal/spectrogram.hpp>
#include <pulp/signal/multi_channel_meter.hpp>

namespace pulp::view {

// ── SpectrogramView ──────────────────────────────────────────────────────────
// Scrolling time-frequency display. Each STFT frame becomes a column of
// colored pixels, scrolling left as new frames arrive.

class SpectrogramView : public View {
public:
    SpectrogramView() = default;

    /// Push a new STFT frame (dB magnitudes) for display.
    void push_spectrum(const float* magnitudes_db, int num_bins);

    /// Configure display dimensions and color mapping.
    void configure(int history_columns, int freq_rows,
                   signal::ColorRamp ramp = signal::ColorRamp::inferno,
                   float min_db = -80.0f, float max_db = 0.0f);

    void set_color_ramp(signal::ColorRamp ramp) { mapper_.set_ramp(ramp); }
    void set_range(float min_db, float max_db) { min_db_ = min_db; max_db_ = max_db; }

    int history_columns() const { return buffer_.width(); }
    int freq_rows() const { return buffer_.height(); }

    void paint(canvas::Canvas& canvas) override;

private:
    signal::SpectrogramBuffer buffer_;
    signal::ColorMapper mapper_{signal::ColorRamp::inferno};
    float min_db_ = -80.0f;
    float max_db_ = 0.0f;
    bool configured_ = false;
};

// ── MultiMeter ──────────────────────────────────────────────────────────────
// Multi-channel level meter with configurable layout. Supports arbitrary
// channel counts (mono through ambisonic). Uses MultiChannelBallistics
// for smooth display.

class MultiMeter : public View {
public:
    enum class Layout { vertical, horizontal };
    enum class DisplayStyle { continuous, segmented };

    MultiMeter() { set_access_role(AccessRole::meter); }

    /// Update from multi-channel meter data. Call once per UI frame.
    void update(const signal::MultiChannelMeterData& data, float dt);

    void set_layout(Layout l) { layout_ = l; }
    Layout layout() const { return layout_; }

    void set_display_style(DisplayStyle s) { display_style_ = s; }
    DisplayStyle display_style() const { return display_style_; }

    void set_channel_count(int count);
    int channel_count() const { return ballistics_.num_channels; }

    /// Access ballistics for testing.
    const signal::MultiChannelBallistics& ballistics() const { return ballistics_; }

    void paint(canvas::Canvas& canvas) override;

private:
    Layout layout_ = Layout::vertical;
    DisplayStyle display_style_ = DisplayStyle::continuous;
    signal::MultiChannelBallistics ballistics_;
};

// ── CorrelationMeter ────────────────────────────────────────────────────────
// Stereo correlation display (-1 to +1). Shows phase relationship between
// left and right channels.

class CorrelationMeter : public View {
public:
    CorrelationMeter() { set_access_role(AccessRole::meter); }

    /// Update with new correlation value (-1 to +1). Call once per UI frame.
    void update(float correlation, float dt);

    float display_correlation() const { return display_correlation_; }

    void paint(canvas::Canvas& canvas) override;

private:
    float display_correlation_ = 0.0f;
    float smoothing_coeff_ = 0.1f; // Exponential smoothing
};

} // namespace pulp::view
