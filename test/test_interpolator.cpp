#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/interpolator.hpp>
#include <cmath>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

TEST_CASE("Interpolator linear at endpoints", "[signal][interp]") {
    REQUIRE_THAT(Interpolator::linear(0.0f, 1.0f, 3.0f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(Interpolator::linear(1.0f, 1.0f, 3.0f), WithinAbs(3.0, 0.001));
}

TEST_CASE("Interpolator linear midpoint", "[signal][interp]") {
    REQUIRE_THAT(Interpolator::linear(0.5f, 0.0f, 2.0f), WithinAbs(1.0, 0.001));
}

TEST_CASE("Interpolator linear extrapolates outside normalized span", "[signal][interp]") {
    REQUIRE_THAT(Interpolator::linear(-0.25f, 2.0f, 6.0f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(Interpolator::linear(1.25f, 2.0f, 6.0f), WithinAbs(7.0, 0.001));
}

TEST_CASE("Interpolator hermite passes through y0 and y1", "[signal][interp]") {
    // For constant signal, should output constant
    REQUIRE_THAT(Interpolator::hermite(0.0f, 1.0f, 1.0f, 1.0f, 1.0f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(Interpolator::hermite(0.5f, 1.0f, 1.0f, 1.0f, 1.0f), WithinAbs(1.0, 0.001));
}

TEST_CASE("Interpolator hermite at frac=0 returns y0", "[signal][interp]") {
    REQUIRE_THAT(Interpolator::hermite(0.0f, 0.0f, 5.0f, 10.0f, 15.0f), WithinAbs(5.0, 0.001));
}

TEST_CASE("Interpolator hermite at frac=1 returns y1", "[signal][interp]") {
    REQUIRE_THAT(Interpolator::hermite(1.0f, -4.0f, 2.0f, 7.0f, 20.0f), WithinAbs(7.0, 0.001));
}

TEST_CASE("Interpolator hermite impulse response spans adjacent samples", "[signal][interp]") {
    REQUIRE_THAT(Interpolator::hermite(0.5f, 0.0f, 1.0f, 0.0f, 0.0f), WithinAbs(0.5625, 0.001));
    REQUIRE_THAT(Interpolator::hermite(0.5f, 0.0f, 0.0f, 1.0f, 0.0f), WithinAbs(0.5625, 0.001));
    REQUIRE_THAT(Interpolator::hermite(0.5f, 1.0f, 0.0f, 0.0f, 0.0f), WithinAbs(-0.0625, 0.001));
    REQUIRE_THAT(Interpolator::hermite(0.5f, 0.0f, 0.0f, 0.0f, 1.0f), WithinAbs(-0.0625, 0.001));
}

TEST_CASE("Interpolator lagrange at frac=0 returns y0", "[signal][interp]") {
    float result = Interpolator::lagrange(0.0f, 0.0f, 5.0f, 10.0f, 15.0f);
    REQUIRE_THAT(result, WithinAbs(5.0, 0.01));
}

TEST_CASE("Interpolator lagrange at frac=1 returns y1", "[signal][interp]") {
    float result = Interpolator::lagrange(1.0f, -4.0f, 2.0f, 7.0f, 20.0f);
    REQUIRE_THAT(result, WithinAbs(7.0, 0.01));
}

TEST_CASE("Interpolator lagrange for linear data", "[signal][interp]") {
    // Linear: ym1=0, y0=1, y1=2, y2=3
    // At frac=0.5, result should be 1.5
    float result = Interpolator::lagrange(0.5f, 0.0f, 1.0f, 2.0f, 3.0f);
    REQUIRE_THAT(result, WithinAbs(1.5, 0.01));
}

TEST_CASE("Interpolator lagrange impulse response spans four samples", "[signal][interp]") {
    REQUIRE_THAT(Interpolator::lagrange(0.5f, 1.0f, 0.0f, 0.0f, 0.0f), WithinAbs(-0.0625, 0.001));
    REQUIRE_THAT(Interpolator::lagrange(0.5f, 0.0f, 1.0f, 0.0f, 0.0f), WithinAbs(0.5625, 0.001));
    REQUIRE_THAT(Interpolator::lagrange(0.5f, 0.0f, 0.0f, 1.0f, 0.0f), WithinAbs(0.5625, 0.001));
    REQUIRE_THAT(Interpolator::lagrange(0.5f, 0.0f, 0.0f, 0.0f, 1.0f), WithinAbs(-0.0625, 0.001));
}

TEST_CASE("Interpolator sinc6 constant signal", "[signal][interp]") {
    float result = Interpolator::sinc6(0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    REQUIRE_THAT(result, WithinAbs(1.0, 0.05));
}

TEST_CASE("Interpolator sinc6 at frac=0 returns y0", "[signal][interp]") {
    float result = Interpolator::sinc6(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    REQUIRE_THAT(result, WithinAbs(1.0, 0.05));
}

TEST_CASE("Interpolator sinc6 at frac=1 returns y1", "[signal][interp]") {
    float result = Interpolator::sinc6(1.0f, -3.0f, -2.0f, 5.0f, 9.0f, 13.0f, 17.0f);
    REQUIRE_THAT(result, WithinAbs(9.0, 0.05));
}

TEST_CASE("Interpolator sinc6 impulse response is symmetric at midpoint", "[signal][interp]") {
    float left = Interpolator::sinc6(0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    float right = Interpolator::sinc6(0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    float outer_left = Interpolator::sinc6(0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    float outer_right = Interpolator::sinc6(0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);

    REQUIRE_THAT(left, WithinAbs(right, 0.001));
    REQUIRE_THAT(outer_left, WithinAbs(outer_right, 0.001));
    REQUIRE(left > 0.0f);
    REQUIRE(outer_left < 0.0f);
}

TEST_CASE("Interpolator linear preserves constants outside the unit interval",
          "[signal][interp][codecov]") {
    REQUIRE_THAT(Interpolator::linear(-2.0f, -0.75f, -0.75f), WithinAbs(-0.75f, 1e-6f));
    REQUIRE_THAT(Interpolator::linear(3.0f, 4.5f, 4.5f), WithinAbs(4.5f, 1e-6f));
}

TEST_CASE("Interpolator hermite reproduces quadratic midpoint samples",
          "[signal][interp][codecov]") {
    REQUIRE_THAT(Interpolator::hermite(0.5f, 1.0f, 0.0f, 1.0f, 4.0f),
                 WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(Interpolator::hermite(0.25f, 4.0f, 1.0f, 0.0f, 1.0f),
                 WithinAbs(0.5625f, 1e-6f));
}

TEST_CASE("Interpolator lagrange reproduces cubic samples",
          "[signal][interp][codecov]") {
    REQUIRE_THAT(Interpolator::lagrange(0.25f, -1.0f, 0.0f, 1.0f, 8.0f),
                 WithinAbs(0.015625f, 1e-6f));
    REQUIRE_THAT(Interpolator::lagrange(0.75f, -8.0f, -1.0f, 0.0f, 1.0f),
                 WithinAbs(-0.015625f, 1e-6f));
}

TEST_CASE("Interpolator sinc6 zero input remains silent",
          "[signal][interp][codecov]") {
    for (float frac : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        const float result = Interpolator::sinc6(frac, 0.0f, 0.0f, 0.0f,
                                                 0.0f, 0.0f, 0.0f);
        REQUIRE(std::isfinite(result));
        REQUIRE_THAT(result, WithinAbs(0.0f, 1e-6f));
    }
}
