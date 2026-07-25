#pragma once

/// @file trigger_kit.hpp
/// The event grammar of a signal-rate patch: what counts as an edge, how a
/// gate is made, and how a clock is divided, multiplied, delayed, and burst.
///
/// In a modular graph the thing that says "now" is another block's output, not
/// a function call. That makes the definition of an edge load-bearing: a
/// sequencer that advances on a level rather than an edge double-fires on a
/// slow clock, and one that uses a single threshold chatters on a noisy one.
/// Every block in this header therefore shares two decisions:
///
///   - **Edges are detected with hysteresis**, not a single threshold. A
///     rising edge needs the input above `high_threshold`; the block will not
///     fire again until the input has fallen below `low_threshold`. A clock
///     that hovers at the threshold produces one edge, not a burst.
///   - **Everything is edge-triggered, never level-triggered**, so a block's
///     behaviour does not depend on how long the upstream gate happened to
///     stay high.
///
/// The domain is deliberately unit-agnostic: thresholds default to values that
/// work for both 0/1 logic and ±5 V control voltage, because the same
/// sequencer is used in both and neither should have to convert.
///
/// **Determinism (series law 2):** only `ProbGateT`-style probabilistic blocks
/// need randomness, and those live with the sequencers that use them. Nothing
/// in this header draws a random number, so every block here is trivially
/// bit-reproducible.
///
/// RT contract: every block's `prepare()` recomputes sample counts and
/// allocates nothing. `set_*`, `process()`, and `reset()` allocate nothing,
/// take no locks, and perform no I/O. All state is POD and zero-init is a
/// valid idle state.

#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// Default thresholds for edge detection, chosen to work unchanged for 0/1
/// logic, 0/5 V gates, and ±5 V bipolar clocks.
/// [design parameter] high 0.5, low 0.25; range 0.01 .. 2.5 with `low < high`.
inline constexpr double kTriggerHighThreshold = 0.5;
inline constexpr double kTriggerLowThreshold = 0.25;

/// Detects rising and falling edges in a continuous signal, with hysteresis.
///
/// This is the block every other one in the header is built from, so an edge
/// means exactly one thing across the whole library.
template <typename SampleType = float>
class TriggerDetectT {
public:
    /// Sets the hysteresis window. `low` is clamped below `high` so an
    /// inverted pair cannot silently disable the hysteresis.
    void set_thresholds(double high, double low) {
        high_ = high;
        low_ = std::min(low, high - 1e-9);
    }

    /// Clears the edge memory. The next sample above `high` is a rising edge
    /// regardless of what the input was before the reset.
    void reset() { armed_ = true; }

    /// True while the input is above the hysteresis window.
    bool high() const { return !armed_; }

    /// Advances one sample. Returns true exactly on the sample where a rising
    /// edge is detected.
    bool process(SampleType input) {
        const double x = static_cast<double>(input);
        if (armed_ && x >= high_) {
            armed_ = false;
            return true;
        }
        if (!armed_ && x <= low_) armed_ = true;
        return false;
    }

    /// Advances one sample and reports both edge directions. `rising` and
    /// `falling` are never both true on the same sample.
    void process(SampleType input, bool& rising, bool& falling) {
        const double x = static_cast<double>(input);
        rising = false;
        falling = false;
        if (armed_ && x >= high_) {
            armed_ = false;
            rising = true;
        } else if (!armed_ && x <= low_) {
            armed_ = true;
            falling = true;
        }
    }

private:
    double high_ = kTriggerHighThreshold;
    double low_ = kTriggerLowThreshold;
    bool armed_ = true;
};

using TriggerDetect = TriggerDetectT<float>;

/// Compares a signal against a threshold and outputs a gate — the analogue
/// front end of `TriggerDetectT`, for the case where the downstream block
/// wants a level rather than an event.
template <typename SampleType = float>
class ComparatorT {
public:
    void set_thresholds(double high, double low) { detector_.set_thresholds(high, low); }

    /// Output levels for the low and high states. Defaults to 0/1; a patch in
    /// the voltage domain sets 0/5.
    void set_levels(SampleType low_level, SampleType high_level) {
        low_level_ = low_level;
        high_level_ = high_level;
    }

    void reset() { detector_.reset(); }

    SampleType process(SampleType input) {
        detector_.process(input);
        return detector_.high() ? high_level_ : low_level_;
    }

private:
    TriggerDetectT<SampleType> detector_{};
    SampleType low_level_ = SampleType{0};
    SampleType high_level_ = SampleType{1};
};

using Comparator = ComparatorT<float>;

/// Turns a trigger into a gate of a stated length. A trigger says "now"; a
/// gate says "for this long", and an envelope needs the second.
///
/// Retriggering during an open gate restarts the timer rather than extending
/// or ignoring it, which is what a re-struck note means.
template <typename SampleType = float>
class GateGenT {
public:
    /// Gate length in ms.
    /// [design parameter] default 10 ms, range 0.1 .. 10000 ms.
    static constexpr double kDefaultLengthMs = 10.0;

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        update();
    }

    void set_length_ms(double ms) {
        length_ms_ = std::max(ms, 0.0);
        update();
    }

    void set_levels(SampleType low_level, SampleType high_level) {
        low_level_ = low_level;
        high_level_ = high_level;
    }

    void reset() {
        remaining_ = 0;
        detector_.reset();
    }

    bool open() const { return remaining_ > 0; }

    /// Advances one sample, opening the gate on a rising edge of `trigger`.
    SampleType process(SampleType trigger) {
        if (detector_.process(trigger)) remaining_ = length_samples_;
        if (remaining_ > 0) {
            --remaining_;
            return high_level_;
        }
        return low_level_;
    }

private:
    void update() {
        length_samples_ = static_cast<std::int64_t>(
            std::llround(units::ms_to_samples(length_ms_, sample_rate_)));
        if (length_samples_ < 0) length_samples_ = 0;
    }

    double sample_rate_ = 44100.0;
    double length_ms_ = kDefaultLengthMs;
    std::int64_t length_samples_ = 0;
    std::int64_t remaining_ = 0;
    TriggerDetectT<SampleType> detector_{};
    SampleType low_level_ = SampleType{0};
    SampleType high_level_ = SampleType{1};
};

using GateGen = GateGenT<float>;

/// Passes one input trigger in every `n` — the clock divider.
///
/// The first edge after a reset always passes, so a divided clock's downbeat
/// lands with its source's rather than `n − 1` edges later. That is the
/// difference between a divider you can start a piece with and one you cannot.
template <typename SampleType = float>
class ClockDividerT {
public:
    /// Division factor. 1 passes everything.
    /// [design parameter] default 2, range 1 .. 64.
    static constexpr int kDefaultDivision = 2;

    void set_division(int n) { division_ = std::clamp(n, 1, 1024); }
    int division() const { return division_; }

    void reset() {
        counter_ = 0;
        detector_.reset();
    }

    /// Advances one sample. Returns true on the edges that pass.
    bool process(SampleType clock) {
        if (!detector_.process(clock)) return false;
        const bool pass = counter_ == 0;
        counter_ = (counter_ + 1) % division_;
        return pass;
    }

private:
    int division_ = kDefaultDivision;
    int counter_ = 0;
    TriggerDetectT<SampleType> detector_{};
};

using ClockDivider = ClockDividerT<float>;

/// Emits `n` evenly spaced triggers per input clock period — the clock
/// multiplier, and the primitive a per-stage ratchet is built from.
///
/// A multiplier cannot know the period of an edge that has not happened yet,
/// so it measures the LAST period and subdivides the next one. The consequence
/// is stated rather than hidden: on a clock that is speeding up, the
/// subdivisions of the current period are slightly too slow, and the error
/// closes over one period. On a steady clock the error is zero. There is no
/// way around this short of a full period of latency, which a ratchet cannot
/// afford.
template <typename SampleType = float>
class ClockMultT {
public:
    /// Multiplication factor. 1 passes the input clock through.
    /// [design parameter] default 2, range 1 .. 32.
    static constexpr int kDefaultMultiple = 2;

    /// Longest input period the multiplier will track, in ms. Beyond this the
    /// clock is treated as stopped rather than as very slow, so a halted
    /// sequencer does not emit a subdivision minutes later.
    /// [design parameter] default 4000 ms, range 250 .. 30000 ms.
    static constexpr double kMaxPeriodMs = 4000.0;

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        max_period_samples_ = units::ms_to_samples(kMaxPeriodMs, sample_rate_);
    }

    void set_multiple(int n) { multiple_ = std::clamp(n, 1, 64); }
    int multiple() const { return multiple_; }

    void reset() {
        detector_.reset();
        since_edge_ = 0.0;
        period_ = 0.0;
        next_at_ = 0.0;
        pending_ = 0;
        have_edge_ = false;
    }

    /// Advances one sample. Returns true on each emitted trigger, including the
    /// one coincident with the input edge.
    bool process(SampleType clock) {
        bool fired = false;
        since_edge_ += 1.0;

        if (detector_.process(clock)) {
            // The FIRST edge after a reset has no preceding edge, so there is
            // no period to subdivide — `since_edge_` at that point is just the
            // number of samples since the reset, which is not musically related
            // to anything. Subdividing it would fire a burst of triggers at
            // whatever moment the graph happened to start. A period longer than
            // the ceiling is the same situation arriving later: the clock
            // stopped and restarted, so this edge starts a new measurement
            // rather than closing an old one.
            const bool usable = have_edge_ && since_edge_ <= max_period_samples_;
            period_ = usable ? since_edge_ : 0.0;
            have_edge_ = true;
            since_edge_ = 0.0;
            pending_ = period_ > 0.0 ? multiple_ - 1 : 0;
            next_at_ = period_ / multiple_;
            return true;  // the input edge is always subdivision 0
        }

        while (pending_ > 0 && period_ > 0.0 && since_edge_ >= next_at_) {
            --pending_;
            next_at_ += period_ / multiple_;
            fired = true;
        }
        return fired;
    }

private:
    double sample_rate_ = 44100.0;
    double max_period_samples_ = 44100.0 * 4.0;
    int multiple_ = kDefaultMultiple;
    double since_edge_ = 0.0;
    double period_ = 0.0;
    double next_at_ = 0.0;
    int pending_ = 0;
    bool have_edge_ = false;
    TriggerDetectT<SampleType> detector_{};
};

using ClockMult = ClockMultT<float>;

/// Delays a trigger by a stated time. The one-shot counterpart of a delay
/// line, for events rather than audio.
///
/// A trigger arriving while another is in flight REPLACES it rather than
/// queueing: a queue would need an unbounded buffer, and the musical meaning
/// of "delay this trigger" on a stream faster than the delay is ambiguous
/// anyway. The choice is stated so a caller who needs a queue builds one.
template <typename SampleType = float>
class TrigDelayT {
public:
    /// [design parameter] default 0 ms, range 0 .. 10000 ms.
    static constexpr double kDefaultDelayMs = 0.0;

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        update();
    }

    void set_delay_ms(double ms) {
        delay_ms_ = std::max(ms, 0.0);
        update();
    }

    void reset() {
        countdown_ = -1;
        detector_.reset();
    }

    /// Advances one sample. Returns true when a delayed trigger fires.
    bool process(SampleType trigger) {
        if (detector_.process(trigger)) countdown_ = delay_samples_;
        if (countdown_ < 0) return false;
        if (countdown_ == 0) {
            countdown_ = -1;
            return true;
        }
        --countdown_;
        return false;
    }

private:
    void update() {
        delay_samples_ = static_cast<std::int64_t>(
            std::llround(units::ms_to_samples(delay_ms_, sample_rate_)));
        if (delay_samples_ < 0) delay_samples_ = 0;
    }

    double sample_rate_ = 44100.0;
    double delay_ms_ = kDefaultDelayMs;
    std::int64_t delay_samples_ = 0;
    std::int64_t countdown_ = -1;
    TriggerDetectT<SampleType> detector_{};
};

using TrigDelay = TrigDelayT<float>;

/// Emits a fixed-count burst of evenly spaced triggers from one input trigger.
///
/// Unlike `ClockMultT` the spacing is stated in ms rather than derived from an
/// input period, because a burst is a gesture with its own tempo — a flam, a
/// stutter, a roll — not a subdivision of the clock that started it.
template <typename SampleType = float>
class BurstGenT {
public:
    /// [design parameter] count default 4, range 1 .. 64.
    static constexpr int kDefaultCount = 4;

    /// [design parameter] interval default 50 ms, range 1 .. 2000 ms.
    static constexpr double kDefaultIntervalMs = 50.0;

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        update();
    }

    void set_count(int count) { count_ = std::clamp(count, 1, 256); }

    void set_interval_ms(double ms) {
        interval_ms_ = std::max(ms, 0.0);
        update();
    }

    void reset() {
        remaining_ = 0;
        countdown_ = 0;
        detector_.reset();
    }

    /// True while a burst is still emitting.
    bool busy() const { return remaining_ > 0; }

    /// Advances one sample. Returns true on each trigger of the burst,
    /// including the one coincident with the input edge.
    bool process(SampleType trigger) {
        if (detector_.process(trigger)) {
            remaining_ = count_ - 1;
            countdown_ = interval_samples_;
            return true;
        }
        if (remaining_ <= 0) return false;
        if (--countdown_ <= 0) {
            --remaining_;
            countdown_ = interval_samples_;
            return true;
        }
        return false;
    }

private:
    void update() {
        interval_samples_ = static_cast<std::int64_t>(
            std::llround(units::ms_to_samples(interval_ms_, sample_rate_)));
        if (interval_samples_ < 1) interval_samples_ = 1;
    }

    double sample_rate_ = 44100.0;
    int count_ = kDefaultCount;
    double interval_ms_ = kDefaultIntervalMs;
    std::int64_t interval_samples_ = 1;
    std::int64_t remaining_ = 0;
    std::int64_t countdown_ = 0;
    TriggerDetectT<SampleType> detector_{};
};

using BurstGen = BurstGenT<float>;

}  // namespace pulp::signal
