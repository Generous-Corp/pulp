#pragma once

/// @file vactrol.hpp
/// The vactrol control-conditioning law: an asymmetric one-pole whose rise and
/// fall are deliberately different.
///
/// A vactrol is an LED facing a photoresistor in a sealed package. The
/// resistance falls quickly when the LED lights and recovers slowly when it
/// goes dark, and that asymmetry is not a design choice anyone made — it is a
/// property of the material. Several very different-sounding circuits are
/// built on exactly that one behaviour:
///
///   - the Buchla lowpass gate, where it produces the unhurried decay that
///     makes struck sounds read as physical (`LowpassGateT`);
///   - the Univibe's four staggered phase stages, where an LED and four
///     photocells lag the LFO into the uneven, "not-quite-a-phaser" sweep the
///     circuit is known for;
///   - any optical compressor's gain element.
///
/// They share the conditioner and differ in what they do with its output, so
/// the conditioner lives here on its own and each circuit supplies its own
/// constants. Duplicating the law per circuit is how two modules end up
/// disagreeing about whether "rise" means the LED's or the photoresistor's.
///
/// Model family: Parker & D'Angelo, "A Digital Model of the Buchla Lowpass
/// Gate", DAFx-13. The asymmetric one-pole here is the tractable core that
/// paper identifies; the power-law control-to-gain mapping that `LowpassGateT`
/// applies on top is a separate stage and stays there.
///
/// This is deliberately NOT `SlewLimiterT` in exponential mode, even though
/// the arithmetic is the same shape. A slew limiter is a control-rate utility
/// whose contract is "bound the rate of change of this signal"; a vactrol is a
/// *physical model* whose contract is "reproduce this component's lag". They
/// are spelled differently so a spec citing one cannot be satisfied by the
/// other, and so the vactrol can grow a more detailed model later without
/// changing what glide means.
///
/// RT contract: `prepare()` recomputes two coefficients and allocates nothing.
/// `set_*`, `process()`, and `reset()` allocate nothing, take no locks, and
/// perform no I/O. All state is POD; zero-init is a valid closed state.

#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal {

/// The asymmetric one-pole that lags a control signal the way a photoresistor
/// lags its LED. Input and output are in `[0, 1]`.
template <typename SampleType = float>
class VactrolConditionerT {
public:
    /// Default rise time in ms — the LED reaching brightness. Fast.
    /// [design parameter] default 2.0 ms, range 0.1 .. 50 ms.
    static constexpr double kMinRiseMs = 0.1;
    static constexpr double kMaxRiseMs = 50.0;
    static constexpr double kDefaultRiseMs = 2.0;

    /// Default fall time in ms — the photoresistor recovering. Slow, and the
    /// control that sets how long a struck note takes to disappear.
    /// [design parameter] default 200 ms, range 10 .. 2000 ms.
    static constexpr double kMinFallMs = 10.0;
    static constexpr double kMaxFallMs = 2000.0;
    static constexpr double kDefaultFallMs = 200.0;

    VactrolConditionerT() { update(); }

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        update();
    }

    void set_rise_ms(double ms) {
        rise_ms_ = std::clamp(ms, kMinRiseMs, kMaxRiseMs);
        update();
    }

    void set_fall_ms(double ms) {
        fall_ms_ = std::clamp(ms, kMinFallMs, kMaxFallMs);
        update();
    }

    double rise_ms() const { return rise_ms_; }
    double fall_ms() const { return fall_ms_; }

    void reset() { control_ = 0.0; }

    /// Current conditioned control in `[0, 1]` without advancing.
    double control() const { return control_; }

    /// Advances one sample toward `target` (clamped to `[0, 1]`) and returns
    /// the conditioned control.
    double process(double target) {
        // Do not admit a non-finite control into the recursive photoresistor
        // state: std::clamp intentionally passes NaN through because every
        // comparison is false. Resetting makes the following finite sample
        // recover deterministically instead of keeping NaN forever.
        if (!std::isfinite(target)) {
            reset();
            return 0.0;
        }
        const double clamped = std::clamp(target, 0.0, 1.0);
        const double coefficient = clamped > control_ ? rise_a_ : fall_a_;
        control_ = snap_to_zero(control_ + coefficient * (clamped - control_));
        return control_;
    }

private:
    void update() {
        // The `max` guards a zero-or-negative sample rate; the time floor above
        // already guards the ms side. Deliberately spelled out rather than
        // routed through `units::ms_to_onepole_coef` so that this stays
        // bit-identical to the coefficient `LowpassGateT` has always used.
        rise_a_ = 1.0 - std::exp(-1.0 / std::max(0.001 * rise_ms_ * sample_rate_, 1e-9));
        fall_a_ = 1.0 - std::exp(-1.0 / std::max(0.001 * fall_ms_ * sample_rate_, 1e-9));
    }

    double sample_rate_ = 44100.0;
    double rise_ms_ = kDefaultRiseMs;
    double fall_ms_ = kDefaultFallMs;
    double rise_a_ = 0.0;
    double fall_a_ = 0.0;
    double control_ = 0.0;
};

using VactrolConditioner = VactrolConditionerT<float>;
using VactrolConditioner64 = VactrolConditionerT<double>;

}  // namespace pulp::signal
