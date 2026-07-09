#pragma once

// Turning clock edges into pulses a piece of hardware will actually see.
//
// An edge is an instant; a gate input needs a *width*. Two physical constraints
// bound that width, and neither is obvious from the software side:
//
//   Floor. A one-sample pulse does not survive a DAC's reconstruction filter. It
//   comes out as a blip too small and too brief to trip a gate input, so the
//   plug-in looks broken while the samples look perfect. Roughly a millisecond is
//   the smallest pulse worth emitting.
//
//   Ceiling. If the width reaches the pulse period, the gate never falls and the
//   downstream module sees one continuous high — a welded gate. This is easy to
//   hit by accident: at 24 ppqn and 300 BPM the period is 8.3 ms, so a
//   perfectly reasonable-looking 10 ms trigger length welds it.
//
// The ceiling wins when the two conflict. A pulse that is shorter than the floor
// is a weak trigger; a pulse that is longer than the period is *no* trigger at
// all, plus a stuck note.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::examples::brew {

/// The shortest pulse worth emitting, in seconds. Below this a DAC's
/// reconstruction filter swallows the edge.
inline constexpr double kMinTriggerSeconds = 0.001;

/// Samples between consecutive clock edges. Zero for a degenerate setting.
[[nodiscard]] inline double pulse_period_samples(double sample_rate,
                                                 double tempo_bpm,
                                                 double edges_per_beat) noexcept {
    if (!(sample_rate > 0.0) || !(tempo_bpm > 0.0) || !(edges_per_beat > 0.0))
        return 0.0;
    return sample_rate * 60.0 / (tempo_bpm * edges_per_beat);
}

/// The pulse width actually emitted for a requested trigger length, in samples.
///
/// Clamped to at least ~1 ms and to strictly less than half the pulse period, so
/// the gate always falls before the next edge raises it. When the period is so
/// short that even the 1 ms floor would weld the gate, the ceiling wins and the
/// pulse is narrower than the floor: a weak trigger beats a stuck one.
///
/// Always returns at least 1 — a zero-width pulse is not a pulse.
[[nodiscard]] inline std::int64_t trigger_width_samples(
    double requested_ms, double sample_rate, double period_samples) noexcept {
    if (!(sample_rate > 0.0)) return 1;

    // Strictly below 50% of the period: `ceil(p/2) - 1` leaves 49 samples for a
    // 100-sample period, where `floor(p/2)` would leave exactly 50%.
    std::int64_t ceiling =
        period_samples > 0.0
            ? static_cast<std::int64_t>(std::ceil(period_samples * 0.5)) - 1
            : 0;
    if (ceiling < 1) ceiling = 1;

    std::int64_t floor_w =
        static_cast<std::int64_t>(std::llround(sample_rate * kMinTriggerSeconds));
    if (floor_w < 1) floor_w = 1;
    if (floor_w > ceiling) floor_w = ceiling;  // the ceiling always wins

    std::int64_t requested = static_cast<std::int64_t>(
        std::llround(requested_ms * sample_rate / 1000.0));

    return std::clamp(requested, floor_w, ceiling);
}

/// Holds a gate high for a fixed number of samples, across block boundaries.
///
/// A pulse triggered near the end of a block must finish in the next one, so the
/// countdown is state that survives the callback. It is reset on the transport's
/// play edge, never mid-run.
class PulseShaper {
public:
    void reset() noexcept { remaining_ = 0; }

    /// (Re)start the pulse. Retriggering before the previous pulse ends restarts
    /// it rather than extending it — though `trigger_width_samples` guarantees
    /// that cannot happen from the clock grid itself.
    void trigger(std::int64_t width_samples) noexcept {
        remaining_ = width_samples > 0 ? width_samples : 1;
    }

    /// Whether the gate is currently high.
    [[nodiscard]] bool high() const noexcept { return remaining_ > 0; }

    /// Consume one sample of the countdown and report the level *for that sample*.
    [[nodiscard]] bool tick() noexcept {
        if (remaining_ <= 0) return false;
        --remaining_;
        return true;
    }

private:
    std::int64_t remaining_ = 0;
};

}  // namespace pulp::examples::brew
