#pragma once

// Scrolling time-frequency display. Each STFT frame becomes a column of
// coloured cells that scroll left as new frames arrive. The dB-to-colour
// mapping and the column history live in pulp::signal (ColorMapper,
// SpectrogramBuffer); the widget only paints them.
//
// Threading: every mutator, including push_spectrum, is UI-thread only.

#include <pulp/signal/spectrogram.hpp>
#include <vellum/view/view.hpp>

namespace pulp::vellum_audio {

class SpectrogramView final : public ::vellum::view::View {
  public:
    SpectrogramView() = default;

    /// Push a new STFT frame (dB magnitudes). Configures a 256-column history
    /// sized to this frame on first use.
    void push_spectrum(const float* magnitudes_db, int num_bins);

    /// Set the history depth, row count, colour ramp and dB range. Clears the
    /// history.
    void configure(int history_columns, int freq_rows,
                   pulp::signal::ColorRamp ramp = pulp::signal::ColorRamp::inferno,
                   float min_db = -80.0f, float max_db = 0.0f);

    void set_color_ramp(pulp::signal::ColorRamp ramp) {
        mapper_.set_ramp(ramp);
        request_repaint();
    }
    void set_range(float min_db, float max_db) {
        min_db_ = min_db;
        max_db_ = max_db;
        request_repaint();
    }

    int history_columns() const {
        return buffer_.width();
    }
    int freq_rows() const {
        return buffer_.height();
    }

    /// The colour history paint() draws.
    const pulp::signal::SpectrogramBuffer& buffer() const {
        return buffer_;
    }

    void paint(::vellum::canvas::Canvas& canvas) override;

  private:
    pulp::signal::SpectrogramBuffer buffer_;
    pulp::signal::ColorMapper mapper_{pulp::signal::ColorRamp::inferno};
    float min_db_ = -80.0f;
    float max_db_ = 0.0f;
    bool configured_ = false;
};

} // namespace pulp::vellum_audio
