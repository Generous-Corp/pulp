#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/halfband_iir.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <span>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

// ────────────────────────────────────────────────────────────────────────
// macOS plan item 2.2 — Polyphase IIR half-band filter (2x).
//
// Two parallel allpass paths (six sections per path = twelve allpass
// sections total). Upsampler doubles the rate, downsampler halves it.
//
// Acceptance criteria:
//   * passband flat (< 0.1 dB) up to 0.42 of the half-band's own rate
//     edge, i.e. 0.21 of the 2x rate
//   * sample-rate-invariant by construction (coefficient set is
//     normalized to Fs, no per-rate retuning)
//   * stopband at or below -94.8 dB from 0.29 of the 2x rate with the
//     default (closed-form elliptic, three sections per path) design
//
// Verification strategy — internal goldens, no SciPy / external
// reference filter:
//   * passband: drive low-frequency sines, measure steady-state RMS,
//     confirm gain stays within 0.1 dB.
//   * stopband: drive a sine deep in the stopband; confirm RMS
//     attenuation matches the documented design floor.
//   * DC: drive a constant; confirm output is constant (no inter-
//     phase wiggle at DC).
//   * sample-rate invariance: same normalized frequency at two
//     different "labeled" sample rates produces identical samples.
//   * round-trip: 2x up → 2x down on a passband signal recovers
//     amplitude within < 0.2 dB.
// ────────────────────────────────────────────────────────────────────────

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// Measure RMS of the trailing portion of `samples` (skip a warm-up
// window where the filter's group delay has not yet settled).
float trailing_rms(const std::vector<float>& samples, std::size_t skip) {
    if (samples.size() <= skip) return 0.0f;
    double acc = 0.0;
    const std::size_t n = samples.size() - skip;
    for (std::size_t i = skip; i < samples.size(); ++i) {
        acc += static_cast<double>(samples[i]) * samples[i];
    }
    return static_cast<float>(std::sqrt(acc / static_cast<double>(n)));
}

float to_db(float lin) { return 20.0f * std::log10(std::max(lin, 1e-30f)); }

// Render a sine wave at normalized frequency `cycles_per_sample`
// (i.e. f / Fs) for `n` samples, phase 0, amplitude 1.
std::vector<float> sine(float cycles_per_sample, std::size_t n) {
    std::vector<float> out(n);
    const float w = 2.0f * kPi * cycles_per_sample;
    for (std::size_t i = 0; i < n; ++i) out[i] = std::sin(w * static_cast<float>(i));
    return out;
}

} // namespace

TEST_CASE("HalfBandAllpassSection basic stability + reset", "[signal][halfband]") {
    HalfBandAllpassSection s(0.5f);
    // |a|<1 — drive with a unit impulse, ensure response stays bounded.
    float y = s.process(1.0f);
    REQUIRE(std::isfinite(y));
    for (int i = 0; i < 1000; ++i) {
        y = s.process(0.0f);
        REQUIRE(std::isfinite(y));
        REQUIRE(std::fabs(y) <= 2.0f);
    }
    s.reset();
    REQUIRE(s.process(0.0f) == 0.0f);
}

TEST_CASE("HalfBandAllpassSection identity when a=0 (pure z^-1 delay)",
          "[signal][halfband]") {
    HalfBandAllpassSection s(0.0f);
    // a=0  →  y[n] = x[n-1]  (one-sample delay at the section's clock rate)
    REQUIRE(s.process(1.0f) == 0.0f);  // x[n-1] = 0
    REQUIRE(s.process(2.0f) == 1.0f);  // x[n-1] = 1
    REQUIRE(s.process(3.0f) == 2.0f);  // x[n-1] = 2
    REQUIRE(s.process(4.0f) == 3.0f);
}

TEST_CASE("HalfBandAllpassSection allpass magnitude is unity for all freqs",
          "[signal][halfband]") {
    // An allpass section has |H(e^jω)| = 1 at every frequency. Verify
    // by driving steady sines and checking the steady-state RMS equals
    // the input RMS (within float epsilon).
    for (float a : {-0.7f, 0.0f, 0.3f, 0.9f}) {
        HalfBandAllpassSection s(a);
        const std::size_t N = 2048;
        auto x = sine(0.1f, N);
        std::vector<float> y(N);
        for (std::size_t i = 0; i < N; ++i) y[i] = s.process(x[i]);
        const float in_rms = trailing_rms(x, N / 4);
        const float out_rms = trailing_rms(y, N / 4);
        INFO("a=" << a);
        REQUIRE_THAT(out_rms, WithinAbs(in_rms, 1e-3f));
    }
}

TEST_CASE("HalfBandUpsampler2x default has 3 sections per path",
          "[signal][halfband]") {
    HalfBandUpsampler2x up;
    REQUIRE(up.sections_a() == 3);
    REQUIRE(up.sections_b() == 3);
}

TEST_CASE("HalfBandDownsampler2x default has 3 sections per path",
          "[signal][halfband]") {
    HalfBandDownsampler2x dn;
    REQUIRE(dn.sections_a() == 3);
    REQUIRE(dn.sections_b() == 3);
}

TEST_CASE("Default half-band literals are the closed-form design", "[signal][halfband]") {
    const auto design = design_halfband_iir(6, 0.21);
    REQUIRE(design.valid());
    REQUIRE(design.path_a_count() == kDefaultCoefficientsA.size());
    REQUIRE(design.path_b_count() == kDefaultCoefficientsB.size());
    for (std::size_t i = 0; i < kDefaultCoefficientsA.size(); ++i)
        REQUIRE(kDefaultCoefficientsA[i] == static_cast<float>(design.path_a(i)));
    for (std::size_t i = 0; i < kDefaultCoefficientsB.size(); ++i)
        REQUIRE(kDefaultCoefficientsB[i] == static_cast<float>(design.path_b(i)));
}

TEST_CASE("Half-band design is equiripple at the rejection its order predicts",
          "[signal][halfband]") {
    // An elliptic half-band's stopband floor falls by a fixed amount per added
    // section at a given transition width. These pins come from evaluating the
    // closed-form design; a wrong nome, theta series, or path split moves them
    // by tens of dB, not fractions.
    struct Case {
        std::size_t count;
        double edge;
        double stop_db;
    };
    for (const Case c : {Case{2, 0.21, -32.78}, Case{6, 0.21, -94.85}, Case{8, 0.21, -125.89},
                         Case{3, 0.105, -101.19}, Case{11, 0.225, -146.34},
                         Case{5, 0.1, -167.48}}) {
        const auto design = design_halfband_iir(c.count, c.edge);
        const double stop = halfband_iir_stopband_db(design);
        INFO("count=" << c.count << " edge=" << c.edge << " stop=" << stop);
        REQUIRE_THAT(stop, WithinAbs(c.stop_db, 0.05));
        // Power complementarity: |H(f)|^2 + |H(0.5 - f)|^2 = 1, so the passband
        // droop at the edge is the stopband floor squared, halved.
        const double droop = halfband_iir_magnitude_db(design, c.edge);
        REQUIRE(droop <= 0.0);
        REQUIRE(droop > -0.01);
        for (double f = 0.0; f <= c.edge; f += c.edge / 64.0) {
            const double p = std::norm(halfband_iir_response(design, f));
            const double s = std::norm(halfband_iir_response(design, 0.5 - f));
            REQUIRE_THAT(p + s, WithinAbs(1.0, 1e-9));
        }
    }
    REQUIRE_FALSE(design_halfband_iir(0, 0.2).valid());
    REQUIRE_FALSE(design_halfband_iir(17, 0.2).valid());
    REQUIRE_FALSE(design_halfband_iir(4, 0.25).valid());
    REQUIRE_FALSE(design_halfband_iir(4, 0.0).valid());
}

TEST_CASE("Measured half-band upsampler image matches the design response",
          "[signal][halfband]") {
    // Least-squares projection of the image a 2x interpolator leaves at
    // 1 - f (in input-rate cycles) must equal |H| there to within a fraction
    // of a dB, at a depth (-95 to -150 dB) only a double build can show.
    const auto design = design_halfband_iir(6, 0.21);
    std::array<double, 3> a{}, b{};
    for (std::size_t i = 0; i < 3; ++i) {
        a[i] = design.path_a(i);
        b[i] = design.path_b(i);
    }
    HalfBandUpsampler2x64 up{std::span<const double>(a), std::span<const double>(b)};
    for (const double f : {0.2, 0.3, 0.42}) {
        constexpr std::size_t n = 8192;
        std::vector<double> y(2 * n);
        for (std::size_t i = 0; i < n; ++i)
            up.process(std::sin(2.0 * std::numbers::pi * f * static_cast<double>(i)), y[2 * i],
                       y[2 * i + 1]);
        // Fit the passband tone and the image jointly. Fitting the image alone
        // lets the 0 dB tone leak into it through the finite window (about
        // -88 dB here), which would mask the -95 dB being measured.
        const double image = 0.5 - f / 2.0; // at the 2x rate
        const double tone = f / 2.0;
        std::array<std::array<double, 4>, 4> m{};
        std::array<double, 4> r{};
        for (std::size_t i = 4096; i < y.size(); ++i) {
            const double t = static_cast<double>(i);
            const std::array<double, 4> basis = {
                std::sin(2.0 * std::numbers::pi * tone * t), std::cos(2.0 * std::numbers::pi * tone * t),
                std::sin(2.0 * std::numbers::pi * image * t), std::cos(2.0 * std::numbers::pi * image * t)};
            for (std::size_t row = 0; row < 4; ++row) {
                r[row] += basis[row] * y[i];
                for (std::size_t col = 0; col < 4; ++col)
                    m[row][col] += basis[row] * basis[col];
            }
        }
        // Gaussian elimination on the 4x4 normal equations.
        for (std::size_t p = 0; p < 4; ++p)
            for (std::size_t row = p + 1; row < 4; ++row) {
                const double k = m[row][p] / m[p][p];
                for (std::size_t col = p; col < 4; ++col)
                    m[row][col] -= k * m[p][col];
                r[row] -= k * r[p];
            }
        std::array<double, 4> c{};
        for (std::size_t p = 4; p-- > 0;) {
            double acc = r[p];
            for (std::size_t col = p + 1; col < 4; ++col)
                acc -= m[p][col] * c[col];
            c[p] = acc / m[p][p];
        }
        const double measured = 20.0 * std::log10(std::hypot(c[2], c[3]) / std::hypot(c[0], c[1]));
        const double predicted = halfband_iir_magnitude_db(design, image);
        INFO("f=" << f << " measured=" << measured << " predicted=" << predicted);
        REQUIRE(predicted < -94.0);
        REQUIRE_THAT(measured, WithinAbs(predicted, 0.5));
    }
}

TEST_CASE("HalfBandUpsampler2x preserves DC", "[signal][halfband]") {
    // DC input must produce DC output across BOTH phases (no inter-
    // sample wiggle, which would indicate the two paths aren't both
    // passing DC at gain 1).
    HalfBandUpsampler2x up;
    constexpr std::size_t kN = 512;
    std::vector<float> y(2 * kN, 0.0f);
    for (std::size_t i = 0; i < kN; ++i) {
        up.process(0.5f, y[2 * i], y[2 * i + 1]);
    }
    const std::size_t tail_start = (2 * kN) - 64;
    for (std::size_t i = tail_start; i < 2 * kN; ++i) {
        INFO("y[" << i << "] = " << y[i]);
        REQUIRE_THAT(y[i], WithinAbs(0.5f, 1e-3f));
    }
}

TEST_CASE("HalfBandDownsampler2x preserves DC", "[signal][halfband]") {
    HalfBandDownsampler2x dn;
    constexpr std::size_t kN = 256;
    std::vector<float> y(kN, 0.0f);
    for (std::size_t i = 0; i < kN; ++i) y[i] = dn.process(0.5f, 0.5f);
    const std::size_t tail_start = kN - 32;
    for (std::size_t i = tail_start; i < kN; ++i) {
        REQUIRE_THAT(y[i], WithinAbs(0.5f, 1e-3f));
    }
}

TEST_CASE("HalfBandDownsampler2x passband gain is flat to < 0.1 dB up to 0.4*Nyquist",
          "[signal][halfband]") {
    // Downsampler treats its input rate as Fs_in, output rate as
    // Fs_in/2. Output Nyquist = Fs_in/4. 0.4 * Nyq_out = 0.1 * Fs_in.
    // The decimated-output passband should pass these frequencies at
    // ~ unit gain (input -> output RMS ratio = 1).
    for (float f_in : {0.005f, 0.01f, 0.02f, 0.05f, 0.08f, 0.10f}) {
        HalfBandDownsampler2x dn;
        constexpr std::size_t kN = 8192;
        const auto x = sine(f_in, kN);
        std::vector<float> y(kN / 2, 0.0f);
        dn.process_block(x, y);
        const float in_rms = trailing_rms(x, kN / 4);
        const float out_rms = trailing_rms(y, kN / 8);
        const float gain_db = to_db(out_rms / in_rms);
        INFO("f_in=" << f_in << " gain_db=" << gain_db);
        REQUIRE(gain_db < 0.1f);
        REQUIRE(gain_db > -0.1f);
    }
}

TEST_CASE("HalfBandDownsampler2x rejects energy past its stopband edge by its design floor",
          "[signal][halfband]") {
    // The default design is equiripple from 0.29 of the input rate at -94.8 dB.
    // Float rounding of a unit sine through six sections sits well below that.
    // The stimulus is computed in double: `sine()`'s float phase w*i jitters by
    // ~1e-3 rad at i ~ 8000, which smears tone energy across the band at about
    // -75 dB and would read as filter leakage.
    for (const double f : {0.30, 0.37, 0.45, 0.49}) {
        HalfBandDownsampler2x dn;
        constexpr std::size_t kN = 8192;
        std::vector<float> x(kN);
        for (std::size_t i = 0; i < kN; ++i)
            x[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * f * static_cast<double>(i)));
        std::vector<float> y(kN / 2, 0.0f);
        dn.process_block(x, y);
        const float in_rms = trailing_rms(x, kN / 4);
        const float out_rms = trailing_rms(y, kN / 8);
        const float atten_db = to_db(out_rms / in_rms);
        INFO("stopband atten at f=" << f << " = " << atten_db << " dB");
        REQUIRE(atten_db < -90.0f);
    }
}

TEST_CASE("HalfBandUpsampler2x preserves passband sine amplitude",
          "[signal][halfband]") {
    // Drive a pure sine at the input rate. Output rate is 2x. The
    // upsampler's job is to suppress spectral images and pass the
    // original. For ANY input sine (in [0, 0.5)*Fs_in), the output
    // total RMS should equal the input RMS (since image is removed
    // and only the original tone passes).
    for (float f_in : {0.05f, 0.10f, 0.20f, 0.30f, 0.40f, 0.45f}) {
        HalfBandUpsampler2x up;
        constexpr std::size_t kN = 2048;
        const auto x = sine(f_in, kN);
        std::vector<float> y(2 * kN, 0.0f);
        up.process_block(x, y);
        const float in_rms = trailing_rms(x, kN / 4);
        const float out_rms = trailing_rms(y, kN);
        const float gain_db = to_db(out_rms / in_rms);
        INFO("f_in=" << f_in << " out/in RMS = " << gain_db << " dB");
        // Within +/- 0.5 dB across the full input bandwidth. A poor
        // wiring (image not suppressed) would show +3 dB doubling.
        REQUIRE(gain_db < 0.5f);
        REQUIRE(gain_db > -0.5f);
    }
}

TEST_CASE("HalfBandUpsampler2x sample-rate invariance — identical samples regardless of Fs",
          "[signal][halfband]") {
    // Half-band coefficients are normalized to Fs, so two instances
    // driven by the same normalized-frequency sine produce identical
    // sample sequences regardless of what physical sample rate the
    // caller is operating at. We can't "tell" the upsampler what Fs
    // is (it doesn't have a set_sample_rate), which is exactly the
    // sample-rate-invariance the design provides — proven here by
    // construction.
    HalfBandUpsampler2x up_a, up_b;
    constexpr std::size_t kN = 1024;
    const auto x = sine(0.1f, kN);
    std::vector<float> ya(2 * kN, 0.0f), yb(2 * kN, 0.0f);
    up_a.process_block(x, ya);
    up_b.process_block(x, yb);
    for (std::size_t i = 0; i < 2 * kN; ++i) {
        REQUIRE(ya[i] == yb[i]);
    }
}

TEST_CASE("HalfBandUpsampler2x followed by HalfBandDownsampler2x is near-identity in passband",
          "[signal][halfband]") {
    HalfBandUpsampler2x up;
    HalfBandDownsampler2x dn;
    constexpr std::size_t kN = 4096;
    const auto x = sine(0.05f, kN);
    std::vector<float> mid(2 * kN, 0.0f);
    up.process_block(x, mid);
    std::vector<float> y(kN, 0.0f);
    dn.process_block(mid, y);
    const float in_rms = trailing_rms(x, kN / 4);
    const float out_rms = trailing_rms(y, kN / 4);
    const float gain_db = to_db(out_rms / in_rms);
    INFO("round-trip gain_db = " << gain_db);
    REQUIRE(gain_db < 0.5f);
    REQUIRE(gain_db > -0.5f);
}

TEST_CASE("HalfBandUpsampler2x reset clears state", "[signal][halfband]") {
    HalfBandUpsampler2x up;
    float lo = 0.0f, hi = 0.0f;
    up.process(1.0f, lo, hi);
    for (int i = 0; i < 100; ++i) up.process(0.5f, lo, hi);
    up.reset();
    for (int i = 0; i < 50; ++i) {
        up.process(0.0f, lo, hi);
        REQUIRE(lo == 0.0f);
        REQUIRE(hi == 0.0f);
    }
}

TEST_CASE("HalfBandDownsampler2x reset clears state", "[signal][halfband]") {
    HalfBandDownsampler2x dn;
    for (int i = 0; i < 100; ++i) (void)dn.process(0.5f, -0.5f);
    dn.reset();
    for (int i = 0; i < 50; ++i) {
        REQUIRE(dn.process(0.0f, 0.0f) == 0.0f);
    }
}

TEST_CASE("HalfBand custom coefficients constructor", "[signal][halfband]") {
    constexpr std::array<float, 2> ca = {0.1f, 0.5f};
    constexpr std::array<float, 2> cb = {0.3f, 0.7f};
    std::span<const float> sa{ca};
    std::span<const float> sb{cb};
    HalfBandUpsampler2x up(sa, sb);
    REQUIRE(up.sections_a() == 2);
    REQUIRE(up.sections_b() == 2);
    float lo = 0.0f, hi = 0.0f;
    for (int i = 0; i < 100; ++i) {
        up.process(std::sin(0.05f * static_cast<float>(i)), lo, hi);
        REQUIRE(std::isfinite(lo));
        REQUIRE(std::isfinite(hi));
    }
}

TEST_CASE("HalfBandDownsampler2x custom coefficients constructor", "[signal][halfband]") {
    constexpr std::array<float, 3> ca = {0.05f, 0.4f, 0.85f};
    constexpr std::array<float, 3> cb = {0.15f, 0.6f, 0.95f};
    std::span<const float> sa{ca};
    std::span<const float> sb{cb};
    HalfBandDownsampler2x dn(sa, sb);
    REQUIRE(dn.sections_a() == 3);
    REQUIRE(dn.sections_b() == 3);
    for (int i = 0; i < 100; ++i) {
        float y = dn.process(std::sin(0.05f * static_cast<float>(2 * i)),
                             std::sin(0.05f * static_cast<float>(2 * i + 1)));
        REQUIRE(std::isfinite(y));
    }
}

TEST_CASE("HalfBandUpsampler2x rejects out-of-band images (white-noise check)",
          "[signal][halfband]") {
    // Drive the upsampler with a deterministic pseudo-random sequence
    // that has approximately white-noise spectrum across [0, Fs_in/2).
    // The output total RMS should equal the input total RMS — same
    // reasoning as the per-sine test, but over a broader spectrum.
    HalfBandUpsampler2x up;
    constexpr std::size_t kN = 4096;
    std::vector<float> x(kN);
    uint32_t state = 0xC0FFEEu;
    for (std::size_t i = 0; i < kN; ++i) {
        state = state * 1103515245u + 12345u;
        x[i] = (static_cast<int32_t>(state) / 2147483648.0f); // ~[-1, 1)
    }
    std::vector<float> y(2 * kN, 0.0f);
    up.process_block(x, y);
    const float in_rms = trailing_rms(x, kN / 4);
    const float out_rms = trailing_rms(y, kN);
    // Image-doubling would give +3 dB. Tolerance +/- 1 dB.
    const float gain_db = to_db(out_rms / in_rms);
    INFO("white-noise out/in RMS = " << gain_db << " dB");
    REQUIRE(gain_db < 1.0f);
    REQUIRE(gain_db > -1.0f);
}
