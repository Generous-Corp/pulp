#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/filter_design.hpp>
#include <cmath>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

TEST_CASE("FilterDesign lowpass coefficients are valid", "[signal][filter_design]") {
    auto c = FilterDesign::lowpass(1000.0f, 0.707f, 44100.0f);
    // Sum of b coefficients ≈ DC gain (should be ~1 for lowpass)
    float dc_gain = (c.b0 + c.b1 + c.b2) / (1.0f + c.a1 + c.a2);
    REQUIRE_THAT(dc_gain, WithinAbs(1.0, 0.01));
}

TEST_CASE("FilterDesign highpass blocks DC", "[signal][filter_design]") {
    auto c = FilterDesign::highpass(1000.0f, 0.707f, 44100.0f);
    float dc_gain = (c.b0 + c.b1 + c.b2) / (1.0f + c.a1 + c.a2);
    REQUIRE_THAT(dc_gain, WithinAbs(0.0, 0.01));
}

TEST_CASE("FilterDesign bandpass has zero DC gain", "[signal][filter_design]") {
    auto c = FilterDesign::bandpass(1000.0f, 1.0f, 44100.0f);
    float dc_gain = (c.b0 + c.b1 + c.b2) / (1.0f + c.a1 + c.a2);
    REQUIRE_THAT(dc_gain, WithinAbs(0.0, 0.01));
}

TEST_CASE("FilterDesign notch passes DC", "[signal][filter_design]") {
    auto c = FilterDesign::notch(1000.0f, 1.0f, 44100.0f);
    float dc_gain = (c.b0 + c.b1 + c.b2) / (1.0f + c.a1 + c.a2);
    REQUIRE_THAT(dc_gain, WithinAbs(1.0, 0.01));
}

TEST_CASE("FilterDesign allpass passes DC", "[signal][filter_design]") {
    auto c = FilterDesign::allpass(1000.0f, 0.707f, 44100.0f);
    float dc_gain = (c.b0 + c.b1 + c.b2) / (1.0f + c.a1 + c.a2);
    REQUIRE_THAT(dc_gain, WithinAbs(1.0, 0.01));
}

TEST_CASE("FilterDesign peaking_eq with 0dB gain is unity", "[signal][filter_design]") {
    auto c = FilterDesign::peaking_eq(1000.0f, 1.0f, 0.0f, 44100.0f);
    // 0 dB gain = no change, coefficients should be identity-like
    REQUIRE_THAT(c.b0, WithinAbs(1.0, 0.01));
    REQUIRE_THAT(c.b2, WithinAbs(c.a2, 0.01));
}

TEST_CASE("FilterDesign low_shelf boosts DC", "[signal][filter_design]") {
    auto c = FilterDesign::low_shelf(200.0f, 6.0f, 44100.0f);
    float dc_gain = (c.b0 + c.b1 + c.b2) / (1.0f + c.a1 + c.a2);
    // +6dB ≈ 2x gain at DC
    REQUIRE(dc_gain > 1.5f);
    REQUIRE(dc_gain < 2.5f);
}

TEST_CASE("FilterDesign high_shelf boosts Nyquist", "[signal][filter_design]") {
    auto c = FilterDesign::high_shelf(5000.0f, 6.0f, 44100.0f);
    // At Nyquist (z = -1): H(-1) = (b0 - b1 + b2) / (1 - a1 + a2)
    float nyquist_gain = (c.b0 - c.b1 + c.b2) / (1.0f - c.a1 + c.a2);
    REQUIRE(nyquist_gain > 1.5f);
}

TEST_CASE("FilterDesign butterworth_lowpass generates correct section count", "[signal][filter_design]") {
    auto cascade = FilterDesign::butterworth_lowpass(4, 2000.0f, 44100.0f);
    REQUIRE(cascade.size() == 2); // 4th order = 2 biquad sections

    auto cascade6 = FilterDesign::butterworth_lowpass(6, 2000.0f, 44100.0f);
    REQUIRE(cascade6.size() == 3); // 6th order = 3 sections
}

TEST_CASE("FilterDesign butterworth_lowpass each section passes DC", "[signal][filter_design]") {
    auto cascade = FilterDesign::butterworth_lowpass(4, 2000.0f, 44100.0f);
    for (const auto& c : cascade) {
        float dc_gain = (c.b0 + c.b1 + c.b2) / (1.0f + c.a1 + c.a2);
        REQUIRE_THAT(dc_gain, WithinAbs(1.0, 0.01));
    }
}

TEST_CASE("FilterDesign butterworth_highpass blocks DC", "[signal][filter_design]") {
    auto cascade = FilterDesign::butterworth_highpass(4, 2000.0f, 44100.0f);
    for (const auto& c : cascade) {
        float dc_gain = (c.b0 + c.b1 + c.b2) / (1.0f + c.a1 + c.a2);
        REQUIRE_THAT(dc_gain, WithinAbs(0.0, 0.01));
    }
}
