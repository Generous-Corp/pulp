#pragma once

#include <pulp/signal/denormal.hpp>

#include <cmath>
#include <type_traits>

namespace pulp::signal {

// Biquad IIR filter — standard second-order section.
// RT contract: coefficient calculation and process/reset paths allocate no
// memory. Retune coefficients at a block boundary if parameter continuity
// matters.
template <typename SampleType = float>
class BiquadT {
    static_assert(std::is_floating_point_v<SampleType>,
                  "BiquadT requires a floating-point sample type");

public:
    enum class Type { lowpass, highpass, bandpass, notch, allpass, peaking, low_shelf, high_shelf };

    BiquadT() = default;

    // Configure from type, frequency, Q, and optional gain (for peaking/shelf)
    void set_coefficients(Type type,
                          SampleType freq_hz,
                          SampleType q,
                          SampleType sample_rate,
                          SampleType gain_db = SampleType{0}) {
        // Guard the inputs that would produce inf/NaN coefficients and wedge the
        // filter state forever: sample_rate ≤ 0 (w0 = ∞), q ≤ 0 (alpha = ∞ at q=0,
        // or a0 = 1+alpha crossing 0 for negative q), and non-finite freq/gain.
        // Clamp only these — any finite positive q, however tiny, yields large but
        // finite coefficients, so every legitimate retune stays bit-identical.
        // Mirrors the input hardening in NoiseGate::set_params.
        if (!(std::isfinite(sample_rate) && sample_rate > SampleType{0}))
            sample_rate = SampleType{44100};
        if (!(std::isfinite(q) && q > SampleType{0}))
            q = SampleType{1e-4f};
        if (!std::isfinite(gain_db)) gain_db = SampleType{0};
        if (!(std::isfinite(freq_hz) && freq_hz > SampleType{0}))
            freq_hz = SampleType{1000};

        SampleType w0 = SampleType{2} * pi * freq_hz / sample_rate;
        SampleType cos_w0 = std::cos(w0);
        SampleType sin_w0 = std::sin(w0);
        SampleType alpha = sin_w0 / (SampleType{2} * q);

        SampleType a0 = SampleType{1};
        SampleType a1 = SampleType{0};
        SampleType a2 = SampleType{0};
        SampleType b0 = SampleType{1};
        SampleType b1 = SampleType{0};
        SampleType b2 = SampleType{0};

        switch (type) {
            case Type::lowpass:
                b0 = (SampleType{1} - cos_w0) / SampleType{2};
                b1 = SampleType{1} - cos_w0;
                b2 = (SampleType{1} - cos_w0) / SampleType{2};
                a0 = SampleType{1} + alpha;
                a1 = -SampleType{2} * cos_w0;
                a2 = SampleType{1} - alpha;
                break;

            case Type::highpass:
                b0 = (SampleType{1} + cos_w0) / SampleType{2};
                b1 = -(SampleType{1} + cos_w0);
                b2 = (SampleType{1} + cos_w0) / SampleType{2};
                a0 = SampleType{1} + alpha;
                a1 = -SampleType{2} * cos_w0;
                a2 = SampleType{1} - alpha;
                break;

            case Type::bandpass:
                b0 = alpha;
                b1 = SampleType{0};
                b2 = -alpha;
                a0 = SampleType{1} + alpha;
                a1 = -SampleType{2} * cos_w0;
                a2 = SampleType{1} - alpha;
                break;

            case Type::notch:
                b0 = SampleType{1};
                b1 = -SampleType{2} * cos_w0;
                b2 = SampleType{1};
                a0 = SampleType{1} + alpha;
                a1 = -SampleType{2} * cos_w0;
                a2 = SampleType{1} - alpha;
                break;

            case Type::allpass:
                b0 = SampleType{1} - alpha;
                b1 = -SampleType{2} * cos_w0;
                b2 = SampleType{1} + alpha;
                a0 = SampleType{1} + alpha;
                a1 = -SampleType{2} * cos_w0;
                a2 = SampleType{1} - alpha;
                break;

            case Type::peaking: {
                SampleType A = std::pow(SampleType{10}, gain_db / SampleType{40});
                b0 = SampleType{1} + alpha * A;
                b1 = -SampleType{2} * cos_w0;
                b2 = SampleType{1} - alpha * A;
                a0 = SampleType{1} + alpha / A;
                a1 = -SampleType{2} * cos_w0;
                a2 = SampleType{1} - alpha / A;
                break;
            }

            case Type::low_shelf: {
                SampleType A = std::pow(SampleType{10}, gain_db / SampleType{40});
                SampleType two_sqrt_a_alpha = SampleType{2} * std::sqrt(A) * alpha;
                b0 = A * ((A + SampleType{1}) - (A - SampleType{1}) * cos_w0 + two_sqrt_a_alpha);
                b1 = SampleType{2} * A * ((A - SampleType{1}) - (A + SampleType{1}) * cos_w0);
                b2 = A * ((A + SampleType{1}) - (A - SampleType{1}) * cos_w0 - two_sqrt_a_alpha);
                a0 = (A + SampleType{1}) + (A - SampleType{1}) * cos_w0 + two_sqrt_a_alpha;
                a1 = -SampleType{2} * ((A - SampleType{1}) + (A + SampleType{1}) * cos_w0);
                a2 = (A + SampleType{1}) + (A - SampleType{1}) * cos_w0 - two_sqrt_a_alpha;
                break;
            }

            case Type::high_shelf: {
                SampleType A = std::pow(SampleType{10}, gain_db / SampleType{40});
                SampleType two_sqrt_a_alpha = SampleType{2} * std::sqrt(A) * alpha;
                b0 = A * ((A + SampleType{1}) + (A - SampleType{1}) * cos_w0 + two_sqrt_a_alpha);
                b1 = -SampleType{2} * A * ((A - SampleType{1}) + (A + SampleType{1}) * cos_w0);
                b2 = A * ((A + SampleType{1}) + (A - SampleType{1}) * cos_w0 - two_sqrt_a_alpha);
                a0 = (A + SampleType{1}) - (A - SampleType{1}) * cos_w0 + two_sqrt_a_alpha;
                a1 = SampleType{2} * ((A - SampleType{1}) - (A + SampleType{1}) * cos_w0);
                a2 = (A + SampleType{1}) - (A - SampleType{1}) * cos_w0 - two_sqrt_a_alpha;
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
    SampleType process(SampleType input) {
        SampleType output = b0_ * input + s1_;
        // Snap the recursive state to zero to keep denormals out of the DF2T
        // feedback path on platforms/threads without an FTZ guard (wasm,
        // MSVC/ARM64, graph worker threads). No-op for any state >= 1e-15.
        s1_ = snap_to_zero(b1_ * input - a1_ * output + s2_);
        s2_ = snap_to_zero(b2_ * input - a2_ * output);
        return output;
    }

    // Process a buffer in-place
    void process(SampleType* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    // Reset state (call on discontinuities)
    void reset() { s1_ = SampleType{0}; s2_ = SampleType{0}; }

private:
    static constexpr SampleType pi =
        std::is_same_v<SampleType, float>
            ? static_cast<SampleType>(3.14159265358979323846f)
            : static_cast<SampleType>(3.141592653589793238462643383279502884L);

    SampleType b0_ = SampleType{1};
    SampleType b1_ = SampleType{0};
    SampleType b2_ = SampleType{0};
    SampleType a1_ = SampleType{0};
    SampleType a2_ = SampleType{0};
    SampleType s1_ = SampleType{0};
    SampleType s2_ = SampleType{0};
};

using Biquad = BiquadT<float>;
using Biquad64 = BiquadT<double>;

} // namespace pulp::signal
