#pragma once

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/tpt_filter.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal {

/// A lowpass gate: an amplitude gate and a lowpass filter driven by one
/// control signal with deliberately asymmetric dynamics.
///
/// The behaviour being modelled is a vactrol — an LED facing a photoresistor
/// in a sealed package. The photoresistor's resistance falls quickly when the
/// LED lights and recovers slowly when it goes dark, and that asymmetry is not
/// a design choice anyone made; it is a property of the material. The musical
/// consequence is that a lowpass gate cannot produce a fast release. Every
/// note ends with the same unhurried decay, and because the filter closes as
/// the level falls, the note also gets darker as it dies. That single
/// coupling — quiet implies dark — is what makes struck and plucked sounds
/// read as physical rather than as an envelope applied to an oscillator.
///
/// `set_colour()` selects how much of the gating is amplitude and how much is
/// filtering: 0 is a pure VCA (level only, no timbral change), 1 is a pure
/// filter (the tail darkens but never fully stops), and intermediate values
/// run both.
///
/// Model family: Parker & D'Angelo, "A Digital Model of the Buchla Lowpass
/// Gate", DAFx-13. The vactrol response here is the one-pole asymmetric
/// smoother that paper identifies as the tractable core of the behaviour,
/// with a power-law mapping from control to gain.
///
/// RT contract: every member allocates nothing and takes no locks.
template <typename SampleType = float>
class LowpassGateT {
public:
    /// A default-constructed gate is usable at 44.1 kHz without a setup call,
    /// so a caller that only overrides one control still gets working vactrol
    /// dynamics rather than a stuck-shut gate.
    LowpassGateT() { update(); }

    void set_sample_rate(double sr) {
        sample_rate_ = sr > 0.0 ? sr : sample_rate_;
        update();
    }

    /// Time constant of the vactrol turning on, in milliseconds. Short: the
    /// LED reaches brightness far faster than the photoresistor recovers.
    void set_rise_ms(double ms) {
        rise_ms_ = std::max(ms, 0.01);
        update();
    }

    /// Time constant of the vactrol turning off, in milliseconds. This is the
    /// control that sets how long a struck note takes to disappear.
    void set_fall_ms(double ms) {
        fall_ms_ = std::max(ms, 0.01);
        update();
    }

    /// Blend between amplitude gating (0) and filtering (1).
    void set_colour(double amount) { colour_ = std::clamp(amount, 0.0, 1.0); }

    /// Filter corner when the gate is fully closed, in Hz.
    void set_closed_cutoff_hz(double hz) {
        closed_hz_ = std::clamp(hz, 10.0, 20000.0);
        update();
    }

    /// Filter corner when the gate is fully open, in Hz.
    void set_open_cutoff_hz(double hz) {
        open_hz_ = std::clamp(hz, 10.0, 20000.0);
        update();
    }

    /// Exponent of the control-to-gain law. Above 1 the gate spends more of
    /// its travel near silence, which is what gives a vactrol its long quiet
    /// tail rather than a linear fade.
    void set_gain_exponent(double e) { gain_exponent_ = std::clamp(e, 0.25, 4.0); }

    void reset() {
        control_ = 0.0;
        gate_filter_.reset();
    }

    /// Current smoothed control value in [0, 1]. Exposed because a voice often
    /// wants to know whether the gate has closed before it stops rendering.
    double control() const { return control_; }

    /// Advances the vactrol by one sample toward `target` and applies the
    /// resulting gate to `input`. `target` is the raw control (an envelope, a
    /// gate signal) in [0, 1].
    SampleType process(SampleType input, double target) {
        const double clamped = std::clamp(target, 0.0, 1.0);
        const double coefficient = clamped > control_ ? rise_a_ : fall_a_;
        control_ = snap_to_zero(control_ + coefficient * (clamped - control_));

        const double vca = std::pow(control_, gain_exponent_);
        // The two paths are complementary: the amplitude term carries the whole
        // gate at colour 0 and is bypassed at colour 1, while the filter blend
        // below does the opposite.
        double x = static_cast<double>(input) * (1.0 - (1.0 - colour_) * (1.0 - vca));

        // The corner sweeps geometrically with the control, because pitch and
        // brightness are heard geometrically: a linear sweep spends nearly all
        // its travel in the top octave and sounds like it does nothing.
        gate_filter_.set_cutoff(static_cast<SampleType>(
            closed_limit_ * std::pow(open_limit_ / closed_limit_, control_)));
        const double filtered = gate_filter_.process_lowpass(static_cast<SampleType>(x));

        // Mirror of the amplitude blend: colour 0 leaves the signal unfiltered.
        x = x + colour_ * (filtered - x);
        return static_cast<SampleType>(x);
    }

private:
    void update() {
        rise_a_ = 1.0 - std::exp(-1.0 / std::max(0.001 * rise_ms_ * sample_rate_, 1e-9));
        fall_a_ = 1.0 - std::exp(-1.0 / std::max(0.001 * fall_ms_ * sample_rate_, 1e-9));
        gate_filter_.prepare(static_cast<SampleType>(sample_rate_));
        const double nyquist = 0.49 * sample_rate_;
        open_limit_ = std::min(open_hz_, nyquist);
        closed_limit_ = std::min(closed_hz_, nyquist);
    }

    double sample_rate_ = 44100.0;
    double rise_ms_ = 2.0;
    double fall_ms_ = 200.0;
    double colour_ = 0.5;
    double closed_hz_ = 60.0;
    double open_hz_ = 12000.0;
    double gain_exponent_ = 1.5;

    double rise_a_ = 0.0;
    double fall_a_ = 0.0;
    double open_limit_ = 12000.0;
    double closed_limit_ = 60.0;

    double control_ = 0.0;
    TptFilterT<SampleType> gate_filter_;
};

using LowpassGate = LowpassGateT<float>;
using LowpassGate64 = LowpassGateT<double>;

}  // namespace pulp::signal
