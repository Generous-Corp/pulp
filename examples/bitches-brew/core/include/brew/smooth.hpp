#pragma once

// Smoothing, but only when asked for.
//
// The suite's rule is that nothing smooths, because a smoothed control voltage is
// a *wrong* voltage: for as long as the ramp lasts, the number the plug-in shows
// and the number at the jack disagree. That rule is about **implicit** smoothing —
// the kind a framework applies to a parameter change behind the user's back.
//
// An explicit control is a different thing entirely. A user who dials in 40 ms of
// slew is asking for a portamento, and a modular has slew limiters in it for
// exactly that reason. So the invariant is not "nothing smooths"; it is *nothing
// smooths unless the user asked*. At zero, this file is a wire — the same float
// out as in, bit for bit — and the bit-exactness tests still hold.
//
// Two modes, because they sound different and both are wanted:
//
//   - **Slew** (positive): a hard limit on how far the value may move per sample.
//     Constant *rate*: a big jump takes proportionally longer. This is what a
//     hardware slew limiter does, and what makes a portamento sound like one.
//   - **Low-pass** (negative): a one-pole filter. Constant *time*: every jump
//     takes about the same time to settle, and it never quite arrives. Rounds the
//     corners rather than ramping the edges.
//
// The control is calibrated in milliseconds for a **full-scale swing** (-1 to +1),
// so a number that means something: at 100 ms, going from one rail to the other
// takes 100 ms. Halving the distance halves the time.

#include <algorithm>
#include <cmath>

namespace pulp::examples::brew {

/// The full-scale distance the slew rate is calibrated against.
inline constexpr double kFullSwing = 2.0;

/// Below this the one-pole's tail is flushed to zero.
///
/// A one-pole never arrives: it approaches its target geometrically, and when the
/// target is zero it eventually lands in the denormal range, where every multiply
/// costs an order of magnitude more than a normal one — a control-rate filter that
/// gets *slower* the quieter it is. The floor is thirty-odd orders of magnitude
/// below any voltage anyone can hear or measure, and eight orders above the first
/// denormal, so nothing real is ever rounded away.
inline constexpr float kDenormalFloor = 1e-30f;

/// A one-sample smoother. One per channel — sharing one across channels makes
/// each channel filter the last one's samples.
class Smoother {
public:
    void reset(float value = 0.0f) noexcept {
        state_ = value;
        coeff_ms_ = 0.0f;   // force the coefficient to be recomputed
    }

    [[nodiscard]] float value() const noexcept { return state_; }

    /// Advance one sample toward `target`.
    ///
    /// `ms == 0` is a wire: the target passes through untouched and the state
    /// follows it, so releasing the control never produces a jump from a stale
    /// value. Positive slews at a constant rate; negative low-passes.
    [[nodiscard]] float process(float target, float ms, double sample_rate) noexcept {
        if (ms == 0.0f || !(sample_rate > 0.0)) {
            state_ = target;
            return target;
        }
        if (ms > 0.0f) {
            const auto step = static_cast<float>(
                kFullSwing / (static_cast<double>(ms) * 0.001 * sample_rate));
            state_ = std::clamp(target, state_ - step, state_ + step);
        } else {
            // A one-pole whose 63% point is |ms| for a full swing. Its coefficient
            // depends only on the control and the sample rate, so it is computed
            // when one of those changes and not once per sample — `std::exp` in an
            // inner loop is the most expensive thing in this suite, and it was
            // being asked the same question forty-eight thousand times a second.
            const float a = coefficient(ms, sample_rate);
            state_ = a * state_ + (1.0f - a) * target;
            if (std::abs(state_) < kDenormalFloor) state_ = 0.0f;
        }
        return state_;
    }

private:
    [[nodiscard]] float coefficient(float ms, double sample_rate) noexcept {
        if (ms != coeff_ms_ || sample_rate != coeff_sr_) {
            const double tau = static_cast<double>(-ms) * 0.001;
            coeff_ = static_cast<float>(std::exp(-1.0 / (tau * sample_rate)));
            coeff_ms_ = ms;
            coeff_sr_ = sample_rate;
        }
        return coeff_;
    }

    float state_ = 0.0f;

    /// The cached one-pole coefficient and the two inputs it was computed from.
    /// `coeff_ms_` starts at zero, which `process()` never reaches this branch
    /// with, so the first low-passed sample always recomputes.
    float coeff_ = 0.0f;
    float coeff_ms_ = 0.0f;
    double coeff_sr_ = 0.0;
};

}  // namespace pulp::examples::brew
