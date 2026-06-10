#pragma once

/// @file audio_inspector_panel.hpp
/// The content view of the developer Audio Inspector — meters, probe-stage
/// status, a copied fixed-capacity waveform, L/R level-match + channel
/// balance, and a device/runtime summary.
///
/// This panel owns its OWN state model. It consumes the shared probe schema
/// (`pulp::audio::AudioProbeSnapshot` + `AudioStats`) and `MultiChannelMeter`
/// L/R match, and is deliberately NOT coupled to the layout inspector's
/// view-tree / property internals. The window shell
/// (`AudioInspectorWindow`) polls a probe once per UI tick and pushes the
/// copied snapshot in via `update()`; the panel never touches the realtime
/// audio thread.
///
/// Honesty contract: when no probe is wired or the sequence number stops
/// advancing, the panel renders an explicit "no probe" / "stale" state — it
/// never fakes zeros that look like a live-but-silent signal.

#include <pulp/audio/audio_probe_snapshot.hpp>
#include <pulp/audio/audio_stats.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::view {

/// Time-domain waveform plot over a copied, fixed-capacity sample buffer.
/// The window copies channel-0 history out of the probe (a non-RT read) and
/// hands it here; this view only stores and paints — it never reads the probe.
class AudioWaveformView : public View {
public:
    /// Fixed display capacity. The window copies at most this many frames.
    static constexpr int kCapacity = 512;

    AudioWaveformView();

    /// Replace the displayed samples (mono, interleaved-by-frame channel 0).
    /// `count` is clamped to `kCapacity`; extra samples are dropped.
    void set_samples(const float* samples, int count);

    /// Drop all samples (renders the empty baseline).
    void clear_samples();

    int sample_count() const { return count_; }
    float sample_at(int i) const { return (i >= 0 && i < count_) ? data_[i] : 0.0f; }

    /// When false the view paints a dimmed baseline + no trace (stale / no
    /// probe). Drives the honest-unavailable visual.
    void set_live(bool live) { live_ = live; }
    bool is_live() const { return live_; }

    void paint(canvas::Canvas& canvas) override;

private:
    std::array<float, kCapacity> data_{};
    int count_ = 0;
    bool live_ = false;
};

/// dBFS floor used by the inspector's meter fill (and the empty-bar anchor).
inline constexpr float kInspectorMeterFloorDb = -60.0f;

/// Map a linear amplitude (1.0 == full scale) to a 0..1 meter fill on the SAME
/// dBFS scale the panel's text labels print, so a bar and its "… dBFS" label
/// agree. `kInspectorMeterFloorDb` maps to an empty bar, 0 dBFS to a full bar;
/// passing the raw linear amplitude to `Meter::set_level` would instead make
/// the bar read amplitude while the label reads dBFS — two different scales.
float dbfs_meter_fill(float linear);

/// Audio Inspector content panel. Self-contained: build the widget tree once,
/// then call `update()` each UI tick with the latest copied snapshot.
class AudioInspectorPanel : public View {
public:
    /// Availability of the underlying probe, surfaced honestly to the user.
    enum class Status {
        kNoProbe,   ///< No probe wired — nothing to observe.
        kStale,     ///< Probe wired but its sequence number stopped advancing.
        kLive,      ///< Probe wired and publishing fresh snapshots.
    };

    AudioInspectorPanel();

    /// Push the latest probe summary + device stats. `live` reflects whether
    /// the window observed the sequence number advance since the last tick.
    /// `waveform` / `waveform_count` carry the copied channel-0 history (may
    /// be null/0 when capture is disabled). When `status == kNoProbe` the
    /// snapshot is ignored and the panel shows the unavailable state.
    void update(Status status,
                const audio::AudioProbeSnapshot& snap,
                const audio::AudioStats& stats,
                const float* waveform,
                int waveform_count);

    // ── State accessors (for headless tests; no GPU/window needed) ──────────

    Status status() const { return status_; }
    audio::AudioProbeStage observed_stage() const { return snapshot_.stage_id; }
    float peak() const { return snapshot_.peak_max; }
    float rms() const { return snapshot_.rms_max; }
    std::uint64_t clip_count() const { return snapshot_.clip_count; }
    std::uint64_t nan_inf_count() const { return snapshot_.nan_inf_count; }
    std::uint64_t silence_run_blocks() const { return snapshot_.silence_run_blocks; }
    std::uint64_t sequence_number() const { return snapshot_.sequence_number; }

    /// L/R level-match in [0, 1]: the ratio of the quieter to the louder
    /// channel RMS (1 = equal energy, 0 = one channel silent). This is NOT a
    /// phase/Pearson correlation — the RT snapshot carries no inter-sample L*R
    /// product, so a true correlation (which would distinguish mono from
    /// anti-phase) is deferred until the probe snapshot carries that term.
    /// 0 when fewer than two active channels.
    float lr_match() const { return lr_match_; }
    /// Channel balance in [-1, +1]: -1 = all left, 0 = centered, +1 = all
    /// right. Derived from per-channel RMS (no first-class balance field
    /// exists on either source). 0 when not stereo.
    float balance() const { return balance_; }

    const AudioWaveformView& waveform() const { return *waveform_view_; }

    // ── Rendered-text accessors (for headless tests) ────────────────────────
    // The exact strings the labels paint. A test can assert these are
    // populated (e.g. the status reads "live", the level reads "dBFS") so a
    // regression that leaves the panel visually blank is caught in CI, not
    // only by eye.
    const std::string& status_text() const { return status_label_->text(); }
    const std::string& level_text() const { return level_label_->text(); }
    /// Color the status heading paints with. A test can assert its channels
    /// are in the valid [0,1] range — the white-on-white regression that hid
    /// every label came from feeding 0-255 values into the [0,1] Color ctor,
    /// which Skia clamps to white. Guards that specific class of bug.
    canvas::Color status_color() const { return status_label_->text_color(); }

private:
    void build_ui();
    void refresh_labels();

    Status status_ = Status::kNoProbe;
    audio::AudioProbeSnapshot snapshot_{};
    audio::AudioStats stats_{};
    float lr_match_ = 0.0f;
    float balance_ = 0.0f;

    // Widget pointers (owned by the view tree).
    Label* status_label_ = nullptr;
    Label* stage_label_ = nullptr;
    Meter* peak_meter_ = nullptr;
    Meter* rms_meter_ = nullptr;
    Label* level_label_ = nullptr;
    Label* content_label_ = nullptr;   ///< clip / NaN-Inf / silence facts
    Label* phase_label_ = nullptr;     ///< L/R level-match + balance
    Label* device_label_ = nullptr;    ///< sample rate / block / callbacks / xruns
    AudioWaveformView* waveform_view_ = nullptr;
};

}  // namespace pulp::view
