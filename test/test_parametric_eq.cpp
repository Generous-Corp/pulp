#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/parametric_eq.hpp>

#include <array>
#include <cmath>

using namespace pulp::signal;

TEST_CASE("ParametricEq validates and applies a bounded band cascade", "[signal][parametric-eq]") {
    ParametricEq eq;
    REQUIRE(eq.prepare(48000.0f, 4) == ParametricEqPrepareStatus::prepared);

    const std::array bands{
        ParametricEqBand{ParametricEqBandType::peaking, 1000.0f, 6.0f, 0.707f, true, false},
        ParametricEqBand{ParametricEqBandType::high_shelf, 8000.0f, -3.0f, 0.8f, true, false},
    };
    REQUIRE(eq.configure(bands) == ParametricEqConfigureStatus::configured);
    REQUIRE(eq.band_count() == 2);
    CHECK(std::isfinite(eq.magnitude(1000.0)));
    CHECK(eq.magnitude_db(1000.0) > 3.0f);
    CHECK(eq.magnitude(1000.0) ==
          Approx(std::pow(10.0, eq.magnitude_db(1000.0) / 20.0)).epsilon(1e-5));
}

TEST_CASE("ParametricEq rejects invalid configuration without changing output",
          "[signal][parametric-eq][negative]") {
    ParametricEq eq;
    REQUIRE(eq.prepare(48000.0f, 2) == ParametricEqPrepareStatus::prepared);
    const std::array valid{
        ParametricEqBand{ParametricEqBandType::peaking, 1000.0f, 3.0f, 1.0f, true, false}};
    REQUIRE(eq.configure(valid) == ParametricEqConfigureStatus::configured);
    const auto before = eq.magnitude(1000.0);

    const std::array invalid{
        ParametricEqBand{ParametricEqBandType::peaking, 1000.0f, 3.0f, 1.0f, true, false},
        ParametricEqBand{ParametricEqBandType::peaking, 900.0f, 0.0f, 1.0f, true, false}};
    CHECK(eq.configure(invalid) ==
          ParametricEqConfigureStatus::frequencies_not_strictly_increasing);
    CHECK(eq.magnitude(1000.0) == before);
}

TEST_CASE("ParametricEq default and disabled bands are identity",
          "[signal][parametric-eq][parity]") {
    ParametricEq eq;
    REQUIRE(eq.prepare(48000.0f) == ParametricEqPrepareStatus::prepared);
    const std::array bypass{
        ParametricEqBand{ParametricEqBandType::peaking, 1000.0f, 0.0f, 1.0f, false, false}};
    REQUIRE(eq.configure(bypass) == ParametricEqConfigureStatus::configured);
    CHECK(eq.magnitude(440.0) == Approx(1.0).margin(1e-6));
    CHECK(eq.process(0.75f) == Approx(0.75f).margin(1e-6));
}
