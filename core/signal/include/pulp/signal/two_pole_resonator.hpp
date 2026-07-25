#pragma once

#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal {

/// A single decaying resonance: one complex pole pair, excited by whatever the
/// caller feeds it.
///
/// This is the smallest useful physical model. A struck body rings at a
/// frequency and dies away at a rate, and a two-pole recurrence reproduces
/// exactly that, so a percussion body is an impulse into one of these rather
/// than an oscillator plus an envelope. The difference is audible: an
/// oscillator with a decay envelope restarts at a known phase and decays on a
/// schedule, while a resonator's ring is a function of what excited it, so the
/// same body struck by a click and by a noise burst sounds different, and a
/// second strike during the tail interferes with what is already ringing
/// instead of replacing it.
///
/// `ModalBankT` is the same mathematics for many modes at once, laid out for
/// SIMD and driven a block at a time. Use the bank for a body with a spectrum
/// (a membrane, a plate, a shell); use this when there is one resonance and
/// the surrounding code is per-sample.
///
/// The decay is specified as a T60 because that is how long a body is heard to
/// ring.
///
/// Excitation is scaled by the sine of the pole angle, which makes the ring's
/// peak roughly the excitation's amplitude at any tuning and any decay. Both
/// normalisations that look more obvious are traps. Unit input gain leaves the
/// impulse response peaking at one over that sine, so a body tuned to 50 Hz
/// rings about fifty times louder than the same body at 5 kHz and the tuning
/// knob becomes a volume knob. Scaling by one minus the pole radius normalises
/// a sustained tone instead, which makes a *longer* decay produce a *quieter*
/// strike. This scaling leaves the two controls doing only what they are
/// named for.
///
/// RT contract: every member allocates nothing and takes no locks. The
/// frequency and decay may be changed per sample; the state is the ring
/// itself, so a coefficient change carries it rather than reinterpreting it.
template <typename SampleType = float>
class TwoPoleResonatorT {
public:
    TwoPoleResonatorT() { update(); }

    void set_sample_rate(double sr) {
        sample_rate_ = sr > 0.0 ? sr : sample_rate_;
        update();
    }

    void set_frequency(double hz) {
        // Above Nyquist the recurrence is still stable but the resonance folds,
        // so clamp rather than let a pitch envelope sweep into nonsense.
        frequency_ = std::clamp(hz, 0.0, 0.499 * sample_rate_);
        update();
    }

    /// Time for the ring to fall by 60 dB.
    void set_t60_ms(double ms) {
        t60_ms_ = std::max(ms, 0.01);
        update();
    }

    double frequency() const { return frequency_; }
    double t60_ms() const { return t60_ms_; }

    void reset() {
        y1_ = 0;
        y2_ = 0;
    }

    /// Whether the ring is still above the denormal floor. A voice uses this
    /// to know the body has finished without tracking a separate envelope.
    bool is_ringing() const {
        return std::fabs(static_cast<double>(y1_)) > 1e-7 ||
               std::fabs(static_cast<double>(y2_)) > 1e-7;
    }

    SampleType process(SampleType excitation) {
        const SampleType y = static_cast<SampleType>(a1_) * y1_ -
                             static_cast<SampleType>(a2_) * y2_ +
                             static_cast<SampleType>(gain_) * excitation;
        y2_ = y1_;
        y1_ = snap_to_zero(y);
        return y;
    }

private:
    void update() {
        // T60 in samples -> pole radius: the radius raised to that many samples
        // must equal -60 dB.
        const double samples = std::max(0.001 * t60_ms_ * sample_rate_, 1.0);
        radius_ = std::pow(10.0, -3.0 / samples);
        const double theta = 2.0 * 3.14159265358979323846 * frequency_ / sample_rate_;
        a1_ = 2.0 * radius_ * std::cos(theta);
        a2_ = radius_ * radius_;
        // Floor the gain so a body tuned to DC still passes its excitation
        // rather than muting it.
        gain_ = std::max(std::sin(theta), 1e-4);
    }

    double sample_rate_ = 44100.0;
    double frequency_ = 100.0;
    double t60_ms_ = 300.0;

    double radius_ = 0.0;
    double a1_ = 0.0;
    double a2_ = 0.0;
    double gain_ = 1.0;

    SampleType y1_{};
    SampleType y2_{};
};

using TwoPoleResonator = TwoPoleResonatorT<float>;
using TwoPoleResonator64 = TwoPoleResonatorT<double>;

}  // namespace pulp::signal
