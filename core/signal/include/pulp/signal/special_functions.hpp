#pragma once

// Special mathematical functions for DSP and filter design.

#include <cmath>

namespace pulp::signal {

/// Sinc function: sin(pi*x) / (pi*x), with sinc(0) = 1
inline float sinc(float x) {
    if (std::abs(x) < 1e-7f) return 1.0f;
    float px = 3.14159265358979f * x;
    return std::sin(px) / px;
}

/// Modified Bessel function of the first kind, order 0
/// Used in Kaiser window design
inline float bessel_i0(float x) {
    float sum = 1.0f;
    float term = 1.0f;
    float x_half = x * 0.5f;

    for (int k = 1; k < 25; ++k) {
        term *= (x_half / static_cast<float>(k));
        term *= (x_half / static_cast<float>(k));
        sum += term;
        if (term < sum * 1e-10f) break;
    }
    return sum;
}

/// Gamma function approximation (Stirling's for large x, series for small)
inline float gamma_fn(float x) {
    return std::tgamma(x);
}

/// Error function
inline float erf_fn(float x) {
    return std::erf(x);
}

/// Complementary error function
inline float erfc_fn(float x) {
    return std::erfc(x);
}

/// Lanczos kernel (used in high-quality resampling)
inline float lanczos(float x, int a) {
    if (std::abs(x) < 1e-7f) return 1.0f;
    if (std::abs(x) >= static_cast<float>(a)) return 0.0f;
    return sinc(x) * sinc(x / static_cast<float>(a));
}

/// dB to linear gain conversion
inline float db_to_linear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

/// Linear gain to dB conversion
inline float linear_to_db(float linear) {
    return 20.0f * std::log10(std::max(linear, 1e-10f));
}

/// Frequency to MIDI note number (A4 = 69)
inline float freq_to_midi(float freq) {
    return 69.0f + 12.0f * std::log2(freq / 440.0f);
}

/// MIDI note number to frequency
inline float midi_to_freq(float note) {
    return 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f);
}

}  // namespace pulp::signal
