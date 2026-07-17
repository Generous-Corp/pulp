#pragma once

/// @file blep.hpp
/// Bandlimited-step (BLEP) and bandlimited-ramp (BLAMP) correction kernels.
///
/// A trivially-generated waveform — a saw that jumps, a square that switches, a
/// triangle that turns a corner — is not bandlimited. Sampling it folds every
/// component above Nyquist back into the audible band. These kernels supply the
/// correction that a generator adds to the trivial signal to suppress that fold.
///
/// Two kinds of discontinuity need two kernels:
///   * **BLEP** corrects a discontinuity in the *value* — a saw wrap, a square
///     edge, a hard-sync reset.
///   * **BLAMP** corrects a discontinuity in the *slope* — a triangle apex, the
///     turning point of a wave whose value is continuous but whose derivative
///     is not. BLAMP is the integral of BLEP, which is exactly what "the value
///     is continuous but the slope jumps" means one derivative down.
///
/// ── What these kernels are NOT ────────────────────────────────────────────
///
/// polyBLEP is a **two-point polynomial approximation** to the ideal BLEP, and
/// the approximation is the dominant error, not a rounding detail. The ideal
/// bandlimited step's residual has infinite support and must be windowed and
/// tabulated (minBLEP/BLIT territory); polyBLEP truncates it to the two samples
/// straddling the discontinuity and fits a polynomial through them. The residual
/// this leaves is real and measurable: it buys tens of dB over the trivial
/// waveform, NOT the ~100 dB a tabulated kernel reaches. `test_osc_blep.cpp`
/// measures the actual figure rather than asserting an aspiration.
///
/// Use these where a cheap, allocation-free, table-free correction that improves
/// on the trivial waveform is the right trade. Do not present the result as
/// alias-free, and do not gate a deep-floor claim on them.
///
/// ── Sign convention (read before calling) ─────────────────────────────────
///
/// **Every kernel here returns a correction to ADD to the trivial signal.**
/// There is no subtract path and no hidden negation: the sign lives entirely in
/// the caller's `height` / `slope_change` argument, which is always measured as
/// *after minus before*. A saw that wraps from +1 down to −1 has
/// `height = -1 - (+1) = -2`, and the kernel's output is added like any other.
///
/// This is the one place a mistake is silent. Applying a correction with the
/// wrong sign does not fail loudly — it *doubles* the discontinuity instead of
/// removing it, making aliasing worse while a naive "did the test run?" check
/// still passes. `test_osc_blep.cpp` pins the convention by asserting that the
/// inverted correction measurably degrades the residual.
///
/// ── Why the phase increment is not a kernel argument ──────────────────────
///
/// These kernels take the discontinuity's position in **samples**, not in phase
/// units, so the phase increment does not appear in the kernel math — it is
/// purely the phase-to-samples conversion, and it belongs to whatever advances
/// the phase. `wrap_position` performs that conversion for the common case of a
/// phase accumulator wrapping through a period boundary; a hard-sync or
/// through-zero-FM generator, whose discontinuities are not predictable from the
/// increment at all, computes the position its own way and calls the same
/// kernels. Keeping the increment out of the kernel is what lets one kernel
/// serve all three.
///
/// RT contract: every function here is a branch-light polynomial evaluation.
/// No allocation, no I/O, no locks, no transcendentals, no state.
///
/// Precision: the kernels compute in `double` throughout and take/return
/// `double`. A `float` generator narrows once, at the point it adds the
/// correction to its own sample — the kernel never sees `float`. The polynomial
/// coefficients are exactly representable, but the position `d` typically comes
/// from a division, and evaluating a cubic at a `float`-rounded position costs
/// more than the narrowing does.

#include <cmath>

namespace pulp::signal::osc {

/// A sub-sample position that means "no discontinuity here".
///
/// It sits outside [0, 1], which `poly_blep` and `poly_blamp` reject outright, so
/// it produces exactly zero from both. No in-range value could serve: 0 and 1 are
/// the two ENDS of the window, not spare sentinels, and each is a real operating
/// point carrying a half-height correction.
inline constexpr double kNoDiscontinuity = -1.0;

/// A discontinuity's correction, split across the two samples it touches.
///
/// A two-point kernel spans the sample before the discontinuity and the sample
/// after it, so a generator must be able to reach back one sample — via a
/// one-sample delay, a correction ring, or a second pass over the block. A
/// generator that can only write the current sample cannot apply the `before`
/// term and will leave part of the step uncorrected.
struct Correction {
    /// Add to the sample preceding the discontinuity.
    double before = 0.0;
    /// Add to the sample following it.
    double after = 0.0;
};

/// Unit BLEP residual at `x`, the signed distance from the discontinuity to the
/// sample, in samples: negative before it, positive after it.
///
/// This is the correction that turns a unit step `u(x)` into its bandlimited
/// counterpart: `u(x) + blep_residual(x)` rises smoothly from 0 at `x = -1`
/// through 0.5 at the discontinuity to 1 at `x = 1`, where a trivial step
/// instead jumps. Support is exactly [−1, 1); outside it the residual is zero,
/// which is precisely the truncation this kernel trades accuracy for.
///
/// `x = 0` belongs to the *after* branch, matching `u(0) = 1` — the sample that
/// lands exactly on a discontinuity is the first one that has taken the step.
///
/// That single choice is why `poly_blep` does NOT evaluate both of its taps
/// through this function: a `before` tap landing exactly on the discontinuity
/// has *not* taken the step and needs the other limit. This is the mathematical
/// residual, and at its one discontinuity it can only answer for one side; a
/// caller that knows which side its sample is on should say so rather than ask
/// here and hope.
inline double blep_residual(double x) noexcept {
    if (x < -1.0 || x >= 1.0) return 0.0;
    if (x < 0.0) return 0.5 * x * x + x + 0.5;
    return x - 0.5 * x * x - 0.5;
}

/// Unit BLAMP residual at `x` — the integral of `blep_residual`, and the
/// correction that turns a trivial ramp `max(x, 0)` into its bandlimited
/// counterpart.
///
/// Where BLEP is discontinuous at the origin (it cancels a jump in the value),
/// BLAMP is continuous there and only its slope breaks — it cancels a jump in
/// the *derivative*, leaving the value alone. Support is [−1, 1), and the
/// residual vanishes at both ends, so it perturbs nothing outside the two
/// samples it owns.
inline double blamp_residual(double x) noexcept {
    if (x < -1.0 || x >= 1.0) return 0.0;
    if (x < 0.0) return x * x * x / 6.0 + 0.5 * x * x + 0.5 * x + 1.0 / 6.0;
    return 0.5 * x * x - x * x * x / 6.0 - 0.5 * x + 1.0 / 6.0;
}

/// polyBLEP correction for a step in the VALUE.
///
/// `d` is the discontinuity's sub-sample position: it occurs `d` samples after
/// the `before` sample, equivalently `1 - d` samples before the `after` sample.
/// Outside [0, 1] there is no correction and both terms are zero — a
/// discontinuity always lands inside the sample it was found in, so a position
/// outside that window is a caller error, and none is the only answer that
/// cannot make the output worse than leaving it alone. `kNoDiscontinuity` is
/// outside the window by construction and so is guaranteed, not merely likely,
/// to produce nothing. A non-finite `d` lands here too.
///
/// `height` is the size of the jump measured **after minus before** — negative
/// for a saw wrapping down, positive for a square's rising edge. Both returned
/// terms are ADDED to the trivial signal (see the file-level sign convention).
///
/// ── Why this does not route both taps through `blep_residual` ─────────────
///
/// The taps sit on OPPOSITE sides of the discontinuity — that is what `before`
/// and `after` mean — and the residual STEPS across it, so each tap has to read
/// its own side. The distinction is invisible in the interior and decisive at
/// the two boundaries, which are ordinary operating points rather than corner
/// cases:
///
///   * `d = 0` puts the discontinuity exactly on the `before` sample, which
///     still carries its pre-step value — a phase sitting at 0 and driven
///     backward wraps out from under it, and the sample was taken before it
///     left. That tap needs the residual's LEFT limit, +1/2.
///   * `d = 1` puts it on the `after` sample, which carries its post-step value.
///     That tap needs the RIGHT limit, -1/2.
///
/// `blep_residual` resolves `x = 0` to the right limit — correct for the `after`
/// tap, and exactly backwards for the `before` one. Reading both through it
/// gives the `d = 0` tap a correction of the wrong SIGN, which does not merely
/// under-correct: it moves the sample a full `height` the wrong way, landing it
/// outside the waveform's own range and leaving the boundary wrap worse
/// corrected than if it had been ignored. Selecting the side per tap is what
/// makes both boundaries land on the step's midpoint, which is what a
/// bandlimited step does there.
///
/// With `d` confined to [0, 1] each tap's side is known outright, so both are
/// branch-free evaluations of their own branch rather than a lookup that has to
/// re-decide which side it is on.
inline Correction poly_blep(double d, double height) noexcept {
    if (!(d >= 0.0 && d <= 1.0)) return {0.0, 0.0};
    // Before tap at x = -d, always in [-1, 0]: the pre-step branch, which is
    // `0.5x^2 + x + 0.5` rewritten in d.
    const double before = 0.5 * d * d - d + 0.5;
    // After tap at x = 1 - d, always in [0, 1]: the post-step branch.
    const double x = 1.0 - d;
    const double after = x - 0.5 * x * x - 0.5;
    return {height * before, height * after};
}

/// polyBLAMP correction for a break in the SLOPE.
///
/// `d` follows `poly_blep`'s convention exactly. `slope_change` is the change in
/// slope measured **after minus before**, in units of *value per sample* — not
/// per phase unit, and not per second. A triangle traverses its full range four
/// times per cycle, so at its apex the slope goes from `+4·increment` to
/// `-4·increment` and `slope_change` is `-8·increment`; converting to per-sample
/// units is where the caller's increment enters, and it is the caller's job.
///
/// Both returned terms are ADDED to the trivial signal.
///
/// Out-of-window positions return nothing, exactly as in `poly_blep`. What this
/// does NOT need is that function's per-tap side selection: BLAMP is continuous
/// at the origin — it corrects a break in the slope and leaves the value alone —
/// so both sides agree there (at 1/6) and reading either is the same answer.
/// That continuity is also why the boundary bug `poly_blep` guards against never
/// showed up on a triangle: only a kernel that steps can be read off the wrong
/// side of its own step.
inline Correction poly_blamp(double d, double slope_change) noexcept {
    if (!(d >= 0.0 && d <= 1.0)) return {0.0, 0.0};
    return {slope_change * blamp_residual(-d), slope_change * blamp_residual(1.0 - d)};
}

/// Sub-sample position of a period wrap, for a phase accumulator that has just
/// wrapped through its boundary.
///
/// `phase_after` is the phase remaining *past* the boundary — the accumulator's
/// value once the period has been subtracted — and `increment` is the per-sample
/// phase step, both in the same units (a [0, 1) phase and a normalized
/// increment, typically). The wrap happened `phase_after / increment` samples
/// before the current sample, so the position relative to the preceding sample
/// is one minus that.
///
/// The result feeds `poly_blep`/`poly_blamp` as `d`, with the `before` term
/// landing on the sample that preceded the wrap and `after` on the current one.
/// This is a convenience for the predictable case only: a hard-sync reset or a
/// through-zero-FM direction change is not derivable from the increment, and
/// those callers compute `d` from the event that caused the discontinuity.
///
/// Returns `kNoDiscontinuity` when `increment` is not positive or not finite: a
/// phase that is not advancing has no wrap to place, so the correction must be
/// exactly zero.
///
/// The guard is not decoration. A frozen phase makes this `0.0 / 0.0`, and NaN
/// survives both kernels' range checks to land in the output buffer. Nor can the
/// guard simply return 0: `d = 0` is a legitimate operating point meaning "the
/// discontinuity sits on the `before` sample", and it yields a half-step
/// correction — so returning it would inject a step on every sample of a stopped
/// oscillator, which is the opposite of the intent.
inline double wrap_position(double phase_after, double increment) noexcept {
    if (!(increment > 0.0)) return kNoDiscontinuity;
    const double d = 1.0 - phase_after / increment;
    return std::isfinite(d) ? d : kNoDiscontinuity;
}

} // namespace pulp::signal::osc
