#pragma once

/// @file trigger.hpp
/// The event-domain kit: edge detection, gate generation, clock division and
/// multiplication, bursts, and trigger delay.
///
/// RT contract: every type here holds fixed scalar state — `TrigDelayT`'s queue
/// is a fixed-capacity array — and owns no memory. `prepare()` and the
/// `set_*()` setters are control-side calls; `process()` and `reset()` allocate
/// nothing and are audio-thread safe.
///
/// USE: complex patches need event plumbing, not only smooth signals. The
/// vocabulary is the modular one: a **trigger** is a one-sample event, a
/// **gate** is a boolean with a duration. `ComparatorT` turns any continuous
/// signal into a gate; these six primitives turn gates and triggers into each
/// other. Every one of them composes with the envelope family, which is what
/// makes "a kick drum's transient fires a ratcheted filter ping" three lines of
/// wiring rather than a bespoke state machine.

#include <pulp/signal/mod_tools.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// Rising-edge detector with a refractory period.
///
/// After it fires, further edges are ignored for `refractory_ms`. Real gate CV
/// and real thresholded audio both chatter at the transition; without the
/// refractory window a single musical event fires two or three triggers, and
/// everything downstream doubles.
///
/// RT contract: `prepare()` and setters are control-side; `process()`,
/// `process_signal()`, and `reset()` allocate nothing.
///
/// USE: extracting note-ons from gate CV; clocking a `SampleHoldT`; resetting
/// an LFO on a beat; converting an envelope follower or a `TransientDetectorT`
/// output into discrete hits.
template <typename SampleType = float>
class TriggerDetectT {
public:
    /// Long enough to swallow contact bounce and the ragged edge of a
    /// thresholded transient, short enough that a 1000 BPM 64th-note stream
    /// still passes.
    static constexpr double kDefaultRefractoryMs = 1.0;

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 1.0;
        update_refractory_();
        reset();
    }

    void set_refractory_ms(double ms) {
        refractory_ms_ = std::max(0.0, ms);
        update_refractory_();
    }

    /// Threshold for `process_signal()`. Ignored by the boolean `process()`.
    void set_threshold(SampleType t) { threshold_ = t; }

    void reset() {
        prev_ = false;
        countdown_ = 0;
    }

    /// @return true on the sample an accepted rising edge occurs.
    bool process(bool gate) {
        if (countdown_ > 0) --countdown_;
        const bool rising = gate && !prev_;
        prev_ = gate;
        if (!rising || countdown_ > 0) return false;
        countdown_ = refractory_samples_;
        return true;
    }

    /// Threshold a continuous signal and detect its rising edge. No hysteresis
    /// here — the refractory period is the chatter guard, and a
    /// `ComparatorT` upstream adds hysteresis when the source needs it.
    bool process_signal(SampleType x) { return process(x > threshold_); }

    bool armed() const { return countdown_ == 0; }

private:
    void update_refractory_() {
        refractory_samples_ =
            static_cast<long long>(std::lround(refractory_ms_ * 0.001 * sample_rate_));
    }

    double sample_rate_ = 48000.0;
    double refractory_ms_ = kDefaultRefractoryMs;
    long long refractory_samples_ = 48;
    long long countdown_ = 0;
    SampleType threshold_ = SampleType{0.5};
    bool prev_ = false;
};

using TriggerDetect = TriggerDetectT<float>;
using TriggerDetect64 = TriggerDetectT<double>;

/// Trigger in, fixed-length gate out.
///
/// RT contract: `prepare()` and setters are control-side; `process()` and
/// `reset()` allocate nothing.
///
/// USE: choke windows; trance-gate shapes driving a `VcaT`; giving an `ArT` or
/// `AdT` a gate whose length is a musical value rather than however long a pad
/// was held. The retrigger policy is the interesting control — `restart` is a
/// machine gun, `ignore` protects the first hit, `extend` accumulates a roll
/// into one long gate.
template <typename SampleType = float>
class GateGenT {
public:
    enum class Retrigger : std::uint8_t {
        /// Reset the countdown to full length. The default: a new event means
        /// a new gate.
        restart,
        /// Drop triggers while the gate is high.
        ignore,
        /// Add another full length to whatever remains.
        extend,
    };

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 1.0;
        update_length_();
        reset();
    }

    void set_length_ms(double ms) {
        length_ms_ = std::max(0.0, ms);
        update_length_();
    }

    /// Direct sample length, for synced use via `units::division_to_samples()`.
    ///
    /// Call it after `prepare()`: it stores the equivalent time using the
    /// prepared sample rate, and a later `prepare()` re-derives the sample
    /// count from that time.
    void set_length_samples(double samples) {
        length_samples_ = static_cast<long long>(std::lround(std::max(0.0, samples)));
        length_ms_ = static_cast<double>(length_samples_) * 1000.0 / sample_rate_;
    }

    void set_retrigger(Retrigger policy) { retrigger_ = policy; }

    void reset() { remaining_ = 0; }

    /// @return the gate state for this sample.
    bool process(bool trigger) {
        if (trigger && length_samples_ > 0) {
            switch (retrigger_) {
                case Retrigger::ignore:
                    if (remaining_ <= 0) remaining_ = length_samples_;
                    break;
                case Retrigger::extend:
                    remaining_ += length_samples_;
                    break;
                case Retrigger::restart:
                default:
                    remaining_ = length_samples_;
                    break;
            }
        }
        if (remaining_ <= 0) return false;
        --remaining_;
        return true;
    }

    bool gate() const { return remaining_ > 0; }

private:
    void update_length_() {
        length_samples_ = static_cast<long long>(std::lround(length_ms_ * 0.001 * sample_rate_));
    }

    double sample_rate_ = 48000.0;
    double length_ms_ = 50.0;
    long long length_samples_ = 2400;
    long long remaining_ = 0;
    Retrigger retrigger_ = Retrigger::restart;
};

using GateGen = GateGenT<float>;
using GateGen64 = GateGenT<double>;

/// Pass every Nth trigger.
///
/// The first trigger after a reset always passes, so a divider that has just
/// been phase-reset is in step with its master rather than N-1 events behind.
///
/// RT contract: two ints of state; everything allocates nothing.
///
/// USE: half- and quarter-time modulation from one master clock; polymeter by
/// running a divide-by-3 and a divide-by-4 off the same trigger.
class ClockDividerT {
public:
    static constexpr int kMaxDivision = 16;

    void set_division(int n) { division_ = std::clamp(n, 1, kMaxDivision); }
    int division() const { return division_; }

    /// Realign so the next trigger passes.
    void reset_phase() { counter_ = 0; }
    void reset() { reset_phase(); }

    bool process(bool trigger) {
        if (!trigger) return false;
        const bool pass = (counter_ == 0);
        if (++counter_ >= division_) counter_ = 0;
        return pass;
    }

private:
    int division_ = 1;
    int counter_ = 0;
};

using ClockDivider = ClockDividerT;
/// Precision-independent event state; the 64 spelling keeps generic toolkit
/// code on the same Foo/Foo64 convention without introducing a dummy scalar.
using ClockDivider64 = ClockDividerT;

/// Emit N evenly spaced triggers per measured input period.
///
/// The period is measured between consecutive input triggers, so the first
/// input trigger only starts the measurement — multiplied ticks begin with the
/// second. This is unavoidable for any multiplier that is not told the tempo:
/// you cannot subdivide an interval you have not seen yet.
///
/// Catch-up is bounded two ways: at most one tick per sample, and at most
/// `multiplier` ticks per period. A collapsing input period therefore cannot
/// make this emit an unbounded number of events in one block.
///
/// RT contract: scalar counters only; `process()` and `reset()` allocate
/// nothing.
///
/// USE: ratchets and rolls from a slow clock; hi-hats derived from a kick
/// trigger; running a modulation source at 4x the pulse the user is tapping.
class ClockMultT {
public:
    static constexpr int kMaxMultiplier = 16;

    void set_multiplier(int n) { multiplier_ = std::clamp(n, 1, kMaxMultiplier); }
    int multiplier() const { return multiplier_; }

    void reset() {
        period_ = 0;
        since_trigger_ = 0;
        ticks_emitted_ = 0;
        have_period_ = false;
        have_first_ = false;
    }

    /// @return true on samples where a multiplied tick fires.
    bool process(bool trigger) {
        // Counted before the trigger test, so the measured period is the true
        // sample distance between two triggers rather than one short.
        ++since_trigger_;
        if (trigger) {
            if (have_first_) {
                period_ = since_trigger_;
                have_period_ = true;
            }
            since_trigger_ = 0;
            have_first_ = true;
            ticks_emitted_ = 1;
            return true;
        }
        if (!have_period_ || multiplier_ <= 1) return false;
        if (ticks_emitted_ >= multiplier_) return false;
        const long long due = tick_offset_(ticks_emitted_);
        if (since_trigger_ < due) return false;
        ++ticks_emitted_;
        return true;
    }

    /// Measured input period in samples; 0 until two triggers have arrived.
    long long period_samples() const { return have_period_ ? period_ : 0; }

private:
    long long tick_offset_(int tick_index) const {
        return static_cast<long long>(
            std::llround(static_cast<double>(tick_index) * static_cast<double>(period_)
                         / static_cast<double>(multiplier_)));
    }

    long long period_ = 0;
    long long since_trigger_ = 0;
    int multiplier_ = 1;
    int ticks_emitted_ = 0;
    bool have_period_ = false;
    bool have_first_ = false;
};

using ClockMult = ClockMultT;
/// Precision-independent event state; identical to `ClockMult`.
using ClockMult64 = ClockMultT;

/// One trigger in, a burst of N triggers out, with a spacing curve and a level
/// ramp.
///
/// Hit `i` of `count` fires at
/// `span * stage_curve(i / (count - 1), spacing_curve)`, where
/// `span = (count - 1) * spacing_ms`. The curve is the shared stage-shaping
/// law, so `spacing_curve = 0` is even spacing, `+1` accelerates (gaps shrink —
/// a bouncing ball landing), and `-1` decelerates (gaps grow — a drag).
///
/// RT contract: fixed scalar state; `process()` and `reset()` allocate nothing.
///
/// USE: ratchet fills; strum-like event fans; bouncing-ball gestures. Feed the
/// output to `LpgT::strike()` with the per-hit level and you have a playable
/// percussion roll from two primitives.
template <typename SampleType = float>
class BurstGenT {
public:
    static constexpr int kMaxCount = 16;

    struct Hit {
        bool fired = false;
        /// Level for this hit, interpolated between the start and end levels.
        SampleType level = SampleType{0};
        /// 0-based index within the burst; meaningful only when `fired`.
        int index = 0;
    };

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 1.0;
        reset();
    }

    /// Changing the count while the burst is idle keeps it idle: `next_index_`
    /// marks "finished" by sitting at the count, so growing the count without
    /// this guard would resume a finished burst and fire hits no trigger asked
    /// for.
    void set_count(int n) {
        const bool idle = next_index_ >= count_;
        count_ = std::clamp(n, 1, kMaxCount);
        if (idle) next_index_ = count_;
    }
    void set_spacing_ms(double ms) { spacing_ms_ = std::max(0.0, ms); }
    void set_spacing_curve(float c) { spacing_curve_ = std::clamp(c, -1.0f, 1.0f); }

    void set_levels(SampleType first, SampleType last) {
        level_first_ = first;
        level_last_ = last;
    }

    void reset() {
        next_index_ = count_;
        elapsed_ = 0;
    }

    /// At most one hit fires per sample. A spacing curve steep enough to put two
    /// hits on the same sample therefore spreads them one sample apart rather
    /// than dropping one — a bounded, audible-as-nothing correction.
    Hit process(bool trigger) {
        Hit hit{};
        if (trigger) {
            next_index_ = 0;
            elapsed_ = 0;
        }
        if (next_index_ >= count_) return hit;
        if (elapsed_ >= hit_offset_(next_index_)) {
            hit.fired = true;
            hit.index = next_index_;
            hit.level = level_for_(next_index_);
            ++next_index_;
        }
        ++elapsed_;
        return hit;
    }

    /// Sample offset of hit `index` from the burst's trigger. Exposed so tests
    /// and sequencer previews can check against the same closed form the
    /// generator uses.
    long long hit_offset_samples(int index) const { return hit_offset_(index); }

    bool active() const { return next_index_ < count_; }

private:
    long long hit_offset_(int index) const {
        if (count_ <= 1 || index <= 0) return 0;
        const double span = static_cast<double>(count_ - 1) * spacing_ms_ * 0.001 * sample_rate_;
        const float p = static_cast<float>(index) / static_cast<float>(count_ - 1);
        return static_cast<long long>(
            std::llround(span * static_cast<double>(stage_curve(p, spacing_curve_))));
    }

    SampleType level_for_(int index) const {
        if (count_ <= 1) return level_first_;
        const SampleType t = static_cast<SampleType>(index) / static_cast<SampleType>(count_ - 1);
        return level_first_ + (level_last_ - level_first_) * t;
    }

    double sample_rate_ = 48000.0;
    double spacing_ms_ = 60.0;
    long long elapsed_ = 0;
    SampleType level_first_ = SampleType{1};
    SampleType level_last_ = SampleType{1};
    float spacing_curve_ = 0.0f;
    int count_ = 4;
    int next_index_ = 4;
};

using BurstGen = BurstGenT<float>;
using BurstGen64 = BurstGenT<double>;

/// Delay a trigger by a fixed time, through a small fixed-capacity queue.
///
/// Capacity is `kCapacity` in-flight triggers. A trigger arriving with the
/// queue full is dropped and counted rather than silently overwriting one —
/// dropping is the honest behavior for a bounded queue, and the counter is
/// there so a caller can notice.
///
/// RT contract: a fixed-size array of counters. `process()` costs `kCapacity`
/// integer operations per sample and allocates nothing.
///
/// USE: flams — run the dry trigger and a 15 ms delayed copy into two voices.
/// Echo-triggering a second layer. Nudging one element of a drum pattern off
/// the grid without touching the sequencer.
class TrigDelayT {
public:
    /// Eight in-flight triggers covers flams, doubles, and a full
    /// `BurstGenT` burst passing through; more would be a delay line, not a
    /// trigger delay.
    static constexpr int kCapacity = 8;

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 1.0;
        update_delay_();
        reset();
    }

    void set_delay_ms(double ms) {
        delay_ms_ = std::max(0.0, ms);
        update_delay_();
    }

    /// Direct sample delay, for synced use. Call it after `prepare()` — see
    /// `GateGenT::set_length_samples()` for why.
    void set_delay_samples(double samples) {
        delay_samples_ = static_cast<long long>(std::lround(std::max(0.0, samples)));
        delay_ms_ = static_cast<double>(delay_samples_) * 1000.0 / sample_rate_;
    }

    void reset() {
        pending_.fill(0);
        dropped_ = 0;
    }

    /// @return true on samples where at least one delayed trigger fires.
    bool process(bool trigger) {
        bool fired = false;
        // 0 is the empty marker, so a zero-initialized queue is an empty one.
        for (auto& slot : pending_) {
            if (slot <= 0) continue;
            if (--slot == 0) fired = true;
        }
        if (trigger) {
            if (delay_samples_ <= 0) {
                fired = true;
            } else if (!enqueue_(delay_samples_)) {
                ++dropped_;
            }
        }
        return fired;
    }

    /// Triggers discarded because the queue was full since the last `reset()`.
    long long dropped() const { return dropped_; }

private:
    bool enqueue_(long long samples) {
        for (auto& slot : pending_) {
            if (slot <= 0) {
                slot = samples;
                return true;
            }
        }
        return false;
    }

    void update_delay_() {
        delay_samples_ = static_cast<long long>(std::lround(delay_ms_ * 0.001 * sample_rate_));
    }

    std::array<long long, kCapacity> pending_{};
    double sample_rate_ = 48000.0;
    double delay_ms_ = 0.0;
    long long delay_samples_ = 0;
    long long dropped_ = 0;
};

using TrigDelay = TrigDelayT;
/// Precision-independent event state; identical to `TrigDelay`.
using TrigDelay64 = TrigDelayT;

} // namespace pulp::signal
