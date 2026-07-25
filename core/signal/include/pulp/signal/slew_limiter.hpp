#pragma once

/// @file slew_limiter.hpp
/// Rate limiting for control signals: the portamento/glide/inertia primitive,
/// and the sample-and-hold that pairs with it.
///
/// `SmoothedValue` and `LogRampedValueT` already cover "ramp to a target over a
/// fixed time" for a value the caller sets. `SlewLimiterT` covers the other
/// shape: a *continuously arriving* control signal whose rate of change must be
/// bounded. That is what a portamento is, what a rotor's spin-up is, and what a
/// pedal's glide is — the target moves every sample, and the limiter is what
/// keeps the output from following it instantly.
///
/// Two modes, because the two families of caller want genuinely different
/// behaviour and picking one silently would be wrong for the other:
///
///   - **`Mode::linear` — constant TIME.** The output covers the whole
///     remaining distance in a fixed time regardless of how far it has to go,
///     by moving at `distance / time` per sample, recomputed when the target
///     changes. This is the TB-303-lineage portamento the stage-sequencer spec
///     cites: a slide takes the same 30 ms whether it is one semitone or an
///     octave. It reaches the target EXACTLY, in finite time.
///   - **`Mode::exponential` — constant TIME CONSTANT.** A one-pole with the
///     τ convention of `units::ms_to_onepole_coef`: the output covers 63.2 % of
///     the remaining distance per τ, so a big jump takes longer in absolute
///     terms. This is what mechanical inertia does (a Leslie rotor, a vactrol)
///     and what a de-zippering smoother wants. It approaches the target
///     asymptotically and is snapped to it once inside `kSettleEpsilon`.
///
/// **Rise and fall are independent** in both modes. Asymmetry is not a
/// nicety — it is the entire character of a rotor that spins up faster than it
/// coasts down, and of an envelope follower with a fast attack and a slow
/// release.
///
/// RT contract: `prepare()` recomputes coefficients and allocates nothing.
/// `set_*`, `process()`, and `reset()` allocate nothing, take no locks, and
/// perform no I/O. All state is POD; zero-init is a valid state sitting at 0.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal {

/// How a `SlewLimiterT` closes the distance to its input.
enum class SlewMode {
    linear,       ///< Constant time: fixed duration, exact arrival.
    exponential,  ///< Constant time constant: one-pole, asymptotic arrival.
};

/// Bounds the rate of change of a control signal, with independent rise and
/// fall times.
template <typename SampleType = float>
class SlewLimiterT {
public:
    using Mode = SlewMode;

    /// Distance below which `Mode::exponential` snaps to the target rather than
    /// approaching it forever. Chosen far below any control resolution anyone
    /// hears — a cutoff, a pitch in cents, a gain in dB — so snapping is never
    /// audible, while keeping the state from decaying into denormals.
    /// [design parameter] default 1e-9, range 1e-12 .. 1e-6.
    static constexpr double kSettleEpsilon = 1e-9;

    SlewLimiterT() { update(); }

    /// Recomputes coefficients for a sample rate. Does not move the current
    /// value: a sample-rate change should not jump a glide.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        update();
    }

    void set_mode(Mode mode) {
        mode_ = mode;
        update();
    }

    Mode mode() const { return mode_; }

    /// Sets rise and fall to the same time in ms.
    void set_time_ms(double ms) {
        rise_ms_ = fall_ms_ = std::max(ms, 0.0);
        update();
    }

    /// Time in ms for an increasing input. In `linear` mode this is the total
    /// slide duration; in `exponential` mode it is the time constant τ.
    void set_rise_ms(double ms) {
        rise_ms_ = std::max(ms, 0.0);
        update();
    }

    /// Time in ms for a decreasing input. Same reading as `set_rise_ms`.
    void set_fall_ms(double ms) {
        fall_ms_ = std::max(ms, 0.0);
        update();
    }

    double rise_ms() const { return rise_ms_; }
    double fall_ms() const { return fall_ms_; }

    /// Jumps to `value` with no glide — the "start here" call, for a voice
    /// beginning on a pitch rather than sliding up to it from silence.
    void set_immediate(SampleType value) {
        value_ = static_cast<double>(value);
        target_ = value_;
        linear_step_ = 0.0;
    }

    void reset() { set_immediate(SampleType{0}); }

    /// Current value without advancing.
    SampleType value() const { return static_cast<SampleType>(value_); }

    /// True once the output has arrived at its target. `linear` mode arrives
    /// exactly; `exponential` mode arrives when inside `kSettleEpsilon`.
    bool settled() const { return value_ == target_; }

    /// Advances one sample toward `input` and returns the new value.
    SampleType process(SampleType input) {
        const double in = static_cast<double>(input);
        const bool rising = in > value_;

        if (mode_ == Mode::linear) {
            // Recompute the per-sample step whenever the target moves, so the
            // duration is a property of THIS slide rather than of whatever
            // distance happened to be in flight when the target last changed.
            if (in != target_) {
                target_ = in;
                const double samples = rising ? rise_samples_ : fall_samples_;
                linear_step_ = samples > 0.0 ? std::abs(in - value_) / samples : 0.0;
            }
            if (linear_step_ <= 0.0) {
                value_ = in;
            } else if (rising) {
                value_ = std::min(value_ + linear_step_, in);
            } else {
                value_ = std::max(value_ - linear_step_, in);
            }
        } else {
            target_ = in;
            const double coef = rising ? rise_coef_ : fall_coef_;
            value_ += coef * (in - value_);
            if (std::abs(in - value_) < kSettleEpsilon) value_ = in;
        }

        value_ = snap_to_zero(value_);
        return static_cast<SampleType>(value_);
    }

private:
    void update() {
        rise_samples_ = units::ms_to_samples(rise_ms_, sample_rate_);
        fall_samples_ = units::ms_to_samples(fall_ms_, sample_rate_);
        rise_coef_ = units::ms_to_onepole_coef(rise_ms_, sample_rate_);
        fall_coef_ = units::ms_to_onepole_coef(fall_ms_, sample_rate_);
    }

    double sample_rate_ = 44100.0;
    double rise_ms_ = 0.0;
    double fall_ms_ = 0.0;
    Mode mode_ = Mode::linear;

    double rise_samples_ = 0.0;
    double fall_samples_ = 0.0;
    double rise_coef_ = 1.0;
    double fall_coef_ = 1.0;

    double value_ = 0.0;
    double target_ = 0.0;
    double linear_step_ = 0.0;
};

using SlewLimiter = SlewLimiterT<float>;
using SlewLimiter64 = SlewLimiterT<double>;

/// Latches its input on a rising trigger edge and holds it until the next one.
///
/// Deliberately edge-triggered on a signal rather than clocked by a call
/// pattern: in a modular graph the thing that says "now" is another block's
/// gate output, and making that explicit means a sample-and-hold can be driven
/// by a clock divider, an LFO's square, or a comparator without any of them
/// knowing about each other.
///
/// RT contract: as `SlewLimiterT`. No allocation, no locks.
template <typename SampleType = float>
class SampleHoldT {
public:
    /// Trigger threshold. A gate signal is 0/1 or 0/±5 V depending on the
    /// domain, so the threshold sits low enough to catch both while staying
    /// clear of noise around zero.
    /// [design parameter] default 0.5, range 0.01 .. 2.5.
    static constexpr double kDefaultThreshold = 0.5;

    void set_threshold(SampleType threshold) {
        threshold_ = static_cast<double>(threshold);
    }

    /// Clears the held value and the edge memory, so the next rising edge
    /// latches regardless of where the trigger was left.
    void reset() {
        held_ = SampleType{0};
        armed_ = true;
    }

    /// Current held value without sampling.
    SampleType value() const { return held_; }

    /// Latches `input` if `trigger` has just crossed the threshold upward,
    /// then returns the held value.
    SampleType process(SampleType input, SampleType trigger) {
        const bool high = static_cast<double>(trigger) >= threshold_;
        if (high && armed_) held_ = input;
        armed_ = !high;
        return held_;
    }

private:
    double threshold_ = kDefaultThreshold;
    SampleType held_ = SampleType{0};
    bool armed_ = true;
};

using SampleHold = SampleHoldT<float>;
using SampleHold64 = SampleHoldT<double>;

}  // namespace pulp::signal
