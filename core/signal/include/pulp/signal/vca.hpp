#pragma once

/// @file vca.hpp
/// Control-driven amplifier with a linear or exponential response and built-in
/// control lag.
///
/// RT contract: `prepare()` and the `set_*()` setters are control-side calls;
/// `process()`, `gain_for()`, `reset()`, and the accessors allocate nothing and
/// are audio-thread safe. The type owns no memory.
///
/// USE — the VCA idioms worth knowing, because "multiply by a control" is one
/// line and every one of these is still a distinct musical result:
///
/// - **Tremolo** — a 4-7 Hz sine at 20-40% depth (the depth belongs to an
///   `AttenuverterT`, not here). The exponential response reads more "amp-like"
///   than linear, because loudness perception is closer to logarithmic than the
///   control is.
/// - **Auto-pan** — `LfoT::next_quadrature()` into two of these. Constant power
///   by construction: `sin^2 + cos^2 = 1`, so the pair cannot dip in the
///   middle the way two offset triangles do.
/// - **Sidechain pump with no compressor** — `TriggerDetectT` on the kick, into
///   an `ArT` (attack = how fast it ducks, release = how fast it recovers),
///   inverted through an `AttenuverterT(gain -0.8, offset 1)`, into this. The
///   exact pump shape you asked for, deterministic, and immune to the kick's
///   level changing.
/// - **Chopper** — a square LFO or a `GateGenT` through the lag. The lag is the
///   click-versus-crunch knob; it is the whole difference between a gate and a
///   glitch.
/// - **AM and ring territory** — an audio-rate LFO into a linear-response VCA
///   is clean amplitude modulation. Sweeping the LFO rate from 5 Hz upward
///   plays the tremolo-to-sideband transition as a gesture rather than as a
///   mode switch.

#include <pulp/signal/smoothed_value.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// Voltage-controlled amplifier.
///
/// The exponential response is `g = (e^(k*c) - 1) / (e^k - 1)` with `k = 4.6`,
/// which spans about 40 dB — enough that the bottom of the control range is
/// perceptually silent without the numerical hazards of an actual log curve.
/// Both responses are exactly 0 at control 0 and exactly 1 at control 1; unity
/// at full is a series law, so a VCA left wide open is bit-transparent rather
/// than approximately transparent.
template <typename SampleType = float>
class VcaT {
public:
    enum class Response : std::uint8_t { linear, exponential };

    /// ~40 dB of span for the exponential response.
    static constexpr float kExpCurve = 4.6f;

    /// Default control lag. Long enough to kill the click from a hard-switched
    /// gate, short enough that a percussive envelope keeps its transient.
    static constexpr double kDefaultLagMs = 1.0;

    void prepare(SampleType sample_rate) {
        sample_rate_ = sample_rate > SampleType{0} ? sample_rate : SampleType{1};
        update_lag_();
        reset();
    }

    void set_response(Response r) { response_ = r; }
    Response response() const { return response_; }

    void set_lag_ms(double ms) {
        lag_ms_ = std::max(0.0, ms);
        update_lag_();
    }

    void reset(SampleType control = SampleType{0}) {
        lag_.set_immediate(control);
        last_control_ = control;
    }

    /// Static response curve, without the lag. Exposed so a UI can draw the
    /// same curve the audio path uses.
    SampleType gain_for(SampleType control) const {
        const SampleType c = std::clamp(control, SampleType{0}, SampleType{1});
        if (response_ == Response::linear) return c;
        // Written as a division rather than a precomputed reciprocal so that
        // c == 1 produces exactly 1: the numerator is then literally the same
        // expression as the denominator.
        return (std::exp(static_cast<SampleType>(kExpCurve) * c) - SampleType{1})
               / (std::exp(static_cast<SampleType>(kExpCurve)) - SampleType{1});
    }

    /// Advance the lag by one sample and return the gain that would be applied.
    SampleType next_gain(SampleType control) {
        const SampleType target = std::clamp(control, SampleType{0}, SampleType{1});
        // Re-targeting every sample would restart the ramp every sample and the
        // value would approach the target without ever arriving. Only a genuine
        // change starts a new ramp, so a held control lands exactly.
        if (target != last_control_) {
            lag_.set_target(target);
            last_control_ = target;
        }
        return gain_for(lag_.next());
    }

    SampleType process(SampleType input, SampleType control) {
        return input * next_gain(control);
    }

    /// The lagged control value, for metering or for driving a second stage
    /// from the same conditioned control.
    SampleType control() const { return lag_.current(); }

private:
    void update_lag_() {
        lag_.set_ramp_time(static_cast<SampleType>(lag_ms_ * 0.001), sample_rate_);
    }

    SmoothedValue<SampleType> lag_{};
    SampleType sample_rate_ = SampleType{48000};
    SampleType last_control_ = SampleType{0};
    double lag_ms_ = kDefaultLagMs;
    Response response_ = Response::linear;
};

using Vca = VcaT<float>;
using Vca64 = VcaT<double>;

} // namespace pulp::signal
