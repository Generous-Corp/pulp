#pragma once

/// @file elliptic_halfband_iir.hpp
/// Elliptic (Valenzuela-Constantinides) polyphase-allpass half-band filter for
/// 2x oversampling — a variable-order, runtime-designed sibling to
/// `halfband_iir.hpp`'s fixed six-section Vaidyanathan / Regalia-Mitra design.
///
/// Both filters realise the same classical "two parallel allpass paths"
/// half-band decomposition; they differ in *which* elliptic design fills in
/// the allpass coefficients and in how many sections that design needs for a
/// requested transition width / stopband floor. `halfband_iir.hpp` pins a
/// single published six-section design. This header instead runs the
/// Valenzuela-Constantinides elliptic design equations at configure() time,
/// so the section count (and therefore the group delay and stopband) tracks
/// whatever transition width and stopband floor the caller asks for, rather
/// than being pinned to one fixed operating point.
///
/// Group delay is materially different between the two designs at matched
/// operating points: this elliptic design's per-stage schedule (see
/// `OversamplerT::Kind::elliptic_polyphase_iir` in oversampling.hpp) lands
/// close to 0 samples of *extra* passthrough error at the same factor,
/// where the six-section default in `halfband_iir.hpp` differs by more
/// than a sample — neither is wrong, they are simply different points on
/// the latency/transition-width/section-count trade-off, and callers that
/// need this design's specific latency/response pick this one.
///
/// ## Design equations
/// `design_elliptic_halfband_allpass()` solves for the number of allpass
/// sections `N` and their coefficients `a_i` from a normalised transition
/// width (fraction of the input Nyquist) and a stopband floor in dB, via the
/// elliptic-filter degree equation and the Vaidyanathan/Constantinides
/// closed form for equiripple half-band allpass coefficients (Valenzuela &
/// Constantinides, "Digital allpass filter design", and the classical
/// elliptic-rational-function machinery it builds on). All arithmetic is
/// double precision; coefficients are narrowed to `SampleType` only at the
/// end, matching how such designs are computed in every reference
/// implementation of this method.
///
/// ## Section layout
/// The `N` coefficients solved above are split even/odd-indexed into a
/// "direct" path (indices 0, 2, 4, ...) and a "delayed" path (1, 3, 5, ...),
/// each run as a per-sample cascade of first-order allpass sections. This is
/// the same even/odd split `halfband_iir.hpp` calls path A / path B, just
/// with a variable, design-dependent number of sections in each path instead
/// of a fixed six.
///
/// RT contract: `configure()` (and the constructor, which calls it) resize
/// per-section state and allocate. `process()`, `process_block()`, and
/// `reset()` allocate no memory once configured.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace pulp::signal {

namespace detail {

/// Solve the Valenzuela-Constantinides elliptic half-band allpass design for
/// a normalised transition width (fraction of the input Nyquist, i.e. `tw`
/// in `(0, 1)`) and a stopband floor in dB (negative). Returns the flat
/// coefficient array `[direct sections..., delayed sections...]` that
/// `EllipticHalfBandUpsampler2xT` / `EllipticHalfBandDownsampler2xT` expect,
/// i.e. even-indexed `a_i` first, then odd-indexed.
template <typename SampleType>
std::vector<SampleType> design_elliptic_halfband_allpass(double normalised_transition_width,
                                                         double stopband_db) {
    constexpr double pi = 3.14159265358979323846;
    const double wt = 2.0 * pi * normalised_transition_width;
    // pow(10, stopband_db * 0.05) stays finite and nonzero for any stopband_db
    // a caller would realistically pass (it's only exactly 0 in the limit
    // stopband_db -> -inf); computing it directly avoids a prior special
    // case that snapped stopband_gain to a hard 0 at exactly -300 dB, which
    // sent k1 to 0, log(k1*k1/16) to -inf, and n to +inf — UB on the
    // ceil-to-int below for a value the class explicitly documents as a
    // valid stopband floor.
    const double stopband_gain = std::pow(10.0, stopband_db * 0.05);

    const double k = std::pow(std::tan((pi - wt) / 4.0), 2.0);
    const double kp = std::sqrt(1.0 - k * k);
    const double e = (1.0 - std::sqrt(kp)) / (1.0 + std::sqrt(kp)) * 0.5;
    const double q =
        e + 2.0 * std::pow(e, 5.0) + 15.0 * std::pow(e, 9.0) + 150.0 * std::pow(e, 13.0);

    const double k1 = stopband_gain * stopband_gain / (1.0 - stopband_gain * stopband_gain);
    const double raw_n = std::log(k1 * k1 / 16.0) / std::log(q);
    // A caller-supplied stopband_db so deep (or a transition width so close
    // to the Nyquist edge that q rounds to 1) that raw_n isn't finite would
    // otherwise hit the same ceil-to-int UB; clamp to a section count no
    // real design here needs an order of magnitude more of.
    constexpr double kMaxOrder = 4095.0;
    int n = static_cast<int>(std::ceil(std::clamp(raw_n, 1.0, kMaxOrder)));
    if (n % 2 == 0)
        ++n;
    if (n == 1)
        n = 3;

    const int order = (n - 1) / 2;
    std::vector<double> ai(static_cast<std::size_t>(order), 0.0);

    for (int i = 1; i <= order; ++i) {
        double num = 0.0;
        double delta = 1.0;
        int m = 0;
        while (std::abs(delta) > 1e-100) {
            delta = std::pow(-1.0, m) * std::pow(q, static_cast<double>(m) * (m + 1)) *
                    std::sin((2.0 * m + 1) * pi * i / static_cast<double>(n));
            num += delta;
            ++m;
        }
        num *= 2.0 * std::pow(q, 0.25);

        double den = 0.0;
        delta = 1.0;
        m = 1;
        while (std::abs(delta) > 1e-100) {
            delta = std::pow(-1.0, m) * std::pow(q, static_cast<double>(m) * m) *
                    std::cos(m * 2.0 * pi * i / static_cast<double>(n));
            den += delta;
            ++m;
        }
        den = 1.0 + 2.0 * den;

        const double wi = num / den;
        const double api = std::sqrt((1.0 - wi * wi * k) * (1.0 - wi * wi / k)) / (1.0 + wi * wi);
        ai[static_cast<std::size_t>(i - 1)] = (1.0 - api) / (1.0 + api);
    }

    std::vector<SampleType> coeffs;
    coeffs.reserve(static_cast<std::size_t>(order));
    for (int i = 0; i < order; i += 2)
        coeffs.push_back(static_cast<SampleType>(ai[static_cast<std::size_t>(i)]));
    for (int i = 1; i < order; i += 2)
        coeffs.push_back(static_cast<SampleType>(ai[static_cast<std::size_t>(i)]));
    return coeffs;
}

/// Split a flat `[direct..., delayed...]` coefficient array into the two
/// cascades, direct getting the ceil half so odd-order designs bias toward
/// direct having one more section than delayed (matches the even/odd `a_i`
/// index split above: there is always at least as many even indices as odd).
template <typename SampleType>
void split_direct_delayed(const std::vector<SampleType>& coeffs, std::vector<SampleType>& direct,
                          std::vector<SampleType>& delayed) {
    const std::size_t total = coeffs.size();
    const std::size_t delayed_count = total / 2;
    const std::size_t direct_count = total - delayed_count;
    direct.assign(coeffs.begin(), coeffs.begin() + static_cast<std::ptrdiff_t>(direct_count));
    delayed.assign(coeffs.begin() + static_cast<std::ptrdiff_t>(direct_count), coeffs.end());
}

/// One cascade of first-order allpass sections, each realised in the
/// transposed direct form `output = a*in + state; state = in - a*output` —
/// a different state-variable choice than `HalfBandAllpassSectionT`'s
/// direct form I, but the same transfer function per section. Shared by
/// the upsampler's two paths and the downsampler's two paths below.
template <typename SampleType>
SampleType run_allpass_cascade(const std::vector<SampleType>& coeffs,
                               std::vector<SampleType>& state, SampleType x) {
    SampleType in = x;
    for (std::size_t n = 0; n < coeffs.size(); ++n) {
        const SampleType alpha = coeffs[n];
        const SampleType out = alpha * in + state[n];
        state[n] = in - alpha * out;
        in = out;
    }
    return in;
}

} // namespace detail

/// 2x half-band upsampler using the Valenzuela-Constantinides elliptic
/// polyphase-allpass design (see file header). Each call to
/// `process(x, out_lo, out_hi)` consumes one input sample and produces two
/// output samples at twice the rate — same calling convention as
/// `HalfBandUpsampler2xT`, so the two are interchangeable at call sites.
template <typename SampleType = float> class EllipticHalfBandUpsampler2xT {
  public:
    /// Design a fresh network. `normalised_transition_width` is a fraction
    /// of the input Nyquist; `stopband_db` is the target stopband floor
    /// (negative dB). The defaults reproduce the first (tightest) stage of
    /// the `Quality::pristine` schedule in `OversamplerT`: narrow transition,
    /// deep stopband floor.
    explicit EllipticHalfBandUpsampler2xT(double normalised_transition_width = 0.05,
                                          double stopband_db = -90.0) {
        configure(normalised_transition_width, stopband_db);
    }

    /// Re-run the design equations, resizing (and so allocating) the
    /// per-section state to match — as documented, only `configure()` and
    /// the constructor allocate.
    void configure(double normalised_transition_width, double stopband_db) {
        const auto coeffs = detail::design_elliptic_halfband_allpass<SampleType>(
            normalised_transition_width, stopband_db);
        detail::split_direct_delayed(coeffs, direct_, delayed_);
        direct_state_.assign(direct_.size(), SampleType{0});
        delayed_state_.assign(delayed_.size(), SampleType{0});
    }

    /// Number of sections in the direct (even-indexed) path.
    std::size_t direct_sections() const {
        return direct_.size();
    }
    /// Number of sections in the delayed (odd-indexed) path.
    std::size_t delayed_sections() const {
        return delayed_.size();
    }

    // std::fill zeroes in place instead of vector::assign, which is free to
    // reallocate — reset() is documented allocation-free and only ever
    // needs to re-zero state configure() already sized.
    void reset() {
        std::fill(direct_state_.begin(), direct_state_.end(), SampleType{0});
        std::fill(delayed_state_.begin(), delayed_state_.end(), SampleType{0});
    }

    /// Produce two output samples for one input sample. `out_lo` is the
    /// direct-path (even-phase) output, `out_hi` the delayed-path
    /// (odd-phase) output — the same phase convention as
    /// `HalfBandUpsampler2xT::process`.
    void process(SampleType x, SampleType& out_lo, SampleType& out_hi) {
        out_lo = detail::run_allpass_cascade(direct_, direct_state_, x);
        out_hi = detail::run_allpass_cascade(delayed_, delayed_state_, x);
    }

    /// Block helper. `input` length N produces `output` length 2N.
    void process_block(std::span<const SampleType> input, std::span<SampleType> output) {
        const std::size_t n = input.size();
        for (std::size_t i = 0; i < n; ++i)
            process(input[i], output[2 * i], output[2 * i + 1]);
    }

  private:
    std::vector<SampleType> direct_, delayed_;
    std::vector<SampleType> direct_state_, delayed_state_;
};

using EllipticHalfBandUpsampler2x = EllipticHalfBandUpsampler2xT<float>;
using EllipticHalfBandUpsampler2x64 = EllipticHalfBandUpsampler2xT<double>;

/// 2x half-band downsampler using the Valenzuela-Constantinides elliptic
/// polyphase-allpass design. Each call to `process(in_lo, in_hi)` consumes
/// two input samples at the higher rate and emits one decimated output —
/// same calling convention as `HalfBandDownsampler2xT`.
template <typename SampleType = float> class EllipticHalfBandDownsampler2xT {
  public:
    explicit EllipticHalfBandDownsampler2xT(double normalised_transition_width = 0.05,
                                            double stopband_db = -90.0) {
        configure(normalised_transition_width, stopband_db);
    }

    void configure(double normalised_transition_width, double stopband_db) {
        const auto coeffs = detail::design_elliptic_halfband_allpass<SampleType>(
            normalised_transition_width, stopband_db);
        detail::split_direct_delayed(coeffs, direct_, delayed_);
        direct_state_.assign(direct_.size(), SampleType{0});
        delayed_state_.assign(delayed_.size(), SampleType{0});
        holdover_ = SampleType{0};
    }

    std::size_t direct_sections() const {
        return direct_.size();
    }
    std::size_t delayed_sections() const {
        return delayed_.size();
    }

    // std::fill zeroes in place instead of vector::assign, which is free to
    // reallocate — reset() is documented allocation-free and only ever
    // needs to re-zero state configure() already sized.
    void reset() {
        std::fill(direct_state_.begin(), direct_state_.end(), SampleType{0});
        std::fill(delayed_state_.begin(), delayed_state_.end(), SampleType{0});
        holdover_ = SampleType{0};
    }

    /// Consume `in_lo` (even-phase, index 2n) and `in_hi` (odd-phase, index
    /// 2n+1) and return one decimated output sample. The delayed path's
    /// output is held over one sample before being combined with the direct
    /// path's — the explicit `holdover_` register that realises the extra
    /// `z^-1` the decimation polyphase identity needs on that branch.
    SampleType process(SampleType in_lo, SampleType in_hi) {
        const SampleType direct_out =
            detail::run_allpass_cascade(direct_, direct_state_, in_lo);
        const SampleType delayed_out =
            detail::run_allpass_cascade(delayed_, delayed_state_, in_hi);
        const SampleType output = (holdover_ + direct_out) * SampleType{0.5};
        holdover_ = delayed_out;
        return output;
    }

    /// Block helper. `input` length 2N produces `output` length N.
    void process_block(std::span<const SampleType> input, std::span<SampleType> output) {
        const std::size_t n = output.size();
        for (std::size_t i = 0; i < n; ++i)
            output[i] = process(input[2 * i], input[2 * i + 1]);
    }

  private:
    std::vector<SampleType> direct_, delayed_;
    std::vector<SampleType> direct_state_, delayed_state_;
    SampleType holdover_ = SampleType{0};
};

using EllipticHalfBandDownsampler2x = EllipticHalfBandDownsampler2xT<float>;
using EllipticHalfBandDownsampler2x64 = EllipticHalfBandDownsampler2xT<double>;

} // namespace pulp::signal
