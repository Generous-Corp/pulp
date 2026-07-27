#pragma once

/// @file junction.hpp
/// The semiconductor junction law, once.
///
/// The same exponential shows up everywhere in circuit-modelled audio — diode
/// clippers, transistor fuzz pairs, optical and diode-bridge compressors, tape
/// bias stages, tube approximations. Every one of them is
///
/// ```
/// i(v) = Is·(exp(v/(n·V_T)) − 1)
/// ```
///
/// with a different `(Is, n)` and a different circuit wrapped around it. Written
/// per module it becomes several implementations of one physical constant and
/// one transcendental, each with its own overflow clamp and its own idea of
/// what `V_T` is. This header is the one implementation.
///
/// What it owns: the thermal voltage, the overflow-safe exponent, and the three
/// closed forms every consumer needs — the current, its derivative (the
/// incremental conductance a Newton solve wants), and its antiderivative (what
/// antiderivative-antialiasing wants).
///
/// What it does NOT own: device families. A diode's silicon row and a
/// transistor's silicon row are different numbers for different components, and
/// collapsing them into one table would be a false economy — the consuming
/// module declares its own `[design parameter]` rows and hands them here.
///
/// ## Two antiparallel legs
///
/// `JunctionPair` generalises to the antiparallel networks clippers use, and
/// parameterises them by the RECIPROCAL series-diode count per leg:
///
/// ```
/// i(v) = Is·( exp(a·v/θ) − exp(−b·v/θ) )        θ = n·V_T
/// ```
///
/// Reciprocals rather than counts is what makes the parameter continuous: "this
/// leg is absent" is `b = 0`, a finite value, where a count would have to be
/// infinite. A single junction is the `b = 0` case, so one type covers both.
///
/// ## The antiderivative's parity
///
/// `F1` is even when the positive and negative legs match, because the current
/// is then odd. An asymmetric pair is neither odd nor even, and neither is its
/// antiderivative. Worth stating because silently forcing even parity would
/// erase the even-harmonic behavior that `set_symmetry` exists to create.
///
/// It is computed through `exprel` rather than the algebraically obvious
/// `θ/b·(exp(−b·v/θ) − 1)`, which divides by a vanishing `b` as a leg is
/// removed and produces a huge value that only cancels in a later subtraction —
/// correct on paper, catastrophic in floating point.
///
/// RT contract: everything here is pure, stateless scalar arithmetic over POD.
/// Nothing allocates, locks, or performs I/O, so all of it is safe per sample on
/// the audio thread. Each call costs one or two `exp` evaluations.
///
/// Reference: Shockley (1949) for the diode law; Ebers & Moll, "Large-Signal
/// Behavior of Junction Transistors", Proc. IRE 42(12):1761–1772, 1954, for the
/// transistor forward-active term. They are the same equation.

#include <algorithm>
#include <cmath>

namespace pulp::signal::junction {

/// Thermal voltage `k·T/q` at 300 K, in volts. A physical constant derived from
/// `k/q = 8.617e-5 V/K`, not a fitted value.
inline constexpr double kThermalVoltage = 0.02585;

/// Ceiling on an exponent argument, so a transient overshoot cannot produce
/// `inf`. `exp(80) ≈ 5.5e34` is far past any current a real collector load will
/// admit, and well short of `exp`'s overflow near 709.
inline constexpr double kMaxExponent = 80.0;

/// `exp(x)` with the argument clamped into the safe range.
inline double safe_exp(double x) {
    return std::exp(std::clamp(x, -kMaxExponent, kMaxExponent));
}

/// `(exp(x) − 1) / x`, with the removable singularity at 0 handled.
///
/// Two Taylor terms are already below double precision at the cutoff, so the
/// branch is exact rather than approximate where it matters.
inline double exprel(double x) {
    if (std::abs(x) < 1e-6) return 1.0 + x * 0.5 + x * x * (1.0 / 6.0);
    return std::expm1(std::clamp(x, -kMaxExponent, kMaxExponent)) / x;
}

/// One junction network: the exponential law over one or two antiparallel legs.
///
/// A consuming module fills in `(saturation_current, ideality)` from its own
/// declared device rows, and `(leg_a, leg_b)` from its own topology.
struct JunctionPair {
    /// `Is`, the reverse saturation current. Units are whatever the consuming
    /// module works in — SI amps or calibration-relative — because every
    /// formula here is homogeneous in it.
    double saturation_current = 1e-14;

    /// `n`, the ideality factor. Sets the knee's SOFTNESS: higher is softer.
    double ideality = 1.0;

    /// Reciprocal series-diode count of the forward leg. 1 is a single diode,
    /// 1/2 is two in series (turns on at twice the voltage), 0 removes it.
    double leg_a = 1.0;

    /// Reciprocal series-diode count of the reverse leg. 0 makes the network a
    /// single junction — half-wave, even harmonics.
    double leg_b = 1.0;

    /// `θ = n·V_T`, the exponential's scale in volts.
    double theta() const { return ideality * kThermalVoltage; }

    /// `i(v)`. The two legs' `−1` terms cancel exactly, which is why this is a
    /// difference of exponentials rather than of `expm1`s.
    double current(double v) const {
        const double t = theta();
        return saturation_current * (safe_exp(leg_a * v / t) - safe_exp(-leg_b * v / t));
    }

    /// `di/dv` — the incremental conductance a Newton solve needs. Strictly
    /// positive, which is what makes a residual built on it monotone.
    double conductance(double v) const {
        const double t = theta();
        return saturation_current / t *
               (leg_a * safe_exp(leg_a * v / t) + leg_b * safe_exp(-leg_b * v / t));
    }

    /// `∫i(v)dv`, in closed form — no numerical quadrature. Even, for the
    /// reason given in the file doc block.
    double antiderivative(double v) const {
        const double t = theta();
        return saturation_current * v * (exprel(leg_a * v / t) - exprel(-leg_b * v / t));
    }

    /// A closed-form estimate of where a clipped node voltage will land, for
    /// use as a solver's warm start.
    ///
    /// This matters more than it looks. Newton on an exponential advances by
    /// roughly one thermal voltage per iteration when it starts deep in
    /// conduction — 26 mV at a time — so a cold start two volts away needs
    /// dozens of iterations against a typical RT cap of eight. Solving the
    /// conducting branch directly, with `v ≪ v_drive`,
    /// `(v_drive − v)/R = Is·exp(a·v/θ)` gives `v ≈ (θ/a)·ln(v_drive/(R·Is))` —
    /// for a silicon diode at 2 V through 10 kΩ that is 0.613 V, the knee to
    /// three figures. From there Newton converges in two or three steps.
    ///
    /// Below conduction, or with the relevant leg absent, the node follows the
    /// drive, which is the fallthrough.
    double conduction_estimate(double v_drive, double resistance) const {
        const double leg = v_drive >= 0.0 ? leg_a : leg_b;
        if (!(leg > 0.0)) return v_drive;
        const double ratio = std::abs(v_drive) / (resistance * saturation_current);
        if (!(ratio > 1.0)) return v_drive;
        const double magnitude = (theta() / leg) * std::log(ratio);
        return std::copysign(std::min(magnitude, std::abs(v_drive)), v_drive);
    }

    /// The junction voltage at which the network conducts `target_current` on
    /// its forward leg — the conduction knee, inverted from the same law.
    ///
    /// This is how a bias point is derived rather than declared: a module states
    /// its operating current and the knee falls out, so the documented
    /// ~0.36 V germanium / ~0.69 V silicon figures are consequences of `(Is, n)`
    /// rather than separately maintained constants that can drift from them.
    double knee_voltage(double target_current) const {
        if (!(leg_a > 0.0)) return 0.0;
        return (theta() / leg_a) * std::log(target_current / saturation_current + 1.0);
    }

    /// First-order antiderivative-antialiasing evaluation of the current
    /// between two consecutive junction voltages.
    ///
    /// Falls back to direct evaluation when the two are too close for the
    /// difference quotient to carry precision — the standard ADAA guard. The
    /// epsilon lives with its one consumer here rather than being copied into
    /// every solver that wants antialiasing.
    double adaa_current(double v, double previous, double epsilon) const {
        const double delta = v - previous;
        if (std::abs(delta) <= epsilon) return current(v);
        return (antiderivative(v) - antiderivative(previous)) / delta;
    }

    /// `d/dv` of `adaa_current` — the Jacobian a Newton solve needs when its
    /// residual is built on the ADAA quotient rather than on `current`.
    ///
    /// This exists because pairing the ADAA residual with `conductance()` is a
    /// silent catastrophe rather than a slow one. `conductance` is the slope of
    /// the PLAIN current; the quotient's slope is a different function, and
    /// feeding Newton a Jacobian that does not belong to its residual makes the
    /// step direction wrong rather than merely inaccurate. Measured, a diode
    /// clipper diverged geometrically at roughly 3300x per sample to 5.8e25 —
    /// full-scale noise, from a solver that never reported failure because
    /// every value stayed finite.
    ///
    /// Differentiating `(F1(v) − F1(p))/(v − p)` gives
    /// `[i(v)·(v − p) − (F1(v) − F1(p))] / (v − p)²`, and it stays strictly
    /// positive for the same reason `conductance` does: the quotient is the
    /// MEAN of `i` over `[p, v]`, `i` is increasing, so `i(v)` is never below
    /// that mean. The bracketed solver's monotone-residual precondition
    /// therefore still holds — which is what makes the safeguard safe rather
    /// than merely present.
    double adaa_conductance(double v, double previous, double epsilon) const {
        const double delta = v - previous;
        // HALF the conductance, not the conductance. As `v → p` the quotient
        // `(F1(v) − F1(p))/(v − p)` tends to `i(p)` but its SLOPE tends to
        // `i'(p)/2` — the mean of `i` over a collapsing interval moves at half
        // the rate of `i` itself. Returning the full conductance here overstates
        // the Jacobian by exactly 2x near the fallback, which is where a solve
        // spends most of its time on a slow-moving signal.
        if (std::abs(delta) <= epsilon) return 0.5 * conductance(v);
        const double integral = antiderivative(v) - antiderivative(previous);
        return (current(v) * delta - integral) / (delta * delta);
    }
};

}  // namespace pulp::signal::junction
