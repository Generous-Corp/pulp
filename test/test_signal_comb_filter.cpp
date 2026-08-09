#include <pulp/signal/comb_filter.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::CombFilterMode;
using pulp::signal::CombFilterStatus;
using pulp::signal::CombFilterT;

namespace {
std::vector<double> impulse_response(CombFilterMode mode, std::size_t delay, double gain,
                                     std::size_t count) {
    CombFilterT<double> comb;
    REQUIRE(comb.prepare(64));
    REQUIRE(comb.configure({mode, delay, gain}));
    std::vector<double> output(count);
    for (std::size_t n = 0; n < count; ++n) {
        const auto result = comb.process(n == 0u ? 1.0 : 0.0);
        REQUIRE(result);
        output[n] = result.sample;
    }
    return output;
}

std::complex<double> measured_response(const std::vector<double>& impulse, double omega) {
    std::complex<double> response{};
    for (std::size_t n = 0; n < impulse.size(); ++n)
        response += impulse[n] * std::polar(1.0, -omega * static_cast<double>(n));
    return response;
}

std::complex<double> independent_response(CombFilterMode mode, std::size_t delay, double gain,
                                          double omega) {
    const auto z_delay = std::polar(1.0, -omega * static_cast<double>(delay));
    if (mode == CombFilterMode::feedforward)
        return 1.0 + gain * z_delay;
    if (mode == CombFilterMode::feedback)
        return 1.0 / (1.0 - gain * z_delay);
    return (z_delay - gain) / (1.0 - gain * z_delay);
}
} // namespace

TEST_CASE("comb modes match independent impulse recurrences", "[signal][comb-filter]") {
    constexpr std::size_t delay = 5;
    constexpr double gain = 0.4;
    const auto feedforward = impulse_response(CombFilterMode::feedforward, delay, gain, 24);
    for (std::size_t n = 0; n < feedforward.size(); ++n)
        CHECK_THAT(feedforward[n], WithinAbs(n == 0 ? 1.0 : (n == delay ? gain : 0.0), 0.0));

    const auto feedback = impulse_response(CombFilterMode::feedback, delay, gain, 31);
    for (std::size_t n = 0; n < feedback.size(); ++n) {
        const auto expected = n % delay == 0 ? std::pow(gain, static_cast<double>(n / delay)) : 0.0;
        CHECK_THAT(feedback[n], WithinAbs(expected, 2.0e-15));
    }

    const auto allpass = impulse_response(CombFilterMode::allpass, delay, gain, 31);
    for (std::size_t n = 0; n < allpass.size(); ++n) {
        auto expected = 0.0;
        if (n == 0)
            expected = -gain;
        else if (n % delay == 0)
            expected = (1.0 - gain * gain) * std::pow(gain, static_cast<double>(n / delay - 1u));
        CHECK_THAT(allpass[n], WithinAbs(expected, 2.0e-15));
    }
}

TEST_CASE("comb measured frequency response matches independent equations",
          "[signal][comb-filter][response]") {
    constexpr std::array modes{CombFilterMode::feedforward, CombFilterMode::feedback,
                               CombFilterMode::allpass};
    constexpr std::array delays{2u, 7u, 19u};
    constexpr std::array gains{-0.72, -0.2, 0.0, 0.35, 0.77};
    constexpr std::array omegas{0.0, 0.013, 0.11, 0.39, 0.83, 1.7, 2.8,
                                std::numbers::pi};
    for (const auto mode : modes)
        for (const auto delay : delays)
            for (const auto gain : gains) {
                const auto impulse = impulse_response(mode, delay, gain, 4096);
                for (const auto omega : omegas) {
                    const auto oracle = independent_response(mode, delay, gain, omega);
                    const auto measured = measured_response(impulse, omega);
                    CHECK_THAT(std::abs(measured - oracle), WithinAbs(0.0, 2.0e-11));
                    CHECK_THAT(std::abs(pulp::signal::comb_filter_response(mode, delay, gain, omega) - oracle),
                               WithinAbs(0.0, 2.0e-15));
                    if (mode == CombFilterMode::allpass)
                        CHECK_THAT(std::abs(measured), WithinAbs(1.0, 2.0e-11));
                }
            }
}

TEST_CASE("comb response oracle rejects a planted feedback-sign mutation",
          "[signal][comb-filter][negative-control]") {
    constexpr std::size_t delay = 11;
    constexpr double gain = 0.73;
    constexpr double omega = 0.37;
    const auto measured = measured_response(impulse_response(CombFilterMode::feedback, delay, gain, 4096), omega);
    const auto delayed = std::polar(1.0, -omega * static_cast<double>(delay));
    const auto correct = 1.0 / (1.0 - gain * delayed);
    const auto planted_wrong_sign = 1.0 / (1.0 + gain * delayed);
    CHECK(std::abs(measured - correct) < 1.0e-12);
    CHECK(std::abs(measured - planted_wrong_sign) > 0.5);
}

TEST_CASE("comb preparation and configuration are transactional", "[signal][comb-filter][contract]") {
    CombFilterT<double> comb;
    CHECK_FALSE(comb.prepared());
    CHECK_FALSE(comb.configured());
    CHECK(comb.process(1.0).status == CombFilterStatus::not_prepared);
    REQUIRE(comb.prepare(2));
    REQUIRE(comb.configure({CombFilterMode::feedforward, 2u, 0.5}));
    REQUIRE(comb.process(1.0));
    REQUIRE(comb.process(0.0));
    CHECK(comb.process(0.0).sample == 0.5);
    REQUIRE(comb.prepare(16));
    CHECK(comb.process(1.0).status == CombFilterStatus::not_configured);
    REQUIRE(comb.configure({CombFilterMode::feedback, 4u, 0.5}));
    REQUIRE(comb.process(1.0));
    for (int n = 0; n < 3; ++n)
        REQUIRE(comb.process(0.0));
    CHECK_FALSE(comb.prepare(std::numeric_limits<std::size_t>::max()));
    CHECK(comb.prepared());
    CHECK(comb.configured());
    CHECK(comb.maximum_delay_samples() == 16u);
    CHECK(comb.process(0.0).sample == 0.5);
    const auto retained = comb.config();
    CHECK_FALSE(comb.configure({CombFilterMode::feedback, 17u, 0.2}));
    CHECK_FALSE(comb.configure({CombFilterMode::feedback, 4u, 1.0}));
    CHECK_FALSE(comb.configure({CombFilterMode::feedforward, 4u, std::numeric_limits<double>::quiet_NaN()}));
    CHECK(comb.config().mode == retained.mode);
    CHECK(comb.config().delay_samples == retained.delay_samples);
    CHECK(comb.config().gain == retained.gain);
}

TEST_CASE("comb block processing is partition and exact in-place invariant", "[signal][comb-filter][block]") {
    constexpr std::size_t count = 1024;
    std::array<double, count> input{};
    for (std::size_t n = 0; n < count; ++n)
        input[n] = 0.4 * std::sin(0.071 * static_cast<double>(n)) + 0.2 * std::cos(0.013 * static_cast<double>(n));
    const auto render = [&](bool partitioned, bool in_place) {
        CombFilterT<double> comb;
        REQUIRE(comb.prepare(32));
        REQUIRE(comb.configure({CombFilterMode::allpass, 13u, -0.61}));
        auto output = input;
        if (!in_place)
            output.fill(0.0);
        if (partitioned) {
            constexpr std::array<std::size_t, 7> chunks{1u, 7u, 63u, 2u, 127u, 5u, 31u};
            std::size_t offset = 0, chunk_index = 0;
            while (offset < count) {
                const auto frames = std::min(chunks[chunk_index++ % chunks.size()], count - offset);
                const auto* source = in_place ? output.data() + offset : input.data() + offset;
                REQUIRE(comb.process(source, output.data() + offset, frames));
                offset += frames;
            }
        } else {
            const auto* source = in_place ? output.data() : input.data();
            REQUIRE(comb.process(source, output.data(), count));
        }
        return output;
    };
    const auto whole = render(false, false);
    CHECK(render(true, false) == whole);
    CHECK(render(false, true) == whole);
    CHECK(render(true, true) == whole);
}

TEST_CASE("comb reset, faults, latency, and tail are explicit", "[signal][comb-filter][fault]") {
    CombFilterT<double> comb;
    REQUIRE(comb.prepare(16));
    REQUIRE(comb.configure({CombFilterMode::feedforward, 4u, 0.5}));
    CHECK(comb.processing_latency_samples() == 0u);
    CHECK(comb.tail_samples() == 4u);
    REQUIRE(comb.process(1.0));
    const auto fault = comb.process(std::numeric_limits<double>::quiet_NaN());
    CHECK(fault.status == CombFilterStatus::non_finite_input);
    CHECK(fault.sample == 0.0);
    CHECK(comb.fault_count() == 1u);
    for (int n = 0; n < 8; ++n)
        CHECK(comb.process(0.0).sample == 0.0);
    REQUIRE(comb.configure({CombFilterMode::feedforward, 4u, 1.0}));
    REQUIRE(comb.process(std::numeric_limits<double>::max()));
    for (int n = 0; n < 3; ++n)
        REQUIRE(comb.process(0.0));
    CHECK(comb.process(std::numeric_limits<double>::max()).status == CombFilterStatus::output_out_of_range);
    for (int n = 0; n < 8; ++n)
        CHECK(comb.process(0.0).sample == 0.0);
    REQUIRE(comb.configure({CombFilterMode::feedback, 4u, 0.5}));
    CHECK_FALSE(comb.tail_samples().has_value());
    REQUIRE(comb.configure({CombFilterMode::feedback, 4u, 0.0}));
    CHECK(comb.tail_samples() == 0u);
    REQUIRE(comb.configure({CombFilterMode::allpass, 4u, 0.0}));
    CHECK(comb.tail_samples() == 4u);
    comb.reset();
    CHECK(comb.fault_count() == 0u);
}

TEST_CASE("comb post-prepare operations allocate nothing", "[signal][comb-filter][rt]") {
    CombFilterT<float> comb;
    REQUIRE(comb.prepare(128));
    REQUIRE(comb.configure({CombFilterMode::feedback, 37u, 0.75}));
    std::array<float, 512> samples{};
    samples[0] = 1.0f;
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(comb.process(samples.data(), samples.data(), samples.size()));
        REQUIRE(comb.configure({CombFilterMode::allpass, 29u, -0.4}));
        comb.reset();
        CHECK(comb.process(std::numeric_limits<float>::infinity()).status == CombFilterStatus::non_finite_input);
        CHECK(probe.allocation_count() == 0u);
    }
}

TEST_CASE("comb block arguments fail closed", "[signal][comb-filter][contract]") {
    CombFilterT<float> comb;
    CHECK(comb.process(nullptr, nullptr, 0u));
    CHECK(comb.process(nullptr, nullptr, 4u).status == CombFilterStatus::invalid_argument);
    std::array<float, 4> samples{};
    CHECK(comb.process(samples.data(), samples.data(), samples.size()).status == CombFilterStatus::not_prepared);
    REQUIRE(comb.prepare(8));
    CHECK(comb.process(samples.data(), samples.data(), samples.size()).status == CombFilterStatus::not_configured);
}
