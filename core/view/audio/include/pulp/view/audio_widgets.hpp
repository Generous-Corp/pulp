#pragma once

#include <pulp/view/widgets.hpp>
#include <pulp/view/audio_bridge.hpp>
#include <pulp/view/midi_keyboard.hpp>
#include <pulp/view/musical_typing_keyboard.hpp>
#include <pulp/view/waveform_recorder_view.hpp>

#include <pulp/signal/spectrum_analyzer.hpp>
#include <pulp/signal/spectrum_bin_mapping.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <span>

namespace pulp::audio {
class WaveformOverview;
}
namespace pulp::view {
// ── Meter ────────────────────────────────────────────────────────────────────
// Audio level meter with peak hold

class Meter : public View,
              public AccessibilityValueInterface,
              public InspectorFacts,
              public LevelDisplay {
  public:
    enum class Orientation { vertical, horizontal };

    Meter() {
        set_access_role(AccessRole::meter);
    }

    // ── AccessibilityValueInterface (read-only gauge, normalized 0..1) ───
    // `meter` maps to a UIA ProgressBar / AT-SPI LEVEL_BAR, both of which
    // advertise a range. Without a source they report 0 in a 0..0 range.
    double get_current_value() const override {
        return display_peak();
    }
    void set_current_value(double) override {} // a meter is not writable
    double get_minimum_value() const override {
        return 0.0;
    }
    double get_maximum_value() const override {
        return 1.0;
    }

    void set_render_style(WidgetRenderStyle s) {
        render_style_ = s;
    }
    WidgetRenderStyle render_style() const {
        return render_style_;
    }

    // Set levels directly (normalized 0-1)
    void set_level(float rms, float peak);
    void set_levels(float primary, float secondary) override {
        set_level(primary, secondary);
    }
    void set_level_orientation(LevelDisplay::Orientation o) override {
        set_orientation(o == LevelDisplay::Orientation::horizontal ? Orientation::horizontal
                                                                   : Orientation::vertical);
    }
    void set_level_fill(std::optional<canvas::Color> background,
                        std::vector<canvas::Color> gradient) override {
        if (background)
            set_skin_background_color(*background);
        set_skin_gradient(std::move(gradient));
    }
    void clear_level_fill() override {
        clear_skin();
    }
    void set_level_bar_ratio(float ratio) override {
        set_bar_fill_ratio(ratio);
    }
    void set_level_render_style(WidgetRenderStyle s) override {
        set_render_style(s);
    }

    // Update from MeterBallistics (call once per frame with dt)
    void update(float raw_peak, float raw_rms, float dt);

    void set_orientation(Orientation o) {
        orientation_ = o;
    }
    Orientation orientation() const {
        return orientation_;
    }

    // Ballistics accessors for testing
    float display_rms() const {
        return ballistics_.display_rms;
    }
    float display_peak() const {
        return ballistics_.display_peak;
    }

    std::string_view inspector_type_name() const override {
        return "Meter";
    }
    void inspector_facts(std::vector<InspectorFacts::Fact>& out) const override;
    float held_peak() const {
        return ballistics_.held_peak;
    }

    // ── Live host→view source ────────────────────────────────────────────────
    // Bind a lock-free MeterSource whose `channel` this meter reads once per
    // frame, driving the ballistic `update()` path from `on_meter_frame`. The
    // subscription lifecycle (attach, re-point on a clock change, self-healing
    // teardown) and the lifetime contract live on View::set_meter_source; these
    // are the meter-shaped spelling of that seam.
    void set_source(std::shared_ptr<MeterSource> source, int channel = 0) {
        set_meter_source(std::move(source), channel);
    }
    bool has_source() const {
        return has_meter_source();
    }
    int source_channel() const {
        return meter_source_channel();
    }

    void paint(canvas::Canvas& canvas) override;

    // ── Skin overrides ────────────────────────────────────────────────────
    // Per-widget gradient + background, generalising the knob sprite-strip
    // path to the meter. The importer samples the captured Figma PNG's
    // gradient (green→orange→red) and hands the stops here; the meter redraws
    // that gradient procedurally, CLIPPED to the current level, so the fill
    // still animates with set_level()/update() instead of freezing the
    // captured image. Stops are ordered low→high (bottom→top for vertical).
    // Unset → today's theme-token threshold behavior (back-compat).
    void set_skin_gradient(std::vector<canvas::Color> stops) {
        gradient_stops_ = std::move(stops);
        request_repaint();
    }
    void set_skin_background_color(canvas::Color c) {
        background_color_ = c;
        has_skin_background_ = true;
        request_repaint();
    }
    void clear_skin() {
        gradient_stops_.clear();
        has_skin_background_ = false;
        bar_fill_ratio_ = 1.0f;
        request_repaint();
    }
    // Fraction of the widget's cross-axis width occupied by the colored bar
    // (0..1). The captured meter draws a narrow colored fill recessed inside a
    // wider dark housing slot; this ratio reproduces that inset so the rendered
    // bar isn't edge-to-edge paint. Derived from the captured asset
    // (colored-bar width / housing width); defaults to 1.0 (full width) when the
    // importer didn't supply it.
    void set_bar_fill_ratio(float r) {
        bar_fill_ratio_ = std::clamp(r, 0.05f, 1.0f);
        request_repaint();
    }
    float bar_fill_ratio() const {
        return bar_fill_ratio_;
    }
    bool has_skin_gradient() const {
        return gradient_stops_.size() >= 2;
    }
    bool has_skin_background_color() const {
        return has_skin_background_;
    }
    const std::vector<canvas::Color>& skin_gradient() const {
        return gradient_stops_;
    }
    canvas::Color skin_background_color() const {
        return background_color_;
    }

    // Sample the skin gradient at normalized position t (0=low/bottom,
    // 1=high/top). Linear interpolation across the supplied stops.
    //
    // Nothing when no stops were set: a meter with no gradient has no colour
    // to give, and the caller must draw nothing rather than receive one. This
    // used to answer a built-in green, which is a colour from nowhere on any
    // design that never chose it — and returning a transparent Color instead
    // would be no safer, since a caller that ignores alpha paints it black.
    std::optional<canvas::Color> gradient_color_at(float t) const;

  private:
    // Drive the ballistic path from the frame the View base snapshotted for us.
    void on_meter_frame(const MeterFrame& frame, float dt) override;
    /// update() already invalidates the meter's own rect whenever its
    /// ballistics move, which a changed snapshot alone does not imply.
    void on_meter_snapshot_changed() override {}

    Orientation orientation_ = Orientation::vertical;
    MeterBallistics ballistics_;
    float current_rms_ = 0;
    float current_peak_ = 0;
    WidgetRenderStyle render_style_ = WidgetRenderStyle::standard;
    std::vector<canvas::Color> gradient_stops_;
    canvas::Color background_color_{};
    bool has_skin_background_ = false;
    float bar_fill_ratio_ = 1.0f;
};

// ── WaveformView ─────────────────────────────────────────────────────────────
// Displays audio waveform data
//
// Two backing sources are supported:
//
//   set_data(...)       — raw normalized samples, redrawn directly. The
//                         existing behavior, used by oscilloscope-style
//                         displays of live audio.
//   set_thumbnail(...)  — points the widget at a pre-decoded
//                         `pulp::audio::WaveformOverview`. Paint reads peak
//                         pairs from the cached level instead of decoding
//                         per redraw, wiring WaveformOverview into the existing
//                         WaveformView without inventing a new widget.
//
// This remains a display-only waveform widget. Sampler/editor behaviors
// such as trim handles, fade curves, loop regions, slice markers, playheads,
// hit testing, and snapping should live in dedicated editor/viewport modules
// that compose this renderer instead of expanding WaveformView itself.

} // namespace pulp::view
namespace pulp::audio {
class WaveformOverview;
}
namespace pulp::view {

class WaveformView : public View, public InspectorFacts, public SeriesDisplay {
  public:
    // Triggering mode for periodic signal display.
    // free_run:     no triggering — caller is responsible for continuity
    // rising_zero:  align to the first rising-edge zero crossing
    // falling_zero: align to the first falling-edge zero crossing
    enum class TriggerMode { free_run, rising_zero, falling_zero };
    enum class PreviewShape { none, saw, sine, square, triangle };

    WaveformView() = default;

    // Set waveform data (normalized -1 to 1). If a non-free_run trigger
    // mode is active, the buffer is rotated so the first matching
    // crossing becomes index 0, producing a stable display for periodic
    // signals. If no crossing is found, the buffer is stored as-is.
    //
    // Calling set_data clears any active thumbnail source.
    void set_data(const float* samples, size_t count);
    void set_data(std::vector<float> samples);
    void set_series(const float* values, std::size_t count) override {
        set_data(values, count);
    }
    void set_series_preview(std::string_view token) override {
        set_preview_shape(token);
    }

    // Wire the widget to read peak (min, max) pairs from a pre-decoded
    // `WaveformOverview`. The shared_ptr overload is the
    // editor/cache-safe path: the widget retains the CPU thumbnail summary
    // while it may paint from it. Setting a thumbnail clears any raw sample
    // buffer.
    //
    // `channel == UINT32_MAX` folds all channels into one display row.
    void set_thumbnail(std::shared_ptr<const pulp::audio::WaveformOverview> thumb,
                       uint32_t channel = static_cast<uint32_t>(-1));
    [[deprecated("set_thumbnail(const WaveformOverview*) is borrowed; prefer shared_ptr overload "
                 "or set_thumbnail_borrowed()")]]
    void set_thumbnail(const pulp::audio::WaveformOverview* thumb,
                       uint32_t channel = static_cast<uint32_t>(-1));
    // Explicit borrowed escape hatch for stack-owned tests or owners that
    // manage lifetime externally. Prefer set_thumbnail(shared_ptr) for editor
    // code and cache-backed thumbnails.
    void set_thumbnail_borrowed(const pulp::audio::WaveformOverview* thumb,
                                uint32_t channel = static_cast<uint32_t>(-1));
    void clear_thumbnail();

    bool has_thumbnail() const noexcept {
        return thumbnail_ != nullptr;
    }
    const pulp::audio::WaveformOverview* thumbnail() const noexcept {
        return thumbnail_;
    }

    size_t sample_count() const {
        return samples_.size();
    }

    std::string_view inspector_type_name() const override {
        return "WaveformView";
    }
    void inspector_facts(std::vector<InspectorFacts::Fact>& out) const override;

    // Static oscillator-preview lane for imported synth-style designs.
    // Live sample data and thumbnails still take precedence when present.
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

    // Locate the first crossing of `mode` in the buffer without mutating
    // it. Returns the crossing index, or 0 if none is found or
    // mode is free_run.
    static size_t find_trigger_index(const float* samples, size_t count, TriggerMode mode);

    void paint(canvas::Canvas& canvas) override;

  private:
    void apply_trigger();

    std::vector<float> samples_;
    TriggerMode trigger_mode_ = TriggerMode::free_run;
    PreviewShape preview_shape_ = PreviewShape::none;

    std::shared_ptr<const pulp::audio::WaveformOverview> thumbnail_owner_;
    const pulp::audio::WaveformOverview* thumbnail_ = nullptr;
    std::vector<float> thumbnail_min_max_;
    std::vector<float> thumbnail_envelope_;
    uint32_t thumbnail_channel_ = static_cast<uint32_t>(-1);
};

// ── SpectrumView ─────────────────────────────────────────────────────────────
// Frequency spectrum display on a log-frequency axis.
//
// Two ways to feed it:
//
//  1. Built-in analysis (preferred). Enable it, then call push_samples() from
//     the audio thread. The view windows, transforms, applies the level mode,
//     releases over time and maps the bins onto the log axis itself.
//
//        spectrum.set_analysis_enabled(true);          // UI thread, once
//        spectrum.set_sample_rate(sample_rate);        // UI thread
//        // audio callback:
//        spectrum.push_samples(mono, frames);          // audio thread
//
//  2. Caller-supplied dB bins via set_spectrum()/set_bins(). This remains
//     supported and keeps its original linear bin-per-column drawing, but it
//     is soft-deprecated: it leaves windowing, calibration, the frequency
//     axis and the release to the caller. When analysis is enabled it takes
//     precedence over supplied bins.
//
// Thread rules, per method:
//   audio thread  push_samples()                      wait-free, no allocation
//   UI thread     everything else, including paint(), set_analysis_enabled(),
//                 the setters, update_analysis() and the getters.
// The tap that push_samples() writes into lives as long as the view; the view
// must not be destroyed while the audio thread can still call push_samples().

class SpectrumView : public View, public InspectorFacts, public BinDisplay {
  public:
    enum class Style { bars, line, filled };
    using LevelMode = signal::SpectrumLevelMode;
    using WindowType = signal::WindowFunction::Type;

    SpectrumView() = default;
    ~SpectrumView() override = default;

    // ── Caller-supplied bins (soft-deprecated; see the note above) ───────
    // Set spectrum magnitudes (dB, typically -100 to 0)
    void set_spectrum(const float* magnitudes_db, size_t bin_count);
    void set_spectrum(std::vector<float> magnitudes_db);
    void set_bins(const float* values, std::size_t count) override {
        set_spectrum(values, count);
    }

    size_t bin_count() const {
        return bins_.size();
    }

    std::string_view inspector_type_name() const override {
        return "SpectrumView";
    }
    void inspector_facts(std::vector<InspectorFacts::Fact>& out) const override;

    void set_style(Style s) {
        style_ = s;
    }
    Style style() const {
        return style_;
    }

    // dB range for display
    void set_range(float min_db, float max_db) {
        min_db_ = min_db;
        max_db_ = max_db;
    }

    // ── Built-in analysis ────────────────────────────────────────────────
    /// UI thread. Creates the analysis state on first enable. Enabling also
    /// opts the view into per-vsync repaints; disabling stops them and makes
    /// push_samples() a no-op.
    void set_analysis_enabled(bool on);
    bool analysis_enabled() const {
        return analysis_enabled_;
    }

    /// AUDIO THREAD. Queue mono samples for analysis. Wait-free, never
    /// allocates, never blocks. A no-op until analysis is enabled. Samples that
    /// do not fit (UI stalled for several frames) are dropped, not queued.
    void push_samples(const float* samples, size_t count) noexcept {
        if (auto* tap = tap_.load(std::memory_order_acquire))
            tap->push_samples(samples, count);
    }

    /// UI thread. Analysis settings; each applies immediately and is safe while
    /// audio is running. FFT size is rounded up to a power of two in
    /// [64, 16384]. Overlap is the fraction of a frame shared with the next.
    void set_sample_rate(double hz);
    void set_fft_size(int size);
    void set_overlap(float overlap);
    void set_window(WindowType window);
    void set_level_mode(LevelMode mode);
    /// Seconds for the shown level to fall by a factor of e. Time-based, so the
    /// fall looks the same at any frame rate.
    void set_release_time(float seconds);
    /// Log-frequency axis range. Ignored unless 0 < min_hz < max_hz.
    void set_frequency_range(float min_hz, float max_hz);

    double sample_rate() const {
        return analysis_.sample_rate;
    }
    int fft_size() const {
        return analysis_.fft_size;
    }
    float overlap() const {
        return analysis_.overlap;
    }
    WindowType window() const {
        return analysis_.window;
    }
    LevelMode level_mode() const {
        return analysis_.level_mode;
    }
    float release_time() const {
        return analysis_.release_seconds;
    }
    float min_frequency() const {
        return min_hz_;
    }
    float max_frequency() const {
        return max_hz_;
    }

    /// UI thread. Drain queued audio, analyse it and advance the release by
    /// `dt_seconds`. paint() calls this with the wall time since the previous
    /// paint; call it directly to drive the view headlessly. Returns the number
    /// of FFT frames analysed.
    int update_analysis(double dt_seconds);

    /// UI thread. The current analysis mapped onto `columns` log-spaced columns
    /// across the frequency range, in dB (what paint() draws). Empty when
    /// analysis is off. Allocates; for tests and tooling, not for paint.
    std::vector<float> analysis_columns(size_t columns) const;

    /// UI thread. FFT frames discarded because the UI fell behind, and samples
    /// the audio thread could not queue. Both 0 when analysis is off.
    std::uint64_t skipped_frame_count() const {
        return analyzer_ ? analyzer_->skipped_frame_count() : 0;
    }
    std::uint64_t overflow_count() const {
        return analyzer_ ? analyzer_->overflow_count() : 0;
    }

    void paint(canvas::Canvas& canvas) override;

  private:
    void apply_analysis_config();
    void paint_analysis(canvas::Canvas& canvas, const Rect& b);
    void paint_supplied_bins(canvas::Canvas& canvas, const Rect& b);
    void paint_grid(canvas::Canvas& canvas, const Rect& b, bool log_axis);

    std::vector<float> bins_;
    Style style_ = Style::filled;
    float min_db_ = -80.0f;
    float max_db_ = 0.0f;

    signal::SpectrumAnalyzerConfig analysis_{};
    float min_hz_ = 20.0f;
    float max_hz_ = 20000.0f;
    bool analysis_enabled_ = false;
    std::unique_ptr<signal::SpectrumAnalyzer> analyzer_;
    // Points at analyzer_ while analysis is enabled, null otherwise. The
    // analyzer itself is never freed before the view, so a push_samples() that
    // loaded the pointer just before a disable still writes into live memory.
    std::atomic<signal::SpectrumAnalyzer*> tap_{nullptr};
    signal::SpectrumBinMapping mapping_;
    std::vector<float> columns_;
    std::vector<canvas::Canvas::Point2D> points_;
    std::vector<canvas::Canvas::Point2D> fill_poly_;
    std::optional<std::chrono::steady_clock::time_point> last_paint_;
};

} // namespace pulp::view
