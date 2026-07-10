#pragma once

// Quantization: snap a continuous control voltage to discrete values.
//
// This is the half of a quantizer that needs no calibration. The full-scale range
// `[-1, +1]` is divided into N equal steps and the input snaps to the nearest
// one. That is a complete, useful behavior — it turns a smooth LFO into a
// staircase, a portamento into a glissando, a random walk into a sequence.
//
// It also restricts the output to a musical scale, and that needs no calibration
// either — see brew/scale.hpp. What *does* need calibration is telling the
// plug-in which voltage is a semitone, and that is `Calibrated` mode, which this
// suite does not have because the interface's full-scale voltage has not been
// measured. Until it is, the user sets the step count so that twelve lattice
// steps span an octave of their oscillator, and everything downstream is exact.
//
// The staircase is a pure function of its input, so an editor can draw it, tests
// can pin it, and no state can drift.

#include <brew/cv.hpp>
#include <brew/scale.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::examples::brew {

/// How the lattice is chosen. `Calibrated` is deliberately absent: it needs the
/// interface's full-scale voltage, and inventing that number would put a wrong
/// pitch on a patch cable.
enum class QuantMode : int {
    manual = 0,  ///< Equal divisions of full scale.
    scale = 1,   ///< ...restricted to the notes of a musical scale.
};

inline constexpr int kQuantModeCount = 2;

/// Fewer than two steps is not a staircase, and a division by a step size of zero
/// is not a quantizer. Named for the quantizer because the sequencer counts steps
/// too, and they are not the same steps.
inline constexpr float kMinQuantizeSteps = 2.0f;
inline constexpr float kMaxQuantizeSteps = 64.0f;

/// Everything the quantizer does to one sample.
struct QuantizeSettings {
    QuantMode mode = QuantMode::manual;
    /// Divisions of the full `[-1, +1]` range. Fractional: `Fine` adds to the
    /// coarse `Steps` knob, and the reference's is fractional too — a
    /// non-integer count simply puts the rails between lattice points.
    float steps = 12.0f;
    /// Where the lattice sits within a step, in step widths. At 0 a lattice point
    /// falls exactly on zero volts; at 0.5 zero falls between two of them.
    float offset = 0.0f;
    /// Shift the chosen step. In `manual` mode this is a *lattice* shift by whole
    /// steps — the only kind of transpose that keeps a quantized signal
    /// quantized. In `scale` mode it shifts by whole *scale degrees*, so +7 in a
    /// seven-note scale is an octave.
    float transpose = 0.0f;
    /// Which notes survive, in `scale` mode. Ignored in `manual`.
    Scale scale = Scale::chromatic;
    /// The scale's root, as a lattice step. `Key` and `Key Offset` are summed
    /// into it: two controls so a pattern can be automated around a root.
    int root = 0;
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

    if (s.mode == QuantMode::scale) {
        // The lattice is a chromatic scale once twelve steps span an octave, so
        // the restriction is arithmetic on the step index and never touches volts.
        // Transpose moves by scale degrees here, not by steps.
        const int i = static_cast<int>(index);
        const int moved = transpose_in_scale(i, s.scale, s.root,
                                             static_cast<int>(std::lround(s.transpose)));
        return std::clamp((static_cast<double>(moved) + off) * w, -1.0, 1.0);
    }

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
