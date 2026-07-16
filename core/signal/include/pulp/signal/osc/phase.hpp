#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace pulp::signal::osc {

/// What produced a phase discontinuity.
enum class PhaseEventKind {
    wrap_forward,  ///< Phase reached 1 and wrapped to 0 (increment > 0).
    wrap_backward, ///< Phase left 0 downward and wrapped to 1 (increment < 0).
    sync,          ///< Phase was forced by `advance_synced()`.
};

/// A single phase discontinuity within one sample period.
///
/// `frac` locates the discontinuity in sub-sample time; `phase_before` and
/// `phase_after` give the jump. A discontinuity-correction stage needs all
/// three: the waveform step it must cancel is
/// `shape(phase_after) - shape(phase_before)`, applied at `frac`.
///
/// For a wrap the endpoints are exact by construction (the crossing *is* the
/// domain boundary), so they carry no information the kind does not. They are
/// still populated so that a corrector can treat wraps and syncs — whose jump
/// is arbitrary — with one code path.
struct PhaseEvent {
    /// Sub-sample position of the discontinuity, relative to the start of the
    /// sample being advanced. See `PhaseAccumulator` for the exact range.
    double frac = 0.0;
    double phase_before = 0.0;
    double phase_after = 0.0;
    PhaseEventKind kind = PhaseEventKind::wrap_forward;
};

/// Phase accumulator over the unit circle [0, 1), suitable as the clock for
/// through-zero FM, hard sync, and per-sample modulation.
///
/// The phase is `double`. At a 20 Hz fundamental and 96 kHz the increment is
/// ~2e-4; `float` carries ~1e-7 of relative error per accumulation there, which
/// integrates into audible pitch error over a held note. `double` keeps the
/// accumulated error below 1.2e-16 per sample (see the drift bound below).
///
/// The increment is passed per sample rather than held as state, because both
/// FM and per-sample modulation change it every sample. Its sign and magnitude
/// are unrestricted:
///
/// - **Negative** increments run the phase backward and wrap through 0 to 1.
///   Through-zero FM requires this: when the modulator drives the instantaneous
///   frequency below zero, the phase must actually reverse. An accumulator that
///   only wraps at the top silently clamps the through-zero region away.
/// - **|increment| > 1** produces several wraps in one sample, which happens
///   when instantaneous frequency exceeds the sample rate.
///
/// ## Wrapping
///
/// `phase += increment` followed by a single `-= 1.0` is wrong for both cases
/// above: it does not wrap at 0 at all, and it wraps at most once. A `while`
/// loop fixes correctness but is unbounded — its trip count is `|increment|`,
/// which a modulator controls, so a runaway modulator stalls the audio thread.
///
/// This uses `n = floor(phase + increment)` instead: it is exact, handles both
/// signs, handles any magnitude, and is branch-free and O(1). On x86-64 (SSE4.1
/// `roundsd`) and AArch64 (`frintm`) `floor` is a single instruction, not a
/// libcall. `std::fmod` would also be correct but is a libcall on the same
/// targets and computes a remainder this already has in hand.
///
/// ## Event positions
///
/// `frac` is measured from the start of the sample being advanced. Its range is
/// `(0, 1]` for a forward wrap and `[0, 1)` for a backward one. The asymmetry is
/// forced, not a convention: with the domain half-open at the top, phase leaves
/// through the top *at* 1 and through the bottom *just past* 0. Closing the
/// range on the other side in either direction drops the wrap that lands exactly
/// on a sample boundary — and a dropped wrap is an uncorrected discontinuity.
/// `sync` events land anywhere in `[0, 1]`, wherever the caller asked.
///
/// Events are reported in chronological order and are valid until the next
/// `advance()`, `advance_synced()`, or `reset()`.
///
/// ## Events compose
///
/// The events of a sample, applied in order, describe the wrapped phase
/// trajectory exactly: each one's `phase_before` is where the previous left off.
/// Two events may share a `frac`, and correction stages must sum them rather
/// than treat that as a contradiction — coincident steps at one position add to
/// a single step of the summed magnitude, which is the right answer in both
/// cases that produce them:
///
/// - Phase rising to exactly 1 at a sample boundary and then reversing reports
///   a forward wrap (1 -> 0) and a backward wrap (0 -> 1). They sum to zero,
///   which is correct: the phase touched the boundary and turned around without
///   ever being discontinuous. Suppressing either one would leave a corrector
///   cancelling a step that never happened.
/// - Syncing to 0 under a negative increment reports the sync (p -> 0) and a
///   backward wrap (0 -> 1) at the same position, summing to p -> 1. The phase
///   is 0 only at that instant; immediately after, it is just below 1.
///
/// ## Bounds and RT contract
///
/// At most `max_events_per_sample` events are reported per advance; past that
/// `truncated()` is set and the surplus is dropped. Truncation needs
/// `|increment| > 8`, i.e. an instantaneous frequency above eight times the
/// sample rate, where the output is not a signal any correction stage could
/// rescue. **The phase itself stays exact when truncated** — only the event list
/// is capped.
///
/// A non-finite increment is absorbed rather than propagated: the phase resets
/// to 0 and no events are reported. Without this a single NaN would poison the
/// phase for the lifetime of the voice.
///
/// Every path allocates nothing, locks nothing, and performs no I/O.
///
/// @code
/// pulp::signal::osc::PhaseAccumulator phase;
/// const int n = phase.advance(freq_hz / sample_rate);
/// for (const auto& e : phase.events())
///     blep.add_step(e.frac, shape(e.phase_after) - shape(e.phase_before));
/// out[i] = shape(phase.phase());
/// @endcode
class PhaseAccumulator {
public:
    /// Event capacity of a single advance. See the truncation note above.
    static constexpr int max_events_per_sample = 8;

    /// Set the phase, wrapped into [0, 1). Clears any reported events.
    void reset(double phase = 0.0) {
        phase_ = wrap_unit(phase);
        count_ = 0;
        truncated_ = false;
    }

    /// Advance one sample by `increment` cycles. Returns the number of events.
    int advance(double increment) {
        count_ = 0;
        truncated_ = false;
        scan(increment, 0.0, 1.0);
        return count_;
    }

    /// Advance one sample by `increment` cycles, forcing the phase to
    /// `sync_phase` at sub-sample position `sync_frac` (clamped to [0, 1]).
    ///
    /// Wraps occurring before and after the reset are reported alongside the
    /// `sync` event, in chronological order — a sync-driven oscillator can wrap
    /// naturally within the same sample that resets it, and both discontinuities
    /// need correcting. Returns the number of events.
    ///
    /// The sync event is reported even when `sync_phase` equals the phase it
    /// replaces; that is a zero-magnitude jump, which a corrector handles as a
    /// no-op without a special case here.
    int advance_synced(double increment, double sync_frac, double sync_phase = 0.0) {
        count_ = 0;
        truncated_ = false;

        const double f = clamp_unit(sync_frac);
        scan(increment * f, 0.0, f);

        const double target = wrap_unit(sync_phase);
        emit(f, phase_, target, PhaseEventKind::sync);
        phase_ = target;

        scan(increment * (1.0 - f), f, 1.0 - f);
        return count_;
    }

    /// Current phase, always in [0, 1).
    double phase() const { return phase_; }

    /// Events from the last advance, in chronological order.
    std::span<const PhaseEvent> events() const {
        return {events_.data(), static_cast<std::size_t>(count_)};
    }

    /// Whether the last advance dropped events past `max_events_per_sample`.
    bool truncated() const { return truncated_; }

private:
    /// Advance the phase by `delta` cycles over the sub-sample span
    /// [`t0`, `t0 + span`], reporting the wraps crossed along the way.
    ///
    /// The phase is linear in sub-sample time, so the unwrapped phase crosses
    /// integer level `m` at `t = (m - p0) / delta`. Going up, levels 1..n are
    /// wraps; going down, levels 0..n+1 are. Either way there are exactly |n|
    /// of them, which is what makes the trip count bounded by the wrap count
    /// rather than by the increment.
    void scan(double delta, double t0, double span) {
        const double p0 = phase_;
        const double raw = p0 + delta;
        const double n = std::floor(raw);

        // Ordered so that a NaN `raw` (from a non-finite increment) fails every
        // comparison and lands on 0 rather than converting NaN to int, which is
        // undefined. The `>= 1.0` case is not defensive: for a raw of -1e-20,
        // floor is -1 and `raw - n` rounds to exactly 1.0, which is outside the
        // half-open domain. Snapping to 0 is exact on the circle.
        double wrapped = raw - n;
        if (!(wrapped >= 0.0 && wrapped < 1.0))
            wrapped = 0.0;

        int events = 0;
        const double magnitude = std::fabs(n);
        if (magnitude > static_cast<double>(max_events_per_sample)) {
            events = max_events_per_sample;
            truncated_ = true;
        } else if (magnitude >= 1.0) {
            events = static_cast<int>(magnitude);
        }

        // p0 is in [0, 1), so raw > 0 whenever delta > 0 and raw < 1 whenever
        // delta < 0. The sign of n therefore names the direction outright, and
        // `delta` is non-zero whenever the loop runs.
        const bool forward = n > 0.0;
        for (int k = 0; k < events; ++k) {
            const double level = forward ? static_cast<double>(k + 1)
                                         : -static_cast<double>(k);
            const double t = (level - p0) / delta;
            emit(t0 + t * span,
                 forward ? 1.0 : 0.0,
                 forward ? 0.0 : 1.0,
                 forward ? PhaseEventKind::wrap_forward
                         : PhaseEventKind::wrap_backward);
        }

        phase_ = wrapped;
    }

    void emit(double frac, double before, double after, PhaseEventKind kind) {
        if (count_ >= max_events_per_sample) {
            truncated_ = true;
            return;
        }
        events_[static_cast<std::size_t>(count_++)] = PhaseEvent{frac, before, after, kind};
    }

    /// Wrap into [0, 1). NaN maps to 0; see `scan` on the `>= 1.0` case.
    static double wrap_unit(double x) {
        double w = x - std::floor(x);
        if (!(w >= 0.0 && w < 1.0))
            w = 0.0;
        return w;
    }

    /// Clamp into [0, 1]. NaN maps to 0.
    static double clamp_unit(double x) {
        if (!(x > 0.0)) return 0.0;
        if (x > 1.0) return 1.0;
        return x;
    }

    double phase_ = 0.0;
    std::array<PhaseEvent, max_events_per_sample> events_{};
    int count_ = 0;
    bool truncated_ = false;
};

} // namespace pulp::signal::osc
