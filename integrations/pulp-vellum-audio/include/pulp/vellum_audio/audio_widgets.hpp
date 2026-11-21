#pragma once

#include <pulp/signal/spectrum_bin_mapping.hpp>
#include <pulp/vellum_audio/audio_display_types.hpp>
#include <pulp/vellum_audio/audio_widget_registration.hpp>
#include <vellum/view/accessibility.hpp>
#include <vellum/view/data_display.hpp>
#include <vellum/view/view.hpp>
#include <vellum/value/value_feed.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace pulp::dataplane {
struct MeterBallistics;
} // namespace pulp::dataplane

namespace pulp::vellum_audio {

/// A Vellum view that snapshots one meter channel at the UI tick.
class LevelMeter final : public ::vellum::view::View,
                         public ::vellum::view::AccessibilityValueInterface,
                         public ::vellum::view::LevelDisplay {
  public:
    explicit LevelMeter(::vellum::value::ValueFeed* feed = nullptr);
    ~LevelMeter() override;
    void set_feed(::vellum::value::ValueFeed* feed, std::size_t channel) noexcept;
    /// Switches channel and resets the sampling cursor and displayed state,
    /// exactly as set_feed does, so the next sample() reads the new channel.
    void set_channel(std::size_t channel) noexcept {
        set_feed(feed_, channel);
    }
    void sample() noexcept;
    void paint(::vellum::canvas::Canvas& canvas) override;

    double get_current_value() const override;
    void set_current_value(double) override {}
    double get_minimum_value() const override {
        return 0.0;
    }
    double get_maximum_value() const override {
        return 1.0;
    }

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
    [[nodiscard]] float displayed_peak() const noexcept;
    [[nodiscard]] float displayed_rms() const noexcept;
    [[nodiscard]] float held_peak() const noexcept;
    [[nodiscard]] ::vellum::canvas::Color level_color(float level) const;
    /// Colour paint() uses for the bar: the explicit set_level_fill() colour
    /// when one is configured, otherwise the threshold colour for @p level.
    [[nodiscard]] ::vellum::canvas::Color bar_color(float level) const;
    [[nodiscard]] bool stale() const noexcept {
        return stale_;
    }
    [[nodiscard]] AudioFrameInfo frame_info() const noexcept {
        return info_;
    }

  private:
    friend void sample_audio_widgets(::vellum::view::View&, ::vellum::value::ValueFeed*);
    ::vellum::value::ValueFeed* feed_ = nullptr;
    std::size_t channel_ = 0;
    float rms_ = 0.0f;
    float peak_ = 0.0f;
    // Behind a pointer so this header does not pull audio_bridge.hpp (and
    // triple_buffer.hpp) into every widget consumer.
    std::unique_ptr<pulp::dataplane::MeterBallistics> ballistics_;
    float bar_ratio_ = 1.0f;
    Orientation orientation_ = Orientation::vertical;
    ::vellum::canvas::Color background_{0.08f, 0.08f, 0.1f, 1.0f};
    // Explicit fill from set_level_fill(); unset means theme threshold colours.
    std::optional<::vellum::canvas::Color> fill_override_;
    AudioFrameInfo info_;
    std::uint32_t last_publish_ = 0;
    std::uint32_t quiet_ticks_ = 0;
    bool stale_ = true;
};

/// Vellum counterpart of Pulp's piano keyboard. Note callbacks are intentionally
/// the same seam used by existing editors to reach Processor note methods.
class MidiKeyboard final : public ::vellum::view::View {
  public:
    MidiKeyboard();
    void set_range(int first_note, int last_note);
    int first_note() const {
        return first_note_;
    }
    int last_note() const {
        return last_note_;
    }
    void note_on(int note, float velocity = 1.0f);
    void note_off(int note);
    void all_notes_off();
    bool is_note_on(int note) const;

    std::function<void(int, float)> on_note_on;
    std::function<void(int)> on_note_off;

    enum class Orientation { horizontal, vertical, vertical_chromatic_rows };
    void set_orientation(Orientation orientation) {
        orientation_ = orientation;
        request_repaint();
    }
    Orientation orientation() const {
        return orientation_;
    }
    void set_show_note_names(bool show) {
        show_names_ = show;
        request_repaint();
    }
    bool show_note_names() const {
        return show_names_;
    }
    void set_highlight_color(::vellum::canvas::Color color) {
        highlight_color_ = color;
        request_repaint();
    }

    void paint(::vellum::canvas::Canvas& canvas) override;
    void on_mouse_down(::vellum::view::Point pos) override;
    void on_mouse_up(::vellum::view::Point pos) override;
    void on_mouse_drag(::vellum::view::Point pos) override;
    void on_mouse_event(const ::vellum::view::MouseEvent& event) override;
    bool wants_mouse_input() const override {
        return true;
    }
    float intrinsic_height() const override {
        return 80.0f;
    }

  private:
    int first_note_ = 36;
    int last_note_ = 96;
    Orientation orientation_ = Orientation::horizontal;
    bool show_names_ = false;
    ::vellum::canvas::Color highlight_color_{0.0f, 0.0f, 0.0f, 0.0f};
    struct NoteState {
        float velocity = 0.0f;
        bool active = false;
    };
    NoteState note_states_[128] = {};
    int mouse_note_ = -1;
    bool is_black_key(int note) const;
    int white_key_count() const;
    float white_key_width() const;
    ::vellum::view::Rect key_rect(int note) const;
    int note_at_position(::vellum::view::Point pos) const;
};

/// A fixed-capacity analyzer scope. Bin mapping is prepared on the UI thread.
class AnalyzerScope final : public ::vellum::view::View, public ::vellum::view::BinDisplay {
  public:
    explicit AnalyzerScope(::vellum::value::ValueFeed* feed = nullptr) noexcept : feed_(feed) {}
    void set_feed(::vellum::value::ValueFeed* feed, std::size_t channel) noexcept;
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
    friend void sample_audio_widgets(::vellum::view::View&, ::vellum::value::ValueFeed*);
    bool map_bins_(const float* values, std::size_t count) noexcept;
    ::vellum::value::ValueFeed* feed_ = nullptr;
    std::size_t channel_ = 0;
    pulp::signal::SpectrumBinMapping mapping_;
    std::array<float, kMaxScopeSamples> bins_{};
    std::array<float, kMaxScopeSamples> columns_{};
    std::size_t column_count_ = 0;   // columns drawn from the latest frame
    std::size_t mapped_columns_ = 0; // configured by configure_mapping; survives staleness
    AudioFrameInfo info_;
    std::uint32_t last_publish_ = 0;
    std::uint32_t quiet_ticks_ = 0;
    bool stale_ = true;
};

} // namespace pulp::vellum_audio
