#pragma once

// Quantization: snap a continuous control voltage to discrete values.
//
// This is the half of a quantizer that needs no calibration. The full-scale range
// `[-1, +1]` is divided into N equal steps and the input snaps to the nearest
// one. That is a complete, useful behavior — it turns a smooth LFO into a
// staircase, a portamento into a glissando, a random walk into a sequence.
//
// It is NOT a musical scale quantizer, and this file will not pretend to be one.
// A semitone is a fixed *voltage* (1/12 V on a 1V/oct input), and mapping that to
// a sample value requires knowing the interface's full-scale voltage. Nothing in
// this suite knows that yet. Until it is measured, "12 steps" means twelve equal
// divisions of full scale, which lands on semitones only by coincidence.
//
// The staircase is a pure function of its input, so an editor can draw it, tests
// can pin it, and no state can drift.

#include <brew/cv.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::examples::brew {

/// Fewer than two steps is not a staircase, and a division by a step size of zero
/// is not a quantizer. Named for the quantizer because the sequencer counts steps
/// too, and they are not the same steps.
inline constexpr float kMinQuantizeSteps = 2.0f;
inline constexpr float kMaxQuantizeSteps = 64.0f;

/// Everything the quantizer does to one sample.
struct QuantizeSettings {
    /// Divisions of the full `[-1, +1]` range. Fractional: `Fine` adds to the
    /// coarse `Steps` knob, and the reference's is fractional too — a
    /// non-integer count simply puts the rails between lattice points.
    float steps = 12.0f;
    /// Where the lattice sits within a step, in step widths. At 0 a lattice point
    /// falls exactly on zero volts; at 0.5 zero falls between two of them.
    float offset = 0.0f;
    /// Shift the chosen step by whole steps. This is a *lattice* shift, not a
    /// voltage offset: it moves the output by an exact number of steps, which is
    /// the only kind of transpose that keeps a quantized signal quantized.
    float transpose = 0.0f;
    float out_scale = 1.0f;
    bool invert = false;
};

/// The width of one step, in full-scale units.
[[nodiscard]] inline double step_width(float steps) noexcept {
    return 2.0 / static_cast<double>(std::clamp(steps, kMinQuantizeSteps, kMaxQuantizeSteps));
}

/// Snap `x` to the nearest lattice point, before the output stage.
///
/// `std::round` rather than `std::floor(t + 0.5)`. Round breaks ties away from
/// zero, which makes it an odd function — `round(-t) == -round(t)` — so the
/// staircase is symmetric about the origin. Floor-of-plus-half breaks every tie
/// toward positive infinity instead: half a step above zero would snap up to the
/// first step while half a step below snapped up to zero, putting a DC offset on a
/// bipolar CV. It also loses the negative rail, because -1.0 sits exactly on a tie
/// whenever the step count is odd.
[[nodiscard]] inline double quantize_value(double x,
                                           const QuantizeSettings& s) noexcept {
    const double w = step_width(s.steps);
    const double off = static_cast<double>(s.offset);
    const double index = std::round(x / w - off);
    const double snapped = (index + off + static_cast<double>(s.transpose)) * w;
    return std::clamp(snapped, -1.0, 1.0);
}

/// Input to jack, through the suite's shared output stage.
///
/// The input is not clamped on the way in: `quantize_value` already clamps the
/// snapped result to the rails, so an over-range input lands on the rail exactly
/// as a full-scale one does.
[[nodiscard]] inline float quantize_transfer(float x,
                                             const QuantizeSettings& s) noexcept {
    return resolve_output(static_cast<float>(quantize_value(static_cast<double>(x), s)),
                          s.out_scale, s.invert);
}

}  // namespace pulp::examples::brew
