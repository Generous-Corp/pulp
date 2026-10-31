#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <limits>

#include <pulp/signal/gain.hpp>

TEST_CASE("Gain ramp is opt-in and reaches its target", "[signal][gain][rt-safety]") {
    pulp::signal::Gain gain;
    gain.set_gain_linear(1.0f);
    gain.set_ramp_time(4.0f / 48000.0f, 48000.0f);
    gain.set_gain_linear(2.0f);

    std::array<float, 4> samples{1.0f, 1.0f, 1.0f, 1.0f};
    gain.process(samples.data(), static_cast<int>(samples.size()));
    REQUIRE(samples[0] == 1.25f);
    REQUIRE(samples[1] == 1.5f);
    REQUIRE(samples[2] == 1.75f);
    REQUIRE(samples[3] == 2.0f);
    REQUIRE(gain.gain_linear() == 2.0f);
}

TEST_CASE("Gain setters remain immediate before ramp opt-in", "[signal][gain]") {
    pulp::signal::Gain gain;
    gain.set_gain_db(6.0205999f);
    REQUIRE_THAT(gain.gain_linear(), Catch::Matchers::WithinAbs(2.0f, 1.0e-5f));
    REQUIRE_THAT(gain.process(0.5f), Catch::Matchers::WithinAbs(1.0f, 1.0e-5f));
}

TEST_CASE("Gain ramp rejects non-finite and invalid timing inputs", "[signal][gain][robustness]") {
    for (const float seconds : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity()}) {
        pulp::signal::Gain gain;
        gain.set_ramp_time(seconds, 48000.0f);
        gain.set_gain_linear(2.0f);
        REQUIRE(gain.process(1.0f) == 2.0f);
    }
    pulp::signal::Gain bad_rate;
    bad_rate.set_ramp_time(0.05f, 0.0f);
    bad_rate.set_gain_linear(2.0f);
    REQUIRE(bad_rate.process(1.0f) == 2.0f);
}

TEST_CASE("Gain ramp clamps timing before integer conversion", "[signal][gain][robustness]") {
    pulp::signal::Gain gain;
    gain.set_ramp_time(std::numeric_limits<float>::max(), 48000.0f);
    gain.set_gain_linear(2.0f);
    REQUIRE(gain.gain_linear() == 1.0f);
    REQUIRE(gain.process(1.0f) == 1.0f);
    REQUIRE(gain.gain_linear() == 1.0f);
}

TEST_CASE("Gain ramp clamps the float-rounded INT_MAX boundary", "[signal][gain][robustness]") {
    pulp::signal::Gain gain;
    gain.set_ramp_time(static_cast<float>((std::numeric_limits<int>::max)()), 1.0f);
    gain.set_gain_linear(2.0f);
    REQUIRE(gain.process(1.0f) == 1.0f);
    REQUIRE(gain.gain_linear() == 1.0f);
}
