#pragma once

#include <cmath>

namespace pulp::signal {

// Band-limited oscillator with polyBLEP anti-aliasing.
// RT contract: setters, reset, and next() allocate no memory. Smooth frequency
// externally if zipper-free retuning is required.
// Supports sine, saw, square, and triangle waveforms.
template <typename SampleType = float>
class OscillatorT {
public:
    enum class Waveform { sine, saw, square, triangle };

    void set_sample_rate(SampleType sr) { sample_rate_ = sr; }
    void set_frequency(SampleType hz) { freq_ = hz; }
    void set_waveform(Waveform w) { waveform_ = w; }

    // Reset phase to 0
    void reset() { phase_ = SampleType{0}; }

    // Generate next sample
    SampleType next() {
        SampleType dt = freq_ / sample_rate_;
        SampleType out = 0;

        switch (waveform_) {
            case Waveform::sine:
                out = std::sin(SampleType{2.0f} * pi * phase_);
                break;

            case Waveform::saw:
                out = SampleType{2.0f} * phase_ - SampleType{1.0f};
                out -= poly_blep(phase_, dt);
                break;

            case Waveform::square: {
                out = phase_ < SampleType{0.5f} ? SampleType{1.0f} : SampleType{-1.0f};
                out += poly_blep(phase_, dt);
                out -= poly_blep(std::fmod(phase_ + SampleType{0.5f}, SampleType{1.0f}), dt);
                break;
            }

            case Waveform::triangle:
                // Integrated square wave
                out = phase_ < SampleType{0.5f} ? SampleType{1.0f} : SampleType{-1.0f};
                out += poly_blep(phase_, dt);
                out -= poly_blep(std::fmod(phase_ + SampleType{0.5f}, SampleType{1.0f}), dt);
                // Integrate: leaky integrator
                tri_state_ = dt * out + (SampleType{1.0f} - dt) * tri_state_;
                // Scale to -1..1 range
                out = tri_state_ * SampleType{4.0f};
                break;
        }

        // Advance phase
        phase_ += dt;
        if (phase_ >= SampleType{1.0f}) phase_ -= SampleType{1.0f};

        return out;
    }

    SampleType phase() const { return phase_; }
    SampleType frequency() const { return freq_; }

private:
    static constexpr SampleType pi = SampleType{3.14159265358979323846f};

    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType freq_ = SampleType{440.0f};
    SampleType phase_ = 0;
    SampleType tri_state_ = 0; // For triangle wave integration
    Waveform waveform_ = Waveform::sine;

    // PolyBLEP anti-aliasing residual
    static SampleType poly_blep(SampleType t, SampleType dt) {
        if (t < dt) {
            t /= dt;
            return t + t - t * t - SampleType{1.0f};
        }
        if (t > SampleType{1.0f} - dt) {
            t = (t - SampleType{1.0f}) / dt;
            return t * t + t + t + SampleType{1.0f};
        }
        return SampleType{0.0f};
    }
};

using Oscillator = OscillatorT<float>;
using Oscillator64 = OscillatorT<double>;

} // namespace pulp::signal
