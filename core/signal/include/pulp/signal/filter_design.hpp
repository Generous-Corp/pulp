#pragma once

/// @file filter_design.hpp
/// Automated filter coefficient generation for biquad and high-order IIR filters.

#include <pulp/signal/biquad.hpp>
#include <vector>
#include <cmath>

namespace pulp::signal {

/// Filter coefficient generation utilities.
///
/// Generates biquad coefficients for standard filter types using the
/// Audio EQ Cookbook formulas (Robert Bristow-Johnson).
///
/// @code
/// auto coeffs = FilterDesign::lowpass(1000.0f, 0.707f, 44100.0f);
/// biquad.set_coefficients(coeffs);
///
/// // High-order Butterworth via cascaded biquads
/// auto cascade = FilterDesign::butterworth_lowpass(4, 2000.0f, 44100.0f);
/// // cascade contains 2 biquad coefficient sets
/// @endcode
struct FilterDesign {

    /// Biquad coefficient set (normalized: a0 = 1).
    struct Coefficients {
        float b0 = 1, b1 = 0, b2 = 0;
        float a1 = 0, a2 = 0;
    };

    // ── Standard biquad types (Audio EQ Cookbook) ────────────────────────

    static Coefficients lowpass(float freq_hz, float Q, float sample_rate) {
        auto [w0, alpha] = compute_w0_alpha(freq_hz, Q, sample_rate);
        float cos_w0 = std::cos(w0);
        float a0 = 1.0f + alpha;
        return normalize(a0, {
            (1.0f - cos_w0) / 2.0f,
            1.0f - cos_w0,
            (1.0f - cos_w0) / 2.0f,
            -2.0f * cos_w0,
            1.0f - alpha
        });
    }

    static Coefficients highpass(float freq_hz, float Q, float sample_rate) {
        auto [w0, alpha] = compute_w0_alpha(freq_hz, Q, sample_rate);
        float cos_w0 = std::cos(w0);
        float a0 = 1.0f + alpha;
        return normalize(a0, {
            (1.0f + cos_w0) / 2.0f,
            -(1.0f + cos_w0),
            (1.0f + cos_w0) / 2.0f,
            -2.0f * cos_w0,
            1.0f - alpha
        });
    }

    static Coefficients bandpass(float freq_hz, float Q, float sample_rate) {
        auto [w0, alpha] = compute_w0_alpha(freq_hz, Q, sample_rate);
        float cos_w0 = std::cos(w0);
        float a0 = 1.0f + alpha;
        return normalize(a0, {
            alpha,
            0.0f,
            -alpha,
            -2.0f * cos_w0,
            1.0f - alpha
        });
    }

    static Coefficients notch(float freq_hz, float Q, float sample_rate) {
        auto [w0, alpha] = compute_w0_alpha(freq_hz, Q, sample_rate);
        float cos_w0 = std::cos(w0);
        float a0 = 1.0f + alpha;
        return normalize(a0, {
            1.0f,
            -2.0f * cos_w0,
            1.0f,
            -2.0f * cos_w0,
            1.0f - alpha
        });
    }

    static Coefficients allpass(float freq_hz, float Q, float sample_rate) {
        auto [w0, alpha] = compute_w0_alpha(freq_hz, Q, sample_rate);
        float cos_w0 = std::cos(w0);
        float a0 = 1.0f + alpha;
        return normalize(a0, {
            1.0f - alpha,
            -2.0f * cos_w0,
            1.0f + alpha,
            -2.0f * cos_w0,
            1.0f - alpha
        });
    }

    /// Peaking EQ filter.
    /// @param gain_db Boost/cut in dB.
    static Coefficients peaking_eq(float freq_hz, float Q, float gain_db, float sample_rate) {
        auto [w0, alpha] = compute_w0_alpha(freq_hz, Q, sample_rate);
        float A = std::pow(10.0f, gain_db / 40.0f);
        float cos_w0 = std::cos(w0);
        float a0 = 1.0f + alpha / A;
        return normalize(a0, {
            1.0f + alpha * A,
            -2.0f * cos_w0,
            1.0f - alpha * A,
            -2.0f * cos_w0,
            1.0f - alpha / A
        });
    }

    /// Low shelf filter.
    static Coefficients low_shelf(float freq_hz, float gain_db, float sample_rate, float S = 1.0f) {
        constexpr float pi = 3.14159265358979323846f;
        float A = std::pow(10.0f, gain_db / 40.0f);
        float w0 = 2.0f * pi * freq_hz / sample_rate;
        float cos_w0 = std::cos(w0);
        float alpha = std::sin(w0) / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
        float sqrtA2alpha = 2.0f * std::sqrt(A) * alpha;
        float a0 = (A + 1.0f) + (A - 1.0f) * cos_w0 + sqrtA2alpha;
        return normalize(a0, {
            A * ((A + 1.0f) - (A - 1.0f) * cos_w0 + sqrtA2alpha),
            2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w0),
            A * ((A + 1.0f) - (A - 1.0f) * cos_w0 - sqrtA2alpha),
            -2.0f * ((A - 1.0f) + (A + 1.0f) * cos_w0),
            (A + 1.0f) + (A - 1.0f) * cos_w0 - sqrtA2alpha
        });
    }

    /// High shelf filter.
    static Coefficients high_shelf(float freq_hz, float gain_db, float sample_rate, float S = 1.0f) {
        constexpr float pi = 3.14159265358979323846f;
        float A = std::pow(10.0f, gain_db / 40.0f);
        float w0 = 2.0f * pi * freq_hz / sample_rate;
        float cos_w0 = std::cos(w0);
        float alpha = std::sin(w0) / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
        float sqrtA2alpha = 2.0f * std::sqrt(A) * alpha;
        float a0 = (A + 1.0f) - (A - 1.0f) * cos_w0 + sqrtA2alpha;
        return normalize(a0, {
            A * ((A + 1.0f) + (A - 1.0f) * cos_w0 + sqrtA2alpha),
            -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w0),
            A * ((A + 1.0f) + (A - 1.0f) * cos_w0 - sqrtA2alpha),
            2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w0),
            (A + 1.0f) - (A - 1.0f) * cos_w0 - sqrtA2alpha
        });
    }

    // ── High-order Butterworth via cascaded biquads ─────────────────────

    /// Generate cascaded biquad coefficients for an Nth-order Butterworth lowpass.
    /// @param order Filter order (must be even, e.g. 2, 4, 6, 8).
    /// @return Vector of biquad coefficient sets (order/2 sections).
    static std::vector<Coefficients> butterworth_lowpass(int order, float freq_hz, float sample_rate) {
        int num_sections = order / 2;
        std::vector<Coefficients> result;
        result.reserve(static_cast<size_t>(num_sections));
        constexpr float pi = 3.14159265358979323846f;

        for (int k = 0; k < num_sections; ++k) {
            // Q for each section of Butterworth cascade
            float angle = pi * (2.0f * static_cast<float>(k) + 1.0f) / (2.0f * static_cast<float>(order));
            float Q = 1.0f / (2.0f * std::cos(angle));
            result.push_back(lowpass(freq_hz, Q, sample_rate));
        }
        return result;
    }

    /// Generate cascaded biquad coefficients for an Nth-order Butterworth highpass.
    static std::vector<Coefficients> butterworth_highpass(int order, float freq_hz, float sample_rate) {
        int num_sections = order / 2;
        std::vector<Coefficients> result;
        result.reserve(static_cast<size_t>(num_sections));
        constexpr float pi = 3.14159265358979323846f;

        for (int k = 0; k < num_sections; ++k) {
            float angle = pi * (2.0f * static_cast<float>(k) + 1.0f) / (2.0f * static_cast<float>(order));
            float Q = 1.0f / (2.0f * std::cos(angle));
            result.push_back(highpass(freq_hz, Q, sample_rate));
        }
        return result;
    }

private:
    static constexpr float pi = 3.14159265358979323846f;

    struct W0Alpha { float w0; float alpha; };

    static W0Alpha compute_w0_alpha(float freq_hz, float Q, float sample_rate) {
        float w0 = 2.0f * pi * freq_hz / sample_rate;
        float alpha = std::sin(w0) / (2.0f * Q);
        return {w0, alpha};
    }

    static Coefficients normalize(float a0, Coefficients raw) {
        float inv = 1.0f / a0;
        return {
            raw.b0 * inv, raw.b1 * inv, raw.b2 * inv,
            raw.a1 * inv, raw.a2 * inv
        };
    }
};

} // namespace pulp::signal
