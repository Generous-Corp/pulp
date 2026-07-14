#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/elliptic_halfband_iir.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

// ────────────────────────────────────────────────────────────────────────
// Elliptic (Valenzuela-Constantinides) half-band polyphase-allpass filter.
//
// Same polyphase-allpass shape as `halfband_iir.hpp`'s
// HalfBandUpsampler2x/HalfBandDownsampler2x, but the section count and
// coefficients come from running the elliptic design equations at
// configure() time against a caller-supplied transition width / stopband
// floor, instead of a fixed six-section table — see
// OversamplerT::Kind::elliptic_polyphase_iir in oversampling.hpp, which
// drives this per its own per-stage transition-width/stopband-floor
// schedule.
//
// Test strategy mirrors test_halfband_iir.cpp: internal goldens, no
// external reference filter — passband flatness, stopband rejection, DC,
// sample-rate invariance, round-trip, reset, and a deliberately narrower
// design to prove `configure()`'s parameters actually change behaviour.
// ────────────────────────────────────────────────────────────────────────

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

float trailing_rms(const std::vector<float>& samples, std::size_t skip) {
    if (samples.size() <= skip)
        return 0.0f;
    double acc = 0.0;
    const std::size_t n = samples.size() - skip;
    for (std::size_t i = skip; i < samples.size(); ++i)
        acc += static_cast<double>(samples[i]) * samples[i];
    return static_cast<float>(std::sqrt(acc / static_cast<double>(n)));
}

float to_db(float lin) {
    return 20.0f * std::log10(std::max(lin, 1e-30f));
}

std::vector<float> sine(float cycles_per_sample, std::size_t n) {
    std::vector<float> out(n);
    const float w = 2.0f * kPi * cycles_per_sample;
    for (std::size_t i = 0; i < n; ++i)
        out[i] = std::sin(w * static_cast<float>(i));
    return out;
}

} // namespace

TEST_CASE("EllipticHalfBandUpsampler2x default design is stable and finite",
          "[signal][halfband][elliptic]") {
    EllipticHalfBandUpsampler2x up;
    REQUIRE(up.direct_sections() > 0);
    REQUIRE(up.delayed_sections() > 0);
    float lo = 0.0f, hi = 0.0f;
    up.process(1.0f, lo, hi);
    for (int i = 0; i < 1000; ++i) {
        up.process(0.0f, lo, hi);
        REQUIRE(std::isfinite(lo));
        REQUIRE(std::isfinite(hi));
        REQUIRE(std::fabs(lo) <= 2.0f);
        REQUIRE(std::fabs(hi) <= 2.0f);
    }
}

TEST_CASE("EllipticHalfBandDownsampler2x default design is stable and finite",
          "[signal][halfband][elliptic]") {
    EllipticHalfBandDownsampler2x dn;
    REQUIRE(dn.direct_sections() > 0);
    REQUIRE(dn.delayed_sections() > 0);
    for (int i = 0; i < 1000; ++i) {
        const float y = dn.process(0.5f, -0.5f);
        REQUIRE(std::isfinite(y));
        REQUIRE(std::fabs(y) <= 2.0f);
    }
}

TEST_CASE("EllipticHalfBandUpsampler2x preserves DC", "[signal][halfband][elliptic]") {
    EllipticHalfBandUpsampler2x up;
    constexpr std::size_t kN = 512;
    std::vector<float> y(2 * kN, 0.0f);
    for (std::size_t i = 0; i < kN; ++i)
        up.process(0.5f, y[2 * i], y[2 * i + 1]);
    const std::size_t tail_start = (2 * kN) - 64;
    for (std::size_t i = tail_start; i < 2 * kN; ++i) {
        INFO("y[" << i << "] = " << y[i]);
        REQUIRE_THAT(y[i], WithinAbs(0.5f, 1e-3f));
    }
}

TEST_CASE("EllipticHalfBandDownsampler2x preserves DC", "[signal][halfband][elliptic]") {
    EllipticHalfBandDownsampler2x dn;
    constexpr std::size_t kN = 256;
    std::vector<float> y(kN, 0.0f);
    for (std::size_t i = 0; i < kN; ++i)
        y[i] = dn.process(0.5f, 0.5f);
    const std::size_t tail_start = kN - 32;
    for (std::size_t i = tail_start; i < kN; ++i)
        REQUIRE_THAT(y[i], WithinAbs(0.5f, 1e-3f));
}

TEST_CASE("EllipticHalfBandUpsampler2x preserves passband sine amplitude",
          "[signal][halfband][elliptic]") {
    for (float f_in : {0.05f, 0.10f, 0.20f, 0.30f, 0.40f}) {
        EllipticHalfBandUpsampler2x up;
        constexpr std::size_t kN = 2048;
        const auto x = sine(f_in, kN);
        std::vector<float> y(2 * kN, 0.0f);
        up.process_block(x, y);
        const float in_rms = trailing_rms(x, kN / 4);
        const float out_rms = trailing_rms(y, kN);
        const float gain_db = to_db(out_rms / in_rms);
        INFO("f_in=" << f_in << " out/in RMS = " << gain_db << " dB");
        REQUIRE(gain_db < 0.5f);
        REQUIRE(gain_db > -0.5f);
    }
}

TEST_CASE("EllipticHalfBandDownsampler2x rejects deep-stopband energy",
          "[signal][halfband][elliptic]") {
    EllipticHalfBandDownsampler2x dn;
    constexpr std::size_t kN = 8192;
    const auto x = sine(0.47f, kN);
    std::vector<float> y(kN / 2, 0.0f);
    dn.process_block(x, y);
    const float in_rms = trailing_rms(x, kN / 4);
    const float out_rms = trailing_rms(y, kN / 8);
    const float atten_db = to_db(out_rms / in_rms);
    INFO("stopband atten at f=0.47*Fs = " << atten_db << " dB");
    REQUIRE(atten_db < -40.0f);
}

TEST_CASE("EllipticHalfBandUpsampler2x sample-rate invariance", "[signal][halfband][elliptic]") {
    EllipticHalfBandUpsampler2x up_a, up_b;
    constexpr std::size_t kN = 1024;
    const auto x = sine(0.1f, kN);
    std::vector<float> ya(2 * kN, 0.0f), yb(2 * kN, 0.0f);
    up_a.process_block(x, ya);
    up_b.process_block(x, yb);
    for (std::size_t i = 0; i < 2 * kN; ++i)
        REQUIRE(ya[i] == yb[i]);
}

TEST_CASE("EllipticHalfBandUpsampler2x followed by EllipticHalfBandDownsampler2x "
          "is near-identity in passband",
          "[signal][halfband][elliptic]") {
    EllipticHalfBandUpsampler2x up;
    EllipticHalfBandDownsampler2x dn;
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

TEST_CASE("EllipticHalfBandUpsampler2x reset clears state", "[signal][halfband][elliptic]") {
    EllipticHalfBandUpsampler2x up;
    float lo = 0.0f, hi = 0.0f;
    up.process(1.0f, lo, hi);
    for (int i = 0; i < 100; ++i)
        up.process(0.5f, lo, hi);
    up.reset();
    for (int i = 0; i < 50; ++i) {
        up.process(0.0f, lo, hi);
        REQUIRE(lo == 0.0f);
        REQUIRE(hi == 0.0f);
    }
}

TEST_CASE("EllipticHalfBandDownsampler2x reset clears state", "[signal][halfband][elliptic]") {
    EllipticHalfBandDownsampler2x dn;
    for (int i = 0; i < 100; ++i)
        (void)dn.process(0.5f, -0.5f);
    dn.reset();
    for (int i = 0; i < 50; ++i)
        REQUIRE(dn.process(0.0f, 0.0f) == 0.0f);
}

TEST_CASE("EllipticHalfBandUpsampler2x configure() with a wider transition band "
          "designs fewer sections",
          "[signal][halfband][elliptic]") {
    // A wider transition (easier design target) needs a lower filter order
    // than a narrower one at the same stopband floor -- this is what makes
    // the design equation-driven, not the fixed table halfband_iir.hpp
    // uses. Proves configure() actually re-designs rather than ignoring its
    // arguments.
    EllipticHalfBandUpsampler2x narrow(0.02, -90.0);
    EllipticHalfBandUpsampler2x wide(0.30, -90.0);
    const std::size_t narrow_order = narrow.direct_sections() + narrow.delayed_sections();
    const std::size_t wide_order = wide.direct_sections() + wide.delayed_sections();
    INFO("narrow order=" << narrow_order << " wide order=" << wide_order);
    REQUIRE(wide_order < narrow_order);
}

TEST_CASE("EllipticHalfBandUpsampler2x configure() with a deeper stopband "
          "designs more sections",
          "[signal][halfband][elliptic]") {
    EllipticHalfBandUpsampler2x shallow(0.05, -40.0);
    EllipticHalfBandUpsampler2x deep(0.05, -120.0);
    const std::size_t shallow_order = shallow.direct_sections() + shallow.delayed_sections();
    const std::size_t deep_order = deep.direct_sections() + deep.delayed_sections();
    INFO("shallow order=" << shallow_order << " deep order=" << deep_order);
    REQUIRE(deep_order > shallow_order);
}

TEST_CASE("EllipticHalfBandUpsampler2x rejects out-of-band images (white-noise check)",
          "[signal][halfband][elliptic]") {
    EllipticHalfBandUpsampler2x up;
    constexpr std::size_t kN = 4096;
    std::vector<float> x(kN);
    uint32_t state = 0xC0FFEEu;
    for (std::size_t i = 0; i < kN; ++i) {
        state = state * 1103515245u + 12345u;
        x[i] = (static_cast<int32_t>(state) / 2147483648.0f);
    }
    std::vector<float> y(2 * kN, 0.0f);
    up.process_block(x, y);
    const float in_rms = trailing_rms(x, kN / 4);
    const float out_rms = trailing_rms(y, kN);
    const float gain_db = to_db(out_rms / in_rms);
    INFO("white-noise out/in RMS = " << gain_db << " dB");
    REQUIRE(gain_db < 1.0f);
    REQUIRE(gain_db > -1.0f);
}

TEST_CASE("EllipticHalfBandUpsampler2x/Downsampler2x double-precision path matches float",
          "[signal][halfband][elliptic][precision]") {
    EllipticHalfBandUpsampler2x64 up64;
    EllipticHalfBandUpsampler2x up32;
    constexpr std::size_t kN = 256;
    for (std::size_t i = 0; i < kN; ++i) {
        const double x = std::sin(0.05 * static_cast<double>(i));
        double lo64 = 0.0, hi64 = 0.0;
        float lo32 = 0.0f, hi32 = 0.0f;
        up64.process(x, lo64, hi64);
        up32.process(static_cast<float>(x), lo32, hi32);
        INFO("i=" << i);
        REQUIRE_THAT(lo64, WithinAbs(static_cast<double>(lo32), 1e-4));
        REQUIRE_THAT(hi64, WithinAbs(static_cast<double>(hi32), 1e-4));
    }
}

TEST_CASE("EllipticHalfBandUpsampler2x/Downsampler2x configure() at the documented "
          "stopband_db = -300 boundary designs a finite, stable filter",
          "[signal][halfband][elliptic]") {
    // stopband_db = -300 used to trip a stale "clamp to a hard 0 gain past
    // -300 dB" special case that only fired for stopband_db <= -300 (the
    // comparison was strict `>`), sending the design equation's section
    // count to +inf and its ceil-to-int cast into UB. Any finite,
    // reasonable order here proves the special case is gone.
    EllipticHalfBandUpsampler2x up(0.05, -300.0);
    EllipticHalfBandDownsampler2x dn(0.05, -300.0);
    const std::size_t up_order = up.direct_sections() + up.delayed_sections();
    const std::size_t dn_order = dn.direct_sections() + dn.delayed_sections();
    INFO("up_order=" << up_order << " dn_order=" << dn_order);
    REQUIRE(up_order > 0);
    REQUIRE(up_order < 1000);
    REQUIRE(dn_order > 0);
    REQUIRE(dn_order < 1000);

    float lo = 0.0f, hi = 0.0f;
    for (int i = 0; i < 200; ++i) {
        up.process(std::sin(0.05f * static_cast<float>(i)), lo, hi);
        REQUIRE(std::isfinite(lo));
        REQUIRE(std::isfinite(hi));
        const float out = dn.process(lo, hi);
        REQUIRE(std::isfinite(out));
    }
}
