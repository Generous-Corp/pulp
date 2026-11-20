#pragma once

#include <pulp/signal/spectrum_bin_mapping.hpp>
#include <pulp/vellum_audio/audio_display_provider.hpp>
#include <vellum/view/data_display.hpp>
#include <vellum/view/view.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace pulp::vellum_audio {

/// A Vellum view that snapshots one meter channel at the UI tick.
class LevelMeter final : public ::vellum::view::View, public ::vellum::view::LevelDisplay {
  public:
    explicit LevelMeter(AudioDisplayProvider* feed = nullptr) noexcept : feed_(feed) {}
    void set_feed(AudioDisplayProvider* feed, std::size_t channel) noexcept;
    /// Switches channel and resets the sampling cursor and displayed state,
    /// exactly as set_feed does, so the next sample() reads the new channel.
    void set_channel(std::size_t channel) noexcept {
        set_feed(feed_, channel);
    }
    void sample() noexcept;
    void paint(::vellum::canvas::Canvas& canvas) override;

    void set_levels(float primary, float secondary) override;
    void set_level_orientation(Orientation orientation) override {
        orientation_ = orientation;
    }
    void set_level_fill(std::optional<::vellum::canvas::Color> background,
                        std::vector<::vellum::canvas::Color> gradient) override;
    void clear_level_fill() override;
    void set_level_bar_ratio(float ratio) override {
        bar_ratio_ = ratio;
    }
    void set_level_render_style(::vellum::view::WidgetRenderStyle) override {}

    [[nodiscard]] float rms() const noexcept {
        return rms_;
    }
    [[nodiscard]] float peak() const noexcept {
        return peak_;
    }
    [[nodiscard]] bool stale() const noexcept {
        return stale_;
    }
    [[nodiscard]] AudioFrameInfo frame_info() const noexcept {
        return info_;
    }

  private:
    AudioDisplayProvider* feed_ = nullptr;
    std::size_t channel_ = 0;
    float rms_ = 0.0f;
    float peak_ = 0.0f;
    float bar_ratio_ = 1.0f;
    Orientation orientation_ = Orientation::vertical;
    ::vellum::canvas::Color background_{0.08f, 0.08f, 0.1f, 1.0f};
    ::vellum::canvas::Color fill_{0.25f, 0.85f, 0.35f, 1.0f};
    AudioFrameInfo info_;
    std::uint32_t last_publish_ = 0;
    std::uint32_t quiet_ticks_ = 0;
    bool stale_ = true;
};

/// A fixed-capacity analyzer scope. Bin mapping is prepared on the UI thread.
class AnalyzerScope final : public ::vellum::view::View, public ::vellum::view::BinDisplay {
  public:
    explicit AnalyzerScope(AudioDisplayProvider* feed = nullptr) noexcept : feed_(feed) {}
    void set_feed(AudioDisplayProvider* feed, std::size_t channel) noexcept;
    /// Switches channel and resets the sampling cursor and displayed state,
    /// exactly as set_feed does, so the next sample() reads the new channel.
    void set_channel(std::size_t channel) noexcept {
        set_feed(feed_, channel);
    }
    void configure_mapping(const pulp::signal::SpectrumBinMappingConfig& config);
    void sample() noexcept;
    void paint(::vellum::canvas::Canvas& canvas) override;
    void set_bins(const float* values, std::size_t count) override;

    [[nodiscard]] std::size_t column_count() const noexcept {
        return column_count_;
    }
    [[nodiscard]] bool stale() const noexcept {
        return stale_;
    }
    [[nodiscard]] std::uint32_t dropped_frames() const noexcept {
        return info_.dropped_frames;
    }
    [[nodiscard]] AudioFrameInfo frame_info() const noexcept {
        return info_;
    }

  private:
    bool map_bins_(const float* values, std::size_t count) noexcept;
    AudioDisplayProvider* feed_ = nullptr;
    std::size_t channel_ = 0;
    pulp::signal::SpectrumBinMapping mapping_;
    std::array<float, AudioDisplayProvider::kMaxScopeSamples> bins_{};
    std::array<float, AudioDisplayProvider::kMaxScopeSamples> columns_{};
    std::size_t column_count_ = 0;   // columns drawn from the latest frame
    std::size_t mapped_columns_ = 0; // configured by configure_mapping; survives staleness
    AudioFrameInfo info_;
    std::uint32_t last_publish_ = 0;
    std::uint32_t quiet_ticks_ = 0;
    bool stale_ = true;
};

} // namespace pulp::vellum_audio
