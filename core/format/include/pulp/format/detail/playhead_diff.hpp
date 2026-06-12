#pragma once

// Shared helpers for item 1.3 — AudioPlayHead transport-extension adapter wiring.
//
// Each format adapter (VST3, AU v2, AU v3, CLAP, AAX) populates the
// item-1.3 fields on `ProcessContext` from its host's playhead API. The
// derived fields — bar index from beats + time signature, the three
// change-flags (tempo / time-sig / transport), and transport-jump metadata
// computed against the
// previous block — are identical across adapters. This header factors
// them out so each adapter just snapshots the previous block's
// transport state and calls these helpers.
//
// Pure functions, header-only, no platform dependencies. Tests live in
// `test/test_playhead_diff.cpp`.

#include <pulp/format/processor.hpp>

#include <cmath>
#include <cstdint>

namespace pulp::format::detail {

/// Per-adapter snapshot of the transport fields that contribute to the
/// `tempo_changed` / `time_sig_changed` / `transport_changed` flags.
/// Adapters keep one instance as a member and pass it to
/// `compute_playhead_changes` once per block.
///
/// Default-constructed = "no previous block" — `apply_first_block()`
/// resets the snapshot in-place so the very first block does not raise
/// spurious change flags. Subsequent blocks diff against the captured
/// state.
struct PlayheadSnapshot {
    bool has_previous = false;
    double tempo_bpm = 0.0;
    int time_sig_numerator = 0;
    int time_sig_denominator = 0;
    bool is_playing = false;
    bool is_recording = false;
    bool is_looping = false;
    int64_t position_samples = 0;
    double position_beats = 0.0;
    double sample_rate = 0.0;
    int num_samples = 0;
};

/// Derive `ctx.bar` from `ctx.position_beats` + the active time
/// signature, when the host does not supply a precomputed bar.
///
/// `bar = floor(position_beats * (time_sig_denominator / 4) /
/// time_sig_numerator)` — matches the formula documented on
/// `ProcessContext::bar`.
///
/// Adapters that already have a host-provided bar (VST3
/// `barPositionMusic`) should write `ctx.bar` directly and skip this
/// helper. Skips work safely when the time signature is degenerate
/// (numerator <= 0 or denominator <= 0) by leaving `ctx.bar` at 0.
inline void derive_bar_from_beats(ProcessContext& ctx) noexcept {
    if (ctx.time_sig_numerator <= 0 || ctx.time_sig_denominator <= 0) {
        ctx.bar = 0;
        return;
    }
    const double beats_per_bar = static_cast<double>(ctx.time_sig_numerator) *
                                 (4.0 / static_cast<double>(ctx.time_sig_denominator));
    if (beats_per_bar <= 0.0) {
        ctx.bar = 0;
        return;
    }
    const double bar_d = std::floor(ctx.position_beats / beats_per_bar);
    ctx.bar = static_cast<int64_t>(bar_d);
}

/// Compute the three change-flags by diffing `ctx` against the
/// adapter's previous-block `snapshot`, then update the snapshot in
/// place so the next block's diff is against the values just written.
///
/// First call after a default-constructed snapshot raises no change
/// flags (the "no previous block" contract) — matches the documented
/// default that the very first block does not falsely signal a
/// transition. Subsequent calls flip the flags whenever any field
/// differs from the previous block.
inline void compute_playhead_changes(ProcessContext& ctx,
                                     PlayheadSnapshot& snapshot) noexcept {
    if (!snapshot.has_previous) {
        ctx.tempo_changed = false;
        ctx.time_sig_changed = false;
        ctx.transport_changed = false;
        ctx.transport_jump = false;
    } else {
        ctx.tempo_changed = (ctx.tempo_bpm != snapshot.tempo_bpm);
        ctx.time_sig_changed =
            (ctx.time_sig_numerator != snapshot.time_sig_numerator) ||
            (ctx.time_sig_denominator != snapshot.time_sig_denominator);
        ctx.transport_changed =
            (ctx.is_playing != snapshot.is_playing) ||
            (ctx.is_recording != snapshot.is_recording) ||
            (ctx.is_looping != snapshot.is_looping);
        const bool has_sample_position =
            ctx.position_samples != 0 || snapshot.position_samples != 0;
        if (has_sample_position) {
            const int64_t expected = (ctx.is_playing && snapshot.is_playing)
                ? snapshot.position_samples + static_cast<int64_t>(snapshot.num_samples)
                : snapshot.position_samples;
            ctx.transport_jump = ctx.position_samples != expected;
        } else {
            const bool has_beat_position =
                ctx.position_beats != 0.0 || snapshot.position_beats != 0.0;
            if (has_beat_position && ctx.is_playing && snapshot.is_playing &&
                snapshot.sample_rate > 0.0 && snapshot.num_samples > 0 &&
                snapshot.tempo_bpm > 0.0) {
                const double expected_delta =
                    (static_cast<double>(snapshot.num_samples) / snapshot.sample_rate) *
                    (snapshot.tempo_bpm / 60.0);
                const double expected = snapshot.position_beats + expected_delta;
                ctx.transport_jump = std::abs(ctx.position_beats - expected) > 1.0e-9;
            } else if (has_beat_position) {
                ctx.transport_jump = ctx.position_beats != snapshot.position_beats;
            } else {
                ctx.transport_jump = false;
            }
        }
    }

    snapshot.has_previous = true;
    snapshot.tempo_bpm = ctx.tempo_bpm;
    snapshot.time_sig_numerator = ctx.time_sig_numerator;
    snapshot.time_sig_denominator = ctx.time_sig_denominator;
    snapshot.is_playing = ctx.is_playing;
    snapshot.is_recording = ctx.is_recording;
    snapshot.is_looping = ctx.is_looping;
    snapshot.position_samples = ctx.position_samples;
    snapshot.position_beats = ctx.position_beats;
    snapshot.sample_rate = ctx.sample_rate;
    snapshot.num_samples = ctx.num_samples;
}

} // namespace pulp::format::detail
