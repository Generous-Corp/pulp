#pragma once

/// @file frequency_shifter.hpp
/// The minimal in-graph frequency shifter: shift in Hz, one knob, no feedback.
///
/// This is the reduced-scope member of the pair. The full Bode/Moog module —
/// up/down/dual-mono/stereo-split modes, barberpole feedback, dry/wet,
/// de-zippering, a stated loop-gain bound and the acceptance suite that
/// measures all of it — is `SsbFrequencyShifterT` in
/// `frequency_shifter_ssb.hpp`. This class stays because `drum/cymbal.hpp`
/// composes it by name and wants exactly this much: a shift amount and a
/// sample, inside a voice that already owns its own gain staging.
///
/// It owns none of the math. The quadrature network and its coefficient table
/// live in `frequency_shifter_ssb.hpp` and are composed from here, so there is
/// one Hilbert network in the tree rather than two that can drift apart. New
/// work should reach for `SsbFrequencyShifterT`; this remains as the thin
/// front end its one caller needs.
///
/// RT contract: `set_sample_rate` and `set_shift_hz` are scalar updates.
/// `process()` and `reset()` allocate nothing and take no locks.

#include <pulp/signal/frequency_shifter_ssb.hpp>

#include <cmath>

namespace pulp::signal {

/// A pair of allpass chains whose outputs stand a quarter cycle apart across
/// the audio band — the analytic-signal splitter, spelled the way this header
/// has always spelled it. See `HilbertQuadratureNetworkT` for the structure,
/// the coefficient table, and why only one of the four I/Q assignments both
/// holds phase across the band and leaves the in-phase branch undelayed.
template <typename SampleType = double>
using HilbertPairT = HilbertQuadratureNetworkT<SampleType>;

/// Shifts every frequency in a signal by the same number of Hz.
///
/// This is not a pitch shift and the difference is the entire point. A pitch
/// shift multiplies every partial by a ratio, so a harmonic series stays
/// harmonic and the sound keeps its identity at a new pitch. A frequency shift
/// *adds* a constant, so a series at 100, 200, 300 Hz becomes 130, 230, 330 Hz
/// — no longer integer multiples of anything. The result has no fundamental and
/// no pitch, which is exactly what is wanted when a bank of resonators is
/// ringing on a chord and the chord is the problem: a shift of a few tens of
/// hertz turns tuned metal into a cymbal.
///
/// Implemented by single-sideband modulation: split the input into a quadrature
/// pair, modulate each by a sine and cosine at the shift frequency, and
/// subtract. Adding instead of subtracting selects the other sideband, which is
/// why the shift can go down as well as up — here that is reached by the sign
/// of `set_shift_hz`, which runs the carrier's phase accumulator backwards.
template <typename SampleType = float>
class FrequencyShifterT {
public:
    void set_sample_rate(double sr) {
        sample_rate_ = sr > 0.0 ? sr : sample_rate_;
        update();
    }

    /// Amount to add to every frequency, in Hz. Negative shifts downward.
    void set_shift_hz(double hz) {
        shift_hz_ = hz;
        update();
    }

    double shift_hz() const { return shift_hz_; }

    void reset() {
        hilbert_.reset();
        phase_ = 0.0;
    }

    SampleType process(SampleType input) {
        const auto pair = hilbert_.process(static_cast<double>(input));

        phase_ += increment_;
        phase_ -= std::floor(phase_);

        const double angle = 2.0 * 3.14159265358979323846 * phase_;
        return static_cast<SampleType>(pair.in_phase * std::cos(angle) -
                                       pair.quadrature * std::sin(angle));
    }

private:
    void update() { increment_ = shift_hz_ / sample_rate_; }

    HilbertPairT<double> hilbert_;
    double sample_rate_ = 44100.0;
    double shift_hz_ = 0.0;
    double increment_ = 0.0;
    double phase_ = 0.0;
};

using HilbertPair = HilbertPairT<double>;
using FrequencyShifter = FrequencyShifterT<float>;
using FrequencyShifter64 = FrequencyShifterT<double>;

}  // namespace pulp::signal
