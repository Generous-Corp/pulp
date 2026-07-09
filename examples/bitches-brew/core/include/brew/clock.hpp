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

#include <cmath>
#include <cstdint>

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

        const std::int64_t first = ceil_index(start_beats * edges_per_beat_);
        const std::int64_t last = ceil_index(end_beats * edges_per_beat_) - 1;

        for (std::int64_t i = first; i <= last; ++i) {
            if (have_last_index_ && i <= last_index_) continue;

            const double edge_beats = static_cast<double>(i) / edges_per_beat_;
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
                                           double edges_per_beat_) noexcept {
        return static_cast<double>(index) / edges_per_beat_;
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
