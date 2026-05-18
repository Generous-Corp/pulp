#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/filter_design.hpp>
#include <cmath>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

void require_finite(const FilterDesign::Coefficients& c) {
    REQUIRE(std::isfinite(c.b0));
    REQUIRE(std::isfinite(c.b1));
    REQUIRE(std::isfinite(c.b2));
    REQUIRE(std::isfinite(c.a1));
    REQUIRE(std::isfinite(c.a2));
}

float dc_gain(const FilterDesign::Coefficients& c) {
    return (c.b0 + c.b1 + c.b2) / (1.0f + c.a1 + c.a2);
}

float nyquist_gain(const FilterDesign::Coefficients& c) {
    return (c.b0 - c.b1 + c.b2) / (1.0f - c.a1 + c.a2);
}

}  // namespace

TEST_CASE("FilterDesign lowpass coefficients are valid", "[signal][filter_design]") {
    auto c = FilterDesign::lowpass(1000.0f, 0.707f, 44100.0f);
    // Sum of b coefficients ≈ DC gain (should be ~1 for lowpass)
    REQUIRE_THAT(dc_gain(c), WithinAbs(1.0, 0.01));
}

TEST_CASE("FilterDesign highpass blocks DC", "[signal][filter_design]") {
    auto c = FilterDesign::highpass(1000.0f, 0.707f, 44100.0f);
    REQUIRE_THAT(dc_gain(c), WithinAbs(0.0, 0.01));
}

TEST_CASE("FilterDesign bandpass has zero DC gain", "[signal][filter_design]") {
    auto c = FilterDesign::bandpass(1000.0f, 1.0f, 44100.0f);
    REQUIRE_THAT(dc_gain(c), WithinAbs(0.0, 0.01));
}

TEST_CASE("FilterDesign notch passes DC", "[signal][filter_design]") {
    auto c = FilterDesign::notch(1000.0f, 1.0f, 44100.0f);
    REQUIRE_THAT(dc_gain(c), WithinAbs(1.0, 0.01));
}

TEST_CASE("FilterDesign allpass passes DC", "[signal][filter_design]") {
    auto c = FilterDesign::allpass(1000.0f, 0.707f, 44100.0f);
    REQUIRE_THAT(dc_gain(c), WithinAbs(1.0, 0.01));
}

TEST_CASE("FilterDesign peaking_eq with 0dB gain is unity", "[signal][filter_design]") {
    auto c = FilterDesign::peaking_eq(1000.0f, 1.0f, 0.0f, 44100.0f);
    // 0 dB gain = no change, coefficients should be identity-like
    REQUIRE_THAT(c.b0, WithinAbs(1.0, 0.01));
    REQUIRE_THAT(c.b2, WithinAbs(c.a2, 0.01));
}

TEST_CASE("FilterDesign low_shelf boosts DC", "[signal][filter_design]") {
    auto c = FilterDesign::low_shelf(200.0f, 6.0f, 44100.0f);
    // +6dB ≈ 2x gain at DC
    REQUIRE(dc_gain(c) > 1.5f);
    REQUIRE(dc_gain(c) < 2.5f);
}

TEST_CASE("FilterDesign high_shelf boosts Nyquist", "[signal][filter_design]") {
    auto c = FilterDesign::high_shelf(5000.0f, 6.0f, 44100.0f);
    // At Nyquist (z = -1): H(-1) = (b0 - b1 + b2) / (1 - a1 + a2)
    REQUIRE(nyquist_gain(c) > 1.5f);
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
        REQUIRE_THAT(dc_gain(c), WithinAbs(0.0, 0.01));
    }
}

TEST_CASE("FilterDesign gain variants stay finite and directional",
          "[signal][filter_design][issue-645]") {
    auto peak_boost = FilterDesign::peaking_eq(1200.0f, 1.5f, 9.0f, 48000.0f);
    auto peak_cut = FilterDesign::peaking_eq(1200.0f, 1.5f, -9.0f, 48000.0f);
    auto low_cut = FilterDesign::low_shelf(250.0f, -6.0f, 48000.0f, 0.75f);
    auto high_cut = FilterDesign::high_shelf(6000.0f, -6.0f, 48000.0f, 0.75f);

    for (const auto& c : {peak_boost, peak_cut, low_cut, high_cut}) {
        require_finite(c);
    }

    REQUIRE(peak_boost.b0 > 1.0f);
    REQUIRE(peak_cut.b0 < 1.0f);
    REQUIRE(dc_gain(low_cut) < 1.0f);
    REQUIRE(nyquist_gain(high_cut) < 1.0f);
}

TEST_CASE("FilterDesign shelf slope variants remain stable",
          "[signal][filter_design][issue-645]") {
    auto gentle_low = FilterDesign::low_shelf(500.0f, 4.0f, 48000.0f, 0.5f);
    auto steep_low = FilterDesign::low_shelf(500.0f, 4.0f, 48000.0f, 2.0f);
    auto gentle_high = FilterDesign::high_shelf(4000.0f, 4.0f, 48000.0f, 0.5f);
    auto steep_high = FilterDesign::high_shelf(4000.0f, 4.0f, 48000.0f, 2.0f);

    for (const auto& c : {gentle_low, steep_low, gentle_high, steep_high}) {
        require_finite(c);
    }

    REQUIRE(dc_gain(gentle_low) > 1.0f);
    REQUIRE(dc_gain(steep_low) > 1.0f);
    REQUIRE(nyquist_gain(gentle_high) > 1.0f);
    REQUIRE(nyquist_gain(steep_high) > 1.0f);
}

TEST_CASE("FilterDesign butterworth edge orders are bounded and finite",
          "[signal][filter_design][issue-645]") {
    REQUIRE(FilterDesign::butterworth_lowpass(0, 1000.0f, 48000.0f).empty());
    REQUIRE(FilterDesign::butterworth_highpass(1, 1000.0f, 48000.0f).empty());

    auto low = FilterDesign::butterworth_lowpass(8, 2000.0f, 48000.0f);
    auto high = FilterDesign::butterworth_highpass(8, 2000.0f, 48000.0f);
    REQUIRE(low.size() == 4);
    REQUIRE(high.size() == 4);

    for (const auto& c : low) {
        require_finite(c);
        REQUIRE_THAT(dc_gain(c), WithinAbs(1.0f, 0.01f));
    }
    for (const auto& c : high) {
        require_finite(c);
        REQUIRE_THAT(dc_gain(c), WithinAbs(0.0f, 0.01f));
    }
}

TEST_CASE("FilterDesign lowpass and highpass have complementary Nyquist behavior",
          "[signal][filter_design][codecov]") {
    auto low = FilterDesign::lowpass(1800.0f, 0.707f, 48000.0f);
    auto high = FilterDesign::highpass(1800.0f, 0.707f, 48000.0f);

    require_finite(low);
    require_finite(high);
    REQUIRE_THAT(nyquist_gain(low), WithinAbs(0.0f, 0.01f));
    REQUIRE_THAT(nyquist_gain(high), WithinAbs(1.0f, 0.01f));
}

TEST_CASE("FilterDesign allpass keeps unity gain at DC and Nyquist",
          "[signal][filter_design][codecov]") {
    for (float q : {0.5f, 0.707f, 2.0f}) {
        auto c = FilterDesign::allpass(3200.0f, q, 48000.0f);
        require_finite(c);
        REQUIRE_THAT(dc_gain(c), WithinAbs(1.0f, 0.01f));
        REQUIRE_THAT(nyquist_gain(c), WithinAbs(1.0f, 0.01f));
    }
}

TEST_CASE("FilterDesign notch preserves endpoint gains across Q values",
          "[signal][filter_design][codecov]") {
    for (float q : {0.25f, 1.0f, 4.0f}) {
        auto c = FilterDesign::notch(2400.0f, q, 48000.0f);
        require_finite(c);
        REQUIRE_THAT(dc_gain(c), WithinAbs(1.0f, 0.01f));
        REQUIRE_THAT(nyquist_gain(c), WithinAbs(1.0f, 0.01f));
    }
}

TEST_CASE("FilterDesign butterworth odd orders truncate to complete biquads",
          "[signal][filter_design][codecov]") {
    auto low = FilterDesign::butterworth_lowpass(5, 1600.0f, 48000.0f);
    auto high = FilterDesign::butterworth_highpass(5, 1600.0f, 48000.0f);
    REQUIRE(low.size() == 2);
    REQUIRE(high.size() == 2);

    for (const auto& c : low) {
        require_finite(c);
        REQUIRE_THAT(dc_gain(c), WithinAbs(1.0f, 0.01f));
    }
    for (const auto& c : high) {
        require_finite(c);
        REQUIRE_THAT(dc_gain(c), WithinAbs(0.0f, 0.01f));
    }
}
