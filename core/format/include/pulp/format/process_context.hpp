#pragma once

// Per-block host state handed to Processor::process(): transport position,
// tempo, render hints, and the SMPTE frame rate. processor.hpp includes this
// header, so existing consumers keep compiling unchanged; new code should
// include it directly.

#include <pulp/format/process_block.hpp>
#include <pulp/format/transport_fields.hpp>
#include <cstdint>

namespace pulp::format {

/// SMPTE video frame rate.
///
/// Used by `ProcessContext::frame_rate` so plugins that drive video sync
/// or display SMPTE timecode can format positions correctly. `unknown` is
/// the documented sentinel for hosts that do not provide a frame rate
/// (e.g. CLAP `clap_event_transport` has no frame-rate field; AU
/// `kAudioUnitProperty_HostCallbacks` only exposes it inside
/// `HostCallback_GetTransportState2`'s `outCurrentSampleInTimeLine`
/// indirectly via the project session — adapters report `unknown` if
/// the host does not surface it).
enum class FrameRate {
    unknown = 0,
    fps_24,           ///< Film. VST3 `kFrameRate24fps`.
    fps_25,           ///< PAL. VST3 `kFrameRate25fps`.
    fps_29_97,        ///< NTSC non-drop. VST3 `kFrameRate2997fps`.
    fps_29_97_drop,   ///< NTSC drop-frame. VST3 `kFrameRate2997DropFps`.
    fps_30,           ///< NTSC integer. VST3 `kFrameRate30fps`.
    fps_30_drop,      ///< 30 drop-frame. VST3 `kFrameRate30DropFps`.
    fps_60,           ///< High-rate. VST3 `kFrameRate60fps`.
};

/// Host render-speed hint for offline or constrained live rendering.
///
/// This is advisory. Processors must still obey the thread contract implied by
/// `ProcessContext::process_mode`; for example, a realtime block with
/// `SlowerThanRealtime` is still an audio-thread callback.
enum class RenderSpeedHint {
    Unknown = 0,
    Realtime,
    FasterThanRealtime,
    SlowerThanRealtime,
};

/// Process context — passed every audio callback with transport state.
///
/// Fields are populated by the host. Not all hosts provide all fields; when an
/// adapter publishes `transport_validity`, it distinguishes unavailable values
/// from valid defaults.
///
/// Adapter sourcing:
///
/// - VST3 — `Vst::ProcessContext` (`SystemTime` →
///   `host_time_ns`, `ProjectTimeMusic` → `position_beats`,
///   `CycleStartMusic` / `CycleEndMusic` → `loop_start_beats` /
///   `loop_end_beats`, `kCycleActive` → `is_looping`,
///   `frameRate.framesPerSecond` → `frame_rate`).
/// - AU v3 / v2 — `kAudioUnitProperty_HostCallbacks` (HostBeatAndTempo
///   → `tempo_bpm` / `position_beats`; HostTransport →
///   `is_playing` / `is_recording` / `is_looping` /
///   `loop_start_beats` / `loop_end_beats`; MusicalTimeLocation →
///   `time_sig_numerator` / `time_sig_denominator`; `mach_absolute_time`
///   → `host_time_ns`). AU has no frame-rate field; `frame_rate`
///   stays `FrameRate::unknown`.
/// - CLAP — `clap_event_transport` (`flags` for is_playing /
///   is_recording / is_looping; `loop_start_beats` /
///   `loop_end_beats` directly; `tsig_num` / `tsig_denom` for time
///   signature). CLAP does not provide frame rate; `frame_rate`
///   stays `FrameRate::unknown`.
/// - AAX (optional Avid SDK) — `IACFTransport` (`GetCurrentTickPosition`
///   for beats; `GetCurrentLoopPosition` for loop range; transport
///   state flags). Frame rate via `IACFTransport::GetTimeCodeInfo`
///   when present, else `FrameRate::unknown`.
///
/// Change-flags (`tempo_changed`, `time_sig_changed`,
/// `transport_changed`) are computed by the adapter once per block by
/// diffing against the previous block's snapshot, so processors can
/// branch on transitions only without re-reading every field.
struct ProcessContext {
    double sample_rate = 0;
    int num_samples = 0;

    /// Runtime mode for this block. Defaults to live realtime processing for
    /// source compatibility; offline/headless/render hosts should set
    /// `ProcessMode::Offline` explicitly.
    ProcessMode process_mode = ProcessMode::Realtime;

    /// True when the host is rendering the plugin's bypass path for this block.
    /// Most current adapters short-circuit before calling `process()` while
    /// bypassed, so the default remains false and this flag is only meaningful
    /// on hosts that intentionally process while bypassed to drain tails.
    bool is_bypassed = false;

    /// True when the host is asking a processor to continue rendering after
    /// input/transport stop so delay, reverb, or lookahead state can settle.
    bool is_tail_drain = false;

    /// True for the first block after a transport jump, seek, or explicit DSP
    /// reset boundary. Processors can use this to clear tempo-synced phase or
    /// delay history without guessing from position discontinuities.
    bool reset_requested = false;

    /// True when the host observed a discontinuous timeline move since the
    /// previous block. This is narrower than `transport_changed`: play/stop
    /// transitions do not imply a jump unless position also changed
    /// discontinuously.
    bool transport_jump = false;

    /// Advisory render-speed category for hosts that know whether the current
    /// pass is live, faster-than-realtime export, or slower/constrained render.
    RenderSpeedHint render_speed_hint = RenderSpeedHint::Unknown;

    bool is_playing = false;
    bool is_recording = false;
    double tempo_bpm = 120.0;
    double position_beats = 0.0;   ///< Position in quarter notes
    int64_t position_samples = 0;
    int time_sig_numerator = 4;
    int time_sig_denominator = 4;

    // New fields default to "host did not provide" sentinels so adapters that
    // don't populate them keep the pre-extension behavior exactly.

    /// Bar index derived from `position_beats` + the active time
    /// signature. Hosts may already publish a precomputed bar (VST3
    /// `Vst::ProcessContext::barPositionMusic`) — in that case the
    /// adapter uses the host value directly. Otherwise the adapter
    /// derives `bar = floor(position_beats * (time_sig_denominator /
    /// 4) / time_sig_numerator)` and writes it here so processors that
    /// just need "what bar am I in" do not recompute it per block.
    /// Default 0 matches a stopped-at-origin playhead.
    int64_t bar = 0;

    /// True when the host's transport is in cycle/loop mode. VST3
    /// exposes this as the `kCycleActive` flag on `ProcessContext`;
    /// AU surfaces it via `HostTransport`'s `outIsCycling`; CLAP via
    /// the `CLAP_TRANSPORT_IS_LOOP_ACTIVE` bit on
    /// `clap_event_transport::flags`; AAX via `IACFTransport`'s loop
    /// state. Default false (no loop).
    bool is_looping = false;

    /// Loop start in quarter notes, only meaningful when `is_looping`
    /// is true. Mirrors `position_beats`'s PPQ convention. Sources:
    /// VST3 `Vst::ProcessContext::cycleStartMusic`; AU
    /// `HostTransport::outCycleStartBeat`; CLAP
    /// `clap_event_transport::loop_start_beats` (converted from CLAP's
    /// fixed-point `clap_beattime`); AAX
    /// `IACFTransport::GetCurrentLoopPosition`. Default 0.
    double loop_start_beats = 0.0;

    /// Loop end in quarter notes, only meaningful when `is_looping`
    /// is true. Same source list as `loop_start_beats`. Default 0
    /// (loop_start == loop_end => no loop range yet).
    double loop_end_beats = 0.0;

    /// Host clock timestamp matching the start of this block, in
    /// nanoseconds since an epoch the host chooses. Used for video
    /// sync against the OS clock. Sources:
    /// VST3 `Vst::ProcessContext::systemTime` (already nanoseconds on
    /// Apple; otherwise host-defined); AU `mach_absolute_time()`
    /// converted via `mach_timebase_info` (the AU adapter performs the
    /// conversion before writing here); CLAP has no host-time field
    /// today, so the adapter leaves the value at 0 (sentinel = "not
    /// provided"); AAX `IACFTransport`'s sample-position only — host
    /// time is unavailable, so leave at 0.
    int64_t host_time_ns = 0;

    /// SMPTE frame rate when the host exposes one. Default
    /// `FrameRate::unknown` is the documented sentinel meaning "host
    /// did not provide" — plugins must check before using.
    FrameRate frame_rate = FrameRate::unknown;

    /// True when the host's reported `tempo_bpm` differs from the
    /// previous block. Lets processors recompute tempo-dependent
    /// derived state (sample-domain envelope rates, delay-line beat
    /// lengths) only on transitions instead of every block. Computed
    /// by the adapter as `current.tempo_bpm != previous.tempo_bpm`.
    /// Default false (no change relative to a hypothetical previous
    /// block, which matches the initial-state contract).
    bool tempo_changed = false;

    /// True when the host's reported time signature
    /// (`time_sig_numerator` or `time_sig_denominator`) differs from
    /// the previous block. Lets processors rebuild bar-grid state on
    /// transitions only. Default false.
    bool time_sig_changed = false;

    /// True when any transport state field (`is_playing`,
    /// `is_recording`, `is_looping`) flipped since the previous
    /// block. Lets processors reset playback-only DSP state (e.g.
    /// flush a reverb tail when transport stops) on transitions only.
    /// Default false.
    bool transport_changed = false;

    /// True on the first block of a run — the block where the host's
    /// transport begins rolling. Set when `is_playing` goes false→true,
    /// and also on the very first block a processor ever sees if the
    /// transport is *already* playing (a plugin inserted mid-playback),
    /// because that block starts a run from the processor's point of view.
    ///
    /// This is deliberately NOT folded into `should_reset_dsp_state()`.
    /// A transport start is not a timeline discontinuity: pressing play at
    /// a parked position leaves `position_samples` unchanged, so
    /// `transport_jump` stays false and delay/reverb tails must survive.
    /// What a transport start *does* invalidate is run-relative phase —
    /// a tempo-synced LFO's cycle origin, a clock generator's first-pulse
    /// delay, a step sequencer's step index.
    ///
    /// Processors that free-run a phase accumulator across a stop and then
    /// "catch up" to the new position on the next play emit a burst of
    /// backlogged events on that block. Capture your run origin here
    /// instead. Prefer deriving event positions from `position_beats`
    /// outright; then a start, a seek, and a loop wrap are all the same
    /// case and none of them can produce a burst.
    bool transport_started = false;

    /// Identifies which transport values were supplied for this block. A clear
    /// bit means the corresponding value is only its compatibility default.
    /// CLAP publishes this mask; adapters awaiting migration leave it empty.
    /// Kept at the end of the data members so existing field offsets do not move.
    TransportValidity transport_validity{};

    constexpr bool has_transport(TransportField field) const noexcept {
        return transport_validity.has(field);
    }

    bool is_realtime() const noexcept {
        return process_mode == ProcessMode::Realtime;
    }

    bool is_offline() const noexcept {
        return process_mode == ProcessMode::Offline;
    }

    bool allows_offline_quality_work() const noexcept {
        return is_offline() &&
               render_speed_hint != RenderSpeedHint::Realtime;
    }

    /// True when DSP state should treat this block as a discontinuity boundary.
    /// Hosts can request this explicitly, and adapters can derive it from a
    /// transport seek/jump. Processors commonly use this to clear synced phase,
    /// delay history, or tempo-grid caches before rendering the block.
    bool should_reset_dsp_state() const noexcept {
        return reset_requested || transport_jump;
    }

    /// True when this block is not a normal input-driven render but the host is
    /// still calling `process()` so a processor can settle internal state,
    /// drain a tail, or maintain bypass-aware state. Processors must still obey
    /// the realtime/offline contract for the block.
    bool is_maintenance_render() const noexcept {
        return is_bypassed || is_tail_drain;
    }

    /// True when the host is asking the processor to render only existing tail
    /// state. This is intentionally separate from bypass: some hosts bypass by
    /// short-circuiting process(), while others continue calling process() to
    /// let tails settle.
    bool should_render_tail_only() const noexcept {
        return is_tail_drain;
    }
};

} // namespace pulp::format
