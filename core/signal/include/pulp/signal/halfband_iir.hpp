#pragma once

/// @file halfband_iir.hpp
/// Polyphase IIR allpass-network half-band filters for 2x oversampling.
///
/// A half-band filter has its transition centred on a quarter of its own
/// sample rate. The polyphase decomposition expresses an odd-order elliptic
/// half-band lowpass as two parallel allpass paths:
///
///     H(z) = 1/2 * (A(z^2) + z^-1 * B(z^2))
///
/// where each path is a cascade of second-order sections with a single real
/// coefficient `a`:
///
///     S(z) = (a + z^-2) / (1 + a * z^-2)
///
/// run at the sub-rate as the first-order recurrence
///
///     y[n] = a * (x[n] - y[n-1]) + x[n-1]
///
/// which costs one multiply-add pair per section per sub-rate sample.
///
/// ## Design
/// `design_halfband_iir(count, passband_edge)` derives the coefficients in
/// closed form. An odd-order (2N+1) elliptic lowpass whose stopband edge
/// mirrors its passband edge about a quarter of the sample rate is power
/// complementary (|H(w)|^2 + |H(pi - w)|^2 = 1), and after the bilinear map
/// every pole lies on the imaginary axis at +/- j*sqrt(a_i). The pole radii
/// follow from the selectivity k = tan^2(pi * passband_edge), the elliptic
/// nome of the complementary modulus, and the theta-series form of the
/// Jacobi sn function at 2*i*K/(2N+1) — see Valenzuela & Constantinides,
/// "Digital signal processing schemes for efficient interpolation and
/// decimation" (IEE Proc. 1983), and Vaidyanathan, "Multirate Systems and
/// Filter Banks", ch. 5. Ascending coefficients alternate between path A
/// (even indices) and path B (odd indices).
///
/// The response is equiripple in the stopband, monotonic in the passband,
/// and sample-rate-invariant by construction: the same coefficients are
/// correct at 44.1 kHz, 48 kHz, 96 kHz, or any other rate. The stopband
/// floor depends only on the order and the transition width:
///
///     passband edge 0.21 (of the 2x rate):  N=5 -79 dB, N=6 -95 dB,
///                                           N=8 -126 dB
///     passband edge 0.225:                  N=6 -80 dB, N=11 -146 dB
///
/// `halfband_iir_response()`, `halfband_iir_group_delay()`, and
/// `halfband_iir_stopband_db()` evaluate a design without running audio
/// through it.
///
/// ## Upsampling (2x)
/// `HalfBandUpsampler2x::process(x, out_lo, out_hi)` takes one input
/// sample and produces two output samples at twice the rate. Path A
/// becomes the "low" output (the phase aligned with the input), path B
/// the "high" output (the in-between sample). Each path is individually
/// allpass; the half-band response emerges once the two are interleaved
/// into a single 2x-rate stream, where the paths' phase difference is 0 in
/// the passband (samples reinforce) and pi in the stopband (they cancel).
/// This is exactly H(z) applied to the zero-stuffed input with a gain of 2.
///
/// ## Downsampling (2x)
/// `HalfBandDownsampler2x::process(in_lo, in_hi)` consumes two input
/// samples at the higher rate and emits one decimated output: the odd
/// sample drives path A, the even sample path B, and the outputs are
/// averaged. That is H(z) followed by keeping every odd-phase sample, so
/// the decimator leads H's own group delay by one high-rate sample.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <span>
#include <type_traits>
#include <vector>

namespace pulp::signal {

/// One single-coefficient allpass section run at the sub-rate.
///
/// Transfer function at the rate this section is clocked at:
///   H(z) = (a + z^-1) / (1 + a * z^-1)
///
/// Difference equation:
///   y[n] = a * (x[n] - y[n-1]) + x[n-1]
///
/// Each path of the polyphase half-band filter runs at HALF the
/// final sample rate, so this z^-1 delay corresponds to z^-2 at the
/// full (upsampled or pre-decimation) rate — exactly what the
/// classical half-band decomposition
/// `H(z) = 1/2 * (A0(z^2) + z^-1 * A1(z^2))`
/// asks for.
///
/// Stable for |a| < 1.
///
/// RT contract: setters, queries, `process()`, and `reset()` are fixed-state
/// and allocate no memory.
template <typename SampleType = float>
class HalfBandAllpassSectionT {
public:
    HalfBandAllpassSectionT() = default;
    explicit HalfBandAllpassSectionT(SampleType a) : a_(a) {}

    void set_coefficient(SampleType a) { a_ = a; }
    SampleType coefficient() const { return a_; }

    void reset() {
        x_z1_ = SampleType{0.0f};
        y_z1_ = SampleType{0.0f};
    }

    /// Process one sample. Returns y[n].
    SampleType process(SampleType x) {
        const SampleType y = a_ * (x - y_z1_) + x_z1_;
        x_z1_ = x;
        y_z1_ = y;
        return y;
    }

private:
    SampleType a_ = SampleType{0.0f};
    SampleType x_z1_ = SampleType{0.0f};
    SampleType y_z1_ = SampleType{0.0f};
};

using HalfBandAllpassSection = HalfBandAllpassSectionT<float>;
using HalfBandAllpassSection64 = HalfBandAllpassSectionT<double>;

/// Largest coefficient count a `HalfBandIirDesign` carries (eight sections
/// per path). Order 33 already reaches the float noise floor at any useful
/// transition width.
inline constexpr std::size_t kHalfBandIirMaxCoefficients = 16;

/// Coefficients of one elliptic polyphase half-band filter, ascending.
/// Path A takes the even indices and path B the odd ones, so path A is never
/// shorter than path B. `passband_edge` and `stopband_edge()` are in cycles
/// per sample at the filter's own (2x) rate and sum to 0.5.
struct HalfBandIirDesign {
    std::array<double, kHalfBandIirMaxCoefficients> coefficients{};
    std::size_t count = 0;
    double passband_edge = 0.0;

    [[nodiscard]] bool valid() const noexcept {
        return count > 0;
    }
    [[nodiscard]] std::size_t path_a_count() const noexcept {
        return (count + 1) / 2;
    }
    [[nodiscard]] std::size_t path_b_count() const noexcept {
        return count / 2;
    }
    [[nodiscard]] double path_a(std::size_t i) const noexcept {
        return coefficients[2 * i];
    }
    [[nodiscard]] double path_b(std::size_t i) const noexcept {
        return coefficients[2 * i + 1];
    }
    [[nodiscard]] double stopband_edge() const noexcept {
        return 0.5 - passband_edge;
    }
};

/// Closed-form elliptic half-band design (see the file comment).
/// `coefficient_count` is the number of allpass sections across both paths
/// (filter order 2 * count + 1) and `passband_edge` the passband edge in
/// cycles per sample at the filter's own rate, strictly inside (0, 0.25).
/// Out-of-range arguments return an invalid (`count == 0`) design.
/// Not realtime: evaluates trigonometric series; call while configuring.
inline HalfBandIirDesign design_halfband_iir(std::size_t coefficient_count,
                                             double passband_edge) noexcept {
    HalfBandIirDesign design;
    if (coefficient_count == 0 || coefficient_count > kHalfBandIirMaxCoefficients ||
        !(passband_edge > 0.0 && passband_edge < 0.25))
        return design;

    constexpr double kPi = 3.14159265358979323846;
    const double order = static_cast<double>(2 * coefficient_count + 1);
    const double t = std::tan(kPi * passband_edge);
    const double k = t * t; // selectivity: passband over stopband bilinear edge
    const double kc_root = std::sqrt(std::sqrt(1.0 - k * k));
    const double e = 0.5 * (1.0 - kc_root) / (1.0 + kc_root);
    const double e4 = e * e * e * e;
    // Nome of the complementary modulus; the series converges fast because
    // e < 0.5 for every passband edge below a quarter of the rate.
    const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + e4 * 150.0)));

    for (std::size_t i = 1; i <= coefficient_count; ++i) {
        const double angle = kPi * static_cast<double>(i) / order;
        // sqrt(k) * sn(2 i K / order) as a ratio of theta series.
        double numerator = 0.0;
        double sign = 1.0;
        for (int m = 0; m < 32; ++m) {
            const double term = sign * std::pow(q, m * (m + 1)) * std::sin((2 * m + 1) * angle);
            numerator += term;
            if (std::abs(term) < 1e-30)
                break;
            sign = -sign;
        }
        double denominator = 0.0;
        sign = -1.0;
        for (int m = 1; m < 32; ++m) {
            const double term = sign * std::pow(q, m * m) * std::cos(2.0 * m * angle);
            denominator += term;
            if (std::abs(term) < 1e-30)
                break;
            sign = -sign;
        }
        const double w = 2.0 * std::pow(q, 0.25) * numerator / (1.0 + 2.0 * denominator);
        const double w2 = w * w;
        const double x = std::sqrt((1.0 - w2 * k) * (1.0 - w2 / k)) / (1.0 + w2);
        design.coefficients[i - 1] = (1.0 - x) / (1.0 + x);
    }
    std::sort(design.coefficients.begin(),
              design.coefficients.begin() + static_cast<std::ptrdiff_t>(coefficient_count));
    design.count = coefficient_count;
    design.passband_edge = passband_edge;
    return design;
}

namespace detail {

// Phase factor and DC-referenced group delay of one path at `frequency`
// (cycles per sample at the 2x rate). Each section (a + z^-2)/(1 + a z^-2)
// has unit magnitude and group delay 2(1 - a^2)/(1 + 2a cos 2w + a^2).
template <typename Coefficient>
inline void halfband_path_response(const HalfBandIirDesign& design, bool path_a, double frequency,
                                   std::complex<double>& response, double& group_delay) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    const double w = 2.0 * kPi * frequency;
    const std::complex<double> z2(std::cos(2.0 * w), -std::sin(2.0 * w)); // z^-2
    response = 1.0;
    group_delay = 0.0;
    const std::size_t n = path_a ? design.path_a_count() : design.path_b_count();
    for (std::size_t i = 0; i < n; ++i) {
        const double a = static_cast<double>(
            static_cast<Coefficient>(path_a ? design.path_a(i) : design.path_b(i)));
        response *= (a + z2) / (1.0 + a * z2);
        group_delay += 2.0 * (1.0 - a * a) / (1.0 + 2.0 * a * std::cos(2.0 * w) + a * a);
    }
}

} // namespace detail

/// Complex response of `design` at `frequency` cycles per sample of the
/// filter's own rate: 1/2 (A(z^2) + z^-1 B(z^2)). `Coefficient` selects the
/// precision the coefficients are rounded to before evaluation, so a float
/// filter's response includes its coefficient quantization.
template <typename Coefficient = double>
inline std::complex<double> halfband_iir_response(const HalfBandIirDesign& design,
                                                  double frequency) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    std::complex<double> a, b;
    double ta = 0.0, tb = 0.0;
    detail::halfband_path_response<Coefficient>(design, true, frequency, a, ta);
    detail::halfband_path_response<Coefficient>(design, false, frequency, b, tb);
    const double w = 2.0 * kPi * frequency;
    return 0.5 * (a + std::complex<double>(std::cos(w), -std::sin(w)) * b);
}

/// Magnitude of `design` in dB at `frequency` (cycles per sample of the
/// filter's own rate).
template <typename Coefficient = double>
inline double halfband_iir_magnitude_db(const HalfBandIirDesign& design,
                                        double frequency) noexcept {
    return 20.0 * std::log10(std::max(
                      std::abs(halfband_iir_response<Coefficient>(design, frequency)), 1e-300));
}

/// Group delay of H(z) in samples of the filter's own rate. Both paths are
/// allpass, so dP/dw = -j tau_P P exactly and the half-band's delay is
/// Re((tau_A A + (tau_B + 1) z^-1 B) / (A + z^-1 B)) with no numeric
/// differentiation.
template <typename Coefficient = double>
inline double halfband_iir_group_delay(const HalfBandIirDesign& design, double frequency) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    std::complex<double> a, b;
    double ta = 0.0, tb = 0.0;
    detail::halfband_path_response<Coefficient>(design, true, frequency, a, ta);
    detail::halfband_path_response<Coefficient>(design, false, frequency, b, tb);
    const double w = 2.0 * kPi * frequency;
    const std::complex<double> zb = std::complex<double>(std::cos(w), -std::sin(w)) * b;
    return std::real((ta * a + (tb + 1.0) * zb) / (a + zb));
}

/// Worst-case (largest) stopband magnitude in dB over
/// [stopband_edge(), 0.5], sampled densely enough to land on every ripple
/// peak of designs up to `kHalfBandIirMaxCoefficients`.
template <typename Coefficient = double>
inline double halfband_iir_stopband_db(const HalfBandIirDesign& design) noexcept {
    if (!design.valid())
        return 0.0;
    constexpr int kPoints = 4096;
    const double lo = design.stopband_edge();
    double worst = -1e300;
    for (int i = 0; i <= kPoints; ++i) {
        const double f = lo + (0.5 - lo) * static_cast<double>(i) / kPoints;
        worst = std::max(worst, halfband_iir_magnitude_db<Coefficient>(design, f));
    }
    return worst;
}

/// Default half-band design: `design_halfband_iir(6, 0.21)`, path A. Passband
/// flat (droop < 1e-8 dB) to 0.21 of the 2x rate (0.42 of the input rate
/// when upsampling), stopband at or below -94.8 dB from 0.29 of the 2x rate,
/// DC group delay 3.16 samples at the 2x rate. A test pins these literals to
/// the design function.
inline constexpr std::array<float, 3> kDefaultCoefficientsA = {
    0.04536216434896102f,
    0.3371496879790739f,
    0.7080641363635385f,
};

/// Default half-band design, path B. See `kDefaultCoefficientsA`.
inline constexpr std::array<float, 3> kDefaultCoefficientsB = {
    0.16808748123450207f,
    0.5223778543083539f,
    0.8974455911727747f,
};

namespace detail {

template <typename SampleType>
inline std::vector<HalfBandAllpassSectionT<SampleType>> make_sections(
    std::span<const SampleType> coeffs) {
    std::vector<HalfBandAllpassSectionT<SampleType>> out;
    out.reserve(coeffs.size());
    for (SampleType a : coeffs) out.emplace_back(a);
    return out;
}

template <typename SampleType>
inline std::vector<HalfBandAllpassSectionT<SampleType>> make_sections_from_float(
    std::span<const float> coeffs) {
    std::vector<HalfBandAllpassSectionT<SampleType>> out;
    out.reserve(coeffs.size());
    for (float a : coeffs) out.emplace_back(static_cast<SampleType>(a));
    return out;
}

template <typename SampleType>
inline SampleType run_path(std::vector<HalfBandAllpassSectionT<SampleType>>& path,
                           SampleType x) {
    SampleType v = x;
    for (auto& s : path) v = s.process(v);
    return v;
}

// Fixed-storage polyphase half-band stage that filters whole blocks. The
// per-path section counts are template parameters inside the kernels, so the
// coefficients and recurrence state live in registers for the length of a
// block and nothing is zeroed or re-loaded per sample. Each section is
// evaluated as y = a*x + x1 - a*y1, which leaves a single fused multiply-add
// on the y1 recurrence. Up and down directions keep separate state.
template <typename SampleType> class HalfBandIirStage {
  public:
    static constexpr std::size_t kMaxPath = kHalfBandIirMaxCoefficients / 2;

    void configure(const HalfBandIirDesign& design) noexcept {
        count_ = design.count;
        for (std::size_t i = 0; i < kMaxPath; ++i) {
            a_[i] = i < design.path_a_count() ? static_cast<SampleType>(design.path_a(i))
                                              : SampleType{0};
            b_[i] = i < design.path_b_count() ? static_cast<SampleType>(design.path_b(i))
                                              : SampleType{0};
        }
        reset();
    }

    void reset() noexcept {
        up_ = {};
        down_ = {};
    }

    std::size_t coefficient_count() const noexcept {
        return count_;
    }

    /// `n` input samples -> `2n` output samples. `output` must not overlap
    /// `input`.
    void upsample(const SampleType* input, SampleType* output, std::size_t n) noexcept {
        dispatch(count_, [&](auto na, auto nb) {
            upsample_kernel<decltype(na)::value, decltype(nb)::value>(input, output, n);
        });
    }

    /// `2n` input samples -> `n` output samples. `output` may alias `input`.
    void downsample(const SampleType* input, SampleType* output, std::size_t n) noexcept {
        dispatch(count_, [&](auto na, auto nb) {
            downsample_kernel<decltype(na)::value, decltype(nb)::value>(input, output, n);
        });
    }

  private:
    struct PathState {
        std::array<SampleType, kMaxPath> x{};
        std::array<SampleType, kMaxPath> y{};
    };
    struct DirectionState {
        PathState a;
        PathState b;
    };

    std::array<SampleType, kMaxPath> a_{};
    std::array<SampleType, kMaxPath> b_{};
    DirectionState up_;
    DirectionState down_;
    std::size_t count_ = 0;

    template <std::size_t N> using Count = std::integral_constant<std::size_t, N>;

    template <typename Fn> static void dispatch(std::size_t count, Fn&& fn) noexcept {
        switch (count) {
        case 1:
            fn(Count<1>{}, Count<0>{});
            break;
        case 2:
            fn(Count<1>{}, Count<1>{});
            break;
        case 3:
            fn(Count<2>{}, Count<1>{});
            break;
        case 4:
            fn(Count<2>{}, Count<2>{});
            break;
        case 5:
            fn(Count<3>{}, Count<2>{});
            break;
        case 6:
            fn(Count<3>{}, Count<3>{});
            break;
        case 7:
            fn(Count<4>{}, Count<3>{});
            break;
        case 8:
            fn(Count<4>{}, Count<4>{});
            break;
        case 9:
            fn(Count<5>{}, Count<4>{});
            break;
        case 10:
            fn(Count<5>{}, Count<5>{});
            break;
        case 11:
            fn(Count<6>{}, Count<5>{});
            break;
        case 12:
            fn(Count<6>{}, Count<6>{});
            break;
        case 13:
            fn(Count<7>{}, Count<6>{});
            break;
        case 14:
            fn(Count<7>{}, Count<7>{});
            break;
        case 15:
            fn(Count<8>{}, Count<7>{});
            break;
        case 16:
            fn(Count<8>{}, Count<8>{});
            break;
        default:
            break;
        }
    }

    template <std::size_t N>
    static SampleType run(const std::array<SampleType, kMaxPath>& c, SampleType* x, SampleType* y,
                          SampleType v) noexcept {
        for (std::size_t k = 0; k < N; ++k) {
            const SampleType out = c[k] * v + x[k] - c[k] * y[k];
            x[k] = v;
            y[k] = out;
            v = out;
        }
        return v;
    }

    template <std::size_t NA, std::size_t NB>
    void upsample_kernel(const SampleType* input, SampleType* output, std::size_t n) noexcept {
        SampleType xa[NA > 0 ? NA : 1], ya[NA > 0 ? NA : 1];
        SampleType xb[NB > 0 ? NB : 1], yb[NB > 0 ? NB : 1];
        for (std::size_t k = 0; k < NA; ++k) {
            xa[k] = up_.a.x[k];
            ya[k] = up_.a.y[k];
        }
        for (std::size_t k = 0; k < NB; ++k) {
            xb[k] = up_.b.x[k];
            yb[k] = up_.b.y[k];
        }
        for (std::size_t i = 0; i < n; ++i) {
            const SampleType v = input[i];
            output[2 * i] = run<NA>(a_, xa, ya, v);
            output[2 * i + 1] = run<NB>(b_, xb, yb, v);
        }
        for (std::size_t k = 0; k < NA; ++k) {
            up_.a.x[k] = xa[k];
            up_.a.y[k] = ya[k];
        }
        for (std::size_t k = 0; k < NB; ++k) {
            up_.b.x[k] = xb[k];
            up_.b.y[k] = yb[k];
        }
    }

    template <std::size_t NA, std::size_t NB>
    void downsample_kernel(const SampleType* input, SampleType* output, std::size_t n) noexcept {
        SampleType xa[NA > 0 ? NA : 1], ya[NA > 0 ? NA : 1];
        SampleType xb[NB > 0 ? NB : 1], yb[NB > 0 ? NB : 1];
        for (std::size_t k = 0; k < NA; ++k) {
            xa[k] = down_.a.x[k];
            ya[k] = down_.a.y[k];
        }
        for (std::size_t k = 0; k < NB; ++k) {
            xb[k] = down_.b.x[k];
            yb[k] = down_.b.y[k];
        }
        for (std::size_t i = 0; i < n; ++i) {
            const SampleType even = input[2 * i];
            const SampleType odd = input[2 * i + 1];
            const SampleType pa = run<NA>(a_, xa, ya, odd);
            const SampleType pb = run<NB>(b_, xb, yb, even);
            output[i] = SampleType{0.5f} * (pa + pb);
        }
        for (std::size_t k = 0; k < NA; ++k) {
            down_.a.x[k] = xa[k];
            down_.a.y[k] = ya[k];
        }
        for (std::size_t k = 0; k < NB; ++k) {
            down_.b.x[k] = xb[k];
            down_.b.y[k] = yb[k];
        }
    }
};

} // namespace detail

/// 2x half-band upsampler.
///
/// Each call to `process(x, out_lo, out_hi)` consumes one input
/// sample and produces two output samples at twice the rate. The two
/// polyphase outputs are interleaved as (out_lo, out_hi) — out_lo is
/// time-aligned with x (after the filter's group delay), out_hi is
/// the new sample inserted between successive inputs.
///
/// RT contract: constructors allocate section storage. `sections_*()`,
/// `process()`, `process_block()`, and `reset()` allocate no memory after
/// construction.
template <typename SampleType = float>
class HalfBandUpsampler2xT {
public:
    HalfBandUpsampler2xT()
        : path_a_(detail::make_sections_from_float<SampleType>(
              std::span<const float>(kDefaultCoefficientsA))),
          path_b_(detail::make_sections_from_float<SampleType>(
              std::span<const float>(kDefaultCoefficientsB))) {}

    HalfBandUpsampler2xT(std::span<const SampleType> coeffs_a,
                         std::span<const SampleType> coeffs_b)
        : path_a_(detail::make_sections<SampleType>(coeffs_a)),
          path_b_(detail::make_sections<SampleType>(coeffs_b)) {}

    /// Number of allpass sections in path A.
    std::size_t sections_a() const { return path_a_.size(); }
    /// Number of allpass sections in path B.
    std::size_t sections_b() const { return path_b_.size(); }

    void reset() {
        for (auto& s : path_a_) s.reset();
        for (auto& s : path_b_) s.reset();
    }

    /// Produce two output samples for one input sample.
    ///
    /// out_lo is the polyphase even-phase output (time-aligned with
    /// the input after the cascade's group delay); out_hi is the new
    /// odd-phase sample inserted between successive inputs. Each path
    /// is individually allpass (unity magnitude at every frequency);
    /// the half-band response emerges when the two outputs are
    /// interleaved into a single 2x-rate stream — the path phase
    /// difference is 0 in the passband (samples reinforce) and pi in
    /// the stopband (samples cancel).
    void process(SampleType x, SampleType& out_lo, SampleType& out_hi) {
        const SampleType a = detail::run_path(path_a_, x);
        const SampleType b = detail::run_path(path_b_, x);
        out_lo = a;
        out_hi = b;
    }

    /// Block helper. `input` length N produces `output` length 2N.
    void process_block(std::span<const SampleType> input,
                       std::span<SampleType> output) {
        const std::size_t n = input.size();
        for (std::size_t i = 0; i < n; ++i) {
            process(input[i], output[2 * i], output[2 * i + 1]);
        }
    }

private:
    std::vector<HalfBandAllpassSectionT<SampleType>> path_a_;
    std::vector<HalfBandAllpassSectionT<SampleType>> path_b_;
};

using HalfBandUpsampler2x = HalfBandUpsampler2xT<float>;
using HalfBandUpsampler2x64 = HalfBandUpsampler2xT<double>;

/// 2x half-band downsampler.
///
/// Each call to `process(in_lo, in_hi)` consumes two input samples
/// at the higher rate and emits one decimated output. The odd-phase
/// input (`in_hi`) drives path A, the even-phase (`in_lo`) drives
/// path B. Content in the stopband (above 0.29 of the input rate with
/// the default coefficients) aliases into the output at or below the
/// design's stopband floor (-94.8 dB with the default coefficients).
///
/// RT contract: constructors allocate section storage. `sections_*()`,
/// `process()`, `process_block()`, and `reset()` allocate no memory after
/// construction.
template <typename SampleType = float>
class HalfBandDownsampler2xT {
public:
    HalfBandDownsampler2xT()
        : path_a_(detail::make_sections_from_float<SampleType>(
              std::span<const float>(kDefaultCoefficientsA))),
          path_b_(detail::make_sections_from_float<SampleType>(
              std::span<const float>(kDefaultCoefficientsB))) {}

    HalfBandDownsampler2xT(std::span<const SampleType> coeffs_a,
                           std::span<const SampleType> coeffs_b)
        : path_a_(detail::make_sections<SampleType>(coeffs_a)),
          path_b_(detail::make_sections<SampleType>(coeffs_b)) {}

    std::size_t sections_a() const { return path_a_.size(); }
    std::size_t sections_b() const { return path_b_.size(); }

    void reset() {
        for (auto& s : path_a_) s.reset();
        for (auto& s : path_b_) s.reset();
    }

    /// Consume two input samples (lo = first / even-phase, hi =
    /// second / odd-phase) and return one decimated output sample.
    ///
    /// `in_lo` corresponds to the input-stream sample at index 2n
    /// and `in_hi` to index 2n+1, i.e. you feed pairs in chronological
    /// order. The half-band-polyphase form delays the second sample
    /// by one full-rate sample on its way into the path-with-the-
    /// "lower-frequency" allpass coefficients, so the wiring runs
    /// `in_hi → path_a_`, `in_lo → path_b_` to land at the correct
    /// `z^-1` alignment for the decimation polyphase identity.
    SampleType process(SampleType in_lo, SampleType in_hi) {
        const SampleType a = detail::run_path(path_a_, in_hi);
        const SampleType b = detail::run_path(path_b_, in_lo);
        return SampleType{0.5f} * (a + b);
    }

    /// Block helper. `input` length 2N produces `output` length N.
    void process_block(std::span<const SampleType> input,
                       std::span<SampleType> output) {
        const std::size_t n = output.size();
        for (std::size_t i = 0; i < n; ++i) {
            output[i] = process(input[2 * i], input[2 * i + 1]);
        }
    }

private:
    std::vector<HalfBandAllpassSectionT<SampleType>> path_a_;
    std::vector<HalfBandAllpassSectionT<SampleType>> path_b_;
};

using HalfBandDownsampler2x = HalfBandDownsampler2xT<float>;
using HalfBandDownsampler2x64 = HalfBandDownsampler2xT<double>;

} // namespace pulp::signal
