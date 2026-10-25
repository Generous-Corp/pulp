// Host timeline sample position (a double from the AU transport callbacks)
// → integer sample. Pins the regression where REAPER's 13823.9999 truncated
// to 13823, so the position stepped back one sample and then jumped two.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/detail/host_sample_position.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

using pulp::format::detail::host_sample_position;

TEST_CASE("host sample position rounds to the nearest sample", "[format][au][transport]") {
    CHECK(host_sample_position(13823.9999) == std::int64_t{13824});
    CHECK(host_sample_position(13824.0001) == std::int64_t{13824});
    CHECK(host_sample_position(13824.0) == std::int64_t{13824});
    CHECK(host_sample_position(0.0) == std::int64_t{0});
    CHECK(host_sample_position(-0.4) == std::int64_t{0});
    CHECK(host_sample_position(-1.6) == std::int64_t{-2});
    // A half sample rounds away from zero whatever the rounding mode.
    CHECK(host_sample_position(2.5) == std::int64_t{3});
    CHECK(host_sample_position(-2.5) == std::int64_t{-3});
}

TEST_CASE("host sample position stays monotonic across slightly-low host values",
          "[format][au][transport]") {
    // A host reporting every block start a hair low, as REAPER does.
    constexpr int kBlock = 512;
    std::int64_t previous = -1;
    for (int block = 1; block < 64; ++block) {
        const double reported = block * kBlock - 1.0e-4;
        const auto position = host_sample_position(reported);
        REQUIRE(position.has_value());
        CHECK(*position == std::int64_t{block} * kBlock);
        CHECK(*position - previous == (previous < 0 ? *position + 1 : kBlock));
        previous = *position;
    }
}

TEST_CASE("host sample position is unset for values it cannot represent",
          "[format][au][transport]") {
    CHECK_FALSE(host_sample_position(std::numeric_limits<double>::quiet_NaN()).has_value());
    CHECK_FALSE(host_sample_position(std::numeric_limits<double>::infinity()).has_value());
    CHECK_FALSE(host_sample_position(-std::numeric_limits<double>::infinity()).has_value());
    CHECK_FALSE(host_sample_position(9.3e18).has_value());
    CHECK_FALSE(host_sample_position(-9.3e18).has_value());
    // The largest double below 2^63 still fits.
    const double below = std::nextafter(9223372036854775808.0, 0.0);
    REQUIRE(host_sample_position(below).has_value());
    CHECK(*host_sample_position(below) == static_cast<std::int64_t>(below));
    CHECK(host_sample_position(-9223372036854775808.0) == std::numeric_limits<std::int64_t>::min());
}
