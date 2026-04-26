#pragma once

/// @file poly_math.hpp
/// Polynomial evaluation and small matrix utilities for filter design.

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace pulp::signal {

/// Polynomial evaluation and manipulation utilities.
///
/// @code
/// // Evaluate polynomial: 3x^2 + 2x + 1
/// float y = Polynomial::eval({1.0f, 2.0f, 3.0f}, x); // coeffs[0] = constant
///
/// // Find roots of quadratic
/// auto roots = Polynomial::roots_quadratic(1.0f, -3.0f, 2.0f); // x=1, x=2
/// @endcode
struct Polynomial {

    /// Evaluate polynomial using Horner's method.
    /// coeffs[0] = constant, coeffs[n] = x^n coefficient.
    static float eval(const std::vector<float>& coeffs, float x) {
        if (coeffs.empty()) return 0;
        float result = coeffs.back();
        for (int i = static_cast<int>(coeffs.size()) - 2; i >= 0; --i) {
            result = result * x + coeffs[static_cast<size_t>(i)];
        }
        return result;
    }

    /// Evaluate complex polynomial (for frequency response).
    static std::complex<float> eval_complex(const std::vector<float>& coeffs,
                                             std::complex<float> z) {
        if (coeffs.empty()) return {0, 0};
        std::complex<float> result = coeffs.back();
        for (int i = static_cast<int>(coeffs.size()) - 2; i >= 0; --i) {
            result = result * z + coeffs[static_cast<size_t>(i)];
        }
        return result;
    }

    /// Roots of quadratic ax^2 + bx + c.
    /// Returns complex roots (may be real with imag=0).
    static std::pair<std::complex<float>, std::complex<float>>
    roots_quadratic(float a, float b, float c) {
        float disc = b * b - 4.0f * a * c;
        if (disc >= 0) {
            float sq = std::sqrt(disc);
            return {(-b + sq) / (2.0f * a), (-b - sq) / (2.0f * a)};
        } else {
            float real = -b / (2.0f * a);
            float imag = std::sqrt(-disc) / (2.0f * a);
            return {{real, imag}, {real, -imag}};
        }
    }

    /// Multiply two polynomials (convolution of coefficients).
    static std::vector<float> multiply(const std::vector<float>& a,
                                        const std::vector<float>& b) {
        if (a.empty() || b.empty()) return {};
        std::vector<float> result(a.size() + b.size() - 1, 0.0f);
        for (size_t i = 0; i < a.size(); ++i) {
            for (size_t j = 0; j < b.size(); ++j) {
                result[i + j] += a[i] * b[j];
            }
        }
        return result;
    }

    /// Add two polynomials.
    static std::vector<float> add(const std::vector<float>& a,
                                   const std::vector<float>& b) {
        auto& longer = (a.size() >= b.size()) ? a : b;
        auto& shorter = (a.size() >= b.size()) ? b : a;
        std::vector<float> result = longer;
        for (size_t i = 0; i < shorter.size(); ++i) {
            result[i] += shorter[i];
        }
        return result;
    }

    /// Scale all coefficients by a constant.
    static std::vector<float> scale(const std::vector<float>& p, float s) {
        std::vector<float> result(p.size());
        for (size_t i = 0; i < p.size(); ++i) result[i] = p[i] * s;
        return result;
    }

    /// Derivative of a polynomial.
    static std::vector<float> derivative(const std::vector<float>& p) {
        if (p.size() <= 1) return {0};
        std::vector<float> result(p.size() - 1);
        for (size_t i = 1; i < p.size(); ++i) {
            result[i - 1] = p[i] * static_cast<float>(i);
        }
        return result;
    }
};

/// Simple 2x2 matrix for biquad coefficient manipulation.
struct Mat2 {
    float m[2][2] = {{1, 0}, {0, 1}};

    static Mat2 identity() { return {{{1, 0}, {0, 1}}}; }

    Mat2 operator*(const Mat2& b) const {
        Mat2 r;
        r.m[0][0] = m[0][0] * b.m[0][0] + m[0][1] * b.m[1][0];
        r.m[0][1] = m[0][0] * b.m[0][1] + m[0][1] * b.m[1][1];
        r.m[1][0] = m[1][0] * b.m[0][0] + m[1][1] * b.m[1][0];
        r.m[1][1] = m[1][0] * b.m[0][1] + m[1][1] * b.m[1][1];
        return r;
    }

    float determinant() const {
        return m[0][0] * m[1][1] - m[0][1] * m[1][0];
    }

    Mat2 inverse() const {
        float d = determinant();
        if (std::abs(d) < 1e-10f) return identity();
        float inv_d = 1.0f / d;
        return {{{m[1][1] * inv_d, -m[0][1] * inv_d},
                 {-m[1][0] * inv_d, m[0][0] * inv_d}}};
    }
};

/// Simple 3x3 matrix for state-space filter representations.
struct Mat3 {
    float m[3][3] = {{1,0,0}, {0,1,0}, {0,0,1}};

    static Mat3 identity() { return {{{1,0,0}, {0,1,0}, {0,0,1}}}; }

    Mat3 operator*(const Mat3& b) const {
        Mat3 r{{{0,0,0}, {0,0,0}, {0,0,0}}};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    r.m[i][j] += m[i][k] * b.m[k][j];
        return r;
    }

    float determinant() const {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
             - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
             + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    }
};

} // namespace pulp::signal
