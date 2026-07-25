#pragma once

/// @file mod_tools.hpp
/// The small modular utilities every patch reaches for between a modulation
/// source and its destination: slew, sample-and-hold, attenuversion,
/// rectification, comparison, quantization, and response shaping. Plus the
/// stage-curve law the envelope and burst families share.
///
/// RT contract: every type here holds fixed scalar state and owns no memory.
/// `prepare()` and the `set_*()` setters recompute coefficients and are
/// control-side calls; `process()`, `next()`, `reset()`, and the accessors
/// allocate nothing and are audio-thread safe. All free functions are pure.
///
/// USE: these are the wires, not the sources. A modulation routing that reads
/// `lfo -> slew -> attenuverter -> destination` is one that can be reasoned
/// about and tested; the same behavior open-coded into a `process()` loop is
/// three magic constants nobody will dare change later.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// Curve span for the shared stage-shaping law. 8 puts the extremes at roughly
/// a 20:1 slope ratio between the start and end of a stage — audibly a strong
/// curve without becoming a step.
inline constexpr float kCurveSpan = 8.0f;

/// The shared stage-shaping law:
///
///     shaped(p) = (1 - e^(-k*p)) / (1 - e^(-k)),   k = kCurveSpan * curve
///
/// Maps `p` in [0, 1] onto [0, 1], monotonically, with `shaped(0) = 0` and
/// `shaped(1) = 1` for every `curve`. `curve = 0` is the linear limit (the
/// formula is 0/0 there, so it is special-cased to the analytic limit `p`).
///
/// This is the raw law. Callers shaping an envelope stage should use
/// `curve_rise()` / `curve_fall()` below rather than this directly — see the
/// note there about what "+1 = exponential" means for a stage that falls.
inline float stage_curve(float p, float curve) {
    const float t = std::clamp(p, 0.0f, 1.0f);
    const float k = kCurveSpan * std::clamp(curve, -1.0f, 1.0f);
    if (std::abs(k) < 1.0e-4f) return t; // linear limit
    return (1.0f - std::exp(-k * t)) / (1.0f - std::exp(-k));
}

/// Shape a *rising* stage: 0 -> 1 over `p`, with `curve` in [-1, +1].
///
/// `+1` is an exponential attack — slow to leave zero, then accelerating into
/// the peak. `-1` is a logarithmic attack: it jumps and then eases in.
///
/// The sign flip against `stage_curve()` is deliberate and is the whole reason
/// these two helpers exist. The house convention labels `+1` "exponential" and
/// `-1` "logarithmic", which is a *decay*-oriented convention (an exponential
/// decay drops fast and then tails; an exponential attack starts slow and then
/// rushes). One raw law cannot satisfy both readings with the same sign, so the
/// law is applied to a rising stage with the sign inverted and to a falling
/// stage directly. The result is that `curve = +1` means "exponential" in the
/// musical sense on every stage of every envelope in the library, which is the
/// property a user setting one knob per stage actually needs.
inline float curve_rise(float p, float curve) { return stage_curve(p, -curve); }

/// Shape a *falling* stage: 1 -> 0 over `p`. `+1` is an exponential decay —
/// fast initial drop into a long tail. `-1` holds near the top and then falls
/// off a cliff. See `curve_rise()` for why the signs differ.
inline float curve_fall(float p, float curve) { return 1.0f - stage_curve(p, curve); }

/// Cubic smoothstep, 3t^2 - 2t^3. Zero slope at both ends, unlike the
/// exponential law, which is the right shape for a crossfade or a knob feel
/// that must not click at either extreme.
inline float smoothstep(float p) {
    const float t = std::clamp(p, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Bipolar [-1, 1] to unipolar [0, 1].
inline float bi_to_uni(float x) { return x * 0.5f + 0.5f; }

/// Unipolar [0, 1] to bipolar [-1, 1].
inline float uni_to_bi(float x) { return x * 2.0f - 1.0f; }

/// Rate limiter with independent rise and fall speeds.
///
/// Two modes, and they are not interchangeable:
///
/// - `linear` travels at a fixed slope. `set_rise_ms(10)` means a full 0 -> 1
///   step takes exactly 10 ms, no matter how far it actually has to go. This is
///   the mode for portamento and for anything where the *time* is the musical
///   quantity.
/// - `exponential` is a one-pole toward the target, so `ms` is a time constant
///   (63.2% of the remaining distance) and the approach is asymptotic. This is
///   the mode for de-clicking, where what matters is that the corner is gone,
///   not how long it took.
///
/// RT contract: `prepare()` and setters recompute coefficients. `process()`,
/// `reset()`, and accessors allocate nothing.
///
/// USE: portamento on a pitch control; gliding a stepped source so
/// sample-and-hold burbles instead of clicking; motor-lag and tape-transport
/// emulation, where the rise and fall genuinely differ; taming a square LFO
/// into a trance gate whose edges are a knob rather than a hazard.
template <typename SampleType = float>
class SlewLimiterT {
public:
    enum class Mode : std::uint8_t { linear, exponential };

    void prepare(SampleType sample_rate) {
        sample_rate_ = sample_rate > SampleType{0} ? sample_rate : SampleType{1};
        update_coefficients();
    }

    void set_mode(Mode m) { mode_ = m; }
    void set_rise_ms(SampleType ms) {
        rise_ms_ = std::max(SampleType{0}, ms);
        update_coefficients();
    }
    void set_fall_ms(SampleType ms) {
        fall_ms_ = std::max(SampleType{0}, ms);
        update_coefficients();
    }
    void set_times_ms(SampleType rise, SampleType fall) {
        set_rise_ms(rise);
        set_fall_ms(fall);
    }

    void reset(SampleType value = SampleType{0}) { value_ = value; }

    SampleType process(SampleType target) {
        const SampleType delta = target - value_;
        if (delta == SampleType{0}) return value_;
        if (mode_ == Mode::linear) {
            const SampleType step = delta > SampleType{0} ? rise_step_ : fall_step_;
            value_ += std::clamp(delta, -step, step);
        } else {
            const SampleType coeff = delta > SampleType{0} ? rise_coeff_ : fall_coeff_;
            value_ += coeff * delta;
        }
        return value_;
    }

    SampleType current() const { return value_; }
    Mode mode() const { return mode_; }

private:
    void update_coefficients() {
        rise_step_ = step_for(rise_ms_);
        fall_step_ = step_for(fall_ms_);
        rise_coeff_ = coeff_for(rise_ms_);
        fall_coeff_ = coeff_for(fall_ms_);
    }

    SampleType step_for(SampleType ms) const {
        const SampleType samples = ms * SampleType{0.001} * sample_rate_;
        // A zero or sub-sample time is an instant jump, not a division by zero.
        return samples < SampleType{1} ? SampleType{1e30} : SampleType{1} / samples;
    }

    SampleType coeff_for(SampleType ms) const {
        const SampleType samples = ms * SampleType{0.001} * sample_rate_;
        if (samples < SampleType{1e-6}) return SampleType{1};
        return SampleType{1} - std::exp(SampleType{-1} / samples);
    }

    SampleType sample_rate_ = SampleType{48000};
    SampleType rise_ms_ = SampleType{10};
    SampleType fall_ms_ = SampleType{10};
    SampleType rise_step_ = SampleType{1} / SampleType{480};
    SampleType fall_step_ = SampleType{1} / SampleType{480};
    SampleType rise_coeff_ = SampleType{1};
    SampleType fall_coeff_ = SampleType{1};
    SampleType value_ = SampleType{0};
    Mode mode_ = Mode::linear;
};

using SlewLimiter = SlewLimiterT<float>;
using SlewLimiter64 = SlewLimiterT<double>;

/// Latch a value on a clock's rising edge, with optional glide to the new
/// value instead of a step.
///
/// RT contract: `prepare()` and setters are control-side; `process()` and
/// `reset()` allocate nothing.
///
/// USE: the classic random-step patch (a clock plus a noise source is a
/// sequencer nobody has to write); freezing an LFO or envelope value at
/// note-on so a per-note variation stays put for the note's duration; turning a
/// continuous controller into a stepped one. The glide is what separates "S&H
/// on a filter cutoff" from "S&H on a filter cutoff that does not click".
template <typename SampleType = float>
class SampleHoldT {
public:
    void prepare(SampleType sample_rate) { glide_.prepare(sample_rate); }

    /// 0 disables the glide entirely (the output steps).
    void set_glide_ms(SampleType ms) {
        glide_ms_ = std::max(SampleType{0}, ms);
        glide_.set_times_ms(glide_ms_, glide_ms_);
    }

    void set_glide_mode(typename SlewLimiterT<SampleType>::Mode m) { glide_.set_mode(m); }

    void reset(SampleType value = SampleType{0}) {
        held_ = value;
        prev_clock_ = false;
        glide_.reset(value);
    }

    /// @param input the value to latch when the clock rises.
    /// @param clock  a gate; the rising edge is the sample instant.
    SampleType process(SampleType input, bool clock) {
        if (clock && !prev_clock_) held_ = input;
        prev_clock_ = clock;
        if (glide_ms_ <= SampleType{0}) {
            glide_.reset(held_);
            return held_;
        }
        return glide_.process(held_);
    }

    /// The latched value before any glide — what the last clock actually saw.
    SampleType held() const { return held_; }

private:
    SlewLimiterT<SampleType> glide_{};
    SampleType held_ = SampleType{0};
    SampleType glide_ms_ = SampleType{0};
    bool prev_clock_ = false;
};

using SampleHold = SampleHoldT<float>;
using SampleHold64 = SampleHoldT<double>;

/// `y = x * gain + offset`, with `gain` allowed to go negative.
///
/// RT contract: stateless apart from its two settings; everything is
/// audio-thread safe.
///
/// USE: this is the depth control for every routing in a patch, and the
/// negative half of its range is the point — inverting an envelope turns a
/// swell into a duck, and inverting an LFO makes a second destination move
/// against the first. `offset` re-centers: a unipolar envelope becomes bipolar
/// with `gain 2, offset -1`, and a bipolar LFO becomes unipolar with
/// `gain 0.5, offset 0.5`.
template <typename SampleType = float>
class AttenuverterT {
public:
    /// Beyond unity in both directions so one routing can also amplify — a
    /// 0..1 envelope driving a +/-2 octave pitch sweep is a single slot.
    static constexpr SampleType kMaxGain = SampleType{2};

    void set_gain(SampleType g) { gain_ = std::clamp(g, -kMaxGain, kMaxGain); }
    void set_offset(SampleType o) { offset_ = o; }

    SampleType process(SampleType x) const { return x * gain_ + offset_; }
    SampleType gain() const { return gain_; }
    SampleType offset() const { return offset_; }

private:
    SampleType gain_ = SampleType{1};
    SampleType offset_ = SampleType{0};
};

using Attenuverter = AttenuverterT<float>;
using Attenuverter64 = AttenuverterT<double>;

/// Full- or half-wave rectification.
///
/// RT contract: stateless; audio-thread safe.
///
/// USE: the front end of an envelope follower (a follower fed a raw signal
/// tracks nothing). As a modulation shaper it is more interesting: full-wave
/// rectifying a sine gives `|sin|`, which is the classic bouncing-pluck LFO at
/// double the rate, and half-wave rectifying a triangle gives a ramp with a
/// flat rest between hits.
template <typename SampleType = float>
class RectifierT {
public:
    enum class Mode : std::uint8_t { full_wave, half_wave };

    void set_mode(Mode m) { mode_ = m; }

    SampleType process(SampleType x) const {
        if (mode_ == Mode::full_wave) return std::abs(x);
        return std::max(SampleType{0}, x);
    }

private:
    Mode mode_ = Mode::full_wave;
};

using Rectifier = RectifierT<float>;
using Rectifier64 = RectifierT<double>;

/// Threshold comparator with hysteresis — the bridge from the signal domain to
/// the event domain.
///
/// The gate goes high above `threshold + hysteresis` and low below
/// `threshold - hysteresis`. Without the dead band, a source that dithers
/// across the threshold — which is every real modulation signal, including a
/// slow sine near its own zero crossing — chatters the gate on and off at the
/// sample rate.
///
/// RT contract: one bool of state; `process()` and `reset()` allocate nothing.
///
/// USE: turning any modulation signal into a clock or a gate. An LFO through a
/// comparator is a clock whose duty cycle is the threshold. An envelope through
/// a comparator is "the note is loud enough to count".
template <typename SampleType = float>
class ComparatorT {
public:
    /// Default dead band, as a fraction of the threshold magnitude.
    static constexpr SampleType kDefaultHysteresisFraction = SampleType{0.05};

    /// Floor for the derived dead band. A threshold of 0 — the natural one for
    /// any bipolar signal — would otherwise derive a dead band of 0, which is
    /// exactly the chatter this class exists to prevent.
    static constexpr SampleType kMinAutoHysteresis = SampleType{0.001};

    /// Setting a threshold re-derives the default hysteresis unless an explicit
    /// one was set first.
    void set_threshold(SampleType t) {
        threshold_ = t;
        if (auto_hysteresis_)
            hysteresis_ = std::max(kMinAutoHysteresis,
                                   kDefaultHysteresisFraction * std::abs(t));
    }

    /// Absolute dead band, in the input's units. Pins the value against later
    /// `set_threshold()` calls.
    void set_hysteresis(SampleType h) {
        hysteresis_ = std::abs(h);
        auto_hysteresis_ = false;
    }

    void reset(bool gate = false) { gate_ = gate; }

    bool process(SampleType x) {
        if (!gate_ && x > threshold_ + hysteresis_) gate_ = true;
        else if (gate_ && x < threshold_ - hysteresis_) gate_ = false;
        return gate_;
    }

    bool gate() const { return gate_; }
    SampleType hysteresis() const { return hysteresis_; }

private:
    SampleType threshold_ = SampleType{0};
    SampleType hysteresis_ = kMinAutoHysteresis;
    bool auto_hysteresis_ = true;
    bool gate_ = false;
};

using Comparator = ComparatorT<float>;
using Comparator64 = ComparatorT<double>;

/// Snap a control value to `N` equally spaced levels across a range.
///
/// `N` levels means `N - 1` intervals: `set_steps(2)` over [0, 1] outputs only
/// 0 and 1. Inputs outside the range clamp to the end levels. Ties round away
/// from zero, matching `std::lround`.
///
/// Pitch quantization to a musical scale is deliberately *not* here — that
/// needs a scale, a root, and a note-vs-frequency decision, and it belongs with
/// the MIDI side rather than in a control-signal utility.
///
/// RT contract: stateless; audio-thread safe.
///
/// USE: stepped filter sweeps and stepped pitch modulation; making a smooth
/// random source into an arpeggio-like sequence; using coarse quantization as a
/// deliberate lo-fi effect on a modulation path rather than on the audio.
template <typename SampleType = float>
class QuantizerT {
public:
    void set_range(SampleType lo, SampleType hi) {
        lo_ = lo;
        hi_ = hi;
    }

    void set_steps(int n) { steps_ = std::clamp(n, 1, 1024); }

    SampleType process(SampleType x) const {
        if (steps_ <= 1) return lo_;
        const SampleType span = hi_ - lo_;
        if (span == SampleType{0}) return lo_;
        const SampleType t = std::clamp((x - lo_) / span, SampleType{0}, SampleType{1});
        const auto divisions = static_cast<SampleType>(steps_ - 1);
        const auto index = static_cast<SampleType>(std::lround(t * divisions));
        return lo_ + index * span / divisions;
    }

    int steps() const { return steps_; }

private:
    SampleType lo_ = SampleType{0};
    SampleType hi_ = SampleType{1};
    int steps_ = 8;
};

using Quantizer = QuantizerT<float>;
using Quantizer64 = QuantizerT<double>;

/// Response shaper for a unit control: the stage-curve law, or smoothstep.
///
/// RT contract: stateless apart from its settings; audio-thread safe.
///
/// USE: the last stage before a destination, where "how the knob feels" lives.
/// A velocity curve is this on the velocity input; a knob taper that has to
/// stay in unit space is this on the parameter. `stage_curve` mode with
/// `curve = +1` is the "you have to lean on it" feel; `-1` is "it moves the
/// moment you touch it"; `smoothstep` is the ease-in-out for anything that must
/// not jerk at either end.
template <typename SampleType = float>
class CurveT {
public:
    enum class Shape : std::uint8_t { stage_curve, smoothstep };

    void set_shape(Shape s) { shape_ = s; }

    /// -1 logarithmic .. 0 linear .. +1 exponential. Ignored in `smoothstep`
    /// mode, which has no free parameter.
    void set_curve(SampleType c) { curve_ = std::clamp(c, SampleType{-1}, SampleType{1}); }

    SampleType process(SampleType x) const {
        const auto p = static_cast<float>(x);
        if (shape_ == Shape::smoothstep) return static_cast<SampleType>(pulp::signal::smoothstep(p));
        return static_cast<SampleType>(curve_rise(p, static_cast<float>(curve_)));
    }

private:
    SampleType curve_ = SampleType{0};
    Shape shape_ = Shape::stage_curve;
};

using Curve = CurveT<float>;
using Curve64 = CurveT<double>;

} // namespace pulp::signal
