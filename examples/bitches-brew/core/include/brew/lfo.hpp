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

#include <brew/clock.hpp>   // Swing, swing_unwarp
#include <brew/random.hpp>

#include <algorithm>
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

/// A full turn, in radians. `M_PI` is a POSIX extension: MSVC's `<cmath>` omits it
/// unless `_USE_MATH_DEFINES` is defined before every include of it, which a header
/// cannot guarantee for its includers. Spelling the constant is portable and exact.
inline constexpr double kTau = 6.283185307179586476925286766559;

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
            return static_cast<float>(std::sin(kTau * p));
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

/// Bend the time axis so the waveform's centre falls at `centre` rather than at
/// half a cycle. This is a pulse-width control generalized to every shape: at
/// 0.25 a triangle becomes a fast-rise slow-fall ramp, a sine leans, and a square
/// spends a quarter of its cycle high.
///
/// Continuous and monotone in `phase`, so it never introduces a discontinuity a
/// clean waveform did not already have. Degenerate centres are refused rather
/// than divided by.
[[nodiscard]] inline double warp_phase(double phase, double centre) noexcept {
    const double p = wrap_phase(phase);
    const double c = std::clamp(centre, 1e-4, 1.0 - 1e-4);
    return p < c ? 0.5 * p / c : 0.5 + 0.5 * (p - c) / (1.0 - c);
}

/// A square with an explicit duty cycle. `pulse_width` is the fraction of the
/// cycle spent high.
[[nodiscard]] inline float square_shape(double phase, double pulse_width) noexcept {
    return wrap_phase(phase) < std::clamp(pulse_width, 0.0, 1.0) ? 1.0f : -1.0f;
}

/// How much of each shape is summed into the output.
///
/// A mixer, not a selector. Four depths subsume the five-way enum this replaced
/// (set one to 1.0 and the rest to 0) and reach everything between. Each depth is
/// bipolar, so a shape can be subtracted as easily as added.
///
/// `random` is a sample-and-hold: one level per cycle, held flat across it, in the
/// manner of a noise source feeding a hardware S&H. It is a hash of the cycle
/// index rather than a generator, so it renders identically every time — see
/// brew/random.hpp.
struct LfoMix {
    float sine = 1.0f;
    float triangle = 0.0f;
    float saw = 0.0f;
    float square = 0.0f;
    float random = 0.0f;
    float pulse_width = 0.5f;
    float asymmetry = 0.5f;
    float offset = 0.0f;
    std::uint32_t seed = 0;
};

/// The mixed waveform at a phase, before the output stage.
///
/// The sum is *not* clamped here. Four depths at full can reach 4.0, and clamping
/// mid-chain would silently flatten a mix the user asked for before `offset` and
/// the output scale have had their say. `resolve_output` clamps once, at the jack.
[[nodiscard]] inline float lfo_mix_value(const LfoMix& m, double phase,
                                         std::int64_t cycle) noexcept {
    const double p = warp_phase(phase, static_cast<double>(m.asymmetry));
    float v = 0.0f;
    if (m.sine != 0.0f) v += m.sine * lfo_shape(Waveform::sine, p);
    if (m.triangle != 0.0f) v += m.triangle * lfo_shape(Waveform::triangle, p);
    if (m.saw != 0.0f) v += m.saw * lfo_shape(Waveform::saw_up, p);
    if (m.square != 0.0f) v += m.square * square_shape(p, static_cast<double>(m.pulse_width));
    // Held across the whole cycle, so it is keyed on the cycle, not the phase.
    if (m.random != 0.0f) v += m.random * hash_bipolar(cycle, m.seed);
    return v + m.offset;
}

/// How the rate knob is read.
///
/// `tempo` locks the LFO to the host's musical grid; `free` runs it in hertz,
/// independent of tempo. Free-running does *not* mean accumulating: the phase is
/// still a pure function of the host's absolute sample position, so a bounce is
/// bit-identical and a locate lands where the timeline says. An LFO that
/// accumulated would drift against the host over a long session, and render
/// differently every time.
enum class RateMode : int { tempo = 0, free = 1 };

[[nodiscard]] inline RateMode rate_mode_from_param(float v) noexcept {
    return v >= 0.5f ? RateMode::free : RateMode::tempo;
}

/// Slow enough to sweep a filter over a minute; fast enough to reach the bottom
/// of the audio band, where a CV becomes a tone.
inline constexpr double kMinFreeHz = 0.01;
inline constexpr double kMaxFreeHz = 40.0;

/// Elapsed cycles at a point on the timeline — the LFO's one time coordinate.
///
/// Both modes reduce to this, which is why the shapes, the phase offset, the
/// quadrature and the sample-and-hold need to know nothing about tempo.
/// Swing warps the beat timeline, so it is applied to the *position* before the
/// position becomes a phase — not to the phase afterwards. `swing_unwarp` maps a
/// sounding beat back to the straight beat it stands for, which is exactly the
/// coordinate the cycle count is measured in.
///
/// It has no meaning in free-run mode. Swing is a subdivision of a beat, and a
/// free-running LFO has no beats; a hertz rate that shuffled would just be a
/// wrong hertz rate. The parameter is ignored there rather than approximated.
[[nodiscard]] inline double lfo_cycles(RateMode mode, double position_beats,
                                       double position_seconds,
                                       double beats_per_cycle,
                                       double free_hz,
                                       const Swing& swing = {}) noexcept {
    if (mode == RateMode::free)
        return position_seconds * std::clamp(free_hz, kMinFreeHz, kMaxFreeHz);
    if (!(beats_per_cycle > 0.0)) return 0.0;
    return swing_unwarp(position_beats, swing) / beats_per_cycle;
}

/// The phase within the current cycle, given elapsed cycles and an offset.
[[nodiscard]] inline double phase_at(double cycles, double offset) noexcept {
    return wrap_phase(cycles + offset);
}

/// Which cycle of the whole timeline the offset position falls in. Signed:
/// cycle -1 precedes the project's origin, and a host will ask for it.
///
/// It takes the *same* offset as `phase_at`, deliberately. Keying the
/// sample-and-hold on an un-offset cycle index while the waveform's phase is
/// offset makes the held value step in the middle of the visible cycle rather
/// than where the shape wraps.
[[nodiscard]] inline std::int64_t cycle_at(double cycles, double offset) noexcept {
    return static_cast<std::int64_t>(std::floor(cycles + offset));
}

/// Map bipolar [-1, +1] to unipolar [0, 1]. Some CV inputs (a VCA, an envelope
/// depth) want only positive voltage; feeding them a bipolar LFO wastes half the
/// travel and, on a VCA, silences half the cycle.
[[nodiscard]] inline constexpr float to_unipolar(float bipolar) noexcept {
    return (bipolar + 1.0f) * 0.5f;
}

}  // namespace pulp::examples::brew
