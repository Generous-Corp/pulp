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
/// rather than asserting an aspiration.
///
/// A free-running sine is exact and bit-identical to `std::sin`: it is continuous
/// across a wrap, so every step the corrector computes for it is exactly zero and
/// is skipped. That is a property of the WRAP, not a property of the shape — a
/// sync jumps a sine to an unrelated phase and genuinely steps, and is corrected
/// like any other break. Deciding "sine, nothing to do" once up front would be
/// wrong for exactly that case.
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
/// ── Hard sync and through-zero FM ─────────────────────────────────────────
///
/// Through-zero FM needs no API of its own: frequency arrives as a per-sample
/// increment, so a modulator driving the instantaneous frequency negative is
/// just a caller passing a negative increment, and the accumulator wraps in both
/// directions already. Hard sync adds one entry point, `next_synced`.
///
/// Both compose with everything above, and they do so structurally rather than
/// by enumerating combinations: each event carries the endpoints of its own jump,
/// so a sample's discontinuities are corrected by summing their steps, and
/// coincident events telescope. Measured benefit over the trivial waveform is
/// 11-34 dB across sync, TZFM, and the two together.
///
/// The wall is the carrier, not the corrector. Once the instantaneous frequency
/// approaches Nyquist the aliasing stops coming from discontinuities and starts
/// coming from the waveform itself being unrepresentable, and no discontinuity
/// correction addresses that — a perfectly corrected step does not bandlimit a
/// carrier running past half the sample rate. Past that point the correction's
/// benefit falls to nothing (measured: a synced sine gains 12 dB at a 5 kHz
/// deviation, 6 dB at 20 kHz, and nothing at all at 60 kHz, where the aliases
/// exceed the fundamental outright). Fixing that needs oversampling on the FM
/// path and is not attempted here.

#include "blep.hpp"
#include "phase.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>

namespace pulp::signal::osc {

/// The static shapes.
enum class VaShape {
    sine,     ///< Continuous across a wrap; a sync still steps it.
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
        const double entry_phase = phase_.phase();
        double out = value(entry_phase) + carry_;
        carry_ = 0.0;

        phase_.advance(increment);
        correct_events(increment, out);
        // No sync, so the phase ran the whole sample at one rate from one place:
        // the sample is a single segment.
        scan_threshold(entry_phase, increment, increment, 0.0, 1.0, out);
        return out;
    }

    /// Generate one sample, advancing by `increment` but forcing the phase to
    /// `sync_phase` at sub-sample position `sync_frac` — a hard sync.
    ///
    /// Call this only on the samples where a sync actually fires. It emits a sync
    /// event unconditionally, including a zero-magnitude one when the target
    /// equals the phase it replaces, so calling it every sample would report a
    /// sync every sample. The zero-magnitude case costs nothing (the corrector
    /// skips a zero step) but the intent would be wrong.
    ///
    /// The reset is a value step of ARBITRARY magnitude, unlike a wrap's known
    /// 1 -> 0. That magnitude is read off the event's endpoints rather than
    /// assumed, which is the same path a wrap takes — see `correct_events`.
    double next_synced(double increment, double sync_frac, double sync_phase = 0.0) noexcept {
        const double entry_phase = phase_.phase();
        double out = value(entry_phase) + carry_;
        carry_ = 0.0;

        phase_.advance_synced(increment, sync_frac, sync_phase);
        correct_events(increment, out);

        // A sync SPLITS the sample in two. The phase runs from `entry_phase` over
        // sub-sample span [0, f], is then replaced outright, and runs from the
        // target over [f, 1]. An internal threshold must therefore be scanned
        // once per segment, from that segment's own starting phase: a single scan
        // of (entry_phase, increment) describes a trajectory the phase never
        // took, and would place the apex where it never was while missing the one
        // it actually crossed after the reset.
        //
        // This mirrors `advance_synced`'s own segmentation — the same split, the
        // same sub-sample spans — because that split IS the semantics, not an
        // implementation detail to approximate.
        const double f = clamp_unit(sync_frac);
        scan_threshold(entry_phase, increment * f, increment, 0.0, f, out);
        scan_threshold(wrap_unit(sync_phase), increment * (1.0 - f), increment, f, 1.0 - f, out);
        return out;
    }

private:
    /// Correct every discontinuity the accumulator reported this sample.
    ///
    /// One path for wraps and syncs alike: both carry the endpoints of their own
    /// jump, so the step is derived rather than assumed, and a sync's arbitrary
    /// magnitude needs no special case. Events at a shared position simply sum,
    /// which is what makes the composed cases come out right without being
    /// enumerated — a sync to 0 under a negative increment reports the sync
    /// (p -> 0) AND a backward wrap (0 -> 1) at one position, and summing their
    /// steps telescopes to p -> 1 with the intermediate cancelling exactly.
    void correct_events(double increment, double& out) noexcept {
        for (const PhaseEvent& e : phase_.events()) {
            const double value_step = value(e.phase_after) - value(e.phase_before);
            const double slope_step =
                (slope_per_cycle(e.phase_after) - slope_per_cycle(e.phase_before)) * increment;
            add_break(e.frac, value_step, slope_step, out);
        }
    }

    /// Correct the shape's internal threshold over one segment of a sample.
    ///
    /// `start_phase` is the phase entering the segment and `delta` the phase it
    /// covers (the increment scaled by the segment's share of the sample), while
    /// `rate` stays the FULL per-sample increment: a slope break's size is a
    /// per-sample slope, which the segmentation does not change — only the span
    /// does. Crossings come back in the segment's own normalized time and are
    /// mapped onto the sample by `t0 + t * span`, exactly as the accumulator maps
    /// its own events.
    void scan_threshold(double start_phase, double delta, double rate, double t0, double span,
                        double& out) noexcept {
        Threshold t;
        if (!internal_threshold(t)) return;

        double fracs[PhaseAccumulator::max_events_per_sample];
        const int n = threshold_crossings(start_phase, delta, t.phase, std::span<double>(fracs));
        if (n == 0) return;

        // Time order decides which side is "before": going forward the phase
        // arrives from below the threshold, going backward from above.
        const bool forward = delta > 0.0;
        const double value_step =
            forward ? (t.value_above - t.value_below) : (t.value_below - t.value_above);
        const double slope_step =
            (forward ? (t.slope_above - t.slope_below) : (t.slope_below - t.slope_above)) * rate;

        for (int k = 0; k < n; ++k)
            add_break(t0 + fracs[k] * span, value_step, slope_step, out);
    }

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
                // sin is periodic, so its limit at the top of the period is
                // sin(0) = 0 EXACTLY. `std::sin(2*pi)` returns -2.4e-16 instead,
                // because 2*pi is not exactly representable — and handing that to
                // the corrector applies a BLEP at every wrap of a shape that does
                // not step there at all. Mapping the sentinel to its exact limit
                // keeps a wrap's step exactly zero, which the corrector then
                // skips outright, leaving a plain sine bit-identical to
                // `std::sin`. That bit-exactness is the null proving the
                // corrector is inert on a continuous shape, and it is worth more
                // than the 2.4e-16 it costs to protect.
                //
                // A SYNC on a sine is a different matter and IS corrected: the
                // reset jumps to an unrelated phase, so the value genuinely
                // steps. It is the wrap that is continuous here, not the shape.
                return p >= 1.0 ? 0.0 : std::sin(2.0 * std::numbers::pi * p);
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

    /// d(value)/d(phase), per cycle — the size of a slope break before it is
    /// scaled by the per-sample increment.
    ///
    /// The sine carries its true derivative because a sync breaks a sine's slope
    /// as well as its value, and both need correcting. It costs nothing at a
    /// wrap: `cos(2*pi)` evaluates to exactly 1.0 (unlike `sin`, whose limit at
    /// the sentinel is the fragile one), so a wrap's slope step is exactly zero
    /// and is skipped.
    double slope_per_cycle(double p) const noexcept {
        switch (shape_) {
            case VaShape::sine:
                return 2.0 * std::numbers::pi * std::cos(2.0 * std::numbers::pi * p);
            case VaShape::saw:
                return 2.0;
            case VaShape::triangle:
                return p < 0.5 ? 4.0 : -4.0;
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

    /// Wrap into [0, 1), matching how the accumulator normalizes a sync target.
    ///
    /// The segmented threshold scan needs the phase the sync will actually land
    /// on, and the accumulator wraps the caller's `sync_phase` before using it.
    /// Reading a raw, unwrapped target here would scan the second segment from a
    /// phase the oscillator never occupies. NaN maps to 0, as it does there.
    static double wrap_unit(double x) noexcept {
        const double w = x - std::floor(x);
        return (w >= 0.0 && w < 1.0) ? w : 0.0;
    }

    PhaseAccumulator phase_;
    double carry_ = 0.0;
    double pulse_width_ = 0.5;
    VaShape shape_ = VaShape::saw;
};

} // namespace pulp::signal::osc
