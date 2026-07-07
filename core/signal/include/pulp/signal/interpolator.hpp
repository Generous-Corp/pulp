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
    template <typename SampleType>
    static SampleType linear(SampleType frac, SampleType y0, SampleType y1) {
        return y0 + frac * (y1 - y0);
    }

    /// Cubic Hermite (Catmull-Rom) interpolation.
    /// Requires 4 points: ym1 (pos -1), y0 (pos 0), y1 (pos 1), y2 (pos 2).
    /// Good balance of quality vs cost for most audio applications.
    template <typename SampleType>
    static SampleType hermite(SampleType frac,
                              SampleType ym1,
                              SampleType y0,
                              SampleType y1,
                              SampleType y2) {
        SampleType c0 = y0;
        SampleType c1 = SampleType{0.5f} * (y1 - ym1);
        SampleType c2 = ym1 - SampleType{2.5f} * y0 +
                        SampleType{2.0f} * y1 - SampleType{0.5f} * y2;
        SampleType c3 = SampleType{0.5f} * (y2 - ym1) +
                        SampleType{1.5f} * (y0 - y1);
        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    /// 4-point Lagrange interpolation.
    /// Requires 4 points: ym1, y0, y1, y2 (same layout as hermite).
    /// Slightly different character than Hermite — no overshoot guarantee
    /// but mathematically exact for polynomials up to degree 3.
    template <typename SampleType>
    static SampleType lagrange(SampleType frac,
                               SampleType ym1,
                               SampleType y0,
                               SampleType y1,
                               SampleType y2) {
        // Nodes at x = -1, 0, 1, 2. Evaluate at x = frac (in [0,1]).
        SampleType d = frac;
        // L_{-1}(d) = d(d-1)(d-2) / ((-1-0)(-1-1)(-1-2)) = d(d-1)(d-2) / (-1)(-2)(-3) = -d(d-1)(d-2)/6
        SampleType L0 = -d * (d - SampleType{1.0f}) *
                        (d - SampleType{2.0f}) / SampleType{6.0f};
        // L_0(d) = (d+1)(d-1)(d-2) / ((0+1)(0-1)(0-2)) = (d+1)(d-1)(d-2) / (1)(-1)(-2) = (d+1)(d-1)(d-2)/2
        SampleType L1 = (d + SampleType{1.0f}) * (d - SampleType{1.0f}) *
                        (d - SampleType{2.0f}) / SampleType{2.0f};
        // L_1(d) = (d+1)d(d-2) / ((1+1)(1-0)(1-2)) = (d+1)d(d-2) / (2)(1)(-1) = -(d+1)d(d-2)/2
        SampleType L2 = -(d + SampleType{1.0f}) * d *
                        (d - SampleType{2.0f}) / SampleType{2.0f};
        // L_2(d) = (d+1)d(d-1) / ((2+1)(2-0)(2-1)) = (d+1)d(d-1) / (3)(2)(1) = (d+1)d(d-1)/6
        SampleType L3 = (d + SampleType{1.0f}) * d *
                        (d - SampleType{1.0f}) / SampleType{6.0f};

        return L0 * ym1 + L1 * y0 + L2 * y1 + L3 * y2;
    }

    /// Windowed-sinc interpolation (6-point).
    /// Highest quality, suitable for mastering-grade resampling.
    /// Requires 6 points: ym2, ym1, y0, y1, y2, y3.
    template <typename SampleType>
    static SampleType sinc6(SampleType frac,
                            SampleType ym2,
                            SampleType ym1,
                            SampleType y0,
                            SampleType y1,
                            SampleType y2,
                            SampleType y3) {
        // Evaluate windowed sinc at 6 fractional offsets
        SampleType sum = SampleType{0.0f};
        SampleType samples[6] = {ym2, ym1, y0, y1, y2, y3};
        for (int i = 0; i < 6; ++i) {
            SampleType x = frac - static_cast<SampleType>(i - 2);
            sum += samples[i] * windowed_sinc(x);
        }
        return sum;
    }

private:
    template <typename SampleType>
    static constexpr SampleType pi = SampleType{3.14159265358979323846f};

    template <typename SampleType>
    static SampleType sinc(SampleType x) {
        if (std::abs(x) < SampleType{1e-7f}) return SampleType{1.0f};
        SampleType px = pi<SampleType> * x;
        return std::sin(px) / px;
    }

    /// Blackman-Harris window for sinc interpolation.
    template <typename SampleType>
    static SampleType blackman_harris(SampleType x, SampleType half_width) {
        if (std::abs(x) >= half_width) return SampleType{0.0f};
        SampleType n = (x / half_width + SampleType{1.0f}) *
                       SampleType{0.5f}; // normalize to [0, 1]
        constexpr SampleType a0 = SampleType{0.35875f};
        constexpr SampleType a1 = SampleType{0.48829f};
        constexpr SampleType a2 = SampleType{0.14128f};
        constexpr SampleType a3 = SampleType{0.01168f};
        SampleType t = SampleType{2.0f} * pi<SampleType> * n;
        return a0 - a1 * std::cos(t) + a2 * std::cos(SampleType{2.0f} * t) -
               a3 * std::cos(SampleType{3.0f} * t);
    }

    template <typename SampleType>
    static SampleType windowed_sinc(SampleType x) {
        return sinc(x) * blackman_harris(x, SampleType{3.0f});
    }
};

} // namespace pulp::signal
