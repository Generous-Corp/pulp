#pragma once

#include <cmath>

namespace pulp::signal {

// Biquad IIR filter — standard second-order section.
// RT contract: coefficient calculation and process/reset paths allocate no
// memory. Retune coefficients at a block boundary if parameter continuity
// matters.
class Biquad {
public:
    enum class Type { lowpass, highpass, bandpass, notch, allpass, peaking, low_shelf, high_shelf };

    Biquad() = default;

    // Configure from type, frequency, Q, and optional gain (for peaking/shelf)
    void set_coefficients(Type type, float freq_hz, float q, float sample_rate, float gain_db = 0.0f) {
        // Guard the inputs that would produce inf/NaN coefficients and wedge the
        // filter state forever: sample_rate ≤ 0 (w0 = ∞), q ≤ 0 (alpha = ∞ at q=0,
        // or a0 = 1+alpha crossing 0 for negative q), and non-finite freq/gain.
        // Clamp only these — any finite positive q, however tiny, yields large but
        // finite coefficients, so every legitimate retune stays bit-identical.
        // Mirrors the input hardening in NoiseGate::set_params.
        if (!(std::isfinite(sample_rate) && sample_rate > 0.0f)) sample_rate = 44100.0f;
        if (!(std::isfinite(q) && q > 0.0f)) q = 1e-4f;
        if (!std::isfinite(gain_db)) gain_db = 0.0f;
        if (!(std::isfinite(freq_hz) && freq_hz > 0.0f)) freq_hz = 1000.0f;

        float w0 = 2.0f * pi * freq_hz / sample_rate;
        float cos_w0 = std::cos(w0);
        float sin_w0 = std::sin(w0);
        float alpha = sin_w0 / (2.0f * q);

        float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f, b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;

        switch (type) {
            case Type::lowpass:
                b0 = (1.0f - cos_w0) / 2.0f;
                b1 = 1.0f - cos_w0;
                b2 = (1.0f - cos_w0) / 2.0f;
                a0 = 1.0f + alpha;
                a1 = -2.0f * cos_w0;
                a2 = 1.0f - alpha;
                break;

            case Type::highpass:
                b0 = (1.0f + cos_w0) / 2.0f;
                b1 = -(1.0f + cos_w0);
                b2 = (1.0f + cos_w0) / 2.0f;
                a0 = 1.0f + alpha;
                a1 = -2.0f * cos_w0;
                a2 = 1.0f - alpha;
                break;

            case Type::bandpass:
                b0 = alpha;
                b1 = 0.0f;
                b2 = -alpha;
                a0 = 1.0f + alpha;
                a1 = -2.0f * cos_w0;
                a2 = 1.0f - alpha;
                break;

            case Type::notch:
                b0 = 1.0f;
                b1 = -2.0f * cos_w0;
                b2 = 1.0f;
                a0 = 1.0f + alpha;
                a1 = -2.0f * cos_w0;
                a2 = 1.0f - alpha;
                break;

            case Type::allpass:
                b0 = 1.0f - alpha;
                b1 = -2.0f * cos_w0;
                b2 = 1.0f + alpha;
                a0 = 1.0f + alpha;
                a1 = -2.0f * cos_w0;
                a2 = 1.0f - alpha;
                break;

            case Type::peaking: {
                float A = std::pow(10.0f, gain_db / 40.0f);
                b0 = 1.0f + alpha * A;
                b1 = -2.0f * cos_w0;
                b2 = 1.0f - alpha * A;
                a0 = 1.0f + alpha / A;
                a1 = -2.0f * cos_w0;
                a2 = 1.0f - alpha / A;
                break;
            }

            case Type::low_shelf: {
                float A = std::pow(10.0f, gain_db / 40.0f);
                float two_sqrt_a_alpha = 2.0f * std::sqrt(A) * alpha;
                b0 = A * ((A + 1) - (A - 1) * cos_w0 + two_sqrt_a_alpha);
                b1 = 2.0f * A * ((A - 1) - (A + 1) * cos_w0);
                b2 = A * ((A + 1) - (A - 1) * cos_w0 - two_sqrt_a_alpha);
                a0 = (A + 1) + (A - 1) * cos_w0 + two_sqrt_a_alpha;
                a1 = -2.0f * ((A - 1) + (A + 1) * cos_w0);
                a2 = (A + 1) + (A - 1) * cos_w0 - two_sqrt_a_alpha;
                break;
            }

            case Type::high_shelf: {
                float A = std::pow(10.0f, gain_db / 40.0f);
                float two_sqrt_a_alpha = 2.0f * std::sqrt(A) * alpha;
                b0 = A * ((A + 1) + (A - 1) * cos_w0 + two_sqrt_a_alpha);
                b1 = -2.0f * A * ((A - 1) + (A + 1) * cos_w0);
                b2 = A * ((A + 1) + (A - 1) * cos_w0 - two_sqrt_a_alpha);
                a0 = (A + 1) - (A - 1) * cos_w0 + two_sqrt_a_alpha;
                a1 = 2.0f * ((A - 1) - (A + 1) * cos_w0);
                a2 = (A + 1) - (A - 1) * cos_w0 - two_sqrt_a_alpha;
                break;
            }
        }

        // Normalize
        b0_ = b0 / a0;
        b1_ = b1 / a0;
        b2_ = b2 / a0;
        a1_ = a1 / a0;
        a2_ = a2 / a0;
    }

    // Process a single sample (Direct Form II Transposed)
    float process(float input) {
        float output = b0_ * input + s1_;
        s1_ = b1_ * input - a1_ * output + s2_;
        s2_ = b2_ * input - a2_ * output;
        return output;
    }

    // Process a buffer in-place
    void process(float* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    // Reset state (call on discontinuities)
    void reset() { s1_ = 0; s2_ = 0; }

private:
    static constexpr float pi = 3.14159265358979323846f;

    float b0_ = 1, b1_ = 0, b2_ = 0;
    float a1_ = 0, a2_ = 0;
    float s1_ = 0, s2_ = 0;
};

} // namespace pulp::signal
