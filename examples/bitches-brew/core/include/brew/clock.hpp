#pragma once

// The clock edge grid: which clock pulses fall inside this block?
//
// The whole design turns on one decision — **edges are derived from the host's
// reported position, never accumulated**. A phase accumulator that advances by
// one block per callback has to be "caught up" whenever the host moves the
// playhead, and catching up is what produces a burst of pulses at the moment the
// transport starts. There is nothing to catch up here: beat 3.7 always maps to
// the same edge index, no matter how the playhead arrived there.
//
// The grid is a pure function of position, so the only state it keeps is the
// bookkeeping needed to *not emit an edge twice*:
//
//   - `last_index_` dedupes a host that renders the same block twice (some do,
//     around loop points and when a plug-in is inserted mid-playback).
//   - a strictly-backwards block start (loop wrap, rewind) clears that dedupe,
//     because the timeline really did move and those edges are new again.
//
// Note the dedupe is keyed on *block start*, not on "did the position move
// backwards at all". A repeated block starts at the same beat as before, so it
// dedupes; a loop wrap starts strictly earlier, so it re-arms. Getting that
// distinction backwards makes one of the two cases fail, and they look alike.
//
// `transport_jump` is deliberately ignored. Hosts set it spuriously, and an edge
// grid that reads only the position cannot be hurt by a lie about how the
// position changed.
//
// Tempo is treated as constant across a block, because that is all Pulp's
// transport currently reports; a host that ramps tempo within a block will place
// edges up to one block late. Position-derived means the error does not
// accumulate — the next block lands back on the grid.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace pulp::examples::brew {

/// Beats per sample at a given tempo. Zero (or nonsense) tempo yields zero, which
/// callers treat as "no edges this block" rather than dividing by it.
[[nodiscard]] inline double beats_per_sample(double tempo_bpm,
                                             double sample_rate) noexcept {
    if (!(tempo_bpm > 0.0) || !(sample_rate > 0.0)) return 0.0;
    return tempo_bpm / (60.0 * sample_rate);
}

/// How many clock edges fall in one beat: the DIN-sync pulse count, scaled by the
/// user's multiplier and divisor. Returns 0 for a degenerate setting.
[[nodiscard]] inline double edges_per_beat(double pulses_per_beat,
                                           int multiplier,
                                           int divisor) noexcept {
    if (!(pulses_per_beat > 0.0) || multiplier < 1 || divisor < 1) return 0.0;
    return pulses_per_beat * static_cast<double>(multiplier) /
           static_cast<double>(divisor);
}

// Swing is a monotonic warp of the beat axis, applied *before* the edge search.
//
// The tempting implementation is to find the straight edges and then nudge the
// odd ones later. It does not survive a locate: the nudge is a function of which
// edge you are on, so an edge found near a block boundary can be pushed into the
// previous block, or emitted twice, depending on where the playhead happened to
// enter. Warping the axis keeps every edge a pure function of position, which is
// the invariant the whole clock rests on.
//
// So: `swing_warp` maps a straight beat to the beat it should sound at, and
// `swing_unwarp` inverts it. The grid un-warps the block's bounds, searches the
// straight grid it already knows how to search, and warps each edge back to find
// its sample. Both maps are piecewise linear, strictly increasing, and fix every
// multiple of the pair period — so a swung clock still lands exactly on the
// downbeat.

/// One eighth note, in beats. The pair period is two of these: swing pushes the
/// off-eighth back within each quarter.
inline constexpr double kEighthBeats = 0.5;
/// One sixteenth note, in beats.
inline constexpr double kSixteenthBeats = 0.25;

/// Below 0.5 the off-beat rushes, above it drags. The bounds keep the warp
/// invertible: at exactly 0 or 1 the beat axis collapses, half of every pair has
/// zero length, and `swing_unwarp` divides by zero. One percent short of each end
/// is as far as the shuffle can be pushed and still be undone, which is far enough
/// that the off-beat lands all but on top of its neighbour.
inline constexpr double kMinSwing = 0.01;
inline constexpr double kMaxSwing = 0.99;

struct Swing {
    /// The subdivision whose off-beat moves. Zero or negative means no swing.
    double unit_beats = 0.0;
    /// The fraction of each pair the *first* half occupies. 0.5 is straight.
    double amount = 0.5;
};

/// Exactly 0.5 is straight, and must be bit-identical to no swing at all — a
/// clock that drifts by an ulp when the user leaves the knob alone is a bug that
/// only shows up as a failed bit-exactness test months later.
[[nodiscard]] inline bool swing_active(const Swing& s) noexcept {
    return s.unit_beats > 0.0 && s.amount != 0.5;
}

namespace detail {
/// Split a beat into the pair it falls in and its phase within that pair.
[[nodiscard]] inline double pair_phase(double beats, double period,
                                       double& pair_start) noexcept {
    pair_start = std::floor(beats / period) * period;
    return beats - pair_start;
}
}  // namespace detail

/// Straight beats to sounding beats.
[[nodiscard]] inline double swing_warp(double beats, const Swing& sw) noexcept {
    if (!swing_active(sw)) return beats;
    const double a = std::clamp(sw.amount, kMinSwing, kMaxSwing);
    const double period = 2.0 * sw.unit_beats;
    double pair_start = 0.0;
    const double p = detail::pair_phase(beats, period, pair_start);
    const double q = p < sw.unit_beats
                         ? p * (2.0 * a)
                         : a * period + (p - sw.unit_beats) * (2.0 * (1.0 - a));
    return pair_start + q;
}

/// Sounding beats back to straight beats. The inverse of `swing_warp`.
[[nodiscard]] inline double swing_unwarp(double beats, const Swing& sw) noexcept {
    if (!swing_active(sw)) return beats;
    const double a = std::clamp(sw.amount, kMinSwing, kMaxSwing);
    const double period = 2.0 * sw.unit_beats;
    double pair_start = 0.0;
    const double q = detail::pair_phase(beats, period, pair_start);
    const double p = q < a * period
                         ? q / (2.0 * a)
                         : sw.unit_beats + (q - a * period) / (2.0 * (1.0 - a));
    return pair_start + p;
}

class ClockGrid {
public:
    /// Forget the dedupe state. Call on the transport's play edge, so a run that
    /// restarts at the same position emits the same edges as the first run.
    void reset() noexcept {
        have_prev_start_ = false;
        have_last_index_ = false;
    }

    /// Emit every edge whose beat lies in `[start_beats, start_beats + span)`,
    /// where the span is `num_samples` long at the given tempo.
    ///
    /// `on_edge(sample_offset, edge_index)` is called in ascending order. It runs
    /// on the audio thread: it must not allocate or lock.
    ///
    /// The interval is half-open and closed at the start, so playing from beat 0
    /// puts an edge at sample 0 — which is what a user means by "the pulse on the
    /// downbeat" — while an edge at the block's end belongs to the next block.
    template <class OnEdge>
    void advance(double start_beats,
                 double edges_per_beat_,
                 double beats_per_sample_,
                 int num_samples,
                 OnEdge&& on_edge) {
        advance(start_beats, edges_per_beat_, beats_per_sample_, num_samples,
                Swing{}, std::forward<OnEdge>(on_edge));
    }

    /// As above, with the beat axis swung. `start_beats` and the block span are
    /// in sounding time — the host's timeline — and the edges come back at the
    /// samples they sound at.
    template <class OnEdge>
    void advance(double start_beats,
                 double edges_per_beat_,
                 double beats_per_sample_,
                 int num_samples,
                 const Swing& swing,
                 OnEdge&& on_edge) {
        // A backwards jump makes previously-emitted indices reachable again.
        // A *repeated* block starts at the same beat, so it does not trip this.
        if (have_prev_start_ && start_beats < prev_start_beats_ - kBeatEpsilon)
            have_last_index_ = false;
        prev_start_beats_ = start_beats;
        have_prev_start_ = true;

        if (num_samples <= 0 || !(edges_per_beat_ > 0.0) ||
            !(beats_per_sample_ > 0.0))
            return;

        const double end_beats =
            start_beats + beats_per_sample_ * static_cast<double>(num_samples);

        // Search the straight grid. The warp is strictly increasing, so an edge
        // sounds inside this block exactly when its straight beat lies inside the
        // un-warped bounds — and the index domain is untouched, which is what
        // keeps the dedupe and the backwards-jump re-arm meaningful.
        const double straight_start = swing_unwarp(start_beats, swing);
        const double straight_end = swing_unwarp(end_beats, swing);

        const std::int64_t first = ceil_index(straight_start * edges_per_beat_);
        const std::int64_t last = ceil_index(straight_end * edges_per_beat_) - 1;

        for (std::int64_t i = first; i <= last; ++i) {
            if (have_last_index_ && i <= last_index_) continue;

            const double edge_beats =
                swing_warp(static_cast<double>(i) / edges_per_beat_, swing);
            const double offset =
                (edge_beats - start_beats) / beats_per_sample_;

            int sample = static_cast<int>(offset + 0.5);
            if (sample < 0) sample = 0;
            if (sample >= num_samples) sample = num_samples - 1;

            on_edge(sample, i);
            last_index_ = i;
            have_last_index_ = true;
        }
    }

    /// The beat at which edge `index` falls. Used to test an edge against a
    /// run-segment gate without re-deriving it from the sample offset (which has
    /// been rounded, and so cannot be compared exactly).
    [[nodiscard]] static double edge_beats(std::int64_t index,
                                           double edges_per_beat_,
                                           const Swing& swing = {}) noexcept {
        return swing_warp(static_cast<double>(index) / edges_per_beat_, swing);
    }

private:
    // Slack in the *index* domain. A host whose reported position wobbles in the
    // last ulp must not straddle an integer boundary and emit an edge twice, or
    // skip it. One part in a million of an edge is far below any real jitter and
    // far above double's rounding error at musical beat counts.
    static constexpr double kIndexEpsilon = 1e-6;
    static constexpr double kBeatEpsilon = 1e-9;

    /// Smallest integer index at or after `x`, with tolerance: a value a hair
    /// below an integer counts as that integer.
    [[nodiscard]] static std::int64_t ceil_index(double x) noexcept {
        return static_cast<std::int64_t>(std::ceil(x - kIndexEpsilon));
    }

    bool have_prev_start_ = false;
    double prev_start_beats_ = 0.0;
    bool have_last_index_ = false;
    std::int64_t last_index_ = 0;
};

}  // namespace pulp::examples::brew
