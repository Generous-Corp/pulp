#pragma once

/// @file interpolator.hpp
/// High-quality sample interpolation for delay lines and resampling.

#include <cmath>
#include <algorithm>
#include <cstddef>

namespace pulp::signal {

/// Collection of interpolation algorithms for reading between samples.
///
/// All functions take a fractional position and neighboring samples.
/// Use with delay lines, wavetable oscillators, and resamplers.
///
/// RT contract: all interpolation functions are stateless scalar helpers and
/// allocate no memory.
///
/// @code
/// // In a delay line read:
/// float frac = delay - std::floor(delay);
/// int i = static_cast<int>(std::floor(delay));
/// float out = Interpolator::hermite(frac, buf[i-1], buf[i], buf[i+1], buf[i+2]);
/// @endcode
struct Interpolator {

    /// Linear interpolation between two samples.
    /// Requires 2 points: y0 (at position 0), y1 (at position 1).
    static float linear(float frac, float y0, float y1) {
        return y0 + frac * (y1 - y0);
    }

    /// Cubic Hermite (Catmull-Rom) interpolation.
    /// Requires 4 points: ym1 (pos -1), y0 (pos 0), y1 (pos 1), y2 (pos 2).
    /// Good balance of quality vs cost for most audio applications.
    static float hermite(float frac, float ym1, float y0, float y1, float y2) {
        float c0 = y0;
        float c1 = 0.5f * (y1 - ym1);
        float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    /// 4-point Lagrange interpolation.
    /// Requires 4 points: ym1, y0, y1, y2 (same layout as hermite).
    /// Slightly different character than Hermite — no overshoot guarantee
    /// but mathematically exact for polynomials up to degree 3.
    static float lagrange(float frac, float ym1, float y0, float y1, float y2) {
        // Nodes at x = -1, 0, 1, 2. Evaluate at x = frac (in [0,1]).
        float d = frac;
        // L_{-1}(d) = d(d-1)(d-2) / ((-1-0)(-1-1)(-1-2)) = d(d-1)(d-2) / (-1)(-2)(-3) = -d(d-1)(d-2)/6
        float L0 = -d * (d - 1.0f) * (d - 2.0f) / 6.0f;
        // L_0(d) = (d+1)(d-1)(d-2) / ((0+1)(0-1)(0-2)) = (d+1)(d-1)(d-2) / (1)(-1)(-2) = (d+1)(d-1)(d-2)/2
        float L1 = (d + 1.0f) * (d - 1.0f) * (d - 2.0f) / 2.0f;
        // L_1(d) = (d+1)d(d-2) / ((1+1)(1-0)(1-2)) = (d+1)d(d-2) / (2)(1)(-1) = -(d+1)d(d-2)/2
        float L2 = -(d + 1.0f) * d * (d - 2.0f) / 2.0f;
        // L_2(d) = (d+1)d(d-1) / ((2+1)(2-0)(2-1)) = (d+1)d(d-1) / (3)(2)(1) = (d+1)d(d-1)/6
        float L3 = (d + 1.0f) * d * (d - 1.0f) / 6.0f;

        return L0 * ym1 + L1 * y0 + L2 * y1 + L3 * y2;
    }

    /// Windowed-sinc interpolation (6-point).
    /// Highest quality, suitable for mastering-grade resampling.
    /// Requires 6 points: ym2, ym1, y0, y1, y2, y3.
    static float sinc6(float frac, float ym2, float ym1, float y0,
                       float y1, float y2, float y3) {
        // Evaluate windowed sinc at 6 fractional offsets
        float sum = 0.0f;
        float samples[6] = {ym2, ym1, y0, y1, y2, y3};
        for (int i = 0; i < 6; ++i) {
            float x = frac - static_cast<float>(i - 2);
            sum += samples[i] * windowed_sinc(x);
        }
        return sum;
    }

private:
    static constexpr float pi = 3.14159265358979323846f;

    static float sinc(float x) {
        if (std::abs(x) < 1e-7f) return 1.0f;
        float px = pi * x;
        return std::sin(px) / px;
    }

    /// Blackman-Harris window for sinc interpolation.
    static float blackman_harris(float x, float half_width) {
        if (std::abs(x) >= half_width) return 0.0f;
        float n = (x / half_width + 1.0f) * 0.5f; // normalize to [0, 1]
        constexpr float a0 = 0.35875f;
        constexpr float a1 = 0.48829f;
        constexpr float a2 = 0.14128f;
        constexpr float a3 = 0.01168f;
        float t = 2.0f * pi * n;
        return a0 - a1 * std::cos(t) + a2 * std::cos(2.0f * t) - a3 * std::cos(3.0f * t);
    }

    static float windowed_sinc(float x) {
        return sinc(x) * blackman_harris(x, 3.0f);
    }
};

} // namespace pulp::signal
