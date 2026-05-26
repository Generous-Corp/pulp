#pragma once

// Special mathematical functions for DSP and filter design.

#include <algorithm>
#include <cmath>
#include <limits>

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

// ---------------------------------------------------------------------------
// Elliptic / Jacobi special functions
//
// These primitives are needed by elliptic (Cauer) IIR filter design and by
// future analog-prototype filter routines. They live in a nested namespace
// so the surface is self-contained and easy to reference from filter design
// code (e.g. `pulp::signal::special::elliptic_K(m)`), without colliding with
// the existing free functions above.
//
// All routines are header-only and `double`-precision because filter design
// requires more headroom than the audio path: coefficient quantization is
// the bottleneck, so we keep design-time math in double and downcast at
// the biquad-cascade boundary.
//
// References:
//   * Abramowitz & Stegun, "Handbook of Mathematical Functions", §17.
//   * Numerical Recipes in C, §6.11 (elliptic integrals and functions).
//   * Vaidyanathan, "Multirate Systems and Filter Banks", §3.
//
// All functions take the *parameter* m = k^2 (Abramowitz convention), not
// the modulus k. Callers used to SciPy's `ellipk(m)` or MATLAB's
// `ellipke(m)` will find this familiar.
// ---------------------------------------------------------------------------

namespace special {

/// Pi as a double-precision constant. Used by every routine below.
inline constexpr double kPi = 3.141592653589793238462643383279502884;

/// Complete elliptic integral of the first kind, K(m), m = k^2 in [0, 1).
///
/// Computed via the arithmetic-geometric mean (AGM): start with
/// (a0, b0) = (1, sqrt(1 - m)); iterate (a_{n+1}, b_{n+1}) = ((a + b)/2,
/// sqrt(a * b)); K(m) = pi / (2 * AGM). Converges quadratically; 8 to 12
/// iterations reach machine precision for m up to ~1 - 1e-12.
///
/// At m = 1 the integral diverges; the implementation returns +infinity.
/// At m = 0 it returns pi/2 exactly.
inline double elliptic_K(double m) {
    if (m < 0.0 || m >= 1.0) {
        if (m == 1.0) return std::numeric_limits<double>::infinity();
        // Caller passed something out of [0, 1); clamp into the valid range
        // rather than NaN-poisoning downstream filter design code. For m < 0
        // there are extension formulas, but we currently have no caller that
        // needs them.
        if (m < 0.0) m = 0.0;
        else m = std::nextafter(1.0, 0.0);
    }
    double a = 1.0;
    double b = std::sqrt(1.0 - m);
    for (int i = 0; i < 60; ++i) {
        const double a_next = 0.5 * (a + b);
        const double b_next = std::sqrt(a * b);
        if (std::abs(a - b) < 1e-16 * std::abs(a)) {
            a = a_next;
            break;
        }
        a = a_next;
        b = b_next;
    }
    return kPi / (2.0 * a);
}

/// Complete elliptic integral of the second kind, E(m), m = k^2 in [0, 1].
///
/// AGM-based: track c_n = (a_n - b_n)/2 alongside the AGM iteration; then
///   E(m) = K(m) * (1 - sum_{n=0}^{inf} 2^{n-1} c_n^2)
/// where c_0 = sqrt(m). The c_n sequence shrinks quadratically so a
/// handful of terms suffices. The sum coefficient pattern is 2^{-1},
/// 2^0, 2^1, ... so we seed pow2 = 1/2 before doubling each step.
///
/// E(0) = pi/2 and E(1) = 1 exactly (returned as edge cases).
inline double elliptic_E(double m) {
    if (m <= 0.0) return kPi / 2.0;
    if (m >= 1.0) return 1.0;
    double a = 1.0;
    double b = std::sqrt(1.0 - m);
    double c = std::sqrt(m);              // c_0
    double pow2 = 0.5;                    // 2^{n-1} for n=0
    double sum = pow2 * c * c;            // first term: m/2
    for (int i = 1; i < 60; ++i) {
        const double a_next = 0.5 * (a + b);
        const double b_next = std::sqrt(a * b);
        c = 0.5 * (a - b);                // c_i
        a = a_next;
        b = b_next;
        pow2 *= 2.0;                      // now 2^{i-1}
        sum += pow2 * c * c;
        if (std::abs(c) < 1e-17 * std::abs(a)) break;
    }
    const double K = kPi / (2.0 * a);
    return K * (1.0 - sum);
}

/// Jacobi nome q(m) = exp(-pi * K(1 - m) / K(m)).
///
/// q is the natural argument for theta-function series and the design of
/// elliptic rational functions. For small m the nome is approximately m/16;
/// for m close to 1 it approaches 1 from below.
inline double jacobi_nome(double m) {
    if (m <= 0.0) return 0.0;
    if (m >= 1.0) return 1.0;
    const double K  = elliptic_K(m);
    const double Kp = elliptic_K(1.0 - m);
    return std::exp(-kPi * Kp / K);
}

/// Jacobi elliptic functions sn(u, m), cn(u, m), dn(u, m) — all three at
/// once because the AGM-based scheme produces them together.
///
/// Algorithm: Numerical Recipes 3rd ed. §6.12 (Press et al.). We run the
/// AGM until the two sequences a_n and emc_n agree, then unwind the
/// recursion using the recurrence
///   dn_{n-1} = (em_n + dn_n * a) / (b + a),   a := c / b
/// to recover sn / cn / dn at the original modulus. This converges to
/// machine precision in ~6 iterations across the entire m ∈ (0, 1) range
/// without needing per-step asin() calls.
///
/// Outputs are written through pointer arguments so callers can request
/// any subset cheaply (pass nullptr for outputs they don't need).
inline void jacobi_sncndn(double u, double m,
                          double* sn_out, double* cn_out, double* dn_out) {
    if (m <= 0.0) {
        if (sn_out) *sn_out = std::sin(u);
        if (cn_out) *cn_out = std::cos(u);
        if (dn_out) *dn_out = 1.0;
        return;
    }
    if (m >= 1.0) {
        const double th = std::tanh(u);
        const double sech = 1.0 / std::cosh(u);
        if (sn_out) *sn_out = th;
        if (cn_out) *cn_out = sech;
        if (dn_out) *dn_out = sech;
        return;
    }

    // Numerical Recipes 3rd ed. §6.12 `sncndn`. The variable names stay
    // close to the published code so it's easy to cross-check, but Pulp
    // uses descriptive comments instead of one-letter mystery.
    constexpr int kMaxN = 14;
    constexpr double kEps = 1.0e-15;
    double em[kMaxN];                 // sequence of a values (AGM upper)
    double en[kMaxN];                 // sequence of complementary moduli
    double emc = 1.0 - m;             // complementary parameter (1 - m)
    double a   = 1.0;
    double dn  = 1.0;
    double c   = 0.0;
    int last_i = 0;
    for (int i = 0; i < kMaxN; ++i) {
        last_i = i;
        em[i] = a;
        emc   = std::sqrt(emc);
        en[i] = emc;
        c     = 0.5 * (a + emc);
        if (std::abs(a - emc) <= kEps * a) break;
        emc = emc * a;                // emc <- a * emc
        a   = c;
    }
    u *= c;                           // u <- final c * u (not a * u)
    double sn = std::sin(u);
    double cn = std::cos(u);
    if (sn != 0.0) {
        a = cn / sn;
        c *= a;
        for (int i = last_i; i >= 0; --i) {
            const double b = em[i];
            a *= c;
            c *= dn;
            dn = (en[i] + a) / (b + a);
            a  = c / b;
        }
        a  = 1.0 / std::sqrt(c * c + 1.0);
        sn = (sn >= 0.0) ? a : -a;
        cn = sn * c;
    }
    if (sn_out) *sn_out = sn;
    if (cn_out) *cn_out = cn;
    if (dn_out) *dn_out = dn;
}

/// Convenience: sn(u, m).
inline double jacobi_sn(double u, double m) {
    double sn = 0.0;
    jacobi_sncndn(u, m, &sn, nullptr, nullptr);
    return sn;
}

/// Convenience: cn(u, m).
inline double jacobi_cn(double u, double m) {
    double cn = 0.0;
    jacobi_sncndn(u, m, nullptr, &cn, nullptr);
    return cn;
}

/// Convenience: dn(u, m).
inline double jacobi_dn(double u, double m) {
    double dn = 0.0;
    jacobi_sncndn(u, m, nullptr, nullptr, &dn);
    return dn;
}

/// Carlson's symmetric elliptic integral of the first kind, RF(x, y, z).
///
/// Used internally by jacobi_asn(). Carlson's duplication theorem reduces
/// the integral to a power series around the average of the three
/// arguments, converging to machine precision in ~5 iterations for
/// well-conditioned inputs. Reference: B.C. Carlson, "Numerical
/// Computation of Real or Complex Elliptic Integrals" (1995), and
/// Numerical Recipes 3rd ed. §6.12 (function `rf`).
///
/// Domain: x, y, z >= 0 with at most one of them zero. Caller is
/// responsible for staying inside this domain; we simply clamp negatives
/// to zero so a stray rounding error in jacobi_asn() doesn't escape as
/// NaN into a coefficient stream.
inline double carlson_RF(double x, double y, double z) {
    if (x < 0.0) x = 0.0;
    if (y < 0.0) y = 0.0;
    if (z < 0.0) z = 0.0;
    constexpr double kErrTol = 0.0025;  // ~ machine eps^(1/6); NR recommendation.
    for (int i = 0; i < 60; ++i) {
        const double sqrt_x = std::sqrt(x);
        const double sqrt_y = std::sqrt(y);
        const double sqrt_z = std::sqrt(z);
        const double lambda = sqrt_x * sqrt_y + sqrt_y * sqrt_z + sqrt_z * sqrt_x;
        x = 0.25 * (x + lambda);
        y = 0.25 * (y + lambda);
        z = 0.25 * (z + lambda);
        const double avg = (x + y + z) / 3.0;
        const double dx = (avg - x) / avg;
        const double dy = (avg - y) / avg;
        const double dz = (avg - z) / avg;
        const double max_d = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
        if (max_d < kErrTol) {
            const double e2 = dx * dy - dz * dz;
            const double e3 = dx * dy * dz;
            // 7th-order series expansion (NR eq. 6.12.17):
            return (1.0 + e2 * (-0.1 + e2 / 24.0 - 3.0 * e3 / 44.0)
                        + e3 / 14.0) / std::sqrt(avg);
        }
    }
    return 1.0 / std::sqrt((x + y + z) / 3.0);  // fallback
}

/// Inverse Jacobi sn: returns u such that sn(u, m) = x, for x in [-1, 1].
///
/// Implementation: asn(x, m) = x * RF(1 - x^2, 1 - m * x^2, 1). This is
/// the standard Carlson form (Numerical Recipes §6.12), accurate to
/// machine precision across the full m ∈ [0, 1] range. The sign of x
/// carries through naturally since RF is even in its arguments and we
/// only multiply by x.
///
/// For m = 0 this reduces to asin(x); for m = 1 it reduces to atanh(x).
/// Inputs outside [-1, 1] are clamped to keep filter-design code numerically
/// safe rather than throwing or NaN-poisoning the cascade.
inline double jacobi_asn(double x, double m) {
    if (x >  1.0) x =  1.0;
    if (x < -1.0) x = -1.0;
    if (m <= 0.0) return std::asin(x);
    if (m >= 1.0) return std::atanh(x);
    const double y = 1.0 - x * x;
    const double z = 1.0 - m * x * x;
    return x * carlson_RF(y, z, 1.0);
}

}  // namespace special

}  // namespace pulp::signal
