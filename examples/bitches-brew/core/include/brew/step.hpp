#pragma once

// Step sequencing, derived from the host's position.
//
// Same rule as the clock grid and the LFO's phase: the step index is a pure
// function of `position_beats`, never a counter advanced one block at a time.
// Bar 57 always plays step 3, however the playhead got to bar 57.
//
// That rule collides with the idea of a "random" step value, and the collision is
// resolved in favour of the rule. Randomness here is a pure function of the
// *absolute* step index — a hash, not a generator — so the sequence is unbounded
// and non-repeating along the timeline, and yet bouncing the same project twice
// produces the same samples. A conventional RNG advanced once per step could
// promise neither: it drifts on a locate, and it renders differently every time.
//
// The cost is that "random" is really "deterministic and unpredictable", which is
// what anyone modulating a filter actually wanted. Reroll it by changing the seed.

#include <brew/random.hpp>

#include <cmath>
#include <cstdint>

namespace pulp::examples::brew {

/// Steps in a pattern. Distinct from the quantizer's step count, which divides
/// a voltage range rather than a bar.
inline constexpr int kMaxSequencerSteps = 8;

/// How a rate knob is interpreted.
///
/// This is the fork between an LFO and a sequencer, and it is a real choice, not
/// a preference. In `cycle` the whole pattern occupies the rate, so shortening it
/// makes the steps faster and the modulation keeps its period — it stays an LFO.
/// In `step` each step occupies the rate, so the pattern's period grows with its
/// length — it becomes a sequencer. Neither can be derived from the other by
/// scaling a knob, because in `cycle` the step duration depends on the length.
enum class SpeedMode : int { cycle = 0, step = 1 };

[[nodiscard]] inline SpeedMode speed_mode_from_param(float v) noexcept {
    return v >= 0.5f ? SpeedMode::step : SpeedMode::cycle;
}

/// Beats occupied by one step, given the rate knob and how it is read.
[[nodiscard]] inline double beats_per_step(SpeedMode mode, double rate_beats,
                                           int length) noexcept {
    if (!(rate_beats > 0.0) || length < 1) return 0.0;
    return mode == SpeedMode::step ? rate_beats
                                   : rate_beats / static_cast<double>(length);
}

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

/// Which step of the whole timeline a beat position falls in. Unbounded and
/// signed: step -1 is the step before the project's origin.
[[nodiscard]] inline std::int64_t absolute_step(double position_beats,
                                                double beats_per_step) noexcept {
    if (!(beats_per_step > 0.0)) return 0;
    return static_cast<std::int64_t>(std::floor(position_beats / beats_per_step));
}

/// How far through its step a position sits, in `[0, 1)`.
[[nodiscard]] inline double step_fraction(double position_beats,
                                          double beats_per_step) noexcept {
    if (!(beats_per_step > 0.0)) return 0.0;
    const double x = position_beats / beats_per_step;
    const double f = x - std::floor(x);
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

/// The gate that accompanies each step: high for the first half of it.
///
/// Half is a choice, not a discovery. It gives an envelope generator downstream a
/// rising edge on every step and a falling edge before the next, at any rate,
/// which is the property that makes a gate useful.
[[nodiscard]] inline constexpr bool step_gate(double fraction) noexcept {
    return fraction < 0.5;
}

}  // namespace pulp::examples::brew
