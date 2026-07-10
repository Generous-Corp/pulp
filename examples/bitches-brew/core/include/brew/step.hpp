#pragma once

// Step sequencing, derived from the host's position.
//
// Same rule as the clock grid and the LFO's phase: the step index is a pure
// function of elapsed cycles, never a counter advanced one block at a time. Bar 57
// always plays step 3, however the playhead got to bar 57.
//
// That rule collides with the idea of a "random" step value, and the collision is
// resolved in favour of the rule — twice, in two different ways. `Random` here is a
// pure function of the *absolute* step index (a hash, not a generator), so the
// dither is unbounded and non-repeating along the timeline and yet bounces
// identically. The shift register in brew/shift_register.hpp keeps the same promise
// by replaying itself from the origin rather than running from wherever it was.
//
// The cost is that "random" is really "deterministic and unpredictable", which is
// what anyone modulating a filter actually wanted. Reroll it by changing the seed.

#include <brew/gate.hpp>   // SchmittGate, kGateHysteresis
#include <brew/lfo.hpp>    // warp_phase, wrap_phase, to_unipolar
#include <brew/random.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::examples::brew {

/// Steps in a pattern. Distinct from the quantizer's step count, which divides
/// a voltage range rather than a bar.
inline constexpr int kMaxSequencerSteps = 8;

/// How a rate knob is interpreted.
///
/// This is the fork between an LFO and a sequencer, and it is a real choice, not
/// a preference. In `cycle` the whole pattern occupies one cycle, so shortening the
/// cycle makes the steps faster and the modulation keeps its period — it stays an
/// LFO. In `step` each step occupies one cycle, so the pattern's period grows with
/// its length — it becomes a sequencer. Neither can be derived from the other by
/// scaling a knob, because in `cycle` the step duration depends on the length.
enum class SpeedMode : int { cycle = 0, step = 1 };

[[nodiscard]] inline SpeedMode speed_mode_from_param(float v) noexcept {
    return v >= 0.5f ? SpeedMode::step : SpeedMode::cycle;
}

/// How the played-through window of the eight steps is described.
///
/// Two ways to say the same thing, and both are worth having: `start_length` keeps
/// the pattern's *duration* fixed while you slide it, `start_end` keeps its
/// *boundaries* fixed while you change its duration. Automating one of them is a
/// musical gesture; automating the other is a different musical gesture.
enum class LengthMode : int { start_length = 0, start_end = 1 };

inline constexpr int kLengthModeCount = 2;

/// The polarity a step's programmed level reaches the jack in.
enum class Range : int { bipolar = 0, unipolar = 1 };

inline constexpr int kRangeCount = 2;

/// Whether a step arrives or is arrived at.
///
/// `linear` is exactly a full-step glide, which is why it is not a third glide
/// value: it is the glide knob pinned to 1.0, and the plug-in says so rather than
/// growing a second control that does the same arithmetic.
enum class Interpolation : int { stepped = 0, linear = 1 };

inline constexpr int kInterpolationCount = 2;

/// What the plug-in does with a voltage on one of its two input channels.
///
/// `off` by default, and for the same reason a bypassed generator is silent: a DAW
/// will happily hand a modulation plug-in a drum loop at full scale, and a drum
/// loop on the reset input is a sequencer that never leaves step 1.
enum class InputRole : int { off = 0, reset = 1, trigger = 2, signal = 3 };

inline constexpr int kInputRoleCount = 4;

/// Euclidean modulo. `%` on a negative index would return a negative step, and a
/// negative step means the sequencer reads outside its own array before the
/// timeline's origin — which a host will happily ask it to do.
[[nodiscard]] inline constexpr std::int64_t wrap_index(std::int64_t i,
                                                       int modulus) noexcept {
    if (modulus <= 0) return 0;
    const std::int64_t m = static_cast<std::int64_t>(modulus);
    const std::int64_t r = i % m;
    return r < 0 ? r + m : r;
}

/// How many of the eight steps the window covers.
///
/// In `start_end` an end *before* the start is not an error — it wraps around the
/// eight, which is the only reading under which sliding `End` past `Start` keeps
/// producing a pattern rather than an empty one.
[[nodiscard]] inline constexpr int window_length(LengthMode mode, int start, int length,
                                                 int end) noexcept {
    if (mode == LengthMode::start_length) return std::clamp(length, 1, kMaxSequencerSteps);
    const int s = std::clamp(start, 0, kMaxSequencerSteps - 1);
    const int e = std::clamp(end, 0, kMaxSequencerSteps - 1);
    return static_cast<int>(wrap_index(e - s, kMaxSequencerSteps)) + 1;
}

/// Which of the eight programmed steps an absolute step index plays.
[[nodiscard]] inline constexpr int pattern_index(std::int64_t abs_step, int start,
                                                 int window) noexcept {
    const int s = std::clamp(start, 0, kMaxSequencerSteps - 1);
    const std::int64_t within = wrap_index(abs_step, window);
    return static_cast<int>(wrap_index(s + within, kMaxSequencerSteps));
}

/// Cycles, converted to a fractional position along the pattern's steps.
///
/// In `step` mode a cycle *is* a step. In `cycle` mode a cycle is the window, so
/// the window's length sets the step rate — which is what makes it an LFO whose
/// period does not change when you shorten the pattern.
[[nodiscard]] inline constexpr double step_position(double cycles, SpeedMode mode,
                                                    int window) noexcept {
    return mode == SpeedMode::step ? cycles : cycles * static_cast<double>(window);
}

/// Which step of the whole timeline a fractional step position falls in.
/// Unbounded and signed: step -1 is the step before the pattern's origin.
[[nodiscard]] inline std::int64_t absolute_step(double position) noexcept {
    return static_cast<std::int64_t>(std::floor(position));
}

/// How far through its step a position sits, in `[0, 1)`.
[[nodiscard]] inline double step_fraction(double position) noexcept {
    const double f = position - std::floor(position);
    // `floor` is exact, but the subtraction can round up to exactly 1.0 for a
    // position just below an integer boundary. A fraction of 1.0 would place a
    // sample in the *next* step while the index says otherwise.
    return f >= 1.0 ? 0.0 : f;
}

/// A bipolar value in `[-1, +1)`, keyed on the absolute step and a seed.
///
/// Keyed on the *absolute* step, not the wrapped one, so an 8-step pattern does
/// not repeat its randomness every 8 steps — the pattern's shape loops, its
/// dither does not.
[[nodiscard]] inline float step_random(std::int64_t abs_step,
                                       std::uint32_t seed) noexcept {
    return hash_bipolar(abs_step, seed);
}

/// The value of one step: its programmed level, plus a bounded random offset.
///
/// The offset is *added* and the sum clamped, rather than interpolating toward a
/// random target, so `random = 0` is exactly the programmed value — a randomness
/// control that alters the pattern at zero would be unusable.
[[nodiscard]] inline float step_value(float programmed,
                                      std::int64_t abs_step,
                                      float random_amount,
                                      std::uint32_t seed) noexcept {
    if (!(random_amount > 0.0f)) return programmed;
    const float v = programmed + random_amount * step_random(abs_step, seed);
    return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}

/// The glide a mode implies. `linear` is a full-step glide, and nothing else.
[[nodiscard]] inline constexpr double effective_glide(Interpolation interp,
                                                      double glide) noexcept {
    return interp == Interpolation::linear ? 1.0 : std::clamp(glide, 0.0, 1.0);
}

/// Blend from the previous step's value into this one over the first `glide` of
/// the step. `glide = 0` is a hard edge; `glide = 1` never rests on a value.
///
/// Slewing at the *start* of a step rather than across the whole of it means a
/// step still spends most of its time at the level it was programmed to. A
/// sequencer whose steps never arrive is a triangle wave with extra parameters.
[[nodiscard]] inline float glide_toward(float previous, float current,
                                        double fraction, double glide) noexcept {
    if (!(glide > 0.0)) return current;
    if (fraction >= glide) return current;
    const auto t = static_cast<float>(fraction / glide);
    return previous + (current - previous) * t;
}

/// The fraction of a step that carries the step's value. The rest of the step is
/// silent.
///
/// One control, two consequences. It punches a hole in the *control voltage* — the
/// tail of each step falls to zero — and it sets the width of the gate that
/// accompanies it. That is deliberate: a sequencer with a `Gate` that shortened the
/// CV but not the gate, or the other way round, would be describing two different
/// notes.
///
/// At the default of 1.0 nothing is punched out: the CV holds for the whole step
/// and the gate never falls, which is right, because with a full-length step there
/// is no gap between one note and the next to fall into. Turn it down and every
/// step grows a rising edge, which is what an envelope generator downstream needs
/// in order to fire once per step rather than once per phrase. It is the reason you
/// no longer have to program a silent step between every sounding one.
[[nodiscard]] inline constexpr bool step_gate_open(double fraction,
                                                   double gate) noexcept {
    return fraction < gate;
}

/// A step whose gate has closed emits zero, not its programmed level.
[[nodiscard]] inline constexpr float step_gated(float value, double fraction,
                                                double gate) noexcept {
    return step_gate_open(fraction, gate) ? value : 0.0f;
}

/// Map a bipolar step level into the range the jack is set to.
[[nodiscard]] inline constexpr float apply_range(float bipolar, Range r) noexcept {
    return r == Range::unipolar ? to_unipolar(bipolar) : bipolar;
}

// ── Trigger detection ────────────────────────────────────────────────────────

/// The two thresholds an advance trigger has to cross, in normalized full scale.
/// Fixed here, because a sequencer's trigger jack has no threshold knob.
inline constexpr float kTriggerHigh = 0.5f;
inline constexpr float kTriggerLow = kTriggerHigh - kGateHysteresis;

/// A one-sample edge detector at the sequencer's fixed thresholds.
class TriggerDetector {
public:
    void reset() noexcept { gate_.reset(); }

    /// True on the sample the input crosses `kTriggerHigh` going up, and not again
    /// until it has fallen back below `kTriggerLow`.
    [[nodiscard]] bool process(float v) noexcept {
        return gate_.process(v, kTriggerHigh, kTriggerLow);
    }

private:
    SchmittGate gate_;
};

}  // namespace pulp::examples::brew
