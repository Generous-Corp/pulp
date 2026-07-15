#pragma once

/// @file adapter_boundary.hpp
/// The shared adapter-boundary core (SF-1).
///
/// Every per-format plugin adapter (CLAP, VST3, AU v2/v3, AAX, LV2, WAM/WCLAP,
/// standalone) sits between a host ABI and one `Processor`. The *host-ABI glue*
/// is genuinely format-specific — the event structs, the pointer layouts, the
/// transport flag bits all differ. But the boundary *logic* those adapters
/// wrap around the glue is identical, and until now it was copy-pasted at
/// different fidelity per adapter, with nothing that noticed when a copy
/// drifted (see the SF-1 audit finding). This header is the one place that
/// logic lives:
///
///   1. **f64 (double) marshalling** — the `float`⇄`double` block copies a
///      double-precision host hands the adapter, previously implemented
///      byte-for-byte in `clap_adapter.cpp`, `vst3_adapter.cpp`, and
///      `processor_f64.cpp`.
///   2. **transport → `ProcessContext`** — `HostTransport` is the neutral,
///      already-decoded transport an adapter fills from its host playhead; a
///      single mapper writes the `ProcessContext` transport fields, derives the
///      bar when the host did not supply one, and diffs the change-flags. The
///      per-format decode (CLAP fixed-point beattime, VST3 `barPositionMusic`,
///      AU seconds, …) stays in the adapter; the mapping does not.
///   3. **latency-compensated bypass** — `LatencyCompensatedBypass` is the
///      per-channel dry delay line that keeps a bypassed signal sample-aligned
///      with the host's plugin-delay-compensation. CLAP/VST3 hand-rolled it;
///      AU/AAX/LV2 did a plain memcpy that lied about latency. One class now.
///   4. **parameter dual-write** — `apply_param_value` enqueues an incoming
///      host param event for the sample-accurate DSP cursor *and* publishes it
///      to the RT-safe listener path, the pair every adapter must keep in sync.
///
/// **Real-time contract.** Everything here runs on the audio/render thread and
/// is allocation-, lock-, and syscall-free after `prepare()`. This preserves
/// the RT-safety guarantees #5911 (MF-8 + PF-3) established: the only place
/// that touches the heap is `LatencyCompensatedBypass::prepare()`, which the
/// adapter calls at activate/setup time off the audio thread. The functions are
/// pure and header-only so the parity-matrix test (`test_adapter_boundary_
/// parity.cpp`) can drive one processor through every format's boundary profile
/// and assert identical observable behavior without standing up a host SDK.

#include <pulp/format/processor.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/state/store.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pulp::format::boundary {

/// Channel ceiling shared with the CLAP / LV2 adapters (`kMaxChannels = 8`).
inline constexpr std::size_t kBoundaryMaxChannels = 8;

// ---------------------------------------------------------------------------
// 1. f64 (double) marshalling
//
// Raw-pointer block copies for the double-precision host boundary. A host that
// runs the graph in `double` hands the adapter `double*` buffers; the shared
// `Processor` renders in `float`, so the adapter converts in and out. These are
// the exact loops that were duplicated in three TUs. `count` is the per-block
// frame count; all three are branch-light and vectorise.
// ---------------------------------------------------------------------------

/// Widen `count` floats `src → dst` as doubles. No-op on a null pointer.
inline void copy_f32_to_f64(const float* src, double* dst,
                            std::uint32_t count) noexcept {
    if (!src || !dst) return;
    for (std::uint32_t i = 0; i < count; ++i)
        dst[i] = static_cast<double>(src[i]);
}

/// Narrow `count` doubles `src → dst` as floats. No-op on a null pointer.
inline void copy_f64_to_f32(const double* src, float* dst,
                            std::uint32_t count) noexcept {
    if (!src || !dst) return;
    for (std::uint32_t i = 0; i < count; ++i)
        dst[i] = static_cast<float>(src[i]);
}

/// Zero `count` doubles. No-op on a null pointer.
inline void zero_f64(double* dst, std::uint32_t count) noexcept {
    if (!dst) return;
    for (std::uint32_t i = 0; i < count; ++i) dst[i] = 0.0;
}

// ---------------------------------------------------------------------------
// 2. transport → ProcessContext
// ---------------------------------------------------------------------------

/// Format-neutral transport snapshot, already decoded from a host playhead into
/// canonical units (quarter-note beats, samples, real BPM). Each `has_*` flag
/// says whether the host actually supplied that field this block; an unset
/// field leaves the corresponding `ProcessContext` value at its default so an
/// adapter that never populates it keeps the pre-extension behavior exactly.
///
/// `valid == false` means "the host provided no transport at all" (e.g. CLAP's
/// `process->transport == nullptr`): the mapper leaves every transport field at
/// its `ProcessContext` default and only runs the change-flag diff.
struct HostTransport {
    bool valid = false;

    bool is_playing = false;
    bool is_recording = false;
    bool is_looping = false;

    bool has_tempo = false;
    double tempo_bpm = 120.0;

    bool has_beats = false;
    double position_beats = 0.0;  ///< quarter notes

    bool has_samples = false;
    std::int64_t position_samples = 0;

    bool has_time_sig = false;
    int time_sig_numerator = 4;
    int time_sig_denominator = 4;

    double loop_start_beats = 0.0;  ///< meaningful only when is_looping
    double loop_end_beats = 0.0;

    /// A host that publishes a precomputed bar (VST3 `barPositionMusic`, CLAP
    /// `bar_number`) sets `has_host_bar` and `host_bar`; otherwise the mapper
    /// derives the bar from beats + time signature.
    bool has_host_bar = false;
    std::int64_t host_bar = 0;

    std::int64_t host_time_ns = 0;
    FrameRate frame_rate = FrameRate::unknown;
};

/// Write @p transport into @p ctx's transport fields, derive the bar when the
/// host did not supply one, then diff @p ctx against @p snapshot to populate
/// `tempo_changed` / `time_sig_changed` / `transport_changed` /
/// `transport_started` / `transport_jump` and advance the snapshot in place.
///
/// Pure aside from the in-place snapshot update; no allocation. The snapshot is
/// the adapter's per-instance previous-block state (a `PlayheadSnapshot`
/// member), so a first block after construction raises no spurious change
/// flags. `ctx.sample_rate` / `ctx.num_samples` must already be set — the diff
/// uses them to tell a continuous beat advance from a seek.
inline void apply_host_transport(ProcessContext& ctx, const HostTransport& transport,
                                 detail::PlayheadSnapshot& snapshot) noexcept {
    if (transport.valid) {
        ctx.is_playing = transport.is_playing;
        ctx.is_recording = transport.is_recording;
        ctx.is_looping = transport.is_looping;
        if (transport.has_tempo) ctx.tempo_bpm = transport.tempo_bpm;
        if (transport.has_beats) ctx.position_beats = transport.position_beats;
        if (transport.has_samples) ctx.position_samples = transport.position_samples;
        if (transport.has_time_sig) {
            ctx.time_sig_numerator = transport.time_sig_numerator;
            ctx.time_sig_denominator = transport.time_sig_denominator;
        }
        if (transport.is_looping) {
            ctx.loop_start_beats = transport.loop_start_beats;
            ctx.loop_end_beats = transport.loop_end_beats;
        }
        ctx.host_time_ns = transport.host_time_ns;
        ctx.frame_rate = transport.frame_rate;
        if (transport.has_host_bar) {
            ctx.bar = transport.host_bar;
        } else {
            detail::derive_bar_from_beats(ctx);
        }
    }
    detail::compute_playhead_changes(ctx, snapshot);
}

// ---------------------------------------------------------------------------
// 3. latency-compensated bypass
// ---------------------------------------------------------------------------

/// Per-channel dry-signal delay used by an adapter's bypass path.
///
/// A latent processor is compensated by the host: the host delays every *other*
/// track by the plugin's reported latency so the plugin path stays aligned.
/// When the plugin is bypassed the adapter emits the dry input directly, but
/// that input is now @e early relative to the host's compensation by exactly
/// the reported latency — so a correct bypass delays the dry signal by the same
/// amount. CLAP and VST3 each hand-rolled this; AU/AAX/LV2 memcpy'd the input
/// straight through and were a `latency` samples early while bypassed. This is
/// the one implementation.
///
/// `prepare()` (off the audio thread) sizes the lines. `process_channel()` is
/// RT-safe. A reported latency of `<= 0` means no compensation, and
/// `process_channel()` degrades to a plain copy so callers use one code path.
template <std::size_t MaxChannels = kBoundaryMaxChannels>
class LatencyCompensatedBypassT {
public:
    /// Size the per-channel delay to @p latency_samples (the processor's
    /// *reported* latency, i.e. already run through `reported_latency_samples()`
    /// so the host-quirk clamp applies). Allocates; call off the audio thread at
    /// activate/setup. A value `<= 0` disables compensation and frees nothing
    /// the hot path touches.
    void prepare(int latency_samples) {
        delay_samples_ = latency_samples > 0 ? latency_samples : 0;
        if (delay_samples_ > 0) {
            for (auto& line : lines_) line.prepare(delay_samples_);
        }
    }

    /// Clear the delay history without resizing (e.g. on a transport reset).
    void reset() {
        for (auto& line : lines_) line.reset();
    }

    /// True when a non-zero reported latency is being compensated.
    bool is_latency_compensated() const noexcept { return delay_samples_ > 0; }

    /// The compensation delay in samples (0 when uncompensated).
    int delay_samples() const noexcept { return delay_samples_; }

    /// Number of per-channel delay lines this instance owns.
    static constexpr std::size_t channel_capacity() noexcept { return MaxChannels; }

    /// Push one dry sample through channel @p ch's delay and return the
    /// compensated output (`in[n - delay]` in steady state). Returns @p in
    /// unchanged when uncompensated or @p ch is out of range, so a caller that
    /// already gated on `is_latency_compensated()` gets a 1:1 replacement for a
    /// raw `DelayLine::process`. RT-safe. Callers that mix compensated and
    /// uncompensated channels in one block (more channels than
    /// `channel_capacity()`) must gate on `is_latency_compensated()` themselves
    /// so every channel takes the same path.
    float process_sample(float in, std::size_t ch) {
        if (delay_samples_ <= 0 || ch >= lines_.size()) return in;
        return lines_[ch].process(in, static_cast<float>(delay_samples_));
    }

    /// Emit `in[n - delay]` into @p out for channel @p ch (steady state), or a
    /// straight copy of @p in when uncompensated. A null @p in writes silence
    /// (a bypassed effect fed no dry signal on that channel). RT-safe: no
    /// allocation, no lock. @p out must be valid for @p count samples.
    void process_channel(float* out, const float* in, std::uint32_t count,
                         std::size_t ch) {
        if (!out) return;
        if (delay_samples_ <= 0 || ch >= lines_.size()) {
            // Uncompensated: straight dry passthrough (or silence with no input).
            if (in) {
                if (out != in) std::memcpy(out, in, sizeof(float) * count);
            } else {
                std::memset(out, 0, sizeof(float) * count);
            }
            return;
        }
        auto& line = lines_[ch];
        const float delay = static_cast<float>(delay_samples_);
        if (in) {
            for (std::uint32_t i = 0; i < count; ++i)
                out[i] = line.process(in[i], delay);
        } else {
            for (std::uint32_t i = 0; i < count; ++i)
                out[i] = line.process(0.0f, delay);
        }
    }

private:
    std::array<signal::DelayLine, MaxChannels> lines_{};
    int delay_samples_ = 0;
};

using LatencyCompensatedBypass = LatencyCompensatedBypassT<kBoundaryMaxChannels>;

// ---------------------------------------------------------------------------
// 4. parameter dual-write
// ---------------------------------------------------------------------------

/// Apply one incoming host parameter-value event, the way every adapter must.
///
/// A host param change has to reach two consumers with different contracts:
///   - the **sample-accurate DSP cursor** (`for_each_subblock` /
///     `ParamCursor`), which reads a sorted `ParameterEventQueue` and applies
///     the value at its exact `sample_offset` within the block; and
///   - the **listener path** (editor / analysis), fed by the RT-safe
///     `set_value_rt` (atomic store + non-allocating SPSC push) rather than the
///     heap-allocating `set_value`.
///
/// Doing only the first leaves listeners stale; doing only the second collapses
/// sample-accurate automation to block-rate. Adapters that read parameters once
/// per block (AU v2, LV2 control ports) pass `sample_offset = 0`.
///
/// @return false if the DSP queue was full and the event was dropped (the
///         listener write still happened); true otherwise. RT-safe.
inline bool apply_param_value(state::ParameterEventQueue& queue,
                              state::StateStore& store,
                              state::ParamID id,
                              std::int32_t sample_offset,
                              float value) {
    const bool queued = queue.push(state::ParameterEvent{id, sample_offset, value});
    store.set_value_rt(id, value);
    return queued;
}

}  // namespace pulp::format::boundary
