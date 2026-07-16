#pragma once

/// @file va.hpp
/// Virtual-analog oscillator: the classic static shapes, bandlimited by
/// composing `PhaseAccumulator` (the clock) with the BLEP/BLAMP kernels (the
/// correction).
///
/// ── What this is ──────────────────────────────────────────────────────────
///
/// Four shapes — sine, saw, square (with pulse width), triangle — generated
/// trivially from the phase and then corrected at each discontinuity. The
/// oscillator itself owns almost no maths: the accumulator locates
/// discontinuities in sub-sample time, the kernels say what to add, and this
/// class is the wiring plus each shape's analytic description of its own breaks.
///
/// ── How good it is, in numbers ────────────────────────────────────────────
///
/// polyBLEP is a two-point approximation, so these shapes are **improved, not
/// alias-free**: roughly 11-15 dB better than the trivial shape below 20 kHz.
/// That is what the kernels can buy and no more; a deep-floor claim needs a
/// tabulated minBLEP, not this. `test_osc_va.cpp` measures the figure per shape
/// rather than asserting an aspiration. Sine is exact — it has no discontinuity
/// to correct, and the correction path does not run for it at all.
///
/// ── Per-sample frequency ──────────────────────────────────────────────────
///
/// `next()` takes the phase increment per call rather than holding a frequency,
/// because FM and per-sample modulation change it every sample. Nothing here
/// caches a value derived from the increment across samples.
///
/// ── Discontinuities, and where they come from ─────────────────────────────
///
/// A shape breaks in two places, and they are found two different ways:
///
///   * **At the phase wrap**, which `PhaseAccumulator` already reports as an
///     event with an exact sub-sample `frac`. The event's `phase_before` /
///     `phase_after` are used to derive the break generically, which is why a
///     `sync` event — whose jump is arbitrary rather than 1 -> 0 — would flow
///     through the same path without a special case.
///   * **At a threshold inside the period** — the square's pulse edge at
///     `pulse_width`, the triangle's apex at 0.5. These are NOT wraps, so the
///     accumulator does not report them and this file locates them itself, with
///     `threshold_crossings` below. Missing one is not a subtle degradation: an
///     uncorrected edge aliases at full strength, which is most of what the
///     correction was for.
///
/// Both feed the same corrector. A value step gets BLEP; a slope break gets
/// BLAMP; a break with both (which a sync on a triangle would produce) gets
/// both, summed.
///
/// ── Latency and the two-sample reach ──────────────────────────────────────
///
/// The kernels span the two samples straddling a discontinuity, so a correction
/// found while generating sample *i* also owes a term to sample *i+1*. That term
/// is carried in a single scalar and added on the next call, which costs one
/// double of state and **no latency** — the alternative, a one-sample delay
/// line, would delay the whole signal to deliver a correction the carry
/// delivers on time. Several discontinuities in one sample simply sum into the
/// same carry, which is the correct composition rule.
///
/// RT contract: no allocation, no locks, no I/O on the per-sample path. `sine`
/// calls `std::sin`, which is a libcall but none of those things; a table-driven
/// sine is a separate concern from correcting discontinuities.
///
/// Precision: `double` throughout, matching the accumulator and the kernels. A
/// `float` caller narrows once on store.
///
/// Not built here: hard sync and through-zero FM. The accumulator supports both
/// and the corrector path is written so neither is designed out, but neither is
/// wired up or measured, and nothing in this file should be read as a claim
/// about them.

#include "blep.hpp"
#include "phase.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>

namespace pulp::signal::osc {

/// The static shapes.
enum class VaShape {
    sine,     ///< Continuous: needs no correction, and gets none.
    saw,      ///< Value step of -2 at the wrap.
    square,   ///< Value steps of +2 at the wrap and -2 at the pulse edge.
    triangle, ///< Slope breaks at the wrap and the apex; value is continuous.
};

/// Sub-sample positions at which the phase crosses `threshold` (mod 1) during
/// one sample, written to `out` in chronological order. Returns the count.
///
/// This is `PhaseAccumulator`'s wrap-finding inversion generalized from integer
/// levels to a level offset by `threshold`: the phase is linear in sub-sample
/// time, so the unwrapped phase reaches level `m + threshold` at exactly
/// `t = (m + threshold - phase_before) / increment`. Shifting the phase by
/// `-threshold` turns "cross the threshold" back into "cross an integer", which
/// is a problem the accumulator has already solved exactly.
///
/// It is that shared derivation, not a re-implementation, that makes an apex or
/// a pulse edge land with the same accuracy as a wrap — and at `threshold = 0`
/// this reproduces the accumulator's own wrap positions, which is asserted
/// rather than assumed (`test_osc_va.cpp`).
///
/// Ranges match the accumulator's for the same reason they do there: `(0, 1]`
/// going forward, `[0, 1)` going backward. Closing the other side would drop a
/// crossing landing exactly on a sample boundary, and a dropped crossing is an
/// uncorrected discontinuity.
///
/// Returns 0 for a zero or non-finite increment: a phase that is not advancing
/// crosses nothing, and absorbing the non-finite case here keeps it from
/// reaching the kernels. At most `out.size()` crossings are reported; the
/// surplus is dropped, which needs an instantaneous frequency above `out.size()`
/// times the sample rate.
inline int threshold_crossings(double phase_before_advance, double increment,
                               double threshold, std::span<double> out) noexcept {
    if (!std::isfinite(increment) || increment == 0.0) return 0;

    // Shift so the threshold sits on an integer, then the crossings are the
    // integers strictly between the endpoints.
    const double q0 = phase_before_advance - threshold;
    const double first = std::floor(q0);
    const double last = std::floor(q0 + increment);
    if (!std::isfinite(first) || !std::isfinite(last)) return 0;

    const bool forward = increment > 0.0;
    const double magnitude = forward ? (last - first) : (first - last);
    if (!(magnitude >= 1.0)) return 0;

    const int capacity = static_cast<int>(out.size());
    const int count = magnitude > static_cast<double>(capacity)
                          ? capacity
                          : static_cast<int>(magnitude);

    for (int k = 0; k < count; ++k) {
        const double level =
            forward ? first + 1.0 + static_cast<double>(k) : first - static_cast<double>(k);
        out[static_cast<std::size_t>(k)] = (level - q0) / increment;
    }
    return count;
}

/// Virtual-analog oscillator over the static shapes.
class VaOscillator {
public:
    void set_shape(VaShape shape) noexcept { shape_ = shape; }
    VaShape shape() const noexcept { return shape_; }

    /// Fraction of the period the square spends high, clamped to [0, 1]. A
    /// width of exactly 0 or 1 leaves no edge and the square degenerates to DC,
    /// which is the honest limit of the shape rather than an error.
    ///
    /// Widths narrower than one sample cannot be represented: the pulse's two
    /// edges land inside one sample period, their corrections overlap, and the
    /// result is a bounded but inaccurate pulse. That is polyBLEP's limit, not a
    /// bug to fix here — the output stays finite and bounded, which is what
    /// `test_osc_va.cpp` pins.
    void set_pulse_width(double width) noexcept { pulse_width_ = clamp_unit(width); }
    double pulse_width() const noexcept { return pulse_width_; }

    /// Set the phase and drop any pending correction.
    void reset(double phase = 0.0) noexcept {
        phase_.reset(phase);
        carry_ = 0.0;
    }

    /// Current phase, in [0, 1).
    double phase() const noexcept { return phase_.phase(); }

    /// Generate one sample and advance by `increment` cycles.
    ///
    /// The returned sample is the shape at the phase on entry, so the advance
    /// spans from this sample to the next one, and a discontinuity inside that
    /// span owes its `before` term to this sample and its `after` term to the
    /// next. That is the whole reason the increment is read here rather than
    /// held: the span this call corrects is defined by the increment this call
    /// was given.
    double next(double increment) noexcept {
        double out = value(phase_.phase()) + carry_;
        carry_ = 0.0;

        const double entry_phase = phase_.phase();
        phase_.advance(increment);

        // Continuous everywhere: there is nothing to correct, and running the
        // corrector anyway would add a rounding-sized term for no reason.
        if (shape_ == VaShape::sine) return out;

        // Wraps (and syncs, were they wired up) come from the accumulator, which
        // reports the jump's endpoints — so the break is derived, not assumed.
        for (const PhaseEvent& e : phase_.events()) {
            const double value_step = value(e.phase_after) - value(e.phase_before);
            const double slope_step =
                (slope_per_cycle(e.phase_after) - slope_per_cycle(e.phase_before)) * increment;
            add_break(e.frac, value_step, slope_step, out);
        }

        // The break inside the period, which the accumulator cannot know about.
        Threshold t;
        if (internal_threshold(t)) {
            double fracs[PhaseAccumulator::max_events_per_sample];
            const int n = threshold_crossings(entry_phase, increment, t.phase,
                                              std::span<double>(fracs));
            // Time order decides which side is "before": going forward the phase
            // arrives from below the threshold, going backward from above.
            const bool forward = increment > 0.0;
            const double value_step =
                forward ? (t.value_above - t.value_below) : (t.value_below - t.value_above);
            const double slope_step =
                (forward ? (t.slope_above - t.slope_below) : (t.slope_below - t.slope_above)) *
                increment;
            for (int k = 0; k < n; ++k)
                add_break(fracs[k], value_step, slope_step, out);
        }

        return out;
    }

private:
    /// A shape's one-sided limits at its internal threshold.
    ///
    /// Declared analytically rather than probed by evaluating the shape at
    /// `threshold ± epsilon`, which would make the answer depend on epsilon and
    /// silently collapse to no break once epsilon fell below the phase's
    /// resolution.
    struct Threshold {
        double phase = 0.0;
        double value_below = 0.0;
        double value_above = 0.0;
        double slope_below = 0.0; ///< Per cycle; scaled by the increment on use.
        double slope_above = 0.0;
    };

    bool internal_threshold(Threshold& t) const noexcept {
        switch (shape_) {
            case VaShape::square:
                // No edge at all when the pulse fills the period or none of it.
                if (!(pulse_width_ > 0.0 && pulse_width_ < 1.0)) return false;
                t = {pulse_width_, 1.0, -1.0, 0.0, 0.0};
                return true;
            case VaShape::triangle:
                // The apex: value is continuous, the slope reverses.
                t = {0.5, 1.0, 1.0, 4.0, -4.0};
                return true;
            case VaShape::sine:
            case VaShape::saw:
                return false;
        }
        return false;
    }

    /// The trivial shape, before any correction.
    ///
    /// **Invariant: `value(1)` must equal the limit of `value(p)` as p rises to
    /// 1.** The phase's domain is half-open, so a wrap event denotes the top of
    /// the period as `phase_before = 1` (or `phase_after = 1` running backward).
    /// That 1 is a sentinel for the limit, not a sample sitting at 1 — no sample
    /// ever has phase 1 — and the corrector derives the wrap's step by
    /// evaluating this function there. A shape that disagrees with its own limit
    /// at 1 hands the corrector a step that does not exist, and a BLEP cancelling
    /// an imaginary step is not a small error: it is a full-amplitude one.
    ///
    /// The triangle is computed straight from the phase rather than by
    /// integrating a square. An integrator's output level depends on the ratio
    /// of the cutoff to the fundamental, which makes the triangle quieter as it
    /// rises in pitch; deriving it from the phase makes the level
    /// pitch-independent by construction. `test_osc_va.cpp` gates that.
    double value(double p) const noexcept {
        switch (shape_) {
            case VaShape::sine:
                return std::sin(2.0 * std::numbers::pi * p);
            case VaShape::saw:
                // Limit at 1 is +1, which is what `2p - 1` gives at p = 1.
                return 2.0 * p - 1.0;
            case VaShape::square:
                // A width of exactly 1 fills the period: the shape is constantly
                // high, so its limit at the top is high too. Testing `p < width`
                // alone would report LOW at the p = 1 sentinel and manufacture a
                // +2 step at a wrap that has no edge on either side of it.
                return (pulse_width_ >= 1.0 || p < pulse_width_) ? 1.0 : -1.0;
            case VaShape::triangle:
                // Limit at 1 is -1, which is what the descending leg gives at 1.
                return p < 0.5 ? (4.0 * p - 1.0) : (3.0 - 4.0 * p);
        }
        return 0.0;
    }

    /// d(value)/d(phase), per cycle. Used only to size slope breaks, so the
    /// sine's exact derivative is irrelevant — it never reaches the corrector.
    double slope_per_cycle(double p) const noexcept {
        switch (shape_) {
            case VaShape::saw:
                return 2.0;
            case VaShape::triangle:
                return p < 0.5 ? 4.0 : -4.0;
            case VaShape::sine:
            case VaShape::square:
                return 0.0;
        }
        return 0.0;
    }

    /// Apply one discontinuity: BLEP for the value step, BLAMP for the slope
    /// break, `before` onto this sample and `after` onto the next.
    void add_break(double frac, double value_step, double slope_step, double& out) noexcept {
        if (value_step != 0.0) {
            const Correction c = poly_blep(frac, value_step);
            out += c.before;
            carry_ += c.after;
        }
        if (slope_step != 0.0) {
            const Correction c = poly_blamp(frac, slope_step);
            out += c.before;
            carry_ += c.after;
        }
    }

    static double clamp_unit(double x) noexcept {
        if (!(x > 0.0)) return 0.0;
        if (x > 1.0) return 1.0;
        return x;
    }

    PhaseAccumulator phase_;
    double carry_ = 0.0;
    double pulse_width_ = 0.5;
    VaShape shape_ = VaShape::saw;
};

} // namespace pulp::signal::osc
