#pragma once

/// @file dynamics_core.hpp
/// The published compressor equations, once.
///
/// Four lineages in this catalog — the transparent feedforward design, the
/// Blackmer/VCA one, the 1176-style FET one and the diode-bridge one — differ
/// in topology, in gain element and in where their ballistics live. What they
/// do NOT differ in is the static characteristic they all compute, which is the
/// soft-knee gain computer of
///
/// > Giannoulis, D., Massberg, M. & Reiss, J.D., "Digital Dynamic Range
/// > Compressor Design — A Tutorial and Analysis", *Journal of the Audio
/// > Engineering Society* 60(6), pp. 399–408, 2012.
///
/// Written per module it became four transcriptions of one published equation,
/// each with its own branch conditions and its own idea of which domain the
/// answer comes back in. This header is the one transcription.
///
/// What it owns: the knee's quadratic term, the two domain forms of the
/// characteristic that wrap it, the add-then-log level conversions, and the
/// one-pole RETAIN coefficient. All free functions over `double`, all pure.
///
/// What it does NOT own — and this is the more important half:
///
/// - **Detectors.** All four have one and no two are the same equation. The
///   feedforward design runs the paper's DECOUPLED two-stage smoother; the VCA
///   lineage runs a single direction-switched pole on the instantaneous mean
///   square; the FET lineage runs the paper's BRANCHING filter at the
///   oversampled rate inside a feedback loop; the diode-bridge lineage runs a
///   dual follower blended on a sustain indicator. Those differences are what
///   the modules exist to express. A base class with a mode switch would be
///   strictly worse than four detectors that each say what they are.
/// - **Gain elements.** A VCA multiply, a FET voltage divider and a
///   diode-bridge attenuator share nothing but their position in the chain.
/// - **The subtraction `y_L − x_L`.** It is a subtraction, not an equation, and
///   the three sign conventions below mean a shared wrapper would have to be
///   parameterised by its own sign — which is how the convention gets lost.
/// - **Floor disciplines other than add-then-log.** The VCA lineage floors its
///   mean square with a `max()` rather than an addend, deliberately: `max` keeps
///   the detector's own state out of the denormal range, which an addend applied
///   at read-out does not do. The diode-bridge lineage composes
///   `units::linear_to_db`, the house conversion, and should keep doing so.
///   Collapsing three floor disciplines into one would change three modules'
///   arithmetic to save one line each.
///
/// ## The sign convention, and why there is more than one
///
/// This is the trap in the whole subsystem, so it is written down here rather
/// than in one module's doc block where the other three cannot see it. Gain
/// reduction is expressed three ways across the catalog, all correct in their
/// own module and none interchangeable:
///
/// | Module | `gain_reduction_db()` | Why |
/// |---|---|---|
/// | feedforward | POSITIVE magnitude | its decoupled detector runs a `max()`, which is only correct on positives |
/// | VCA | SIGNED, ≤ 0 | it is added to makeup before one dB→linear conversion, matching the paper's equations |
/// | FET | POSITIVE magnitude | same `max()`-free branching detector, but the divider is calibrated in positive dB of attenuation |
/// | diode-bridge | SIGNED, ≤ 0 | its static curve returns reduction directly |
///
/// **The specific hazard**, carried here from the feedforward module's doc block
/// because it applies to any future member of this family: running a `max()` on
/// the NEGATIVE gain-computer output silently swaps attack and release. The
/// detector snaps toward LESS reduction and eases into more. That version still
/// compresses, still sounds like a working compressor, and fails a step-response
/// test immediately — which is why every module in this family has one.
///
/// `soft_knee_reduction_db` returns the SIGNED form (≤ 0 for a compressing
/// ratio). A caller wanting the positive magnitude negates once, at its own
/// boundary, and then stays in positives.
///
/// ## Why the characteristic ships in two domains
///
/// `soft_knee_output_db` returns an output LEVEL; `soft_knee_reduction_db`
/// returns a gain REDUCTION. They are the same equation and algebraically
/// `output = input + reduction` — but they are NOT equal in floating point on
/// the linear branch, where one computes `T + over/R` and the other computes
/// `(1/R − 1)·over`. Deriving either from the other would perturb three modules'
/// results in the last bits, which is a behaviour change made to save a
/// function. So both ship, adjacent, sharing the knee term — which IS bit-exact
/// between them — and a module picks the domain its own public contract is
/// stated in.
///
/// The two also differ in which branch they take exactly ON the knee's lower
/// edge (`2·over == −W`): the level form enters the quadratic branch, where the
/// term evaluates to zero, and the reduction form returns zero directly. Same
/// value, different path — preserved as-is so each module's arithmetic is
/// untouched.
///
/// ## Retain, not coefficient
///
/// `one_pole_retain` returns `α = exp(−1/(τ·fs))`, the `α` in
/// `y = α·y + (1−α)·x`. That is the COMPLEMENT of `units::ms_to_onepole_coef`,
/// which returns the `a` in `y += a·(x − y)`. Both conventions are in use in
/// this family and they are one subtraction apart, so the name says which one
/// this is. τ is the 63.2 % time constant in SECONDS; the 10–90 % rise is
/// `τ·ln 9` and the 60 dB decay is `τ·ln 1000`, per `units.hpp`.
///
/// Note also that `BallisticsFilterT`'s coefficient is built on an
/// `exp(−2.2/·)` 10–90 % convention. Composing it where a τ is meant redefines
/// the user's time by a factor of `ln 9`.
///
/// RT contract: everything here is pure, stateless scalar arithmetic over POD.
/// Nothing allocates, locks, or performs I/O, so all of it is safe per sample on
/// the audio thread. `amplitude_db`/`power_db` cost one `log10`,
/// `one_pole_retain` one `exp`, and the characteristic none — it is arithmetic
/// and branches only.

#include <algorithm>
#include <cmath>

namespace pulp::signal::dynamics {

/// Control setters retain their last valid value when automation supplies a
/// non-finite sample. `std::clamp` alone does not reject NaN because every
/// comparison with NaN is false.
inline double retain_finite(double candidate, double current) noexcept {
    return std::isfinite(candidate) ? candidate : current;
}

/// Level-conversion floor, guarding `log10(0)`. Any value far below the
/// quietest representable signal works: this sits ~26 orders above the float32
/// denormal floor and ~7 orders below −120 dBFS.
/// [design parameter] default 1e-12, range 1e-15 .. 1e-9.
inline constexpr double kLevelEpsilon = 1e-12;

// ── Level conversion ──────────────────────────────────────────────────────

/// An amplitude → dB, floored by an ADDEND rather than a `max()`.
///
/// `20·log10`, because the argument is an amplitude. The addend form is chosen
/// over `max()` here because a detector reading a peak wants a continuous
/// function of its input near silence — a `max()` puts a corner in the curve at
/// the floor, which a smoother downstream then has to ride over.
inline double amplitude_db(double amplitude, double epsilon) {
    return 20.0 * std::log10(std::abs(amplitude) + epsilon);
}

/// A mean square → dB, floored by an addend.
///
/// `10·log10`, NOT 20, because the integrator already holds a squared quantity.
/// Getting this wrong doubles every level the detector reports, which reads as
/// a compressor with twice the ratio it was set to and no obvious symptom
/// otherwise — so the two conversions are named separately rather than left to
/// a factor at each call site.
inline double power_db(double mean_square, double epsilon) {
    return 10.0 * std::log10(mean_square + epsilon);
}

// ── Ballistics ────────────────────────────────────────────────────────────

/// `α = exp(−1/(τ·fs))` — the RETAIN coefficient (see the file doc block).
/// τ in seconds. Returns 0 (instant) for a non-positive τ or sample rate rather
/// than dividing by zero.
inline double one_pole_retain(double tau_seconds, double sample_rate) {
    if (!(tau_seconds > 0.0) || !(sample_rate > 0.0)) return 0.0;
    return std::exp(-1.0 / (tau_seconds * sample_rate));
}

// ── The soft-knee static characteristic (the paper's eq. 4) ───────────────

/// The knee branch's quadratic term: the dB the characteristic bends away from
/// unity inside the knee, `(slope − 1)·(over + W/2)² / (2W)`.
///
/// This is the shared kernel — it is bit-identical in both domain forms below
/// and in all four lineages, which is what makes it the thing worth owning
/// here. `slope` is `1/R`; pass 0 for a limiter's infinite ratio and a negative
/// value for an over-unity ("infinity+") curve. Caller guarantees `W > 0`.
inline double knee_term_db(double over_db, double knee_db, double slope) {
    const double t = over_db + knee_db * 0.5;
    return (slope - 1.0) * t * t / (2.0 * knee_db);
}

/// The characteristic in the OUTPUT-LEVEL domain: input dB → output dB.
///
/// ```
/// y(x) = x                                         2(x−T) < −W
///      = x + (1/R − 1)(x − T + W/2)² / (2W)        2|x−T| ≤ W
///      = T + (x−T)/R                               otherwise
/// ```
///
/// `ratio` may be negative (an "infinity+" curve that keeps pushing output down
/// as input rises); the equation is continuous through that and the caller owns
/// any floor.
///
/// **Preconditions**, which every module enforced with its own clamp before this
/// was shared code and which are therefore not re-checked here: `knee_db >= 0`
/// and `ratio != 0`. There is deliberately no guard — a compressor whose ratio
/// reached zero has a bug upstream, and swallowing it here would hide that
/// while costing a branch per sample. A module whose top-of-range ratio means
/// "limit" should resolve the slope itself and use `soft_knee_reduction_db`,
/// which takes `slope = 0` for an infinite ratio without any division.
inline double soft_knee_output_db(double input_db, double threshold_db, double knee_db,
                                  double ratio) {
    const double over = input_db - threshold_db;
    if (2.0 * over < -knee_db) return input_db;
    if (knee_db > 0.0 && 2.0 * std::abs(over) <= knee_db)
        return input_db + knee_term_db(over, knee_db, 1.0 / ratio);
    return threshold_db + over / ratio;
}

/// The same characteristic in the GAIN-REDUCTION domain: dB over threshold →
/// reduction in dB, SIGNED and ≤ 0 for a compressing slope.
///
/// Takes `slope = 1/R` already resolved rather than `R`, so a caller that maps
/// its top-of-range ratio onto a true limiter passes 0 without this function
/// needing to know what that module's limit threshold is.
inline double soft_knee_reduction_db(double over_db, double knee_db, double slope) {
    if (knee_db > 0.0) {
        if (2.0 * over_db <= -knee_db) return 0.0;
        if (2.0 * over_db < knee_db) return knee_term_db(over_db, knee_db, slope);
    } else if (over_db <= 0.0) {
        return 0.0;
    }
    return (slope - 1.0) * over_db;
}

}  // namespace pulp::signal::dynamics
