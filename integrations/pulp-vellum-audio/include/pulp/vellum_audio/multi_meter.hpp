#pragma once

// Multi-channel level meter and stereo correlation meter for Vellum views.
// The level data comes from pulp::signal (MultiChannelMeterData, with
// MultiChannelBallistics smoothing it for display); the widgets only draw it.

#include <pulp/signal/multi_channel_meter.hpp>
#include <vellum/view/view.hpp>

namespace pulp::vellum_audio {

/// Multi-channel level meter with a configurable layout. Supports any channel
/// count up to signal::kMaxMeterChannels (mono through ambisonic).
class MultiMeter final : public ::vellum::view::View {
  public:
    enum class Layout { vertical, horizontal };
    enum class DisplayStyle { continuous, segmented };

    MultiMeter() {
        set_access_role(AccessRole::meter);
    }

    /// Update from multi-channel meter data. Call once per UI frame.
    ///
    /// Returns true when a displayed level moved, i.e. when this frame would
    /// differ from the last. A meter polled through silence has nothing new to
    /// show, and requesting a composite anyway pins the host's render loop at
    /// the display refresh rate. The caller knows what the meter is part of, so
    /// it owns the invalidation: call request_repaint() when this returns true.
    [[nodiscard]] bool update(const pulp::signal::MultiChannelMeterData& data, float dt);

    void set_layout(Layout layout) {
        layout_ = layout;
        request_repaint();
    }
    Layout layout() const {
        return layout_;
    }

    void set_display_style(DisplayStyle style) {
        display_style_ = style;
        request_repaint();
    }
    DisplayStyle display_style() const {
        return display_style_;
    }

    void set_channel_count(int count);
    int channel_count() const {
        return ballistics_.num_channels;
    }

    /// The smoothed per-channel state paint() draws.
    const pulp::signal::MultiChannelBallistics& ballistics() const {
        return ballistics_;
    }

    void paint(::vellum::canvas::Canvas& canvas) override;

  private:
    Layout layout_ = Layout::vertical;
    DisplayStyle display_style_ = DisplayStyle::continuous;
    pulp::signal::MultiChannelBallistics ballistics_;
};

/// Stereo correlation display (-1 to +1): the phase relationship between the
/// left and right channels.
class CorrelationMeter final : public ::vellum::view::View {
  public:
    CorrelationMeter() {
        set_access_role(AccessRole::meter);
    }

    /// Update with a new correlation value (-1 to +1). Call once per UI frame.
    ///
    /// Returns true when the displayed needle moved; repaint only when it does,
    /// for the reason given on MultiMeter::update. This meter settles at a
    /// non-zero resting value, so it snaps toward its target rather than toward
    /// zero.
    [[nodiscard]] bool update(float correlation, float dt);

    float display_correlation() const {
        return display_correlation_;
    }

    void paint(::vellum::canvas::Canvas& canvas) override;

  private:
    float display_correlation_ = 0.0f;
};

} // namespace pulp::vellum_audio
