#pragma once

#include <pulp/signal/denormal.hpp>

#include <cmath>

namespace pulp::signal {

// State Variable Filter — Topology Preserving Transform (TPT) design.
// RT contract: setters recompute scalar coefficients; process/reset allocate no
// memory. Retune at block boundaries if coefficient continuity matters.
// Provides simultaneous lowpass, highpass, bandpass, and notch outputs.
// Numerically stable at all frequencies, no cramping at Nyquist.
template <typename SampleType = float>
class SvfT {
public:
    enum class Mode { lowpass, highpass, bandpass, notch };

    void set_sample_rate(SampleType sr) { sample_rate_ = sr; update(); }
    void set_frequency(SampleType hz) { freq_ = hz; update(); }
    void set_resonance(SampleType q) { q_ = q; update(); }
    void set_mode(Mode m) { mode_ = m; }

    SampleType process(SampleType input) {
        SampleType v3 = input - ic2_;
        SampleType v1 = a1_ * ic1_ + a2_ * v3;
        SampleType v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        // Snap the two integrator states to keep denormals out of the TPT
        // feedback path where no FTZ guard applies. No-op above 1e-15.
        ic1_ = snap_to_zero(SampleType{2.0f} * v1 - ic1_);
        ic2_ = snap_to_zero(SampleType{2.0f} * v2 - ic2_);

        switch (mode_) {
            case Mode::lowpass:  return v2;
            case Mode::highpass: return input - k_ * v1 - v2;
            case Mode::bandpass: return v1;
            case Mode::notch:    return input - k_ * v1;
        }
        return v2;
    }

    void process(SampleType* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    void reset() { ic1_ = 0; ic2_ = 0; }

private:
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType freq_ = SampleType{1000.0f};
    SampleType q_ = SampleType{0.707f};
    Mode mode_ = Mode::lowpass;

    SampleType g_ = 0, k_ = 0;
    SampleType a1_ = 0, a2_ = 0, a3_ = 0;
    SampleType ic1_ = 0, ic2_ = 0;

    void update() {
        g_ = std::tan(SampleType{3.14159265f} * freq_ / sample_rate_);
        k_ = SampleType{1.0f} / q_;
        a1_ = SampleType{1.0f} / (SampleType{1.0f} + g_ * (g_ + k_));
        a2_ = g_ * a1_;
        a3_ = g_ * a2_;
    }
};

using Svf = SvfT<float>;
using Svf64 = SvfT<double>;

} // namespace pulp::signal
