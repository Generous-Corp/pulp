#pragma once

// Shared helpers for per-block host transport state.
//
// Each format adapter (VST3, AU v2, AU v3, CLAP, AAX) populates the
// transport fields on `ProcessContext` from its host's playhead API. The
// Derived bar and change metadata are format-neutral. Adapters can supply
// explicit field validity as they migrate; callers that still leave the mask
// empty retain the value-based behavior used before validity was exposed.
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
    TransportValidity transport_validity{};
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

enum class TransportDiffMode : std::uint8_t {
    LegacyValues,
    FieldValidity,
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
/// First call after a default-constructed snapshot raises no change flags. A
/// validity acquisition or loss counts as a change to that field, while a
/// transport jump requires a position source valid in both snapshots.
///
/// `LegacyValues` preserves change tracking for adapters that have not migrated
/// to field validity. Migrated adapters select `FieldValidity` on every block.
inline void compute_playhead_changes(ProcessContext& ctx,
                                     PlayheadSnapshot& snapshot,
                                     TransportDiffMode mode =
                                         TransportDiffMode::LegacyValues) noexcept {
    const bool explicit_validity = mode == TransportDiffMode::FieldValidity;

    if (!snapshot.has_previous) {
        ctx.tempo_changed = false;
        ctx.time_sig_changed = false;
        ctx.transport_changed = false;
        ctx.transport_jump = false;
        // A processor instantiated while the transport is already rolling has
        // no previous block to diff against, but this block still begins a run
        // from its point of view. Reporting no start here would leave a clock
        // or tempo-synced generator with no origin until the user happened to
        // stop and restart the transport.
        ctx.transport_started = explicit_validity
            ? ctx.has_transport(TransportField::Playing) && ctx.is_playing
            : ctx.is_playing;
    } else if (explicit_validity) {
        const auto field_changed = [&](TransportField field,
                                       bool value_changed) noexcept {
            const bool current_valid = ctx.has_transport(field);
            const bool previous_valid = snapshot.transport_validity.has(field);
            return current_valid != previous_valid ||
                   (current_valid && value_changed);
        };

        const bool current_playing_valid =
            ctx.has_transport(TransportField::Playing);
        const bool previous_playing_valid =
            snapshot.transport_validity.has(TransportField::Playing);
        ctx.transport_started = current_playing_valid && ctx.is_playing &&
            (!previous_playing_valid || !snapshot.is_playing);
        ctx.tempo_changed = field_changed(
            TransportField::Tempo, ctx.tempo_bpm != snapshot.tempo_bpm);
        ctx.time_sig_changed = field_changed(
            TransportField::TimeSignature,
            ctx.time_sig_numerator != snapshot.time_sig_numerator ||
                ctx.time_sig_denominator != snapshot.time_sig_denominator);
        ctx.transport_changed =
            field_changed(TransportField::Playing,
                          ctx.is_playing != snapshot.is_playing) ||
            field_changed(TransportField::Recording,
                          ctx.is_recording != snapshot.is_recording) ||
            field_changed(TransportField::Looping,
                          ctx.is_looping != snapshot.is_looping);

        ctx.transport_jump = false;
        const bool samples_valid =
            ctx.has_transport(TransportField::SamplePosition) &&
            snapshot.transport_validity.has(TransportField::SamplePosition);
        const bool playing_valid = current_playing_valid && previous_playing_valid;
        if (samples_valid) {
            const int64_t held = snapshot.position_samples;
            const int64_t advanced =
                snapshot.position_samples + static_cast<int64_t>(snapshot.num_samples);
            if (playing_valid && ctx.is_playing && snapshot.is_playing) {
                ctx.transport_jump = ctx.position_samples != advanced;
            } else {
                // Without authoritative play state, both a parked and a rolling
                // playhead are continuous possibilities.
                ctx.transport_jump = ctx.position_samples != held &&
                                     ctx.position_samples != advanced;
            }
        } else {
            const bool beats_valid =
                ctx.has_transport(TransportField::BeatPosition) &&
                snapshot.transport_validity.has(TransportField::BeatPosition);
            const bool tempo_valid =
                snapshot.transport_validity.has(TransportField::Tempo) &&
                snapshot.tempo_bpm > 0.0 && std::isfinite(snapshot.tempo_bpm);
            const bool can_predict = beats_valid && tempo_valid &&
                snapshot.sample_rate > 0.0 && snapshot.num_samples > 0;
            if (can_predict) {
                const double expected_delta =
                    (static_cast<double>(snapshot.num_samples) /
                     snapshot.sample_rate) * (snapshot.tempo_bpm / 60.0);
                const double expected = snapshot.position_beats + expected_delta;
                const bool continuous =
                    std::abs(ctx.position_beats - expected) <= 1.0e-9;
                const bool held =
                    std::abs(ctx.position_beats - snapshot.position_beats) <= 1.0e-9;
                if (playing_valid && ctx.is_playing && snapshot.is_playing) {
                    ctx.transport_jump = !continuous;
                } else {
                    ctx.transport_jump = !(continuous || held);
                }
            } else if (beats_valid && playing_valid &&
                       !ctx.is_playing && !snapshot.is_playing) {
                ctx.transport_jump =
                    ctx.position_beats != snapshot.position_beats;
            }
        }
    } else {
        // false -> true on is_playing. Note this is independent of
        // transport_jump: pressing play at a parked position leaves the
        // position unchanged, so a jump is not reported and must not be.
        ctx.transport_started = ctx.is_playing && !snapshot.is_playing;
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
            const int64_t held = snapshot.position_samples;
            const int64_t advanced =
                snapshot.position_samples + static_cast<int64_t>(snapshot.num_samples);
            if (ctx.is_playing && snapshot.is_playing) {
                ctx.transport_jump = ctx.position_samples != advanced;
            } else if (ctx.transport_changed && (ctx.is_playing || snapshot.is_playing)) {
                ctx.transport_jump = ctx.position_samples != held &&
                                     ctx.position_samples != advanced;
            } else {
                ctx.transport_jump = ctx.position_samples != held;
            }
        } else {
            const bool has_beat_position =
                ctx.position_beats != 0.0 || snapshot.position_beats != 0.0;
            if (has_beat_position && (ctx.is_playing || snapshot.is_playing) &&
                snapshot.sample_rate > 0.0 && snapshot.num_samples > 0 &&
                snapshot.tempo_bpm > 0.0) {
                const double expected_delta =
                    (static_cast<double>(snapshot.num_samples) / snapshot.sample_rate) *
                    (snapshot.tempo_bpm / 60.0);
                const double expected = snapshot.position_beats + expected_delta;
                const bool continuous =
                    std::abs(ctx.position_beats - expected) <= 1.0e-9;
                const bool held =
                    std::abs(ctx.position_beats - snapshot.position_beats) <= 1.0e-9;
                ctx.transport_jump = (ctx.is_playing && snapshot.is_playing)
                    ? !continuous
                    : !(continuous || held);
            } else if (has_beat_position) {
                ctx.transport_jump = ctx.position_beats != snapshot.position_beats;
            } else {
                ctx.transport_jump = false;
            }
        }
    }

    snapshot.has_previous = true;
    snapshot.transport_validity = ctx.transport_validity;
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
