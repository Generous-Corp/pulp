// SaturatorT — the memoryless saturation toolkit.
//
// This is the spec's acceptance suite A1–A12 (see
// planning/2026-07-25-dsp-series-round2.md, module M01). Expected harmonic
// values are COMPUTED here from the same closed forms the spec derives them
// from, never restated as literals — so a change to a shipped constant fails
// the test that documents it rather than quietly disagreeing with it.
//
// Measurement recipe: 1 kHz sine at fs = 48 kHz. Harmonic magnitudes come from
// a coherent DFT over an integer number of periods rather than a windowed FFT.
// At 48 kHz a 1 kHz period is exactly 48 samples, so an integer-period window
// has zero spectral leakage and each harmonic's magnitude is exact — no window
// correction factor, no bin-collision reasoning, and no leakage term hiding
// inside the tolerance. The broadband alias-band measurement (A8) sums a
// Hann-windowed DFT over a 50 Hz grid across the band, for the reason given at
// that test.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/saturator.hpp>
#include <pulp/signal/units.hpp>

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kSr = 48000.0;
constexpr double kToneHz = 1000.0;
constexpr int kPeriodSamples = 48;   // exactly fs / kToneHz
constexpr int kPeriods = 1000;       // 1 s of coherent analysis
constexpr int kAnalysisLen = kPeriodSamples * kPeriods;
constexpr int kSettle = 4096;

/// Peak amplitude of the test sine, in full scale. The spec's `A = d · x_peak`
/// is the scale-invariant quantity every harmonic expectation is a function of.
constexpr double kTestPeak = 0.5;

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

/// The drive that puts the shaper's u-domain amplitude at `A` for the standard
/// test tone: `A = d · x_peak`, so `drive_db = 20·log10(A / x_peak)`.
double drive_db_for_amplitude(double a) {
    return 20.0 * std::log10(a / kTestPeak);
}

/// Renders `settle + analysis` samples of a sine through a configured saturator
/// and returns the analysis window.
std::vector<double> render_sine(SaturatorT<double>& sat, double freq_hz, double peak,
                                int analysis_len = kAnalysisLen) {
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(analysis_len));
    const double w = 2.0 * std::numbers::pi * freq_hz / kSr;
    for (int n = 0; n < kSettle + analysis_len; ++n) {
        const double y = sat.process(peak * std::sin(w * n));
        if (n >= kSettle) out.push_back(y);
    }
    return out;
}

/// Coherent DFT magnitude at harmonic `k` of the analysis fundamental. Exact
/// for an integer-period window: no leakage, no window correction.
double harmonic_magnitude(const std::vector<double>& x, int k) {
    const double w = 2.0 * std::numbers::pi * k / kPeriodSamples;
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        re += x[n] * std::cos(w * static_cast<double>(n));
        im += x[n] * std::sin(w * static_cast<double>(n));
    }
    const double scale = 2.0 / static_cast<double>(x.size());
    return std::hypot(re * scale, im * scale);
}

/// Coherent DFT magnitude at an arbitrary frequency, over an analysis window
/// that is an exact whole number of that frequency's periods. Any multiple of
/// `kSr / len` qualifies; at the 4800-sample length used for the tone sweep
/// that is every multiple of 10 Hz, which covers all the sweep points.
///
/// This replaces the obvious-looking "track the peak sample" measurement, which
/// is wrong for a discrete sine: at 8 kHz / 48 kHz there are exactly 6 samples
/// per cycle and none of them lands on the crest, so peak-tracking under-reads
/// by 20·log10(sin(π/3)) = −1.25 dB — an error large enough to look exactly
/// like a filter that is not flat.
double magnitude_at_hz(const std::vector<double>& x, double hz) {
    const double w = 2.0 * std::numbers::pi * hz / kSr;
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        re += x[n] * std::cos(w * static_cast<double>(n));
        im += x[n] * std::sin(w * static_cast<double>(n));
    }
    const double scale = 2.0 / static_cast<double>(x.size());
    return std::hypot(re * scale, im * scale);
}

/// Total harmonic distortion over exactly the harmonics `harmonics` names.
///
/// The set is a parameter rather than "2 through 9" so a measurement can be
/// compared LIKE FOR LIKE against a truncated-series expectation. The spec's
/// Taylor estimates are `√(H3² + H5²)/H1`; measuring 2..9 against that counts
/// 7th and 9th harmonic energy the estimate never claimed to include, which at
/// A = 1 is enough to fail a 15 % tolerance on a shaper that is behaving
/// correctly.
double measured_thd(const std::vector<double>& x, std::initializer_list<int> harmonics) {
    const double h1 = harmonic_magnitude(x, 1);
    double sum_sq = 0.0;
    for (int k : harmonics) {
        const double h = harmonic_magnitude(x, k);
        sum_sq += h * h;
    }
    return std::sqrt(sum_sq) / h1;
}

/// The harmonics the spec's 5th-order Taylor estimates actually include.
double measured_thd_35(const std::vector<double>& x) { return measured_thd(x, {3, 5}); }

// ── The spec's closed forms, transcribed as functions, not as numbers ──────
//
// Every expectation below is evaluated from these at the test's own `A`, so a
// test point can move without anyone hand-editing an expected literal.

/// Cubic soft clip, unbiased. EXACT (the shape is an exact polynomial in the
/// unclipped region, so `sin³θ = (3sinθ − sin3θ)/4` gives a closed form).
double cubic_h1(double a) { return a - a * a * a / 4.0; }
double cubic_h3(double a) { return a * a * a / 12.0; }
double cubic_thd3(double a) { return cubic_h3(a) / cubic_h1(a); }

/// tanh, unbiased — 5th-order Taylor estimate (`u − u³/3 + 2u⁵/15`).
/// An approximation, stated as such; the tolerance below reflects that.
double tanh_thd(double a) {
    const double a3 = a * a * a, a5 = a3 * a * a;
    const double h1 = a - a3 / 4.0 + a5 / 12.0;
    const double h3 = a3 / 12.0 - a5 / 24.0;
    const double h5 = a5 / 120.0;
    return std::sqrt(h3 * h3 + h5 * h5) / h1;
}

/// atan, unbiased — 5th-order Taylor estimate (`u − u³/3 + u⁵/5`).
double atan_thd(double a) {
    const double a3 = a * a * a, a5 = a3 * a * a;
    const double h1 = a - a3 / 4.0 + a5 / 8.0;
    const double h3 = a3 / 12.0 - a5 / 16.0;
    const double h5 = a5 / 80.0;
    return std::sqrt(h3 * h3 + h5 * h5) / h1;
}

/// asinh, unbiased — 5th-order Taylor estimate (`u − u³/6 + 3u⁵/40`).
double asinh_thd(double a) {
    const double a3 = a * a * a, a5 = a3 * a * a;
    const double h1 = a - a3 / 8.0 + 3.0 * a5 / 64.0;
    const double h3 = a3 / 24.0 - 3.0 * a5 / 64.0;
    const double h5 = 3.0 * a5 / 640.0;
    return std::sqrt(h3 * h3 + h5 * h5) / h1;
}

/// tanh, biased — leading-order second-harmonic estimate, `THD2 ≈ (A/2)·tanh(b)`.
double tanh_thd2(double a, double b) { return 0.5 * a * std::tanh(b); }

/// The EXACT magnitude of harmonic `k` of the shipped curve driven by
/// `peak·sin(t)`, by direct quadrature of `(2/2π)∮ shaped(peak·sin t)·sin(kt) dt`.
///
/// This is the ground truth the transcendental shapes are measured against,
/// and it is computed from the shipped `shaped()` itself rather than from a
/// restated number — so it tracks any change to the drive law or the shape
/// functions automatically. Comparing the RENDERED spectrum against this
/// checks the whole per-sample path (drive normalisation, bias construction,
/// alias policy) against pure mathematics.
double exact_harmonic(const SaturatorT<double>& sat, double peak, int k) {
    constexpr int kQuadraturePoints = 200000;
    double sum = 0.0;
    for (int n = 0; n < kQuadraturePoints; ++n) {
        const double t = 2.0 * std::numbers::pi * (n + 0.5) / kQuadraturePoints;
        sum += sat.shaped(peak * std::sin(t)) * std::sin(k * t);
    }
    return std::abs(2.0 * sum / kQuadraturePoints);
}

/// Exact THD over harmonics 3 and 5 — the same set the Taylor estimates cover,
/// so the two can be compared directly.
double exact_thd_35(const SaturatorT<double>& sat, double peak) {
    const double h1 = exact_harmonic(sat, peak, 1);
    const double h3 = exact_harmonic(sat, peak, 3);
    const double h5 = exact_harmonic(sat, peak, 5);
    return std::sqrt(h3 * h3 + h5 * h5) / h1;
}

SaturatorT<double> make_saturator(SaturatorShape shape, double drive_db, double bias = 0.0,
                                  SaturatorAliasPolicy policy = SaturatorAliasPolicy::off) {
    SaturatorT<double> sat;
    sat.prepare(kSr);
    sat.set_shape(shape);
    sat.set_drive_db(drive_db);
    sat.set_bias(bias);
    sat.set_alias_policy(policy);
    return sat;
}

}  // namespace

// ── A1 — cubic soft-clip harmonic ratios (exact closed form) ──────────────

TEST_CASE("A1 cubic soft-clip THD3 matches its exact closed form", "[saturator][harmonics]") {
    for (double a : {0.3, 0.6, 1.0}) {
        auto sat = make_saturator(SaturatorShape::cubic_soft, drive_db_for_amplitude(a));
        const auto out = render_sine(sat, kToneHz, kTestPeak);
        const double measured = harmonic_magnitude(out, 3) / harmonic_magnitude(out, 1);
        // Tolerance covers only numerical error: the closed form is exact for
        // this shape and the DFT window is leakage-free.
        REQUIRE_THAT(measured, WithinRel(cubic_thd3(a), 0.05));
    }
}

// ── A2 / A2b — transcendental shapes against their Taylor estimates ───────

TEST_CASE("A2 / A2b the transcendental shapes match their EXACT harmonic content",
          "[saturator][harmonics]") {
    // Measured against quadrature of the shipped curve, not against the spec's
    // 5th-order Taylor estimates. Those estimates are not usable as acceptance
    // expectations at A = 1: the `atan` and `asinh` series both have radius of
    // convergence 1, so at A = 1 the argument sits exactly on that boundary and
    // the 5th-order truncation errs by +107 % and +294 % respectively. See
    // adjudication A-7 — a ±15 % tolerance against a 294 %-wrong number is not
    // achievable by any correct implementation (series law 6).
    for (auto shape : {SaturatorShape::tanh_soft, SaturatorShape::atan_soft,
                       SaturatorShape::sinh_arc}) {
        for (double a : {0.5, 1.0}) {
            auto sat = make_saturator(shape, drive_db_for_amplitude(a));
            const auto out = render_sine(sat, kToneHz, kTestPeak);
            REQUIRE_THAT(measured_thd_35(out), WithinRel(exact_thd_35(sat, kTestPeak), 0.01));
        }
    }
}

TEST_CASE("A2 the Taylor estimates hold where they are legitimate",
          "[saturator][harmonics]") {
    // The estimates are not useless — they are the design-time sanity check the
    // spec presents them as, and they are accurate at small A. Pinning where
    // they hold keeps them honest instead of quietly dropping them: `tanh` and
    // `atan` agree at A = 0.5 (1.4 % and 3.7 % error against exact), while
    // `asinh` already errs 22 % there and is therefore not asserted at all.
    {
        auto sat = make_saturator(SaturatorShape::tanh_soft, drive_db_for_amplitude(0.5));
        REQUIRE_THAT(exact_thd_35(sat, kTestPeak), WithinRel(tanh_thd(0.5), 0.05));
    }
    {
        auto sat = make_saturator(SaturatorShape::atan_soft, drive_db_for_amplitude(0.5));
        REQUIRE_THAT(exact_thd_35(sat, kTestPeak), WithinRel(atan_thd(0.5), 0.05));
    }
    // And the fact that they break down at A = 1 is itself asserted, so a
    // future revision that "fixes" the tolerance instead of the estimate fails
    // here rather than passing silently.
    {
        auto sat = make_saturator(SaturatorShape::sinh_arc, drive_db_for_amplitude(1.0));
        REQUIRE(exact_thd_35(sat, kTestPeak) > 3.0 * asinh_thd(1.0));
    }
}

TEST_CASE("A2b the cross-shape THD ranking holds at A = 1", "[saturator][harmonics]") {
    // A structural check that survives any one shape's absolute number drifting
    // from its Taylor estimate: softest to hardest is sinh_arc, atan, tanh,
    // cubic. Asserted only at A = 1, the one amplitude where all four shapes
    // have a measured value in this suite.
    const double drive = drive_db_for_amplitude(1.0);
    const auto thd_of = [&](SaturatorShape shape) {
        auto sat = make_saturator(shape, drive);
        return measured_thd_35(render_sine(sat, kToneHz, kTestPeak));
    };
    const double sinh_v = thd_of(SaturatorShape::sinh_arc);
    const double atan_v = thd_of(SaturatorShape::atan_soft);
    const double tanh_v = thd_of(SaturatorShape::tanh_soft);
    const double cubic_v = thd_of(SaturatorShape::cubic_soft);

    REQUIRE(sinh_v < atan_v);
    REQUIRE(atan_v < tanh_v);
    REQUIRE(tanh_v < cubic_v);
}

TEST_CASE("A2 bias raises the second harmonic monotonically", "[saturator][harmonics]") {
    const double a = 0.5;
    const double drive = drive_db_for_amplitude(a);
    double previous = 0.0;
    for (double b : {0.0, 0.1, 0.3}) {
        auto sat = make_saturator(SaturatorShape::tanh_soft, drive, b);
        const auto out = render_sine(sat, kToneHz, kTestPeak);
        const double h2_ratio = harmonic_magnitude(out, 2) / harmonic_magnitude(out, 1);
        REQUIRE(h2_ratio >= previous);
        previous = h2_ratio;
        // ±20 % against the leading-order estimate, which drops every term past
        // x² — loose on purpose, and only meaningful where bias is non-zero.
        if (b > 0.0) REQUIRE_THAT(h2_ratio, WithinRel(tanh_thd2(a, b), 0.20));
    }
}

// ── A3 — DC compensation is exact ─────────────────────────────────────────

TEST_CASE("A3 zero input produces exactly zero output at every bias and drive",
          "[saturator][bias]") {
    // The whole point of normalising by f(b): bias must not hand the rest of
    // the chain a DC step to eat. 4 shapes x 3 drives x 5 biases.
    for (auto shape : {SaturatorShape::tanh_soft, SaturatorShape::atan_soft,
                       SaturatorShape::cubic_soft, SaturatorShape::sinh_arc}) {
        for (double drive : {-12.0, 0.0, 36.0}) {
            for (double bias : {-0.9, -0.3, 0.0, 0.3, 0.9}) {
                auto sat = make_saturator(shape, drive, bias);
                for (int n = 0; n < 4096; ++n) REQUIRE(std::abs(sat.process(0.0)) <= 1e-6);
            }
        }
    }
}

// ── A4 — unbiased spectra are odd-only ────────────────────────────────────

TEST_CASE("A4 unbiased shapes generate no even harmonics", "[saturator][harmonics]") {
    for (auto shape : {SaturatorShape::tanh_soft, SaturatorShape::atan_soft,
                       SaturatorShape::cubic_soft, SaturatorShape::sinh_arc}) {
        auto sat = make_saturator(shape, drive_db_for_amplitude(0.5));
        const auto out = render_sine(sat, kToneHz, kTestPeak);
        const double h1 = harmonic_magnitude(out, 1);
        for (int k : {2, 4, 6}) {
            const double even_db = units::linear_to_db(harmonic_magnitude(out, k) / h1);
            // Honest framing: this is the numerical floor of the measurement,
            // not a proof of exact zero.
            REQUIRE(even_db <= -80.0);
        }
    }
}

// ── A5 — unity small-signal gain: the registry invariant ──────────────────

TEST_CASE("A5 small-signal gain is unity, backing worst_case_gain = 1.0",
          "[saturator][gain]") {
    const double peak = units::db_to_linear(-40.0);
    for (auto shape : {SaturatorShape::tanh_soft, SaturatorShape::atan_soft,
                       SaturatorShape::cubic_soft, SaturatorShape::sinh_arc}) {
        auto sat = make_saturator(shape, SaturatorT<double>::kDriveDbMin);
        const auto out = render_sine(sat, kToneHz, peak);
        const double gain_db = units::linear_to_db(harmonic_magnitude(out, 1) / peak);
        REQUIRE_THAT(gain_db, WithinAbs(0.0, 0.05));
    }
}

TEST_CASE("A5 unbiased gain never exceeds unity anywhere on the drive grid",
          "[saturator][gain]") {
    // The supremum claim for the UNBIASED case, not just the nominal point:
    // every shape is globally compressive past the origin, so 1.0 bounds the
    // output at any drive.
    for (auto shape : {SaturatorShape::tanh_soft, SaturatorShape::atan_soft,
                       SaturatorShape::cubic_soft, SaturatorShape::sinh_arc}) {
        for (double drive : {-12.0, -6.0, 0.0, 12.0, 24.0, 36.0}) {
            auto sat = make_saturator(shape, drive, 0.0);
            REQUIRE_THAT(sat.worst_case_gain(), WithinAbs(1.0, 1e-12));
            for (double x = -1.0; x <= 1.0; x += 0.005)
                REQUIRE(std::abs(sat.shaped(x)) <= std::abs(x) + 1e-9);
        }
    }
}

TEST_CASE("A5 worst_case_gain bounds the biased curve too — and 1.0 does not",
          "[saturator][gain]") {
    // Bias makes the curve EXPANSIVE on the side of the operating point that
    // runs back toward the origin, because the construction normalises by the
    // slope at the operating point and the origin's slope is steeper. A flat
    // "worst case gain = 1.0" registry entry would therefore be wrong, so what
    // is asserted is the computed bound f'(0)/f'(b) = 1/f'(b).
    for (auto shape : {SaturatorShape::tanh_soft, SaturatorShape::atan_soft,
                       SaturatorShape::cubic_soft, SaturatorShape::sinh_arc}) {
        for (double drive : {-12.0, -6.0, 0.0, 12.0, 24.0, 36.0}) {
            for (double bias : {-0.9, -0.5, 0.0, 0.5, 0.9}) {
                auto sat = make_saturator(shape, drive, bias);
                const double bound = sat.worst_case_gain();
                for (double x = -1.0; x <= 1.0; x += 0.005)
                    REQUIRE(std::abs(sat.shaped(x)) <= bound * std::abs(x) + 1e-9);
            }
        }
    }

    // And the bound is tight, not a safe over-estimate: it is the supremum of
    // the curve's SLOPE, which is what a chained gain bound needs. `tanh` at
    // b = 0.9 has f'(b) = sech²(0.9), so the bound is cosh²(0.9) ≈ 2.03, and
    // the slope reaches it where the curve crosses the origin.
    auto sat = make_saturator(SaturatorShape::tanh_soft, 0.0, 0.9);
    const double expected = std::cosh(0.9) * std::cosh(0.9);
    REQUIRE_THAT(sat.worst_case_gain(), WithinRel(expected, 1e-9));

    const double h = 1e-6;
    double max_slope = 0.0;
    double max_chord = 0.0;
    for (double x = -2.0; x <= 2.0; x += 0.0002) {
        max_slope = std::max(max_slope, std::abs((sat.shaped(x + h) - sat.shaped(x - h)) / (2 * h)));
        if (std::abs(x) > 1e-6) max_chord = std::max(max_chord, std::abs(sat.shaped(x) / x));
    }
    REQUIRE_THAT(max_slope, WithinRel(expected, 1e-3));
    REQUIRE(max_slope <= expected + 1e-6);

    // The chord gain is necessarily lower than the slope supremum — a secant
    // cannot be steeper than the steepest tangent it spans — but it still
    // exceeds 1, which is the point: the flat "worst case gain = 1.0" claim
    // really is violated by a biased shaper.
    REQUIRE(max_chord > 1.0);
    REQUIRE(max_chord <= expected + 1e-9);
}

// ── A6 — tone bracketing cancels ──────────────────────────────────────────

TEST_CASE("A6 a tracked tone pair is flat end to end at floor drive",
          "[saturator][tone]") {
    // At the drive floor the nonlinearity is effectively linear, so only the
    // shelf pair's match is under test. An RBJ shelf at −G is the exact
    // reciprocal of the same shelf at +G, so this should be tight.
    // Every sweep point is a multiple of 10 Hz so the 4800-sample analysis
    // window holds a whole number of periods and the DFT below is exact.
    for (double freq : {100.0, 500.0, 1000.0, 3000.0, 8000.0, 15000.0, 20000.0}) {
        SaturatorT<double> sat;
        sat.prepare(kSr);
        sat.set_shape(SaturatorShape::tanh_soft);
        sat.set_drive_db(SaturatorT<double>::kDriveDbMin);
        sat.set_alias_policy(SaturatorAliasPolicy::off);
        sat.set_tone_pre_hz(SaturatorT<double>::kTonePreHzDefault);
        sat.set_tone_tracking(true);

        const double peak = units::db_to_linear(-40.0);
        const auto out = render_sine(sat, freq, peak, 4800);
        REQUIRE_THAT(units::linear_to_db(magnitude_at_hz(out, freq) / peak),
                     WithinAbs(0.0, 0.2));
    }
}

// ── A7 — the two alias policies agree on the audible content ──────────────

TEST_CASE("A7 ADAA and oversample_2x agree below Nyquist/2", "[saturator][aliasing]") {
    const double drive = 24.0;
    auto adaa = make_saturator(SaturatorShape::tanh_soft, drive, 0.0,
                               SaturatorAliasPolicy::adaa);
    auto over = make_saturator(SaturatorShape::tanh_soft, drive, 0.0,
                               SaturatorAliasPolicy::oversample_2x);

    const auto a = render_sine(adaa, kToneHz, kTestPeak);
    const auto o = render_sine(over, kToneHz, kTestPeak);

    for (int k = 1; k <= 5; ++k) {
        const double a_db = units::linear_to_db(harmonic_magnitude(a, k));
        const double o_db = units::linear_to_db(harmonic_magnitude(o, k));
        REQUIRE_THAT(a_db, WithinAbs(o_db, 0.3));
    }
}

// ── A8 — ADAA actually reduces aliasing ───────────────────────────────────

TEST_CASE("A8 ADAA suppresses the aliased image band", "[saturator][aliasing]") {
    // 13 kHz, deliberately NOT 12 kHz: 12 kHz is fs/4, where every odd harmonic
    // folds back onto the fundamental itself and there is no separate image
    // band to measure. At 13 kHz the 7th harmonic (91 kHz) folds to
    // |((91+24) mod 48) − 24| = 5 kHz, squarely inside the measured band and
    // clear of the fundamental.
    // Measured by direct Hann-windowed DFT on a 50 Hz grid across the band,
    // rather than by an FFT. The band is small and the probe runs once per
    // policy, so the cost is irrelevant, and a hand-written correlation has no
    // bin-indexing or scaling convention to get subtly wrong — which matters
    // for a test whose whole job is to distinguish a 30 dB difference from a
    // 0 dB one.
    //
    // The band starts at 200 Hz, not at DC. The Hann window's own skirt around
    // DC is identical for both policies and far larger than the aliased
    // content, so including it makes the ratio ~0 dB for a path that is in
    // fact suppressing aliases by 30 dB. No aliased energy lives below 200 Hz
    // here, so excluding it costs the measurement nothing.
    constexpr int kProbeLen = 32768;

    const auto band_energy = [](SaturatorAliasPolicy policy) {
        auto sat = make_saturator(SaturatorShape::cubic_soft, 36.0, 0.0, policy);
        const auto out = render_sine(sat, 13000.0, kTestPeak, kProbeLen);

        std::vector<double> windowed(kProbeLen);
        for (int n = 0; n < kProbeLen; ++n) {
            const double hann =
                0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * n / (kProbeLen - 1)));
            windowed[static_cast<std::size_t>(n)] = out[static_cast<std::size_t>(n)] * hann;
        }

        double energy = 0.0;
        for (double f = 200.0; f <= 6000.0; f += 50.0) {
            const double w = 2.0 * std::numbers::pi * f / kSr;
            double re = 0.0, im = 0.0;
            for (int n = 0; n < kProbeLen; ++n) {
                re += windowed[static_cast<std::size_t>(n)] * std::cos(w * n);
                im += windowed[static_cast<std::size_t>(n)] * std::sin(w * n);
            }
            const double scale = 1.0 / static_cast<double>(kProbeLen);
            energy += (re * re + im * im) * scale * scale;
        }
        return energy;
    };

    const double raw = band_energy(SaturatorAliasPolicy::off);
    const double antialiased = band_energy(SaturatorAliasPolicy::adaa);
    const double reduction_db = 10.0 * std::log10(raw / antialiased);
    REQUIRE(reduction_db >= 20.0);
}

// ── A9 — determinism ──────────────────────────────────────────────────────

TEST_CASE("A9 render, reset, re-render is bit-identical", "[saturator][determinism]") {
    for (auto shape : {SaturatorShape::tanh_soft, SaturatorShape::atan_soft,
                       SaturatorShape::cubic_soft, SaturatorShape::sinh_arc}) {
        for (auto policy : {SaturatorAliasPolicy::adaa, SaturatorAliasPolicy::oversample_2x,
                            SaturatorAliasPolicy::off}) {
            SaturatorT<double> sat;
            sat.prepare(kSr);
            sat.set_shape(shape);
            sat.set_drive_db(18.0);
            sat.set_bias(0.25);
            sat.set_alias_policy(policy);
            sat.set_tone_pre_hz(SaturatorT<double>::kTonePreHzDefault);
            sat.set_mix(0.75);

            const auto first = render_sine(sat, kToneHz, kTestPeak, 4800);
            sat.reset();
            const auto second = render_sine(sat, kToneHz, kTestPeak, 4800);
            REQUIRE(first.size() == second.size());
            for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(first[i] == second[i]);
        }
    }
}

// ── A11 — latency is reported exactly ─────────────────────────────────────

TEST_CASE("A11 reported latency matches the measured group delay",
          "[saturator][latency]") {
    SaturatorT<double> sat;
    sat.prepare(kSr);
    sat.set_shape(SaturatorShape::tanh_soft);

    sat.set_alias_policy(SaturatorAliasPolicy::adaa);
    REQUIRE(sat.latency_samples() == 0);
    sat.set_alias_policy(SaturatorAliasPolicy::off);
    REQUIRE(sat.latency_samples() == 0);

    sat.set_alias_policy(SaturatorAliasPolicy::oversample_2x);
    const int reported = sat.latency_samples();
    REQUIRE(reported > 0);

    // Confirm the reported number against the actual impulse response, rather
    // than trusting the oversampler's own claim.
    sat.reset();
    int peak_index = 0;
    double peak_value = 0.0;
    for (int n = 0; n < reported * 4; ++n) {
        const double y = sat.process(n == 0 ? 0.5 : 0.0);
        if (std::abs(y) > peak_value) {
            peak_value = std::abs(y);
            peak_index = n;
        }
    }
    REQUIRE(peak_index == reported);
}

TEST_CASE("A11 the dry path is delayed to match the wet path", "[saturator][latency]") {
    // Without this the dry/wet mix becomes a comb filter the moment
    // oversample_2x is selected — a 50 % mix of a signal with its own
    // 64-sample-delayed copy, which is a very audible bug that no harmonic
    // measurement would catch.
    SaturatorT<double> sat;
    sat.prepare(kSr);
    sat.set_shape(SaturatorShape::tanh_soft);
    sat.set_drive_db(SaturatorT<double>::kDriveDbMin);
    sat.set_alias_policy(SaturatorAliasPolicy::oversample_2x);
    sat.set_mix(0.5);

    // At floor drive the wet path is effectively a delayed copy of the dry, so
    // an aligned mix reproduces the input at its latency and a misaligned one
    // would comb. Measure the fundamental: combing at 1 kHz with a 64-sample
    // offset would show up as a large magnitude error.
    const auto out = render_sine(sat, kToneHz, kTestPeak);
    REQUIRE_THAT(units::linear_to_db(harmonic_magnitude(out, 1) / kTestPeak),
                 WithinAbs(0.0, 0.2));
}

// ── A12 — the cubic's bias clamp ──────────────────────────────────────────

TEST_CASE("A12 cubic bias is clamped short of its zero-slope boundary",
          "[saturator][bias]") {
    SaturatorT<double> sat;
    sat.prepare(kSr);
    sat.set_shape(SaturatorShape::cubic_soft);
    sat.set_bias(1.0);
    REQUIRE_THAT(sat.bias(), WithinAbs(SaturatorT<double>::kCubicBiasClamp, 1e-12));
    sat.set_bias(-1.0);
    REQUIRE_THAT(sat.bias(), WithinAbs(-SaturatorT<double>::kCubicBiasClamp, 1e-12));

    // The other three shapes keep their full range — the clamp is specific to
    // the one curve whose slope actually reaches zero.
    for (auto shape : {SaturatorShape::tanh_soft, SaturatorShape::atan_soft,
                       SaturatorShape::sinh_arc}) {
        sat.set_shape(shape);
        sat.set_bias(1.0);
        REQUIRE_THAT(sat.bias(), WithinAbs(1.0, 1e-12));
    }

    // No NaN or Inf anywhere at the boundary, at maximum drive.
    sat.set_shape(SaturatorShape::cubic_soft);
    sat.set_drive_db(SaturatorT<double>::kDriveDbMax);
    sat.set_bias(1.0);
    sat.set_alias_policy(SaturatorAliasPolicy::adaa);
    const auto out = render_sine(sat, kToneHz, 1.0, 4800);
    for (double y : out) REQUIRE(std::isfinite(y));
}

// ── The ADAA antiderivative's parity — the defect this suite exists to catch ──

TEST_CASE("ADAA converges on direct evaluation for a slow signal",
          "[saturator][aliasing]") {
    // ADAA's difference quotient approximates the curve's average over the
    // sample interval. For a signal moving slowly relative to the sample rate
    // that average IS the curve, so the two must agree closely — and they do
    // NOT if the antiderivative has the wrong parity, which is exactly the
    // error the spec's cubic branch invites: `sign(u)·[...]` makes an even
    // function odd, and the resulting output is wrong only on the negative
    // half of every cycle. That looks like distortion, not like a bug.
    for (auto shape : {SaturatorShape::tanh_soft, SaturatorShape::atan_soft,
                       SaturatorShape::cubic_soft, SaturatorShape::sinh_arc}) {
        auto adaa = make_saturator(shape, 12.0, 0.0, SaturatorAliasPolicy::adaa);
        auto direct = make_saturator(shape, 12.0, 0.0, SaturatorAliasPolicy::off);

        // 10 Hz: 4800 samples per cycle, so consecutive samples are very close.
        const double w = 2.0 * std::numbers::pi * 10.0 / kSr;
        double worst = 0.0;
        for (int n = 0; n < 9600; ++n) {
            const double x = 0.9 * std::sin(w * n);
            worst = std::max(worst, std::abs(adaa.process(x) - direct.process(x)));
        }
        REQUIRE(worst < 1e-3);
    }

    // And the same check where it bites hardest: a signal that spends half its
    // time negative, at a drive that pushes the cubic well past its clip point.
    auto adaa = make_saturator(SaturatorShape::cubic_soft, 30.0, 0.0,
                               SaturatorAliasPolicy::adaa);
    auto direct = make_saturator(SaturatorShape::cubic_soft, 30.0, 0.0,
                                 SaturatorAliasPolicy::off);
    const double w = 2.0 * std::numbers::pi * 10.0 / kSr;
    for (int n = 0; n < 9600; ++n) {
        const double x = 0.9 * std::sin(w * n);
        const double a = adaa.process(x);
        const double d = direct.process(x);
        // Same sign, always. A parity error flips one half of the waveform.
        if (std::abs(d) > 1e-6) REQUIRE(a * d > 0.0);
    }
}

// ── A10 — RT allocation probe ─────────────────────────────────────────────

TEST_CASE("A10 the saturator allocates nothing on the audio thread",
          "[saturator][rt-safety]") {
    SaturatorT<float> sat;
    sat.prepare(kSr);
    sat.set_tone_pre_hz(SaturatorT<float>::kTonePreHzDefault);

    require_allocates_no_memory([&] {
        for (int n = 0; n < 10000; ++n) {
            // Parameter churn included: an update path that allocates would be
            // just as fatal as a process path that does.
            sat.set_drive_db(-12.0 + 0.004 * n);
            sat.set_bias(std::sin(0.001 * n));
            if ((n % 1000) == 0) {
                sat.set_shape(static_cast<SaturatorShape>((n / 1000) % 4));
                sat.set_alias_policy(static_cast<SaturatorAliasPolicy>((n / 1000) % 3));
            }
            (void)sat.process(0.5f * std::sin(0.05f * static_cast<float>(n)));
        }
        sat.reset();
    });
}

TEST_CASE("non-finite saturator controls retain every finite setting",
          "[saturator][nan-recovery]") {
    for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity()}) {
        SaturatorT<double> poisoned, reference;
        for (auto* sat : {&poisoned, &reference}) {
            sat->prepare(kSr);
            sat->set_shape(SaturatorShape::cubic_soft);
            sat->set_alias_policy(SaturatorAliasPolicy::oversample_2x);
            sat->set_drive_db(17.0); sat->set_bias(-0.37);
            sat->set_tone_tracking(false); sat->set_tone_pre_hz(2100.0);
            sat->set_tone_de_hz(4700.0); sat->set_pre_boost_db(7.5);
            sat->set_mix(0.63); sat->set_output_trim_db(-4.0);
        }
        poisoned.set_drive_db(bad); poisoned.set_bias(bad);
        poisoned.set_tone_pre_hz(bad); poisoned.set_tone_de_hz(bad);
        poisoned.set_pre_boost_db(bad); poisoned.set_mix(bad);
        poisoned.set_output_trim_db(bad);
        for (int i = 0; i < 512; ++i) {
            const double x = 0.31 * std::sin(0.071 * i);
            REQUIRE(poisoned.process(x) == reference.process(x));
        }
    }
}

TEST_CASE("non-finite saturator audio has exact bounded fresh recovery",
          "[saturator][nan-recovery][rt-safety]") {
    for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity()}) {
        SaturatorT<double> poisoned, fresh;
        for (auto* sat : {&poisoned, &fresh}) {
            sat->prepare(kSr); sat->set_alias_policy(SaturatorAliasPolicy::oversample_2x);
            sat->set_drive_db(22.0); sat->set_bias(0.41);
            sat->set_tone_pre_hz(3300.0); sat->set_pre_boost_db(8.0); sat->set_mix(0.57);
        }
        for (int i = 0; i < 300; ++i) (void)poisoned.process(0.4 * std::sin(0.11 * i));
        require_allocates_no_memory([&] { REQUIRE(poisoned.process(bad) == 0.0); });
        fresh.reset();
        for (int i = 0; i < 512; ++i) {
            const double x = 0.27 * std::sin(0.037 * i);
            REQUIRE(poisoned.process(x) == fresh.process(x));
        }
    }
}
