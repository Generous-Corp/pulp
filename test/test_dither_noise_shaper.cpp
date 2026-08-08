// Numerical, statistical, spectral-moment, determinism, and RT-contract proof
// for the reusable signed-PCM dither/noise-shaping output stage.

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/dither_noise_shaper.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

namespace {

using pulp::signal::DitherMode;
using pulp::signal::DitherNoiseShaper64;
using pulp::signal::NoiseShapingOrder;

double mean(const std::vector<double>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

double variance(const std::vector<double>& values, double centre) {
    double total = 0.0;
    for (double value : values) {
        const double delta = value - centre;
        total += delta * delta;
    }
    return total / static_cast<double>(values.size());
}

// By Parseval, sum((x[n]-x[n-1])^2) / sum(x[n]^2) is the power-spectrum
// integral weighted by 4 sin^2(w/2). It is an exact spectral high-frequency
// moment computed in the time domain, with no FFT/window/backend ambiguity.
double high_frequency_spectral_moment(const std::vector<double>& values) {
    double difference_power = 0.0;
    double signal_power = 0.0;
    for (std::size_t i = 1; i < values.size(); ++i) {
        const double difference = values[i] - values[i - 1];
        difference_power += difference * difference;
        signal_power += values[i] * values[i];
    }
    return difference_power / signal_power;
}

std::vector<double> quantization_error(NoiseShapingOrder order, std::size_t frames) {
    DitherNoiseShaper64 stage;
    stage.set_bit_depth(10);
    stage.set_dither_mode(DitherMode::none);
    stage.set_noise_shaping_order(order);

    std::vector<double> error(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        // Two incommensurate tones keep the quantiser exercised without using
        // randomness as both stimulus and oracle.
        const double input = 0.217 * std::sin(0.0137 * static_cast<double>(i)) +
                             0.083 * std::sin(0.0371 * static_cast<double>(i));
        error[i] = stage.process(input, i) - input;
    }
    return error;
}

} // namespace

TEST_CASE("Signed PCM quantizer fixes range ties and clipping semantics",
          "[signal][dither][numerical]") {
    constexpr double step = 0.25; // three-bit signed normalised PCM

    REQUIRE(DitherNoiseShaper64::quantize(0.49 * step, 3) == 0.0);
    REQUIRE(DitherNoiseShaper64::quantize(0.5 * step, 3) == step);
    REQUIRE(DitherNoiseShaper64::quantize(-0.5 * step, 3) == -step);
    REQUIRE(DitherNoiseShaper64::quantize(10.0, 3) == 1.0 - step);
    REQUIRE(DitherNoiseShaper64::quantize(-10.0, 3) == -1.0);
    REQUIRE(DitherNoiseShaper64::quantize(0.125, 1) == 0.0); // clamps to 2 bit
    REQUIRE(DitherNoiseShaper64::quantize(0.125, 99) == 0.125);
}

TEST_CASE("Coordinate TPDF is deterministic lane-separated and triangular",
          "[signal][dither][statistics]") {
    constexpr std::size_t frames = 262144;
    DitherNoiseShaper64 first;
    DitherNoiseShaper64 second;
    first.set_seed(0x123456789ABCDEFull);
    second.set_seed(0x123456789ABCDEFull);

    // Golden bits make the coordinate contract independent of a second
    // instance executing the same implementation.
    const std::array<std::uint64_t, 4> expected_bits = {
        0xBFAD6F30C5E740F0ull,
        0x3FE1E8382610F908ull,
        0x3FD7527AAFD3A380ull,
        0x3FD27F6C67E3435Eull,
    };
    for (std::size_t i = 0; i < expected_bits.size(); ++i)
        REQUIRE(std::bit_cast<std::uint64_t>(first.tpdf_lsb(i, 3u)) == expected_bits[i]);

    std::vector<double> draws;
    draws.reserve(frames);
    std::size_t centre_count = 0;
    std::size_t edge_count = 0;
    bool lane_differs = false;
    for (std::size_t i = 0; i < frames; ++i) {
        const double draw = first.tpdf_lsb(i, 3u);
        REQUIRE(draw == second.tpdf_lsb(i, 3u));
        REQUIRE(draw > -1.0);
        REQUIRE(draw < 1.0);
        draws.push_back(draw);
        if (std::fabs(draw) < 0.25)
            ++centre_count;
        if (std::fabs(draw) > 0.75)
            ++edge_count;
        lane_differs = lane_differs || draw != first.tpdf_lsb(i, 4u);
    }

    const double measured_mean = mean(draws);
    const double measured_variance = variance(draws, measured_mean);
    REQUIRE(std::fabs(measured_mean) < 0.003);
    REQUIRE(measured_variance > 0.163);
    REQUIRE(measured_variance < 0.170);     // analytic TPDF variance is 1/6
    REQUIRE(centre_count > edge_count * 4); // triangular, not uniform
    REQUIRE(lane_differs);
}

TEST_CASE("Noise shaping moves quantization error toward high frequencies",
          "[signal][dither][spectral]") {
    constexpr std::size_t frames = 131072;
    const auto unshaped = quantization_error(NoiseShapingOrder::none, frames);
    const auto first = quantization_error(NoiseShapingOrder::first, frames);
    const auto second = quantization_error(NoiseShapingOrder::second, frames);

    const double unshaped_moment = high_frequency_spectral_moment(unshaped);
    const double first_moment = high_frequency_spectral_moment(first);
    const double second_moment = high_frequency_spectral_moment(second);
    INFO(unshaped_moment);
    INFO(first_moment);
    INFO(second_moment);
    REQUIRE(first_moment > unshaped_moment * 1.25);
    REQUIRE(second_moment > first_moment * 1.10);
}

TEST_CASE("Reset overload and hostile inputs leave bounded recoverable state",
          "[signal][dither][state]") {
    DitherNoiseShaper64 stage;
    stage.set_bit_depth(8);
    stage.set_noise_shaping_order(NoiseShapingOrder::second);
    stage.set_dither_mode(DitherMode::none);

    for (std::uint64_t i = 0; i < 1000; ++i) {
        (void)stage.process(0.1234, i);
        REQUIRE(std::fabs(stage.error_state_1()) <= stage.step() * 0.5);
        REQUIRE(std::fabs(stage.error_state_2()) <= stage.step() * 0.5);
    }

    REQUIRE(stage.process(20.0, 1001) == stage.maximum_code());
    REQUIRE(stage.error_state_1() == 0.0);
    REQUIRE(stage.error_state_2() == 0.0);
    REQUIRE(stage.process(std::numeric_limits<double>::quiet_NaN(), 1002) == 0.0);
    REQUIRE(stage.process(std::numeric_limits<double>::infinity(), 1003) == stage.maximum_code());
    REQUIRE(stage.process(-std::numeric_limits<double>::infinity(), 1004) == stage.minimum_code());

    const double first = stage.process(0.1234, 2000);
    stage.reset();
    REQUIRE(stage.process(0.1234, 2000) == first);
}

TEST_CASE("Coordinate processing is exactly block-partition invariant",
          "[signal][dither][determinism]") {
    constexpr std::size_t frames = 4096;
    constexpr std::uint64_t start = 987654321u;
    std::array<double, frames> input{};
    std::array<double, frames> whole{};
    std::array<double, frames> partitioned{};
    for (std::size_t i = 0; i < frames; ++i)
        input[i] = 0.6 * std::sin(0.021 * static_cast<double>(i));

    DitherNoiseShaper64 a;
    DitherNoiseShaper64 b;
    a.set_bit_depth(12);
    b.set_bit_depth(12);
    a.set_seed(77u);
    b.set_seed(77u);
    a.set_noise_shaping_order(NoiseShapingOrder::second);
    b.set_noise_shaping_order(NoiseShapingOrder::second);

    a.process_block(input.data(), whole.data(), frames, start, 2u);
    constexpr std::size_t split1 = 37;
    constexpr std::size_t split2 = 2031;
    b.process_block(input.data(), partitioned.data(), split1, start, 2u);
    b.process_block(input.data() + split1, partitioned.data() + split1, split2 - split1,
                    start + split1, 2u);
    b.process_block(input.data() + split2, partitioned.data() + split2, frames - split2,
                    start + split2, 2u);
    REQUIRE(partitioned == whole);

    // The same block API must be safe when input and output alias.
    auto in_place = input;
    DitherNoiseShaper64 c;
    c.set_bit_depth(12);
    c.set_seed(77u);
    c.set_noise_shaping_order(NoiseShapingOrder::second);
    c.process_block(in_place.data(), in_place.data(), frames, start, 2u);
    REQUIRE(in_place == whole);
}

TEST_CASE("Dither and noise shaping processing performs no allocation", "[signal][dither][rt]") {
    DitherNoiseShaper64 stage;
    stage.set_bit_depth(16);
    stage.set_noise_shaping_order(NoiseShapingOrder::second);
    std::array<double, 512> input{};
    std::array<double, 512> output{};

    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        stage.process_block(input.data(), output.data(), input.size(), 4000u, 1u);
        stage.reset();
        stage.set_dither_mode(DitherMode::none);
        stage.set_noise_shaping_order(NoiseShapingOrder::first);
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}
