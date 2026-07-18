#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/graph_scale.hpp>
#include <pulp/signal/biquad.hpp>
#include <pulp/state/store.hpp>
#include <vector>
#include <functional>
#include <span>

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
        // Per-band color. Left fully transparent by default as the "unset"
        // sentinel — the view then assigns one from band_palette_color(). A
        // plain Color{} is opaque BLACK (alpha defaults to 1), which would read
        // as an explicitly-chosen black, so the sentinel must zero alpha.
        Color color{0.0f, 0.0f, 0.0f, 0.0f};
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

    // Per-band curves + translucent fills, each in the band's color, drawn under
    // the composite so every handle sits on its own curve. On by default.
    void set_show_band_curves(bool show) { show_band_curves_ = show; }
    bool show_band_curves() const { return show_band_curves_; }

    // Draw handles for DISABLED (bypassed) bands as dimmed dots instead of
    // hiding them. Off by default so existing callers are unaffected; a host
    // that models bypass (the composite already excludes a disabled band) turns
    // this on so a bypassed band still shows a faded, re-grabbable handle.
    void set_show_disabled_handles(bool show) { show_disabled_handles_ = show; }
    bool show_disabled_handles() const { return show_disabled_handles_; }

    // dB (vertical) and frequency (horizontal) axis labels. On by default.
    void set_show_labels(bool show) { show_labels_ = show; }
    bool show_labels() const { return show_labels_; }

    // Live readout (frequency / gain / Q) of the band under the pointer or being
    // dragged. On by default.
    void set_show_readout(bool show) { show_readout_ = show; }
    bool show_readout() const { return show_readout_; }

    // Reserve empty space at the TOP of the plotting area (pixels). The grid,
    // curve, handles, and the top dB axis label are all pushed down by this
    // amount so a host with no chrome above the graph (a plugin editor with no
    // header) has room to paint its own readout / controls in the gap without
    // clipping the "+12" label against the window edge. Off (0) by default so
    // existing callers are unaffected. The bottom axis (frequency labels) does
    // not move.
    void set_content_top_padding(float px) { content_top_pad_ = px > 0.0f ? px : 0.0f; }
    float content_top_padding() const { return content_top_pad_; }

    // Index of the band handle at a local point, or -1. Exposes the handle
    // hit-test so a host that layers its own gestures on the dots (e.g.
    // ⌥-click to bypass) can reuse the exact handle geometry instead of
    // duplicating it. Additive; existing callers are unaffected.
    int hit_test_handle(Point pos) const { return hit_test_band(pos); }

    // Ease each handle's radius toward its target (resting vs. hovered/active)
    // instead of snapping, so a hover reads as a small settle rather than a pop.
    // Off by default (existing callers snap as before). A host that turns this
    // on should keep the render loop alive while hover_animating() is true — the
    // shared needs_continuous_frames() predicate already consults it.
    void set_hover_animation(bool on) { hover_animation_ = on; }
    bool hover_animation() const { return hover_animation_; }

    // True while any handle radius is mid-transition (drives continuous frames).
    bool hover_animating() const { return hover_animating_; }

    // Default color assigned to band i when its Band::color is unset, from a
    // built-in palette. Exposed so callers can match handles or legends to it.
    static Color band_palette_color(size_t index);

    // Spectrum analyzer overlay (optional)
    void set_spectrum(const float* magnitudes_db, size_t bin_count);
    void clear_spectrum();

    // ── Analyzer dBFS scale ─────────────────────────────────────────────────
    // The spectrum overlay is drawn on its OWN dBFS scale spanning the FULL
    // plotting height, decoupled from the ±gain axis the EQ curve/dots use.
    // Default: analyzer_top_db (0 dBFS) at the top of the plot, analyzer_bottom_db
    // (−60 dBFS) at the bottom — so a smoothed envelope fills the whole graph
    // (Logic / Pro-Q style) instead of being crushed into the gain axis. The
    // setters exist for a future drag-to-zoom; the defaults are hardcoded.
    void set_analyzer_range(float top_db, float bottom_db) {
        analyzer_top_db_ = top_db;
        analyzer_bottom_db_ = bottom_db;
    }
    float analyzer_top_db() const { return analyzer_top_db_; }
    float analyzer_bottom_db() const { return analyzer_bottom_db_; }
    // Map a dBFS value to a plot-space y via the analyzer scale (top_db → top of
    // plot, bottom_db → bottom). Clamped to the plot. Independent of the gain
    // axis; the spectrum overlay and any analyzer tick labels use this.
    float analyzer_db_to_y(float dbfs) const;

    // ── StateStore binding (parameter automation: record + playback) ────────
    // Parameter IDs for one band's frequency, gain, and Q. One entry per band,
    // in the same order as the bands passed to set_bands(). A zero id means the
    // band has no parameter for that field (e.g. a gain-less filter).
    struct BandParamIds {
        pulp::state::ParamID freq = 0;
        pulp::state::ParamID gain = 0;
        pulp::state::ParamID q = 0;
    };

    // Bind this view to a StateStore so parameter automation works end-to-end:
    //
    //   • Record (Direction A): the built-in drag / scroll / pinch / reset
    //     handlers bracket their edits in begin_gesture → set_value → end_gesture
    //     so the host records automation and groups the drag into one undo step.
    //   • Playback (Direction B): a Main-thread listener maps a host-originated
    //     parameter change back onto the corresponding band and repaints, so the
    //     curve follows automation. The listener is a PURE widget update — it
    //     never re-opens a gesture nor writes back to the store, so it cannot
    //     feed back into the record path.
    //
    // Additive/opt-in: on_band_changed still fires. RT-safety: begin/end_gesture
    // and set_value run on the host main thread (the drag handlers already do);
    // the Main listener fires only from the UI-thread listener pump.
    void bind_bands(pulp::state::StateStore& store, std::span<const BandParamIds> ids);

    // Whether the built-in handlers record their edits to the bound store
    // (Direction A). On by default. A host that runs its OWN edit logic
    // (relative / group moves, numeric scrubbing) and drives the store itself
    // turns this OFF so the view does not double-write, while STILL receiving
    // host-playback updates through the bind_bands listener (Direction B).
    void set_record_gestures(bool on) { record_gestures_ = on; }
    bool record_gestures() const { return record_gestures_; }

    // Interaction callbacks
    std::function<void(size_t band_index, Band band)> on_band_changed;
    std::function<void(size_t band_index)> on_band_selected;

    // Selected band
    int selected_band() const { return selected_band_; }
    void set_selected_band(int index) { selected_band_ = index; }

    // Band currently under the pointer (-1 if none). Drives the hover highlight
    // and readout; exposed so hosts/tests can observe hover state.
    int hovered_band() const { return hovered_band_; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    // Trackpad pinch adjusts the Q of the hovered (else selected) band: pinch-in
    // narrows (higher Q), pinch-out widens (lower Q). Targets the hovered/selected
    // band rather than the gesture position — the platform reports the gesture
    // center in window coords, which a nested/offset view cannot hit-test.
    void on_gesture_event(const GestureEvent& event) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_mouse_drag(Point pos) override;
    // Passive hover (no button) arrives here on real hosts, NOT through
    // on_mouse_event — so the hover highlight + readout must track it here.
    void on_hover_move(Point local_pos) override;
    void on_mouse_leave() override;
    bool wants_mouse_input() const override { return true; }

private:
    std::vector<Band> bands_;
    float min_freq_ = 20.0f;
    float max_freq_ = 20000.0f;
    float min_db_ = -24.0f;
    float max_db_ = 24.0f;
    float sample_rate_ = 48000.0f;
    bool show_grid_ = true;
    bool show_band_curves_ = true;
    bool show_disabled_handles_ = false;
    bool show_labels_ = true;
    bool show_readout_ = true;
    float content_top_pad_ = 0.0f;  // empty space reserved at the top (px)
    bool hover_animation_ = false;  // ease handle radius toward target (opt-in)
    bool hover_animating_ = false;  // any handle mid-transition this frame
    std::vector<float> handle_radius_;  // per-band eased radius (-1 = uninit)
    std::vector<float> spectrum_;
    // Persistent temporally-smoothed spectrum (fast attack / slow release),
    // updated per frame in set_spectrum() and drawn from in paint() so the
    // analyzer flows instead of flickering.
    std::vector<float> spectrum_smoothed_;
    int selected_band_ = -1;
    int dragging_band_ = -1;
    int hovered_band_ = -1;
    // A double-click both resets a band AND is followed by a drag-start press
    // from the platform; this suppresses that one spurious on_mouse_down.
    bool suppress_next_down_ = false;
    Point drag_start_{};

    // Coefficients of the enabled bands, rebuilt only when a band or the
    // sample rate changes — not per frame, and never per curve point.
    std::vector<signal::BiquadCoefficients> coefficients_;
    // Scratch buffers for the drawn curves. Members so paint() reuses the
    // allocation across frames instead of allocating on every repaint.
    mutable std::vector<float> curve_db_;
    mutable std::vector<float> spectrum_db_;
    mutable std::vector<float> band_db_;
    mutable std::vector<canvas::Canvas::Point2D> fill_poly_;

    // Analyzer dBFS scale (full plot height), independent of the ±gain axis.
    float analyzer_top_db_ = 0.0f;
    float analyzer_bottom_db_ = -60.0f;

    // ── StateStore binding state ────────────────────────────────────────────
    pulp::state::StateStore* store_ = nullptr;
    std::vector<BandParamIds> band_params_;
    bool record_gestures_ = true;
    // True while a Direction-A write is in flight, so the bind_bands listener
    // ignores our own echo (the host adapter suppresses its own separately).
    bool writing_to_store_ = false;
    // Param ids whose gesture is open for the active dot drag (0 = none).
    pulp::state::ParamID drag_freq_gesture_ = 0;
    pulp::state::ParamID drag_gain_gesture_ = 0;

    void rebuild_coefficients();
    float freq_to_x(float freq) const;
    float x_to_freq(float x) const;
    float db_to_y(float db) const;
    float y_to_db(float y) const;
    int hit_test_band(Point pos) const;

    // StateStore helpers. store_set brackets the write with the echo guard;
    // store_begin/store_end forward gesture boundaries (no value write).
    void store_begin(pulp::state::ParamID id);
    void store_set(pulp::state::ParamID id, float value);
    void store_end(pulp::state::ParamID id);
    // Direction B: apply a host-originated parameter change to the matching band.
    void apply_param_from_host(pulp::state::ParamID id, float value);

    // Listener subscriptions for Direction B. Declared LAST so they are torn
    // down FIRST — the listener callback reads bands_ / band_params_, so it must
    // be removed before those members are destroyed.
    std::vector<pulp::state::ListenerToken> param_listener_tokens_;
};

} // namespace pulp::view
