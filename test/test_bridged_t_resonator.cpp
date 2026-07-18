#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/bridged_t_resonator.hpp>

#include <algorithm>
#include <cmath>

namespace {

using pulp::signal::BridgedTComponents;
using pulp::signal::BridgedTResonator;
using pulp::signal::Q43Leakage;

}  // namespace

TEST_CASE("bridged-T nominal component values produce the expected topology",
          "[bridged-t][topology]") {
    const BridgedTComponents components{};
    const double series = components.r165 + components.r166;
    const double shunt =
        pulp::signal::bridged_t_shunt_resistance(series, components);

    REQUIRE(shunt == Catch::Approx(46.05e3).epsilon(0.001));
    REQUIRE(pulp::signal::bridged_t_center_frequency(shunt, components) ==
            Catch::Approx(49.44).epsilon(0.02));
    REQUIRE(pulp::signal::bridged_t_q(shunt, components) ==
            Catch::Approx(2.33).epsilon(0.01));
}

TEST_CASE("bridged-T attack shunt raises centre frequency",
          "[bridged-t][topology]") {
    const BridgedTComponents components{};
    const double normal = pulp::signal::bridged_t_center_frequency(
        pulp::signal::bridged_t_shunt_resistance(
            components.r165 + components.r166, components),
        components);
    const double attack = pulp::signal::bridged_t_center_frequency(
        pulp::signal::bridged_t_shunt_resistance(components.r166, components),
        components);

    REQUIRE(attack == Catch::Approx(130.03).epsilon(0.02));
    REQUIRE(std::log2(attack / normal) > 1.0);
}

TEST_CASE("Q43 leakage stays monotonic and physical",
          "[bridged-t][topology]") {
    const BridgedTComponents components{};
    const Q43Leakage leakage{};

    double previous = pulp::signal::q43_branch_resistance(
        -0.1, components, leakage);
    for (double v = -0.2; v > -15.0; v -= 0.05) {
        const double resistance = pulp::signal::q43_branch_resistance(
            v, components, leakage);
        REQUIRE(std::isfinite(resistance));
        REQUIRE(resistance >= components.r166);
        REQUIRE(resistance <= previous);
        previous = resistance;
    }
}

TEST_CASE("bridged-T resonator is bounded and realtime-safe",
          "[bridged-t][rt]") {
    BridgedTResonator resonator;
    resonator.prepare(48000.0);

    bool finite_and_bounded = true;
    std::size_t allocation_count = 0;
    {
        pulp::test::RtAllocationProbe allocation_probe;
        for (int sample = 0; sample < 48000; ++sample) {
            resonator.set_attack_shunt(sample < 240);
            const double excitation = sample == 0 ? 10.0 : 0.0;
            const auto output = resonator.process(excitation, 0.0, 0.0);
            finite_and_bounded = finite_and_bounded &&
                std::isfinite(output.vbt) && std::isfinite(output.vcomm) &&
                std::fabs(output.vbt) <= 15.0;
        }
        allocation_count = allocation_probe.allocation_count();
    }
    REQUIRE(finite_and_bounded);
    REQUIRE(allocation_count == 0);
}
