#pragma once

#include <algorithm>
#include <cmath>

namespace pulp::signal {

/// Which phase warp a `PhaseDistortionOscT` applies.
enum class PhaseDistortionShape {
    saw,        ///< the bend point slides toward zero: sine becomes saw-like
    pulse,      ///< a ramp then a hold: sine becomes pulse-like
    resonant_saw,   ///< a swept carrier windowed by a falling ramp
    resonant_triangle,  ///< the same carrier windowed by a triangle
    resonant_trapezoid  ///< the same carrier windowed by a clamped trapezoid
};

/// Phase distortion: a cosine read at a warped phase.
///
/// The oscillator is always a cosine. What changes is how fast the phase moves
/// through it — slowly through one part of the cycle and quickly through
/// another — and that alone turns a sine into a saw, a pulse, or a resonant
/// sweep. Nothing is filtered and nothing is added, so the output is exactly as
/// band-limited as a cosine is, which is why the technique produced clean
/// bright tones on hardware that could not afford a filter.
///
/// The resonant shapes are the distinctive ones and they work differently: the
/// cosine is read at a *multiple* of the phase, and the result is multiplied by
/// a window that falls once per cycle. The multiple is what sets the apparent
/// resonant peak and the window is what makes it a formant rather than a tone,
/// so sweeping the amount sounds like opening a resonant filter even though no
/// filter exists. That illusion is the reason the technique is worth having
/// alongside a real filter rather than instead of one.
///
/// Lineage: Casio's phase-distortion patent, US 4,658,691 (1987, expired).
/// Implemented from the published description of the technique.
///
/// RT contract: every member allocates nothing and takes no locks.
template <typename SampleType = float>
class PhaseDistortionOscT {
public:
    void set_sample_rate(double sr) { sample_rate_ = sr > 0.0 ? sr : sample_rate_; }
    void set_frequency(double hz) {
        frequency_ = std::clamp(hz, 0.0, 0.49 * sample_rate_);
    }
    void set_shape(PhaseDistortionShape shape) { shape_ = shape; }

    /// How far the phase is warped, 0 (an undistorted cosine) to 1.
    void set_amount(double amount) { amount_ = std::clamp(amount, 0.0, 1.0); }

    /// Highest carrier multiple the resonant shapes reach at full amount.
    void set_resonant_depth(double multiples) {
        resonant_depth_ = std::clamp(multiples, 1.0, 32.0);
    }

    void reset() { phase_ = 0.0; }

    double phase() const { return phase_; }

    SampleType process() {
        phase_ += frequency_ / sample_rate_;
        if (phase_ >= 1.0) phase_ -= std::floor(phase_);
        return static_cast<SampleType>(evaluate(phase_));
    }

private:
    static constexpr double kPi = 3.14159265358979323846;

    double evaluate(double p) const {
        switch (shape_) {
            case PhaseDistortionShape::saw:
                return std::cos(2.0 * kPi * warp_saw(p));
            case PhaseDistortionShape::pulse:
                return std::cos(2.0 * kPi * warp_pulse(p));
            case PhaseDistortionShape::resonant_saw:
                return resonant(p, 1.0 - p);
            case PhaseDistortionShape::resonant_triangle:
                return resonant(p, 1.0 - std::fabs(2.0 * p - 1.0));
            case PhaseDistortionShape::resonant_trapezoid:
                return resonant(p, std::clamp(2.0 * (1.0 - p), 0.0, 1.0));
        }
        return std::cos(2.0 * kPi * p);
    }

    // Two straight segments meeting at a bend point. At amount 0 the bend sits
    // at the half-cycle and the warp is the identity, so the output is a plain
    // cosine; as the bend slides toward zero the second segment stretches and
    // the waveform develops a saw's asymmetry.
    double warp_saw(double p) const {
        const double bend = 0.5 - 0.49 * amount_;
        return p < bend ? p * (0.5 / bend) : 0.5 + (p - bend) * (0.5 / (1.0 - bend));
    }

    // A ramp that reaches the far side of the cosine early and then holds
    // there, which narrows the excursion into a pulse.
    double warp_pulse(double p) const {
        const double width = 1.0 - 0.98 * amount_;
        return p < width ? p * (1.0 / width) : 1.0;
    }

    // A carrier at a swept multiple of the phase, windowed once per cycle. The
    // window is what turns a high harmonic into a formant instead of a whistle,
    // and it is why the sweep reads as a filter opening.
    double resonant(double p, double window) const {
        const double multiple = 1.0 + amount_ * (resonant_depth_ - 1.0);
        return std::cos(2.0 * kPi * p * multiple) * std::clamp(window, 0.0, 1.0);
    }

    double sample_rate_ = 44100.0;
    double frequency_ = 220.0;
    double amount_ = 0.0;
    double resonant_depth_ = 12.0;
    PhaseDistortionShape shape_ = PhaseDistortionShape::saw;
    double phase_ = 0.0;
};

using PhaseDistortionOsc = PhaseDistortionOscT<float>;
using PhaseDistortionOsc64 = PhaseDistortionOscT<double>;

}  // namespace pulp::signal
