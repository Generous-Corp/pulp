#pragma once

// Jiles-Atherton magnetic hysteresis, and the oversampling wrap it needs.
//
// This is what makes the physical tape tier a MODEL rather than a curve. A
// static waveshaper maps input to output; magnetization does not, because the
// state of the medium depends on where it has been. Feed a slow sine through
// this stage and plot output against input and you get a loop with area — the
// same input value produces different outputs on the rising and falling edges.
// That memory is why real tape compresses transients differently from steady
// tone, and it is the property a waveshaper cannot fake at any drive.
//
// The magnetization ODE and the audio-rate discrete form follow Jiles &
// Atherton (1986) and Chowdhury (2019):
//
//   dM/dH = [(1−c)·δ_M·(M_an − M)] / [(1−c)·δ·k − α·(M_an − M)] + c·dM_an/dH
//   M_an  = M_s·L(H_e/a),  H_e = H + α·M,  L(x) = coth(x) − 1/x
//
// Everything is solved in units NORMALIZED by M_s, so the state stays O(1) in
// double precision instead of swinging over ±3.5×10^5 A/m — the ODE is stiff
// enough without also asking the solver to work at that scale.
//
// Two implementation choices are load-bearing:
//
//   * A Newton-Raphson solve on the TRAPEZOIDAL form, not forward Euler.
//     Explicit integration of this ODE is unstable at the drives that make it
//     interesting, and an unstable magnetization in a FEEDBACK LOOP is not a
//     glitch, it is a divergence.
//   * Oversampling of this stage ONLY. The nonlinearity generates harmonics
//     well past Nyquist; everything else in the tape chain is linear and gains
//     nothing from running faster. Chowdhury's finding is that 2× is acceptable
//     at moderate drive and 4× is good across the range, with ≥8× showing
//     diminishing returns.

#include <pulp/signal/character_delay/primitives.hpp>
#include <pulp/signal/character_delay/tables.hpp>
#include <pulp/signal/oversampling_fir.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pulp::signal::chardelay {

/// Langevin function L(x) = coth(x) − 1/x and its first two derivatives.
///
/// The closed forms are numerically useless for small arguments and the reason
/// is worth stating, because the failure is silent. L is the DIFFERENCE of two
/// terms that both blow up as x → 0: at x = 0.06, coth(x) and 1/x are both
/// about 16.4 and their difference is 0.02, so three significant digits are
/// destroyed by cancellation before any approximation error is considered.
/// L'(x) = 1/x² − coth²(x) + 1 is worse — 268.7 − 269.3 + 1 — and L' is what
/// the Newton solver's Jacobian is built from. Feeding it a rational tanh
/// approximation good to "well under a percent" leaves the derivative with a
/// double-digit percentage error, and Newton stops converging quadratically:
/// measured as the solver hitting its iteration cap at exactly the coercive
/// point of every cycle, which is the steepest and most audible part of the
/// hysteresis loop.
///
/// So the series expansion is used wherever cancellation would bite (|x| < 1,
/// where four terms are exact to better than a part in 10^4) and the closed
/// form only above it, where the two terms are well separated and the cheap
/// rational tanh is genuinely sufficient.
struct Langevin {
    static constexpr double kSeriesLimit = 1.0;

    static double value(double x) noexcept {
        if (std::abs(x) < kSeriesLimit) {
            const double x2 = x * x;
            return x * (1.0 / 3.0 + x2 * (-1.0 / 45.0 + x2 * (2.0 / 945.0 - x2 / 4725.0)));
        }
        return coth(x) - 1.0 / x;
    }

    static double derivative(double x) noexcept {
        if (std::abs(x) < kSeriesLimit) {
            const double x2 = x * x;
            return 1.0 / 3.0 + x2 * (-1.0 / 15.0 + x2 * (2.0 / 189.0 - x2 / 675.0));
        }
        const double c = coth(x);
        return 1.0 / (x * x) - c * c + 1.0;
    }

    static double second_derivative(double x) noexcept {
        if (std::abs(x) < kSeriesLimit) {
            const double x2 = x * x;
            return x * (-2.0 / 15.0 + x2 * (8.0 / 189.0 - x2 * (2.0 / 225.0)));
        }
        const double c = coth(x);
        return -2.0 / (x * x * x) + 2.0 * c * (c * c - 1.0);
    }

    /// coth via the sanctioned rational tanh approximation and a reciprocal.
    /// Only reached for |x| >= kSeriesLimit, where coth and 1/x differ enough
    /// that the approximation's error survives the subtraction intact.
    static double coth(double x) noexcept {
        const double t = fast_tanh(x);
        const double magnitude = std::max(std::abs(t), 1e-12);
        return (t < 0.0 ? -1.0 : 1.0) / magnitude;
    }
};

/// The hysteresis stage for one channel, in normalized units.
class JilesAthertonHysteresis {
public:
    void prepare(double solver_rate_hz) noexcept {
        period_ = 1.0 / std::max(solver_rate_hz, 1.0);
        reset();
    }

    void reset() noexcept {
        magnetization_ = 0.0;
        field_ = 0.0;
        field_derivative_ = 0.0;
        rate_previous_ = 0.0;
    }

    /// `drive` and `bias` come from the tape character table; both are re-mapped
    /// onto physical constants here rather than being used as gain and offset.
    ///   drive → a  (the anhysteretic shape parameter: smaller a is a steeper
    ///               approach to saturation, i.e. more saturation for the same
    ///               field, which is what a drive control should do)
    ///   bias  → c  (the reversible fraction: less reversibility means a wider
    ///               loop and more odd-harmonic content — a worn machine)
    /// k and α stay at their published γ-Fe2O3 values. [the mapping CURVES are
    /// design parameters; the endpoints they map onto are published]
    void set_character(double drive, double bias) noexcept {
        const double reference_drive = kTapeDrive.front();
        const double safe_drive = std::max(drive, 1e-3);
        shape_ = (kJaAnhystereticShape / kJaSaturationMagnetization) *
                 (reference_drive / safe_drive);
        shape_ = std::max(shape_, 1e-4);
        pinning_ = kJaPinning / kJaSaturationMagnetization;

        const double bias_span = std::max(kTapeBias.back(), 1e-9);
        const double t = std::clamp(bias / bias_span, 0.0, 1.0);
        reversibility_ = 0.3 + t * (0.1 - 0.3);  // published c range, new → worn
    }

    /// One solver step. Input and output are normalized magnetic field and
    /// magnetization respectively.
    double process(double field) noexcept {
        // Trapezoidal recurrence for dH/dt — the standard companion to a
        // trapezoidal integration of the state.
        const double rate =
            (2.0 / period_) * (field - field_) - rate_previous_;

        // Silence snap: with no field and no field motion, hold zero rather
        // than letting the solver park on a frozen DC magnetization that would
        // bias every later repeat.
        if (std::abs(field) < kJaSilenceThreshold && std::abs(rate) < kJaSilenceThreshold) {
            magnetization_ = 0.0;
            field_ = field;
            field_derivative_ = 0.0;
            rate_previous_ = 0.0;
            return 0.0;
        }

        const double derivative_previous = field_derivative_;
        // Start from a forward-Euler extrapolation rather than from the previous
        // state. Newton converges from either, but the extrapolated guess is
        // already first-order correct, which is the difference between two
        // iterations and running out the cap at high drive.
        double m = magnetization_ + period_ * derivative_previous;

        // δ_M is a SWITCH on sign(M_an − M), so it flips as Newton moves the
        // iterate across the anhysteretic curve — and a residual that changes
        // definition mid-solve makes Newton oscillate instead of converge. It is
        // decided once from the initial guess and held for the step, which is
        // the standard linearization: over one audio-rate step the branch is a
        // property of where the material is, not of where the solver is looking.
        const bool irreversible_active = branch_active(m, field, rate);

        for (int iteration = 0; iteration < kJaMaxIterations; ++iteration) {
            double slope = 0.0;
            const double derivative_now = evaluate(m, field, rate, slope, irreversible_active);
            const double residual =
                m - magnetization_ - 0.5 * period_ * (derivative_now + derivative_previous);
            // Test the RESIDUAL as well as the step. At a field reversal the
            // δ_M switch makes dM/dH piecewise, so Newton's step can stay
            // finite while the equation is already satisfied to well below the
            // 24-bit floor; without this the solver reports a cap-out once per
            // cycle at high drive even though the answer is correct.
            if (std::abs(residual) < kJaConvergenceThreshold) {
                ++converged_;
                break;
            }
            const double jacobian = 1.0 - 0.5 * period_ * slope;
            if (std::abs(jacobian) < 1e-12) break;
            const double delta = residual / jacobian;
            m -= delta;
            if (std::abs(delta) < kJaConvergenceThreshold) {
                ++converged_;
                break;
            }
            if (iteration == kJaMaxIterations - 1) ++capped_;
        }

        double unused_slope = 0.0;
        field_derivative_ = evaluate(m, field, rate, unused_slope, irreversible_active);

        if (!std::isfinite(m) || !std::isfinite(field_derivative_)) {
            // Reset BOTH the state and the derivative: restoring only the
            // magnetization re-poisons it from the stale derivative on the very
            // next trapezoidal step.
            m = 0.0;
            field_derivative_ = 0.0;
            rate_previous_ = 0.0;
            field_ = 0.0;
            magnetization_ = 0.0;
            return 0.0;
        }

        magnetization_ = flush_denormal(m);
        field_ = field;
        rate_previous_ = rate;
        return magnetization_;
    }

    /// Diagnostics for the acceptance suite: how often the solver converged
    /// inside the iteration cap versus ran out of iterations.
    std::size_t converged_steps() const noexcept { return converged_; }
    std::size_t capped_steps() const noexcept { return capped_; }
    void clear_solver_counters() noexcept {
        converged_ = 0;
        capped_ = 0;
    }

private:
    /// dM/dt at (M, H, dH/dt), and via `slope` its partial derivative with
    /// respect to M — the Jacobian term Newton-Raphson needs. Computing the
    /// partial analytically rather than by finite difference is what keeps the
    /// solve inside a handful of iterations at high drive.
    /// Which branch of the hysteresis switch applies, decided once per step.
    bool branch_active(double m, double h, double rate) const noexcept {
        const double effective_field = h + kJaInterdomainCoupling * m;
        const double q =
            std::clamp(effective_field / shape_, -kJaLangevinClamp, kJaLangevinClamp);
        const double difference = Langevin::value(q) - m;
        return (difference > 0.0) == (rate >= 0.0) && difference != 0.0;
    }

    double evaluate(double m, double h, double rate, double& slope,
                    bool irreversible_active) const noexcept {
        const double effective_field = h + kJaInterdomainCoupling * m;
        const double q =
            std::clamp(effective_field / shape_, -kJaLangevinClamp, kJaLangevinClamp);

        const double anhysteretic = Langevin::value(q);
        const double langevin_first = Langevin::derivative(q);
        const double langevin_second = Langevin::second_derivative(q);

        const double difference = anhysteretic - m;
        const double delta = (rate >= 0.0) ? 1.0 : -1.0;
        const double delta_m = irreversible_active ? 1.0 : 0.0;

        const double one_minus_c = 1.0 - reversibility_;
        double denominator = one_minus_c * delta * pinning_ - kJaInterdomainCoupling * difference;
        if (std::abs(denominator) < 1e-12) denominator = (denominator < 0.0 ? -1e-12 : 1e-12);

        const double irreversible = one_minus_c * delta_m * difference / denominator;
        const double reversible = reversibility_ * langevin_first / shape_;
        const double dm_dh = irreversible + reversible;

        // ∂(dM/dH)/∂M, with dQ/dM = α/a.
        const double dq_dm = kJaInterdomainCoupling / shape_;
        const double u = langevin_first * dq_dm - 1.0;
        const double irreversible_slope = one_minus_c * delta_m * u * (one_minus_c * delta * pinning_) /
                                          (denominator * denominator);
        const double reversible_slope = reversibility_ * langevin_second * dq_dm / shape_;

        slope = (irreversible_slope + reversible_slope) * rate;
        return dm_dh * rate;
    }

    double period_ = 1.0 / 192000.0;
    double magnetization_ = 0.0;
    double field_ = 0.0;
    double field_derivative_ = 0.0;
    double rate_previous_ = 0.0;

    double shape_ = kJaAnhystereticShape / kJaSaturationMagnetization;
    double pinning_ = kJaPinning / kJaSaturationMagnetization;
    double reversibility_ = kJaReversibility;

    std::size_t converged_ = 0;
    std::size_t capped_ = 0;
};

/// 4× linear-phase half-band oversampling wrap around a per-sample callback.
///
/// Two cascaded 2× half-band stages at the house 65-tap / Kaiser β ≈ 8 design.
/// The pair's group delay is a fixed integer number of host samples, which is
/// exactly what lets the module keep latency_samples() == 0: the constant is
/// subtracted from the delay line's read distance instead of being reported to
/// the host, so echo timing stays right and the loop's total delay is unchanged.
class HalfBandOversampler4x {
public:
    void prepare() {
        stage_a_.configure(0.45, kHysteresisHalfBandStopbandDb, kHysteresisHalfBandTaps);
        stage_b_.configure(0.225, kHysteresisHalfBandStopbandDb, kHysteresisHalfBandTaps);
        reset();
    }

    void reset() noexcept {
        stage_a_.reset();
        stage_b_.reset();
    }

    /// Group delay of the up/down pair, in HOST samples. Rate-independent.
    static int latency_samples() noexcept {
        const auto taps = static_cast<int>(kHysteresisHalfBandTaps);
        return (taps - 1) / 2 + (taps - 1) / 4;
    }

    template <typename Callback>
    double process(double x, Callback&& callback) {
        double even = 0.0;
        double odd = 0.0;
        stage_a_.upsample(x, even, odd);
        const double processed_even = inner(even, callback);
        const double processed_odd = inner(odd, callback);
        return stage_a_.downsample(processed_even, processed_odd);
    }

private:
    template <typename Callback>
    double inner(double x, Callback& callback) {
        double even = 0.0;
        double odd = 0.0;
        stage_b_.upsample(x, even, odd);
        return stage_b_.downsample(callback(even), callback(odd));
    }

    detail::LinearPhaseOversamplingStage2x<double> stage_a_;
    detail::LinearPhaseOversamplingStage2x<double> stage_b_;
};

}  // namespace pulp::signal::chardelay
