#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/nway_crossfade.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::CrossfadeGainLaw;
using pulp::signal::NWayCrossfadePrepareError;
using pulp::signal::NWayCrossfadeProcessStatus;
using Crossfade = pulp::signal::NWayCrossfadeT<double, 8>;
using Plan = Crossfade::Plan;

namespace {

std::array<std::span<const double>, 4>
views(const std::array<std::vector<double>, 4>& inputs) {
    return {std::span<const double>{inputs[0]}, std::span<const double>{inputs[1]},
            std::span<const double>{inputs[2]}, std::span<const double>{inputs[3]}};
}

double scalar_oracle(const std::array<std::vector<double>, 4>& inputs,
                     std::size_t frame, double position, CrossfadeGainLaw law) {
    const double clamped = std::clamp(position, 0.0, 3.0);
    const auto left = static_cast<std::size_t>(clamped);
    if (left == 3) return inputs[3][frame];
    const double u = clamped - static_cast<double>(left);
    if (law == CrossfadeGainLaw::EqualGain)
        return inputs[left][frame] * (1.0 - u) + inputs[left + 1][frame] * u;
    constexpr long double half_pi = 1.57079632679489661923L;
    return inputs[left][frame] * static_cast<double>(std::cos(u * half_pi)) +
           inputs[left + 1][frame] * static_cast<double>(std::sin(u * half_pi));
}

}  // namespace

TEST_CASE("N-way crossfade has exact ordered endpoints and two-way parity",
          "[signal][nway-crossfade]") {
    for (const auto law : {CrossfadeGainLaw::EqualGain, CrossfadeGainLaw::EqualPower}) {
        for (std::size_t endpoint = 0; endpoint < 5; ++endpoint) {
            Plan plan;
            REQUIRE(plan.prepare(5, static_cast<double>(endpoint), law) ==
                    NWayCrossfadePrepareError::None);
            for (std::size_t path = 0; path < 5; ++path)
                REQUIRE(plan.weights()[path] == (path == endpoint ? 1.0 : 0.0));
        }

        Plan two_way;
        REQUIRE(two_way.prepare(2, 0.37, law) == NWayCrossfadePrepareError::None);
        double old_gain = 0.0;
        double new_gain = 0.0;
        pulp::signal::crossfade_gains(0.37, law, old_gain, new_gain);
        REQUIRE(two_way.weights()[0] == old_gain);
        REQUIRE(two_way.weights()[1] == new_gain);
    }

    Plan clamped;
    REQUIRE(clamped.prepare(4, -20.0, CrossfadeGainLaw::EqualGain) ==
            NWayCrossfadePrepareError::None);
    REQUIRE(clamped.position() == 0.0);
    REQUIRE(clamped.weights()[0] == 1.0);
    REQUIRE(clamped.prepare(4, 20.0, CrossfadeGainLaw::EqualGain) ==
            NWayCrossfadePrepareError::None);
    REQUIRE(clamped.position() == 3.0);
    REQUIRE(clamped.weights()[3] == 1.0);
}

TEST_CASE("N-way crossfade laws obey their declared normalization and headroom",
          "[signal][nway-crossfade]") {
    for (int step = 0; step <= 100; ++step) {
        const double position = 3.0 * static_cast<double>(step) / 100.0;
        Plan equal_gain;
        Plan equal_power;
        REQUIRE(equal_gain.prepare(4, position, CrossfadeGainLaw::EqualGain) ==
                NWayCrossfadePrepareError::None);
        REQUIRE(equal_power.prepare(4, position, CrossfadeGainLaw::EqualPower) ==
                NWayCrossfadePrepareError::None);
        double l1 = 0.0;
        double l2_squared = 0.0;
        for (const double weight : equal_gain.weights()) l1 += weight;
        for (const double weight : equal_power.weights()) l2_squared += weight * weight;
        REQUIRE_THAT(l1, WithinAbs(1.0, 1e-12));
        REQUIRE_THAT(l2_squared, WithinAbs(1.0, 1e-12));
        double correlated_gain = 0.0;
        for (const double weight : equal_power.weights()) correlated_gain += weight;
        REQUIRE(correlated_gain <= std::sqrt(2.0) + 1e-12);
    }
}

TEST_CASE("N-way crossfade agrees with an independent scalar oracle",
          "[signal][nway-crossfade]") {
    constexpr std::size_t frames = 257;
    std::array<std::vector<double>, 4> inputs;
    for (std::size_t path = 0; path < inputs.size(); ++path) {
        inputs[path].resize(frames);
        for (std::size_t frame = 0; frame < frames; ++frame)
            inputs[path][frame] = std::sin(0.017 * static_cast<double>((path + 1) * (frame + 3))) +
                                          static_cast<double>(path) * 0.25;
    }
    const auto input_views = views(inputs);
    for (const auto law : {CrossfadeGainLaw::EqualGain, CrossfadeGainLaw::EqualPower}) {
        Crossfade mixer;
        constexpr double position = 1.375;
        REQUIRE(mixer.configure(4, position, law) == NWayCrossfadePrepareError::None);
        std::vector<double> output(frames);
        REQUIRE(mixer.process(input_views, output, frames) == NWayCrossfadeProcessStatus::Ok);
        for (std::size_t frame = 0; frame < frames; ++frame)
            REQUIRE_THAT(output[frame], WithinAbs(scalar_oracle(inputs, frame, position, law),
                                                  2e-15));
    }
}

TEST_CASE("Equal-gain N-way crossfade reconstructs identical paths",
          "[signal][nway-crossfade]") {
    constexpr std::size_t frames = 113;
    std::array<std::vector<double>, 4> inputs;
    for (auto& input : inputs) {
        input.resize(frames);
        for (std::size_t i = 0; i < frames; ++i)
            input[i] = std::cos(0.031 * static_cast<double>(i));
    }
    Crossfade mixer;
    REQUIRE(mixer.configure(4, 2.42, CrossfadeGainLaw::EqualGain) ==
            NWayCrossfadePrepareError::None);
    std::vector<double> output(frames);
    const auto input_views = views(inputs);
    REQUIRE(mixer.process(input_views, output, frames) == NWayCrossfadeProcessStatus::Ok);
    for (std::size_t i = 0; i < frames; ++i)
        REQUIRE_THAT(output[i], WithinAbs(inputs[0][i], 2e-15));
}

TEST_CASE("N-way crossfade is deterministic across block partitions",
          "[signal][nway-crossfade]") {
    constexpr std::size_t frames = 193;
    std::array<std::vector<double>, 4> inputs;
    for (std::size_t path = 0; path < inputs.size(); ++path) {
        inputs[path].resize(frames);
        for (std::size_t i = 0; i < frames; ++i)
            inputs[path][i] = static_cast<double>((path + 2) * (i % 17)) / 19.0;
    }
    Crossfade whole;
    Crossfade split;
    REQUIRE(whole.configure(4, 0.625, CrossfadeGainLaw::EqualPower) ==
            NWayCrossfadePrepareError::None);
    REQUIRE(split.configure(4, 0.625, CrossfadeGainLaw::EqualPower) ==
            NWayCrossfadePrepareError::None);
    std::vector<double> expected(frames);
    std::vector<double> actual(frames);
    const auto all_views = views(inputs);
    REQUIRE(whole.process(all_views, expected, frames) == NWayCrossfadeProcessStatus::Ok);

    constexpr std::array<std::size_t, 4> chunks{1, 31, 64, 97};
    std::size_t offset = 0;
    for (const auto chunk : chunks) {
        std::array<std::span<const double>, 4> chunk_views;
        for (std::size_t path = 0; path < inputs.size(); ++path)
            chunk_views[path] = std::span<const double>{inputs[path]}.subspan(offset, chunk);
        REQUIRE(split.process(chunk_views, std::span<double>{actual}.subspan(offset, chunk), chunk) ==
                NWayCrossfadeProcessStatus::Ok);
        offset += chunk;
    }
    REQUIRE(actual == expected);
}

TEST_CASE("N-way crossfade preparation is closed and publication is transactional",
          "[signal][nway-crossfade]") {
    Plan plan;
    REQUIRE(plan.prepare(1, 0.0, CrossfadeGainLaw::EqualGain) ==
            NWayCrossfadePrepareError::TooFewPaths);
    REQUIRE(plan.prepare(9, 0.0, CrossfadeGainLaw::EqualGain) ==
            NWayCrossfadePrepareError::TooManyPaths);
    REQUIRE(plan.prepare(4, std::numeric_limits<double>::quiet_NaN(),
                         CrossfadeGainLaw::EqualGain) ==
            NWayCrossfadePrepareError::NonFinitePosition);
    REQUIRE(plan.prepare(4, 0.0, static_cast<CrossfadeGainLaw>(99)) ==
            NWayCrossfadePrepareError::InvalidGainLaw);
    REQUIRE_FALSE(plan.prepared());

    Crossfade mixer;
    REQUIRE(mixer.configure(4, 1.0, CrossfadeGainLaw::EqualGain) ==
            NWayCrossfadePrepareError::None);
    REQUIRE(mixer.configure(9, 0.0, CrossfadeGainLaw::EqualGain) ==
            NWayCrossfadePrepareError::TooManyPaths);
    std::array<std::vector<double>, 4> inputs;
    for (std::size_t path = 0; path < inputs.size(); ++path)
        inputs[path] = std::vector<double>(3, static_cast<double>(path + 1));
    auto input_views = views(inputs);
    std::array<double, 3> output{};
    REQUIRE(mixer.process(input_views, output, output.size()) == NWayCrossfadeProcessStatus::Ok);
    REQUIRE(output == std::array<double, 3>{2.0, 2.0, 2.0});
}

TEST_CASE("N-way crossfade fails closed before modifying output",
          "[signal][nway-crossfade]") {
    Crossfade unprepared;
    std::array<double, 4> storage{};
    std::array<std::span<const double>, 1> one_input{std::span<const double>{storage}};
    std::array<double, 4> output{7.0, 7.0, 7.0, 7.0};
    REQUIRE(unprepared.process(one_input, output, output.size()) ==
            NWayCrossfadeProcessStatus::Unprepared);
    REQUIRE(output == std::array<double, 4>{7.0, 7.0, 7.0, 7.0});

    Crossfade mixer;
    REQUIRE(mixer.configure(2, 0.5, CrossfadeGainLaw::EqualGain) ==
            NWayCrossfadePrepareError::None);
    REQUIRE(mixer.process(one_input, output, output.size()) ==
            NWayCrossfadeProcessStatus::InsufficientInputs);
    std::array<std::span<const double>, 2> short_input{
        std::span<const double>{storage}.first(3), std::span<const double>{storage}};
    REQUIRE(mixer.process(short_input, output, output.size()) ==
            NWayCrossfadeProcessStatus::ShortInput);
    REQUIRE(mixer.process(short_input, std::span<double>{output}.first(2), 3) ==
            NWayCrossfadeProcessStatus::ShortOutput);
    std::array<std::span<const double>, 2> shifted_overlap{
        std::span<const double>{output}.first(3), std::span<const double>{storage}.first(3)};
    REQUIRE(mixer.process(shifted_overlap, std::span<double>{output}.subspan(1, 3), 3) ==
            NWayCrossfadeProcessStatus::OverlappingBuffers);
    REQUIRE(output == std::array<double, 4>{7.0, 7.0, 7.0, 7.0});
}

TEST_CASE("N-way crossfade process performs no allocation",
          "[signal][nway-crossfade][rt]") {
    constexpr std::size_t frames = 128;
    Crossfade mixer;
    REQUIRE(mixer.configure(4, 1.75, CrossfadeGainLaw::EqualPower) ==
            NWayCrossfadePrepareError::None);
    std::array<std::array<double, frames>, 4> inputs{};
    std::array<std::span<const double>, 4> input_views;
    for (std::size_t path = 0; path < inputs.size(); ++path) {
        inputs[path].fill(static_cast<double>(path));
        input_views[path] = inputs[path];
    }
    std::array<double, frames> output{};
    pulp::test::RtAllocationProbe probe;
    for (int iteration = 0; iteration < 1000; ++iteration)
        REQUIRE(mixer.process(input_views, output, frames) == NWayCrossfadeProcessStatus::Ok);
    REQUIRE_FALSE(probe.saw_allocation());
}
