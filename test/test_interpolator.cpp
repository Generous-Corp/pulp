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

TEST_CASE("Interpolator hermite passes through y0 and y1", "[signal][interp]") {
    // For constant signal, should output constant
    REQUIRE_THAT(Interpolator::hermite(0.0f, 1.0f, 1.0f, 1.0f, 1.0f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(Interpolator::hermite(0.5f, 1.0f, 1.0f, 1.0f, 1.0f), WithinAbs(1.0, 0.001));
}

TEST_CASE("Interpolator hermite at frac=0 returns y0", "[signal][interp]") {
    REQUIRE_THAT(Interpolator::hermite(0.0f, 0.0f, 5.0f, 10.0f, 15.0f), WithinAbs(5.0, 0.001));
}

TEST_CASE("Interpolator lagrange at frac=0 returns y0", "[signal][interp]") {
    float result = Interpolator::lagrange(0.0f, 0.0f, 5.0f, 10.0f, 15.0f);
    REQUIRE_THAT(result, WithinAbs(5.0, 0.01));
}

TEST_CASE("Interpolator lagrange for linear data", "[signal][interp]") {
    // Linear: ym1=0, y0=1, y1=2, y2=3
    // At frac=0.5, result should be 1.5
    float result = Interpolator::lagrange(0.5f, 0.0f, 1.0f, 2.0f, 3.0f);
    REQUIRE_THAT(result, WithinAbs(1.5, 0.01));
}

TEST_CASE("Interpolator sinc6 constant signal", "[signal][interp]") {
    float result = Interpolator::sinc6(0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    REQUIRE_THAT(result, WithinAbs(1.0, 0.05));
}

TEST_CASE("Interpolator sinc6 at frac=0 returns y0", "[signal][interp]") {
    float result = Interpolator::sinc6(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    REQUIRE_THAT(result, WithinAbs(1.0, 0.05));
}
