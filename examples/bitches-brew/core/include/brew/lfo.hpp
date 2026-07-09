#pragma once

// LFO shapes and phase, derived from the host's position.
//
// Same decision as the clock grid (brew/clock.hpp), for the same reason: the
// phase is a pure function of `position_beats`, never an accumulator advanced one
// block at a time. An accumulator drifts against the host over a long session,
// has to be re-synced on every locate, and lands somewhere arbitrary when the
// user hits play at bar 57. Deriving it means bar 57 always sounds like bar 57.
//
// It also means the LFO is exactly reproducible: bounce the same project twice
// and the modulation lands on the same samples. For a CV signal patched into a
// filter cutoff, that is the difference between a repeatable take and a
// suspicious one.
//
// Everything here returns bipolar [-1, +1]. The unipolar conversion is one
// multiply at the output stage, and keeping the shapes bipolar means `invert`
// and `output_scale` compose the same way they do for every other plug-in.

#include <cmath>
#include <cstdint>

namespace pulp::examples::brew {

/// The shapes. Ordered so the enum's integer value can be a parameter, and so
/// inserting a shape later would renumber a saved preset — hence: append only.
enum class Waveform : int {
    sine = 0,
    triangle = 1,
    saw_up = 2,
    saw_down = 3,
    square = 4,
};

inline constexpr int kWaveformCount = 5;

/// Coerce a parameter float to a Waveform, clamping rather than wrapping. A host
/// handing back an out-of-range value must not silently select a different shape.
[[nodiscard]] inline Waveform waveform_from_param(float v) noexcept {
    const int i = static_cast<int>(std::lround(v));
    if (i < 0) return Waveform::sine;
    if (i >= kWaveformCount) return static_cast<Waveform>(kWaveformCount - 1);
    return static_cast<Waveform>(i);
}

/// Wrap a phase into [0, 1). `std::fmod` keeps the sign of its argument, so a
/// negative phase offset would otherwise index the wrong half of the cycle.
[[nodiscard]] inline double wrap_phase(double p) noexcept {
    p = std::fmod(p, 1.0);
    return p < 0.0 ? p + 1.0 : p;
}

/// The LFO's phase at a position on the host timeline.
///
/// `beats_per_cycle` is the musical rate: 1.0 is one cycle per quarter note, 4.0
/// is one per bar of 4/4. Returns 0 for a degenerate rate rather than dividing.
[[nodiscard]] inline double lfo_phase(double position_beats,
                                      double beats_per_cycle,
                                      double phase_offset = 0.0) noexcept {
    if (!(beats_per_cycle > 0.0)) return 0.0;
    return wrap_phase(position_beats / beats_per_cycle + phase_offset);
}

/// Evaluate a shape at a phase, bipolar in [-1, +1].
///
/// Sine and both saws cross zero rising at phase 0; triangle starts at its
/// trough and peaks at half a cycle; square starts high. These are the
/// conventional phase relationships, and they are what the quadrature output
/// below assumes.
[[nodiscard]] inline float lfo_shape(Waveform w, double phase) noexcept {
    const double p = wrap_phase(phase);
    switch (w) {
        case Waveform::sine:
            return static_cast<float>(std::sin(2.0 * M_PI * p));
        case Waveform::triangle:
            return static_cast<float>(1.0 - 4.0 * std::abs(p - 0.5));
        case Waveform::saw_up:
            return static_cast<float>(2.0 * p - 1.0);
        case Waveform::saw_down:
            return static_cast<float>(1.0 - 2.0 * p);
        case Waveform::square:
            return p < 0.5 ? 1.0f : -1.0f;
    }
    return 0.0f;
}

/// A quarter cycle ahead. Patched alongside the main output it gives a circular
/// (X, Y) pair — the usual way to drive a two-axis modulation from one LFO.
inline constexpr double kQuadratureOffset = 0.25;

/// Map bipolar [-1, +1] to unipolar [0, 1]. Some CV inputs (a VCA, an envelope
/// depth) want only positive voltage; feeding them a bipolar LFO wastes half the
/// travel and, on a VCA, silences half the cycle.
[[nodiscard]] inline constexpr float to_unipolar(float bipolar) noexcept {
    return (bipolar + 1.0f) * 0.5f;
}

}  // namespace pulp::examples::brew
