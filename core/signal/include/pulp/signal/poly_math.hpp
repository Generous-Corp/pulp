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
/// RT contract: eval(), eval_complex(), roots_quadratic(), and fixed-size
/// matrix operations are allocation-free. Helpers that return std::vector
/// allocate result storage and are design/control-thread utilities.
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
    template <typename SampleType = float>
    static SampleType eval(const std::vector<SampleType>& coeffs, SampleType x) {
        if (coeffs.empty()) return 0;
        SampleType result = coeffs.back();
        for (int i = static_cast<int>(coeffs.size()) - 2; i >= 0; --i) {
            result = result * x + coeffs[static_cast<size_t>(i)];
        }
        return result;
    }

    /// Evaluate complex polynomial (for frequency response).
    template <typename SampleType = float>
    static std::complex<SampleType> eval_complex(
        const std::vector<SampleType>& coeffs,
        std::complex<SampleType> z) {
        if (coeffs.empty()) return {0, 0};
        std::complex<SampleType> result = coeffs.back();
        for (int i = static_cast<int>(coeffs.size()) - 2; i >= 0; --i) {
            result = result * z + coeffs[static_cast<size_t>(i)];
        }
        return result;
    }

    /// Roots of quadratic ax^2 + bx + c.
    /// Returns complex roots (may be real with imag=0).
    template <typename SampleType = float>
    static std::pair<std::complex<SampleType>, std::complex<SampleType>>
    roots_quadratic(SampleType a, SampleType b, SampleType c) {
        if (std::abs(a) < SampleType{1e-12f}) {
            if (std::abs(b) < SampleType{1e-12f}) {
                return {{SampleType{0.0f}, SampleType{0.0f}},
                        {SampleType{0.0f}, SampleType{0.0f}}};
            }
            const auto root = std::complex<SampleType>{-c / b, SampleType{0.0f}};
            return {root, root};
        }

        SampleType disc = b * b - SampleType{4.0f} * a * c;
        if (disc >= 0) {
            SampleType sq = std::sqrt(disc);
            return {(-b + sq) / (SampleType{2.0f} * a),
                    (-b - sq) / (SampleType{2.0f} * a)};
        } else {
            SampleType real = -b / (SampleType{2.0f} * a);
            SampleType imag = std::sqrt(-disc) / (SampleType{2.0f} * a);
            return {{real, imag}, {real, -imag}};
        }
    }

    /// Multiply two polynomials (convolution of coefficients). Not RT-safe.
    template <typename SampleType = float>
    static std::vector<SampleType> multiply(const std::vector<SampleType>& a,
                                            const std::vector<SampleType>& b) {
        if (a.empty() || b.empty()) return {};
        std::vector<SampleType> result(a.size() + b.size() - 1, SampleType{0.0f});
        for (size_t i = 0; i < a.size(); ++i) {
            for (size_t j = 0; j < b.size(); ++j) {
                result[i + j] += a[i] * b[j];
            }
        }
        return result;
    }

    /// Add two polynomials. Not RT-safe.
    template <typename SampleType = float>
    static std::vector<SampleType> add(const std::vector<SampleType>& a,
                                       const std::vector<SampleType>& b) {
        auto& longer = (a.size() >= b.size()) ? a : b;
        auto& shorter = (a.size() >= b.size()) ? b : a;
        std::vector<SampleType> result = longer;
        for (size_t i = 0; i < shorter.size(); ++i) {
            result[i] += shorter[i];
        }
        return result;
    }

    /// Scale all coefficients by a constant. Not RT-safe.
    template <typename SampleType = float>
    static std::vector<SampleType> scale(const std::vector<SampleType>& p,
                                         SampleType s) {
        std::vector<SampleType> result(p.size());
        for (size_t i = 0; i < p.size(); ++i) result[i] = p[i] * s;
        return result;
    }

    /// Derivative of a polynomial. Not RT-safe.
    template <typename SampleType = float>
    static std::vector<SampleType> derivative(const std::vector<SampleType>& p) {
        if (p.size() <= 1) return {0};
        std::vector<SampleType> result(p.size() - 1);
        for (size_t i = 1; i < p.size(); ++i) {
            result[i - 1] = p[i] * static_cast<SampleType>(i);
        }
        return result;
    }
};

/// Simple 2x2 matrix for biquad coefficient manipulation.
template <typename SampleType = float>
struct Mat2T {
    SampleType m[2][2] = {{SampleType{1}, SampleType{0}},
                          {SampleType{0}, SampleType{1}}};

    static Mat2T identity() {
        return {{{SampleType{1}, SampleType{0}},
                 {SampleType{0}, SampleType{1}}}};
    }

    Mat2T operator*(const Mat2T& b) const {
        Mat2T r;
        r.m[0][0] = m[0][0] * b.m[0][0] + m[0][1] * b.m[1][0];
        r.m[0][1] = m[0][0] * b.m[0][1] + m[0][1] * b.m[1][1];
        r.m[1][0] = m[1][0] * b.m[0][0] + m[1][1] * b.m[1][0];
        r.m[1][1] = m[1][0] * b.m[0][1] + m[1][1] * b.m[1][1];
        return r;
    }

    SampleType determinant() const {
        return m[0][0] * m[1][1] - m[0][1] * m[1][0];
    }

    Mat2T inverse() const {
        SampleType d = determinant();
        if (std::abs(d) < SampleType{1e-10f}) return identity();
        SampleType inv_d = SampleType{1.0f} / d;
        return {{{m[1][1] * inv_d, -m[0][1] * inv_d},
                 {-m[1][0] * inv_d, m[0][0] * inv_d}}};
    }
};

using Mat2 = Mat2T<float>;
using Mat2d = Mat2T<double>;

/// Simple 3x3 matrix for state-space filter representations.
template <typename SampleType = float>
struct Mat3T {
    SampleType m[3][3] = {{SampleType{1}, SampleType{0}, SampleType{0}},
                          {SampleType{0}, SampleType{1}, SampleType{0}},
                          {SampleType{0}, SampleType{0}, SampleType{1}}};

    static Mat3T identity() {
        return {{{SampleType{1}, SampleType{0}, SampleType{0}},
                 {SampleType{0}, SampleType{1}, SampleType{0}},
                 {SampleType{0}, SampleType{0}, SampleType{1}}}};
    }

    Mat3T operator*(const Mat3T& b) const {
        Mat3T r{{{SampleType{0}, SampleType{0}, SampleType{0}},
                 {SampleType{0}, SampleType{0}, SampleType{0}},
                 {SampleType{0}, SampleType{0}, SampleType{0}}}};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    r.m[i][j] += m[i][k] * b.m[k][j];
        return r;
    }

    SampleType determinant() const {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
             - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
             + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    }
};

using Mat3 = Mat3T<float>;
using Mat3d = Mat3T<double>;

} // namespace pulp::signal
