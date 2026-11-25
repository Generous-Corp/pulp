#pragma once

// Display-only waveform widgets. WaveformView draws either live samples, a
// decoded pulp::audio::WaveformOverview, or a static oscillator preview;
// WaveformOverviewView draws the min/max peak columns of an overview over a
// caller-set viewport. Peak data and decimation stay in pulp::audio; trim
// handles, fades, loops and playheads belong to WaveformEditor.

#include <pulp/audio/waveform_overview.hpp>
#include <vellum/view/data_display.hpp>
#include <vellum/view/inspector_facts.hpp>
#include <vellum/view/view.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace pulp::vellum_audio {

class WaveformView final : public ::vellum::view::View,
                           public ::vellum::view::InspectorFacts,
                           public ::vellum::view::SeriesDisplay {
  public:
    /// free_run: no triggering. rising_zero / falling_zero: rotate the buffer
    /// so the first matching zero crossing becomes index 0, which gives a
    /// stable picture of a periodic signal.
    enum class TriggerMode { free_run, rising_zero, falling_zero };
    enum class PreviewShape { none, saw, sine, square, triangle };

    WaveformView() = default;

    /// Set waveform data (normalised -1 to 1), applying the trigger mode. If no
    /// crossing is found the buffer is stored as is. Clears any thumbnail.
    void set_data(const float* samples, std::size_t count);
    void set_data(std::vector<float> samples);
    void set_series(const float* values, std::size_t count) override {
        set_data(values, count);
    }
    void set_series_preview(std::string_view token) override {
        set_preview_shape(token);
    }

    /// Read peak pairs from a decoded overview instead of raw samples. The
    /// widget keeps the overview alive while it may paint from it. Setting a
    /// thumbnail drops any raw sample buffer. `channel == UINT32_MAX` folds all
    /// channels into one row.
    void set_thumbnail(std::shared_ptr<const pulp::audio::WaveformOverview> thumb,
                       std::uint32_t channel = static_cast<std::uint32_t>(-1));
    /// Borrowed variant for stack-owned overviews; the caller keeps `thumb`
    /// alive until it is cleared or replaced.
    void set_thumbnail_borrowed(const pulp::audio::WaveformOverview* thumb,
                                std::uint32_t channel = static_cast<std::uint32_t>(-1));
    void clear_thumbnail();

    bool has_thumbnail() const noexcept {
        return thumbnail_ != nullptr;
    }
    const pulp::audio::WaveformOverview* thumbnail() const noexcept {
        return thumbnail_;
    }

    std::size_t sample_count() const {
        return samples_.size();
    }
    /// The samples paint() draws, after trigger rotation.
    const std::vector<float>& samples() const {
        return samples_;
    }

    std::string_view inspector_type_name() const override {
        return "WaveformView";
    }
    void inspector_facts(std::vector<InspectorFacts::Fact>& out) const override;

    /// Static oscillator preview for imported synth-style designs. Live samples
    /// and thumbnails take precedence when present.
    void set_preview_shape(std::string_view shape);
    PreviewShape preview_shape() const {
        return preview_shape_;
    }

    void set_trigger_mode(TriggerMode mode) {
        trigger_mode_ = mode;
    }
    TriggerMode trigger_mode() const {
        return trigger_mode_;
    }

    /// Index of the first crossing of `mode` in the buffer, without mutating
    /// it. Returns 0 for free_run or when no crossing exists.
    static std::size_t find_trigger_index(const float* samples, std::size_t count,
                                          TriggerMode mode);

    void paint(::vellum::canvas::Canvas& canvas) override;

  private:
    void apply_trigger();

    std::vector<float> samples_;
    TriggerMode trigger_mode_ = TriggerMode::free_run;
    PreviewShape preview_shape_ = PreviewShape::none;

    std::shared_ptr<const pulp::audio::WaveformOverview> thumbnail_owner_;
    const pulp::audio::WaveformOverview* thumbnail_ = nullptr;
    std::vector<float> thumbnail_min_max_;
    std::vector<float> thumbnail_envelope_;
    std::uint32_t thumbnail_channel_ = static_cast<std::uint32_t>(-1);
};

/// Renders a pre-decoded min/max peak overview as vertical columns over a
/// source-frame viewport, through plain Canvas verbs (fill_rect and
/// stroke_line). Silence collapses to a one-pixel line on the centre axis.
class WaveformOverviewView final : public ::vellum::view::View {
  public:
    WaveformOverviewView() = default;

    /// Point the view at an overview (borrowed; the caller keeps it alive or
    /// calls clear_overview). Resets the viewport to the full source range.
    void set_overview(const pulp::audio::WaveformOverview* overview,
                      std::uint32_t channel = pulp::audio::WaveformOverview::kAllChannels);
    void clear_overview();

    bool has_overview() const {
        return overview_ != nullptr && !overview_->empty();
    }

    /// Total source frames behind the overview (0 when none is set).
    std::uint64_t source_frames() const {
        return overview_ ? overview_->info().num_source_frames : 0;
    }

    /// Source-frame window [start, end) to display, clamped to the overview's
    /// length. An inverted or empty range paints only the background.
    void set_viewport(std::uint64_t start_frame, std::uint64_t end_frame);
    std::uint64_t viewport_start() const {
        return vp_start_;
    }
    std::uint64_t viewport_end() const {
        return vp_end_;
    }
    void view_all();

    void set_line_color(::vellum::canvas::Color c) {
        line_color_ = c;
        request_repaint();
    }
    void set_background_color(::vellum::canvas::Color c) {
        bg_color_ = c;
        request_repaint();
    }
    ::vellum::canvas::Color line_color() const {
        return line_color_;
    }
    ::vellum::canvas::Color background_color() const {
        return bg_color_;
    }

    /// Folded (min, max) over the source-frame window [f0, f1) at the finest
    /// overview level. False when there is no overview, the window is empty, or
    /// it falls outside the peak table.
    bool column_peak(std::uint64_t f0, std::uint64_t f1, float& out_min, float& out_max) const;

    void paint(::vellum::canvas::Canvas& canvas) override;

  private:
    const pulp::audio::WaveformOverview* overview_ = nullptr;
    std::uint32_t channel_ = pulp::audio::WaveformOverview::kAllChannels;
    std::uint64_t vp_start_ = 0;
    std::uint64_t vp_end_ = 0;
    ::vellum::canvas::Color line_color_ = ::vellum::canvas::Color::rgba8(100, 180, 250);
    ::vellum::canvas::Color bg_color_ = ::vellum::canvas::Color::rgba8(20, 20, 30);
};

} // namespace pulp::vellum_audio
