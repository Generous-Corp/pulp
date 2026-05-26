#pragma once

/// @file iir_design.hpp
/// High-order analog-prototype IIR design (Chebyshev Type I, Chebyshev
/// Type II, Elliptic / Cauer) producing biquad cascades (SOS) via the
/// bilinear transform.
///
/// Design math runs in @c double for numerical headroom and emits
/// @c float coefficients compatible with @c FilterDesign::Coefficients
/// so the existing biquad infrastructure consumes the output unchanged.
///
/// References:
///   - A. Antoniou, "Digital Signal Processing: Signals, Systems, and
///     Filters", McGraw-Hill, 2005 — analog prototypes and bilinear.
///   - L. R. Rabiner & B. Gold, "Theory and Application of Digital
///     Signal Processing", Prentice-Hall, 1975.
///   - Audio EQ Cookbook (Robert Bristow-Johnson) — biquad form.
///   - S. J. Orfanidis, "Lecture Notes on Elliptic Filter Design"
///     (2006) — analog elliptic prototype via Jacobi sn / cd.
///   - W. Cody, "Chebyshev approximations for the complete elliptic
///     integrals K and E" (1965) — AGM for K(m).

#include <pulp/signal/filter_design.hpp>
#include <pulp/signal/special_functions.hpp>

#include <cmath>
#include <complex>
#include <vector>
#include <algorithm>

namespace pulp::signal {

/// Chebyshev / Elliptic IIR design utilities.
///
/// @code
/// // 4th-order Chebyshev Type I lowpass, 1 dB passband ripple, 2 kHz cutoff
/// auto cascade = IirDesign::chebyshev1_lowpass(4, 2000.0f, 1.0f, 44100.0f);
/// // -> cascade.size() == 2 biquad sections
///
/// // 4th-order Chebyshev Type II, 40 dB stopband, stop-freq 5 kHz
/// auto cheb2 = IirDesign::chebyshev2_lowpass(4, 5000.0f, 40.0f, 44100.0f);
///
/// // 6th-order Elliptic (Cauer) lowpass, 0.5 dB passband, 60 dB stopband
/// auto cauer = IirDesign::elliptic_lowpass(6, 2000.0f, 0.5f, 60.0f, 48000.0f);
/// @endcode
struct IirDesign {
    using Coefficients = FilterDesign::Coefficients;

    // ── Chebyshev Type I ─────────────────────────────────────────────────

    /// Chebyshev Type I lowpass cascade.
    /// Equiripple in passband, monotonic in stopband.
    ///
    /// @param order        Filter order (>= 2, even). Odd orders are rejected.
    /// @param freq_hz      Passband edge frequency (where ripple ends).
    /// @param ripple_db    Passband ripple in dB (e.g. 0.5, 1.0, 3.0).
    /// @param sample_rate  Sample rate in Hz.
    /// @return order/2 biquad sections; empty if order < 2 or invalid input.
    static std::vector<Coefficients>
    chebyshev1_lowpass(int order, float freq_hz, float ripple_db, float sample_rate) {
        return cheb1_cascade(order, freq_hz, ripple_db, sample_rate, /*highpass=*/false);
    }

    /// Chebyshev Type I highpass cascade.
    static std::vector<Coefficients>
    chebyshev1_highpass(int order, float freq_hz, float ripple_db, float sample_rate) {
        return cheb1_cascade(order, freq_hz, ripple_db, sample_rate, /*highpass=*/true);
    }

    // ── Chebyshev Type II (Inverse Chebyshev) ────────────────────────────

    /// Chebyshev Type II lowpass cascade.
    /// Maximally flat in passband, equiripple in stopband.
    ///
    /// @param order              Filter order (>= 2, even).
    /// @param stop_freq_hz       Stopband edge frequency (where attenuation
    ///                           reaches @p stop_atten_db).
    /// @param stop_atten_db      Stopband attenuation in dB (positive, e.g. 40).
    /// @param sample_rate        Sample rate in Hz.
    static std::vector<Coefficients>
    chebyshev2_lowpass(int order, float stop_freq_hz, float stop_atten_db, float sample_rate) {
        return cheb2_cascade(order, stop_freq_hz, stop_atten_db, sample_rate, /*highpass=*/false);
    }

    /// Chebyshev Type II highpass cascade.
    static std::vector<Coefficients>
    chebyshev2_highpass(int order, float stop_freq_hz, float stop_atten_db, float sample_rate) {
        return cheb2_cascade(order, stop_freq_hz, stop_atten_db, sample_rate, /*highpass=*/true);
    }

    // ── Elliptic (Cauer) ─────────────────────────────────────────────────
    //
    // Equiripple in BOTH passband (ripple_db) and stopband (stopband_attn_db).
    // For a given order it achieves the steepest transition of the four
    // classic designs. Passband edge is at @p freq_hz; the stopband edge is
    // implied by the order and the two ripple specifications via the
    // Cauer degree equation:
    //
    //     N · K(k')·K(k1) = K(k)·K(k1')
    //
    // where k is the filter selectivity (Ω_p/Ω_s) and k1 = ε/√(10^(As/10)−1).
    // We solve for k given N + ripples; the resulting stopband edge is
    // @c freq_hz / k. Callers that need a specific stopband edge should
    // pick the order that places it; @c elliptic_required_order is a
    // helper that returns the minimum order for given specs.

    /// Elliptic (Cauer) lowpass cascade — equiripple in both bands.
    ///
    /// @param order              Filter order (>= 2, even). Odd orders rejected.
    /// @param freq_hz            Passband edge (where ripple ends).
    /// @param passband_ripple_db Passband ripple in dB (positive, e.g. 0.5).
    /// @param stopband_attn_db   Minimum stopband attenuation in dB
    ///                           (positive, e.g. 60).
    /// @param sample_rate        Sample rate in Hz.
    /// @return order/2 biquad sections; empty if inputs invalid.
    static std::vector<Coefficients>
    elliptic_lowpass(int order, float freq_hz,
                     float passband_ripple_db, float stopband_attn_db,
                     float sample_rate) {
        return elliptic_cascade(order, freq_hz, passband_ripple_db,
                                stopband_attn_db, sample_rate,
                                /*highpass=*/false);
    }

    /// Elliptic highpass cascade.
    static std::vector<Coefficients>
    elliptic_highpass(int order, float freq_hz,
                      float passband_ripple_db, float stopband_attn_db,
                      float sample_rate) {
        return elliptic_cascade(order, freq_hz, passband_ripple_db,
                                stopband_attn_db, sample_rate,
                                /*highpass=*/true);
    }

private:
    static constexpr double kPi = 3.14159265358979323846;

    // ── Common: bilinear transform of an SOS section from analog s-plane ─
    //
    // Given an analog biquad H(s) = (B0 s^2 + B1 s + B2) / (A0 s^2 + A1 s + A2)
    // and a sample period T, return the digital biquad. Pre-warping is the
    // caller's responsibility (i.e. analog cutoff already chosen so warped
    // digital cutoff is at the desired Hz).
    static Coefficients bilinear_sos(double B0, double B1, double B2,
                                     double A0, double A1, double A2,
                                     double T) {
        // s = (2/T) * (z-1)/(z+1)
        double K = 2.0 / T;
        double K2 = K * K;
        double a0_d = A0 * K2 + A1 * K + A2;
        double a1_d = 2.0 * (A2 - A0 * K2);
        double a2_d = A0 * K2 - A1 * K + A2;
        double b0_d = B0 * K2 + B1 * K + B2;
        double b1_d = 2.0 * (B2 - B0 * K2);
        double b2_d = B0 * K2 - B1 * K + B2;
        double inv = 1.0 / a0_d;
        return Coefficients{
            static_cast<float>(b0_d * inv),
            static_cast<float>(b1_d * inv),
            static_cast<float>(b2_d * inv),
            static_cast<float>(a1_d * inv),
            static_cast<float>(a2_d * inv),
        };
    }

    static double prewarp_omega(double freq_hz, double sample_rate) {
        double T = 1.0 / sample_rate;
        return (2.0 / T) * std::tan(kPi * freq_hz / sample_rate);
    }

    // ── Chebyshev I poles (analog prototype, unit cutoff) ────────────────
    //
    // For order N and passband ripple R dB,
    //   eps = sqrt(10^(R/10) - 1)
    //   mu  = (1/N) * asinh(1/eps)
    //   s_k = -sinh(mu) * sin(theta_k) + j * cosh(mu) * cos(theta_k)
    //   theta_k = pi * (2k - 1) / (2N), k = 1..N
    static std::vector<std::complex<double>>
    cheb1_prototype_poles(int order, double ripple_db) {
        double eps = std::sqrt(std::pow(10.0, ripple_db / 10.0) - 1.0);
        double mu = std::asinh(1.0 / eps) / static_cast<double>(order);
        double sinh_mu = std::sinh(mu);
        double cosh_mu = std::cosh(mu);

        std::vector<std::complex<double>> poles;
        poles.reserve(static_cast<size_t>(order));
        for (int k = 1; k <= order; ++k) {
            double theta = kPi * (2.0 * k - 1.0) / (2.0 * order);
            poles.emplace_back(-sinh_mu * std::sin(theta),
                                cosh_mu * std::cos(theta));
        }
        return poles;
    }

    static std::vector<Coefficients>
    cheb1_cascade(int order, double freq_hz, double ripple_db,
                  double sample_rate, bool highpass) {
        if (order < 2 || (order & 1) != 0) return {};
        if (freq_hz <= 0.0 || freq_hz >= 0.5 * sample_rate) return {};
        if (ripple_db <= 0.0) return {};

        double T = 1.0 / sample_rate;
        double wc = prewarp_omega(freq_hz, sample_rate);

        auto poles = cheb1_prototype_poles(order, ripple_db);
        int N = order;
        int num_sections = N / 2;

        // DC gain of even-N Cheb I sits at the bottom of the ripple band:
        //   |H(j0)| = 1 / sqrt(1 + eps^2)
        // Apply that scale to the first section's numerator after the per-
        // section gains are normalized to 1.
        double eps = std::sqrt(std::pow(10.0, ripple_db / 10.0) - 1.0);
        double target_dc = 1.0 / std::sqrt(1.0 + eps * eps);  // even N

        // Sort poles by imag part for consistent pairing — purely numerical,
        // doesn't affect overall cascade response.
        std::sort(poles.begin(), poles.end(),
                  [](const std::complex<double>& a, const std::complex<double>& b) {
                      return a.imag() < b.imag();
                  });

        std::vector<Coefficients> result;
        result.reserve(static_cast<size_t>(num_sections));

        for (int sec = 0; sec < num_sections; ++sec) {
            auto p = poles[static_cast<size_t>(sec)];
            // Per pair: denom = (s - p)(s - conj(p)) = s^2 - 2 Re(p) s + |p|^2
            double A0 = 1.0;
            double A1 = -2.0 * p.real();
            double A2 = p.real() * p.real() + p.imag() * p.imag();
            // Lowpass numerator = A2 (so prototype section DC gain = 1).
            double B0 = 0.0, B1 = 0.0, B2 = A2;

            // Frequency-scale prototype (cutoff at omega=1) to actual wc:
            // s -> s/wc, multiplied through to keep A0 normalized.
            A1 *= wc;       A2 *= wc * wc;
            B1 *= wc;       B2 *= wc * wc;

            if (highpass) {
                // HP prototype: 2 zeros at s=0, poles at 1/s_k.
                std::complex<double> p_hp = 1.0 / poles[static_cast<size_t>(sec)];
                A0 = 1.0;
                A1 = -2.0 * p_hp.real() * wc;
                A2 = (p_hp.real() * p_hp.real() + p_hp.imag() * p_hp.imag()) * wc * wc;
                // numer = s^2 (after freq scaling, scaled by wc^0 since prototype
                // was wc-independent for the zeros-at-origin case)
                B0 = 1.0; B1 = 0.0; B2 = 0.0;
            }

            result.push_back(bilinear_sos(B0, B1, B2, A0, A1, A2, T));
        }

        // Apply overall DC/Nyquist gain correction.
        // LP: target |H(z=1)| = 1/sqrt(1+eps^2)
        // HP: target |H(z=-1)| = 1/sqrt(1+eps^2) (symmetric via zeros at origin)
        if (!result.empty()) {
            float scale = static_cast<float>(target_dc);
            result[0].b0 *= scale;
            result[0].b1 *= scale;
            result[0].b2 *= scale;
        }
        return result;
    }

    // ── Chebyshev II (inverse Chebyshev) ─────────────────────────────────
    //
    // Inverse Chebyshev poles + zeros (analog prototype, unit stopband edge):
    //   eps = 1 / sqrt(10^(As/10) - 1)
    //   mu  = (1/N) * asinh(1/eps)
    //   theta_k = pi*(2k-1)/(2N)
    //   pole_k     = 1 / (-sinh(mu) sin(theta_k) + j cosh(mu) cos(theta_k))
    //   zero_k     = j / cos(theta_k)   (pure imag, on the j-axis)
    static std::vector<Coefficients>
    cheb2_cascade(int order, double stop_freq_hz, double stop_atten_db,
                  double sample_rate, bool highpass) {
        if (order < 2 || (order & 1) != 0) return {};
        if (stop_freq_hz <= 0.0 || stop_freq_hz >= 0.5 * sample_rate) return {};
        if (stop_atten_db <= 0.0) return {};

        double T = 1.0 / sample_rate;
        double ws = prewarp_omega(stop_freq_hz, sample_rate);

        double eps = 1.0 / std::sqrt(std::pow(10.0, stop_atten_db / 10.0) - 1.0);
        double mu = std::asinh(1.0 / eps) / static_cast<double>(order);
        double sinh_mu = std::sinh(mu);
        double cosh_mu = std::cosh(mu);

        int N = order;
        int num_sections = N / 2;

        std::vector<std::complex<double>> poles;
        std::vector<std::complex<double>> zeros;
        poles.reserve(static_cast<size_t>(N));
        zeros.reserve(static_cast<size_t>(N));

        for (int k = 1; k <= N; ++k) {
            double theta = kPi * (2.0 * k - 1.0) / (2.0 * N);
            std::complex<double> cheb_pole(-sinh_mu * std::sin(theta),
                                            cosh_mu * std::cos(theta));
            poles.push_back(1.0 / cheb_pole);
            double c = std::cos(theta);
            zeros.emplace_back(0.0, 1.0 / c);
        }

        // Pair conjugates by sorted imag part.
        std::sort(poles.begin(), poles.end(),
                  [](const std::complex<double>& a, const std::complex<double>& b) {
                      return a.imag() < b.imag();
                  });
        std::sort(zeros.begin(), zeros.end(),
                  [](const std::complex<double>& a, const std::complex<double>& b) {
                      return a.imag() < b.imag();
                  });

        std::vector<Coefficients> result;
        result.reserve(static_cast<size_t>(num_sections));

        for (int sec = 0; sec < num_sections; ++sec) {
            auto p = poles[static_cast<size_t>(sec)];
            auto z = zeros[static_cast<size_t>(sec)];
            double A0 = 1.0;
            double A1 = -2.0 * p.real();
            double A2 = p.real() * p.real() + p.imag() * p.imag();
            // Numer: (s - z)(s - conj(z)) = s^2 + |z|^2  (z is pure imag)
            double B0 = 1.0;
            double B1 = 0.0;
            double B2 = z.imag() * z.imag();
            // Normalize section DC gain to 1: multiply numer by (A2 / B2).
            double scale_num = A2 / B2;
            B0 *= scale_num;
            B1 *= scale_num;
            B2 *= scale_num;

            // Frequency-scale prototype (stop edge at omega=1) to actual ws.
            A1 *= ws;       A2 *= ws * ws;
            B1 *= ws;       B2 *= ws * ws;

            if (highpass) {
                // Reciprocate prototype poles+zeros, then freq-scale by ws.
                std::complex<double> p_hp_proto = 1.0 / poles[static_cast<size_t>(sec)];
                std::complex<double> z_hp_proto = 1.0 / zeros[static_cast<size_t>(sec)];
                A0 = 1.0;
                A1 = -2.0 * p_hp_proto.real() * ws;
                A2 = (p_hp_proto.real() * p_hp_proto.real() +
                      p_hp_proto.imag() * p_hp_proto.imag()) * ws * ws;
                double zr = z_hp_proto.real();
                double zi = z_hp_proto.imag();
                B0 = 1.0;
                B1 = -2.0 * zr * ws;
                B2 = (zr * zr + zi * zi) * ws * ws;
                result.push_back(bilinear_sos(B0, B1, B2, A0, A1, A2, T));
                continue;
            }

            result.push_back(bilinear_sos(B0, B1, B2, A0, A1, A2, T));
        }
        return result;
    }

    // ── Elliptic (Cauer) implementation ──────────────────────────────────
    //
    // Real-modulus Jacobi machinery is provided by `special_functions.hpp`
    // (Pulp's shared analytic toolbox, shipped in #2958):
    //   • special::elliptic_K(m)          AGM
    //   • special::jacobi_sncndn(u, m, …) NR §6.12 AGM back-substitution
    //   • special::jacobi_asn(x, m)       Carlson R_F inverse
    //   • special::jacobi_nome(m)         q = exp(−π K'/K)
    //
    // Pole/zero placement transcribes scipy.signal._filter_design.ellipap
    // (BSD-3, reference-grade): the formula collapses the complex
    // Jacobi-cd machinery into real-valued sn/cn/dn at two distinct
    // arguments, yielding bilinear-ready biquad coefficients.

    /// Solve the Cauer degree equation for k (selectivity) given order N
    /// and discrimination factor k1 = ε / sqrt(10^(As/10) − 1).
    ///
    /// The degree equation is
    ///     N · K(k')·K(k1) = K(k)·K(k1')
    /// Equivalently, in terms of the Jacobi nome q = exp(−π K'/K):
    ///     q(k) = q(k1)^(1/N)
    ///
    /// We invert q → k via the theta-function ratio
    ///     k = ϑ₂(q)² / ϑ₃(q)²
    /// where the theta series converge geometrically (q < 1).
    static double elliptic_solve_k(int N, double k1) {
        if (k1 <= 0.0) return 0.0;
        if (k1 >= 1.0) return 1.0 - 1e-15;

        double q1 = special::jacobi_nome(k1 * k1);
        double q  = std::pow(q1, 1.0 / static_cast<double>(N));

        // ϑ₂(q) = 2 q^(1/4) · Σ_{n=0..∞} q^(n(n+1))
        // ϑ₃(q) = 1 + 2 Σ_{n=1..∞} q^(n²)
        double theta2 = 0.0;
        double theta3 = 1.0;
        for (int n = 0; n < 30; ++n) {
            theta2 += std::pow(q, n * (n + 1));
        }
        theta2 *= 2.0 * std::pow(q, 0.25);
        for (int n = 1; n < 30; ++n) {
            theta3 += 2.0 * std::pow(q, static_cast<double>(n) * n);
        }
        double r = theta2 / theta3;
        double k = r * r;
        if (k <= 0.0) k = 1e-15;
        if (k >= 1.0) k = 1.0 - 1e-15;
        return k;
    }

    static std::vector<Coefficients>
    elliptic_cascade(int order, double freq_hz,
                     double passband_ripple_db, double stopband_attn_db,
                     double sample_rate, bool highpass) {
        if (order < 2 || (order & 1) != 0) return {};
        if (freq_hz <= 0.0 || freq_hz >= 0.5 * sample_rate) return {};
        if (passband_ripple_db <= 0.0) return {};
        if (stopband_attn_db <= passband_ripple_db) return {};

        double eps = std::sqrt(std::pow(10.0, passband_ripple_db / 10.0) - 1.0);
        double As_lin = std::sqrt(std::pow(10.0, stopband_attn_db / 10.0) - 1.0);
        double k1 = eps / As_lin;

        // Sanity: k1 must lie in (0, 1).
        if (!(k1 > 0.0 && k1 < 1.0)) return {};

        double k  = elliptic_solve_k(order, k1);
        double m  = k * k;
        double K  = special::elliptic_K(m);

        // Design-parameter v0 (SciPy ellipap convention,
        // _filter_design.py):
        //
        //   v0 = K(m) · arc_jac_sc1(1/ε, k1²) / (N · K(k1²))
        //
        // where arc_jac_sc1(w, k1²) is the *real* inverse Jacobi sc with
        // complementary modulus, defined by w = sc(z, 1 − k1²). It is the
        // imaginary part of the complex sn⁻¹(j·w, k1²) under Jacobi's
        // imaginary transformation:
        //
        //   sn(j·u, m) = j · sc(u, 1 − m)
        //   ⇒ sn⁻¹(j·w, m) = j · sc⁻¹(w, 1 − m)
        //
        // The previous implementation used `jacobi_asn(1/ε, k1²)` directly,
        // but `jacobi_asn` clamps its input to [−1, 1]. For any reasonable
        // ripple spec (Rp < ~3 dB ⇒ ε < 1 ⇒ 1/ε > 1), the input was
        // silently clamped to 1 and v0 collapsed to a constant independent
        // of the user-supplied passband_ripple_db — i.e. the API stopped
        // honoring its main ripple parameter (#2964, Codex 3305447117).
        //
        // The real `sc⁻¹(w, m')` for w ≥ 0, m' ∈ (0,1) can be reduced to a
        // domain-safe asn call. From sc² = sn²/cn² = sn²/(1−sn²):
        //
        //   sn(sc⁻¹(w, m'); m') = w / sqrt(1 + w²)
        //   ⇒ sc⁻¹(w, m') = asn( w / sqrt(1 + w²), m' )
        //
        // The asn argument here is bounded in [0, 1) for all real w, so
        // no clamp is hit and v0 tracks the requested ripple properly.
        double K_k1 = special::elliptic_K(k1 * k1);
        const double inv_eps = 1.0 / eps;
        const double sn_arg = inv_eps / std::sqrt(1.0 + inv_eps * inv_eps);
        const double m_prime = 1.0 - k1 * k1;
        double r = special::jacobi_asn(sn_arg, m_prime);  // sc⁻¹(1/ε, 1−k1²)
        double v0 = (K * r) / (static_cast<double>(order) * K_k1);
        double sv, cv, dv;
        // jacobi_sncndn(u, m, ...): note `m` is the parameter (k²),
        // and for v0 we evaluate at modulus k' (m' = 1 - m).
        special::jacobi_sncndn(v0, 1.0 - m, &sv, &cv, &dv);

        // Per-section pole+zero placement follows the classic Cauer
        // formula (cf. scipy.signal._filter_design.ellipap, BSD-3,
        // and Antoniou ch. 12) so the design is interoperable with a
        // well-trodden reference lane:
        //
        //   For j = 1, 3, 5, …, N−1 (N/2 conjugate pairs):
        //     s, c, d = sn(j·K/N, k), cn(j·K/N, k), dn(j·K/N, k)
        //     ω_z     = 1 / (k · |s|)                     (zero on jω)
        //     p       = −(c·d·sv·cv + j·s·dv) / (1 − (d·sv)²)
        //   where (sv, cv, dv) = (sn, cn, dn)(v0, k').
        //
        // The pole's conjugate p* gives the second member of the pair;
        // we collapse each pair into one biquad section.

        int N = order;
        int L = N / 2;
        double T = 1.0 / sample_rate;
        double wc = prewarp_omega(freq_hz, sample_rate);

        std::vector<Coefficients> result;
        result.reserve(static_cast<size_t>(L));

        for (int idx = 0; idx < L; ++idx) {
            int jpos = 2 * idx + 1;   // 1, 3, 5, …
            double u_k = jpos * K / static_cast<double>(N);

            double s_u, c_u, d_u;
            special::jacobi_sncndn(u_k, m, &s_u, &c_u, &d_u);

            // ── zero on jω axis ──
            double omega_z = 1.0 / (k * std::abs(s_u));

            // ── pole pair ──
            double dsv = d_u * sv;
            double inv = 1.0 / (1.0 - dsv * dsv);
            double p_re = -(c_u * d_u * sv * cv) * inv;
            double p_im = -(s_u * dv) * inv;
            // Take the upper half-plane representative; conjugate
            // contributes the other half of the biquad denominator.
            p_im = std::abs(p_im);

            // Safety: stability requires Re(p) < 0.
            if (p_re >= 0.0) p_re = -std::abs(p_re);

            // Prototype (normalized to passband edge ωp = 1):
            //   pole pair  → (s − p)(s − p*) = s² − 2 Re(p) s + |p|²
            //   zero pair  → (s − jω_z)(s + jω_z) = s² + ω_z²
            double pa1_proto = -2.0 * p_re;
            double pa2_proto = p_re * p_re + p_im * p_im;
            double pb2_proto = omega_z * omega_z;  // pb0 = 1, pb1 = 0

            double A0, A1, A2, B0, B1, B2;
            if (!highpass) {
                // LP: substitute s → s/wc  ⇒  multiply A1,B1 by wc and
                // A2,B2 by wc². Then normalize section DC gain to 1.
                A0 = 1.0;
                A1 = pa1_proto * wc;
                A2 = pa2_proto * wc * wc;
                B0 = 1.0;
                B1 = 0.0;
                B2 = pb2_proto * wc * wc;
                double scale_num = A2 / B2;
                B0 *= scale_num;
                B1 *= scale_num;
                B2 *= scale_num;
            } else {
                // HP: substitute s → wc/s in the prototype
                // (s² + ω_z²) / (s² + a·s + b), giving
                //   (ω_z²·s² + wc²) / (b·s² + a·wc·s + wc²).
                // Normalize cascade-level by dividing each numerator
                // by ω_z²/b so per-section gain at s=∞ becomes 1.
                // Then divide everything by b to get A0=1 form:
                //   A0 = 1
                //   A1 = (-2·Re(p))·wc / |p|²
                //   A2 = wc² / |p|²
                //   B0 = 1
                //   B1 = 0
                //   B2 = wc² / ω_z²
                // At s=0 (DC), section gain = B2·b/wc² = b/ω_z² < 1
                // (DC blocked), product across cascade gives full HP
                // stopband attenuation.
                double pole_mag2 = pa2_proto;  // |p|² in prototype
                A0 = 1.0;
                A1 = (pa1_proto * wc) / pole_mag2;
                A2 = (wc * wc) / pole_mag2;
                B0 = 1.0;
                B1 = 0.0;
                B2 = (wc * wc) / pb2_proto;     // = wc²/ω_z²
            }

            result.push_back(bilinear_sos(B0, B1, B2, A0, A1, A2, T));
        }

        // Overall passband-floor scaling. Even-order elliptic LP has
        //   |H_LP(0)| = 1 / √(1+ε²)
        // (the floor of the ripple band). The HP dual sits at z=−1
        // (Nyquist), where after bilinear the analog s→∞ maps; the
        // section construction above leaves cascade-level analog
        // |H_HP(∞)| = 1 (each section's b0 = 1, leading-order ratio
        // 1/1). To pull the HP passband peaks to 0 dB matching the
        // LP convention, apply the same √(1+ε²) ripple-floor scale.
        if (!result.empty()) {
            double target = 1.0 / std::sqrt(1.0 + eps * eps);
            float scale = static_cast<float>(target);
            result[0].b0 *= scale;
            result[0].b1 *= scale;
            result[0].b2 *= scale;
        }
        return result;
    }
};

// ── Cascade evaluation helpers (utility) ─────────────────────────────────

/// Evaluate the magnitude response of a cascade of biquads at a given
/// normalized digital frequency omega = 2*pi*f/fs. Pure utility — useful
/// for testing and for runtime spectrum-display widgets.
inline double cascade_magnitude(const std::vector<FilterDesign::Coefficients>& sos,
                                double omega) {
    std::complex<double> z = std::polar(1.0, omega);
    std::complex<double> z_inv = 1.0 / z;
    std::complex<double> h(1.0, 0.0);
    for (const auto& c : sos) {
        std::complex<double> num = static_cast<double>(c.b0)
                                 + static_cast<double>(c.b1) * z_inv
                                 + static_cast<double>(c.b2) * z_inv * z_inv;
        std::complex<double> den = 1.0
                                 + static_cast<double>(c.a1) * z_inv
                                 + static_cast<double>(c.a2) * z_inv * z_inv;
        h *= num / den;
    }
    return std::abs(h);
}

/// Returns true if every biquad section's poles lie strictly inside the
/// unit circle (i.e. the filter is BIBO stable).
inline bool cascade_is_stable(const std::vector<FilterDesign::Coefficients>& sos) {
    for (const auto& c : sos) {
        // Poles are roots of z^2 + a1 z + a2 = 0. With real coefficients:
        //   |a2| < 1 AND |a1| < 1 + a2.
        double a1 = c.a1, a2 = c.a2;
        if (!(std::abs(a2) < 1.0 && std::abs(a1) < 1.0 + a2)) return false;
    }
    return true;
}

} // namespace pulp::signal
