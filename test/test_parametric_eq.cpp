#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/parametric_eq.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace pulp::signal;
using Catch::Approx;

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

TEST_CASE("ParametricEq block processing matches the sample path through a crossfade",
          "[signal][parametric-eq][determinism]") {
    const std::array first{
        ParametricEqBand{ParametricEqBandType::low_shelf, 120.0f, 4.0f, 0.707f, true, false},
        ParametricEqBand{ParametricEqBandType::peaking, 1000.0f, -6.0f, 2.0f, true, false},
        ParametricEqBand{ParametricEqBandType::high_shelf, 8000.0f, 3.0f, 0.707f, true, false},
    };
    const std::array second{
        ParametricEqBand{ParametricEqBandType::peaking, 300.0f, 6.0f, 1.0f, true, false},
        ParametricEqBand{ParametricEqBandType::peaking, 2500.0f, -3.0f, 4.0f, true, false},
    };
    std::vector<float> input(4096);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = 0.6f * std::sin(0.0371f * static_cast<float>(i)) +
                   0.3f * std::sin(0.4113f * static_cast<float>(i));
    input[2500] = std::numeric_limits<float>::quiet_NaN();

    ParametricEq by_block;
    ParametricEq by_sample;
    REQUIRE(by_block.prepare(48000.0f, 4) == ParametricEqPrepareStatus::prepared);
    REQUIRE(by_sample.prepare(48000.0f, 4) == ParametricEqPrepareStatus::prepared);
    REQUIRE(by_block.configure(first) == ParametricEqConfigureStatus::configured);
    REQUIRE(by_sample.configure(first) == ParametricEqConfigureStatus::configured);

    auto a = input;
    auto b = input;
    // A 300-sample crossfade starts at 1000 and ends inside the 1024..2047
    // block, so that block mixes the sample and block paths.
    for (std::size_t start = 0; start < a.size(); start += 1024) {
        if (start == 1024) {
            REQUIRE(by_block.configure(second, 300) == ParametricEqConfigureStatus::configured);
            REQUIRE(by_sample.configure(second, 300) == ParametricEqConfigureStatus::configured);
        }
        REQUIRE(by_block.process_block(a.data() + start, 1024));
        for (std::size_t i = start; i < start + 1024; ++i)
            b[i] = by_sample.process(b[i]);
    }
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
    for (float x : a)
        CHECK(std::isfinite(x));
}
