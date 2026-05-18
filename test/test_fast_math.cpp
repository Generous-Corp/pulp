#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/fast_math.hpp>
#include <cmath>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("FastMath tanh approximation", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::tanh(0.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(FastMath::tanh(1.0f), WithinAbs(std::tanh(1.0f), 0.001));
    REQUIRE_THAT(FastMath::tanh(-1.0f), WithinAbs(std::tanh(-1.0f), 0.001));
    REQUIRE_THAT(FastMath::tanh(3.0f), WithinAbs(std::tanh(3.0f), 0.01));
    REQUIRE_THAT(FastMath::tanh(-4.0f), WithinAbs(std::tanh(-4.0f), 0.001));
    REQUIRE_THAT(FastMath::tanh(4.0f), WithinAbs(std::tanh(4.0f), 0.001));
    REQUIRE_THAT(FastMath::tanh(-5.0f), WithinAbs(-1.0, 0.001));
    REQUIRE_THAT(FastMath::tanh(5.0f), WithinAbs(1.0, 0.001));
}

TEST_CASE("FastMath tanh clamps exactly beyond approximation range",
          "[signal][fast_math][coverage][phase3]") {
    REQUIRE(FastMath::tanh(-100.0f) == -1.0f);
    REQUIRE(FastMath::tanh(-4.0001f) == -1.0f);
    REQUIRE(FastMath::tanh(4.0001f) == 1.0f);
    REQUIRE(FastMath::tanh(100.0f) == 1.0f);
}

TEST_CASE("FastMath sin approximation", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::sin(0.0f), WithinAbs(0.0, 0.01));
    REQUIRE_THAT(FastMath::sin(1.5707963f), WithinAbs(1.0, 0.01)); // pi/2
    REQUIRE_THAT(FastMath::sin(3.1415926f), WithinAbs(0.0, 0.02)); // pi
    REQUIRE_THAT(FastMath::sin(-1.5707963f), WithinAbs(-1.0, 0.01));
}

TEST_CASE("FastMath trigonometry wraps large phases",
          "[signal][fast_math][issue-645]") {
    constexpr float pi = 3.14159265f;
    constexpr float two_pi = 6.28318530f;

    REQUIRE_THAT(FastMath::sin(two_pi + pi * 0.5f), WithinAbs(1.0f, 0.01f));
    REQUIRE_THAT(FastMath::sin(-two_pi - pi * 0.5f), WithinAbs(-1.0f, 0.01f));
    REQUIRE_THAT(FastMath::cos(-two_pi), WithinAbs(1.0f, 0.01f));
}

TEST_CASE("FastMath cos approximation", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::cos(0.0f), WithinAbs(1.0, 0.01));
    REQUIRE_THAT(FastMath::cos(1.5707963f), WithinAbs(0.0, 0.02)); // pi/2
    REQUIRE_THAT(FastMath::cos(3.1415926f), WithinAbs(-1.0, 0.02)); // pi
}

TEST_CASE("FastMath exp2 approximation", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::exp2(0.0f), WithinAbs(1.0, 0.01));
    REQUIRE_THAT(FastMath::exp2(1.0f), WithinAbs(2.0, 0.01));
    REQUIRE_THAT(FastMath::exp2(3.0f), WithinAbs(8.0, 0.05));
    REQUIRE_THAT(FastMath::exp2(-1.0f), WithinAbs(0.5, 0.01));
    REQUIRE_THAT(FastMath::exp2(-1.5f), WithinAbs(std::exp2(-1.5f), 0.01));
    REQUIRE_THAT(FastMath::exp2(0.5f), WithinAbs(std::sqrt(2.0f), 0.01));
}

TEST_CASE("FastMath exp2 preserves ordering across integer boundaries",
          "[signal][fast_math][coverage][phase3]") {
    const float below = FastMath::exp2(1.999f);
    const float exact = FastMath::exp2(2.0f);
    const float above = FastMath::exp2(2.001f);

    REQUIRE(below < exact);
    REQUIRE(exact < above);
    REQUIRE_THAT(exact, WithinAbs(4.0f, 0.01f));
}

TEST_CASE("FastMath log2 approximation", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::log2(1.0f), WithinAbs(0.0, 0.01));
    REQUIRE_THAT(FastMath::log2(2.0f), WithinAbs(1.0, 0.01));
    REQUIRE_THAT(FastMath::log2(8.0f), WithinAbs(3.0, 0.02));
    REQUIRE_THAT(FastMath::log2(0.5f), WithinAbs(-1.0, 0.02));
}

TEST_CASE("FastMath pow approximation", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::pow(2.0f, 3.0f), WithinAbs(8.0, 0.1));
    REQUIRE_THAT(FastMath::pow(10.0f, 2.0f), WithinAbs(100.0, 1.0));
    REQUIRE_THAT(FastMath::pow(0.5f, 2.0f), WithinAbs(0.25, 0.01));
}

TEST_CASE("FastMath pow and reciprocal guard edge inputs",
          "[signal][fast_math][issue-645]") {
    REQUIRE_THAT(FastMath::pow(0.0f, 2.0f), WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(FastMath::pow(-2.0f, 3.0f), WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(FastMath::pow(4.0f, 0.5f), WithinAbs(2.0f, 0.05f));

    REQUIRE_THAT(FastMath::rcp(4.0f), WithinAbs(0.25f, 0.001f));
    REQUIRE_THAT(FastMath::rcp(-2.0f), WithinAbs(-0.5f, 0.001f));
}

TEST_CASE("FastMath db_to_gain", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::db_to_gain(0.0f), WithinAbs(1.0, 0.01));
    REQUIRE_THAT(FastMath::db_to_gain(6.0f), WithinAbs(std::pow(10.0f, 6.0f / 20.0f), 0.05));
    REQUIRE_THAT(FastMath::db_to_gain(-6.0f), WithinAbs(std::pow(10.0f, -6.0f / 20.0f), 0.02));
    REQUIRE_THAT(FastMath::db_to_gain(-60.0f), WithinAbs(0.001, 0.001));
}

TEST_CASE("FastMath gain_to_db", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::gain_to_db(1.0f), WithinAbs(0.0, 0.1));
    REQUIRE_THAT(FastMath::gain_to_db(2.0f), WithinAbs(6.02, 0.1));
    REQUIRE_THAT(FastMath::gain_to_db(0.5f), WithinAbs(-6.02, 0.1));
    REQUIRE(FastMath::gain_to_db(0.0f) < -100.0f);
}

TEST_CASE("FastMath gain conversion handles negative silence floor",
          "[signal][fast_math][issue-645]") {
    REQUIRE_THAT(FastMath::gain_to_db(-1.0f), WithinAbs(-200.0f, 0.001f));
    REQUIRE(FastMath::db_to_gain(-120.0f) > 0.0f);
    REQUIRE(FastMath::db_to_gain(-120.0f) < 0.00001f);
}

TEST_CASE("FastMath rsqrt approximation", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::rsqrt(1.0f), WithinAbs(1.0, 0.02));
    REQUIRE_THAT(FastMath::rsqrt(4.0f), WithinAbs(0.5, 0.02));
    REQUIRE_THAT(FastMath::rsqrt(0.25f), WithinAbs(2.0, 0.05));
}

TEST_CASE("FastMath soft_clip", "[signal][fast_math]") {
    REQUIRE_THAT(FastMath::soft_clip(0.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(FastMath::soft_clip(0.5f), WithinAbs(0.481, 0.01));
    REQUIRE_THAT(FastMath::soft_clip(1.5f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(FastMath::soft_clip(-1.5f), WithinAbs(-1.0, 0.001));
    REQUIRE_THAT(FastMath::soft_clip(2.0f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(FastMath::soft_clip(-2.0f), WithinAbs(-1.0, 0.001));
    // Symmetry
    REQUIRE_THAT(FastMath::soft_clip(0.3f), WithinAbs(-FastMath::soft_clip(-0.3f), 0.001));
}

TEST_CASE("FastMath clamp_unit", "[signal][fast_math]") {
    REQUIRE(FastMath::clamp_unit(1.0f) == 1.0f);
    REQUIRE(FastMath::clamp_unit(-1.0f) == -1.0f);
    REQUIRE(FastMath::clamp_unit(0.5f) == 0.5f);
    REQUIRE(FastMath::clamp_unit(2.0f) == 1.0f);
    REQUIRE(FastMath::clamp_unit(-2.0f) == -1.0f);
}
