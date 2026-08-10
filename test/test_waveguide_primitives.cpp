#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/waveguide_junction.hpp>
#include <pulp/signal/waveguide_line.hpp>
#include <pulp/signal/waveguide_reflection_filter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::uint32_t xorshift32(std::uint32_t& state) {
    state ^= state << 13u;
    state ^= state >> 17u;
    state ^= state << 5u;
    return state;
}

double bipolar(std::uint32_t& state) {
    return 2.0 * static_cast<double>(xorshift32(state)) /
               static_cast<double>(std::numeric_limits<std::uint32_t>::max()) -
           1.0;
}

template <std::size_t N>
double weighted_energy(const std::array<double, N>& waves,
                       const std::array<double, N>& impedances, std::size_t count) {
    double energy = 0.0;
    for (std::size_t i = 0; i < count; ++i)
        energy += waves[i] * waves[i] / impedances[i];
    return energy;
}

} // namespace

TEST_CASE("WaveguideLine routes both rails at the requested physical delay",
          "[signal][waveguide][line]") {
    pulp::signal::WaveguideLine64 line;
    REQUIRE(line.prepare(48000.0, 0.01));
    line.set_length_samples(4.0);

    std::array<double, 10> left_outputs{};
    std::array<double, 10> right_outputs{};
    for (std::size_t n = 0; n < left_outputs.size(); ++n) {
        const auto left_input = n == 0 ? 1.0 : 0.0;
        const auto right_input = n == 1 ? -0.5 : 0.0;
        line.process(left_input, right_input, left_outputs[n], right_outputs[n]);
    }
    for (std::size_t n = 0; n < left_outputs.size(); ++n) {
        CHECK(right_outputs[n] == (n == 4 ? 1.0 : 0.0));
        CHECK(left_outputs[n] == (n == 5 ? -0.5 : 0.0));
    }
    CHECK_THAT(line.round_trip_seconds(), WithinAbs(8.0 / 48000.0, 1.0e-15));
}

TEST_CASE("WaveguideLine fractional reads match an independent linear-sequence oracle",
          "[signal][waveguide][line][fractional]") {
    pulp::signal::WaveguideLine64 line;
    REQUIRE(line.prepare(48000.0, 0.01));
    line.set_length_samples(6.25);
    for (int n = 0; n < 40; ++n) {
        double left = 0.0;
        double right = 0.0;
        line.process(static_cast<double>(n + 1), 0.0, left, right);
        if (n >= 12)
            CHECK_THAT(right, WithinAbs(static_cast<double>(n) - 5.25, 2.0e-12));
        CHECK(left == 0.0);
    }
}

TEST_CASE("WaveguideLine fractional interpolation is passive with documented high-band loss",
          "[signal][waveguide][line][fractional][passivity]") {
    constexpr auto frequency = 0.4;
    constexpr auto delay = 6.5;
    pulp::signal::WaveguideLine64 line;
    REQUIRE(line.prepare(48000.0, 0.01));
    line.set_length_samples(delay);

    long double input_energy = 0.0L;
    long double output_energy = 0.0L;
    for (int n = 0; n < 8192; ++n) {
        const auto input = std::sin(2.0 * std::numbers::pi * frequency * n);
        double left = 0.0;
        double output = 0.0;
        line.process(input, 0.0, left, output);
        if (n >= 256) {
            input_energy += static_cast<long double>(input) * input;
            output_energy += static_cast<long double>(output) * output;
        }
    }
    const auto gain = std::sqrt(static_cast<double>(output_energy / input_energy));
    CHECK(gain <= 1.0 + 1.0e-12);
    CHECK_THAT(gain, WithinAbs(0.5436, 5.0e-4));
}

TEST_CASE("WaveguideLine preparation is transactional and retained memory is exact",
          "[signal][waveguide][line][lifecycle]") {
    pulp::signal::WaveguideLine line;
    REQUIRE(line.prepare(48000.0, 0.01));
    line.set_length_samples(8.0f);
    const auto retained = line.retained_bytes();
    CHECK(retained == 2u * (480u + 3u) * sizeof(double));
    CHECK_FALSE(line.prepare(std::numeric_limits<double>::quiet_NaN(), 0.01));
    CHECK_FALSE(line.prepare(48000.0, 2.0));
    CHECK(line.prepared());
    CHECK(line.retained_bytes() == retained);
    CHECK(line.length_samples() == 8.0f);

    line.set_length_samples(-100.0f);
    CHECK(line.length_samples() == 3.0f);
    line.set_length_samples(std::numeric_limits<float>::infinity());
    CHECK(line.length_samples() == 3.0f);
    line.set_length_samples(100000.0f);
    CHECK(line.length_samples() == 480.0f);
}

TEST_CASE("WaveguideLine retuning glides rather than replacing a delay head",
          "[signal][waveguide][line][retune]") {
    pulp::signal::WaveguideLine64 line;
    REQUIRE(line.prepare(48000.0, 0.01));
    line.set_length_samples(12.0);
    double previous = 0.0;
    for (int n = 0; n < 600; ++n) {
        double ignored = 0.0;
        line.process(0.001 * static_cast<double>(n), 0.0, ignored, previous);
    }

    line.set_length_samples(80.0);
    CHECK(line.length_samples() == 80.0);
    CHECK_THAT(line.current_length_samples(), WithinAbs(12.0, 1.0e-15));
    CHECK_THAT(line.round_trip_seconds(), WithinAbs(24.0 / 48000.0, 1.0e-15));
    double first = 0.0;
    double ignored = 0.0;
    line.process(0.6, 0.0, ignored, first);
    // An immediate head replacement on this ramp changes by about 0.068.
    CHECK(std::abs(first - previous) < 0.005);

    double largest_step = std::abs(first - previous);
    previous = first;
    for (int n = 601; n < 900; ++n) {
        double output = 0.0;
        line.process(0.001 * static_cast<double>(n), 0.0, ignored, output);
        largest_step = std::max(largest_step, std::abs(output - previous));
        previous = output;
    }
    CHECK(largest_step < 0.005);
    CHECK_THAT(line.current_length_samples(), WithinAbs(80.0, 1.0e-12));
    CHECK_THAT(line.round_trip_seconds(), WithinAbs(160.0 / 48000.0, 1.0e-15));
}

TEST_CASE("WaveguideLine reset is deterministic and RT paths allocate nothing",
          "[signal][waveguide][line][rt]") {
    pulp::signal::WaveguideLine line;
    REQUIRE(line.prepare(48000.0, 0.01));
    line.set_length_samples(9.5f);
    const auto render = [&] {
        std::array<float, 128> result{};
        line.reset();
        for (std::size_t n = 0; n < result.size(); ++n) {
            float left = 0.0f;
            line.process(n == 0 ? 1.0f : 0.0f, 0.0f, left, result[n]);
        }
        return result;
    };
    const auto first = render();
    const auto second = render();
    CHECK(first == second);

    line.reset();
    {
        pulp::test::RtAllocationProbe probe;
        for (int n = 0; n < 512; ++n) {
            float left = 0.0f;
            float right = 0.0f;
            line.set_length_samples(3.0f + static_cast<float>(n % 128));
            line.process(n == 2 ? std::numeric_limits<float>::quiet_NaN() : 0.25f, 0.0f,
                         left, right);
            CHECK(std::isfinite(left));
            CHECK(std::isfinite(right));
        }
        CHECK(probe.allocation_count() == 0);
    }
}

TEST_CASE("WaveguideReflectionFilter matches its independent impulse recurrence",
          "[signal][waveguide][reflection]") {
    pulp::signal::WaveguideReflectionFilter64 filter;
    filter.set_reflection_gain(-0.75);
    filter.set_loss_pole(0.6);
    for (int n = 0; n < 32; ++n) {
        const auto actual = filter.process(n == 0 ? 1.0 : 0.0);
        const auto expected = -0.75 * 0.4 * std::pow(0.6, n);
        CHECK_THAT(actual, WithinAbs(expected, 2.0e-15));
    }
}

TEST_CASE("WaveguideReflectionFilter is passive across legal coefficient extremes",
          "[signal][waveguide][reflection][passivity]") {
    constexpr std::array gains{-0.999, 0.0, 0.999};
    constexpr std::array poles{0.0, 0.2, 0.98};
    constexpr std::array frequencies{0.0, 0.05, 0.25, 0.49};
    for (const auto gain : gains) {
        for (const auto pole : poles) {
            pulp::signal::WaveguideReflectionFilter64 filter;
            filter.set_reflection_gain(gain);
            filter.set_loss_pole(pole);
            std::array<double, 8192> impulse{};
            for (std::size_t n = 0; n < impulse.size(); ++n)
                impulse[n] = filter.process(n == 0 ? 1.0 : 0.0);
            for (const auto normalized_frequency : frequencies) {
                std::complex<long double> measured{};
                const auto omega = 2.0L * std::numbers::pi_v<long double> *
                                   static_cast<long double>(normalized_frequency);
                for (std::size_t n = 0; n < impulse.size(); ++n)
                    measured += static_cast<long double>(impulse[n]) *
                                std::exp(std::complex<long double>{
                                    0.0L, -omega * static_cast<long double>(n)});
                CHECK(static_cast<double>(std::abs(measured)) <= std::abs(gain) + 2.0e-12);
            }
        }
    }
}

TEST_CASE("WaveguideReflectionFilter clamps controls and contains non-finite samples",
          "[signal][waveguide][reflection][fault]") {
    pulp::signal::WaveguideReflectionFilter filter;
    filter.set_reflection_gain(2.0f);
    filter.set_loss_pole(-1.0f);
    CHECK(filter.reflection_gain() == 0.999f);
    CHECK(filter.loss_pole() == 0.0f);
    const auto finite = filter.process(0.5f);
    CHECK(filter.process(std::numeric_limits<float>::quiet_NaN()) == finite);
    filter.set_reflection_gain(std::numeric_limits<float>::infinity());
    CHECK(filter.reflection_gain() == 0.999f);
    filter.reset();
    CHECK(filter.process(0.0f) == 0.0f);
    {
        pulp::test::RtAllocationProbe probe;
        for (int n = 0; n < 1024; ++n)
            CHECK(std::isfinite(filter.process(n == 0 ? 1.0f : 0.0f)));
        filter.set_loss_pole(0.5f);
        filter.reset();
        CHECK(probe.allocation_count() == 0);
    }
}

TEST_CASE("WaveguideJunction conserves weighted traveling-wave energy",
          "[signal][waveguide][junction][conservation]") {
    std::uint32_t state = 0x57474A54u;
    for (const auto count : {2u, 3u, 4u}) {
        pulp::signal::WaveguideJunction64 junction;
        junction.set_port_count(count);
        const std::array<double, 4> impedances{0.25, 1.0, 3.0, 9.0};
        for (std::size_t i = 0; i < count; ++i)
            junction.set_impedance(i, impedances[i]);
        for (int iteration = 0; iteration < 2000; ++iteration) {
            std::array<double, 4> incoming{};
            std::array<double, 4> outgoing{};
            for (std::size_t i = 0; i < count; ++i)
                incoming[i] = bipolar(state);
            REQUIRE(junction.scatter(incoming.data(), outgoing.data(), count));
            CHECK_THAT(weighted_energy(incoming, impedances, count),
                       WithinRel(weighted_energy(outgoing, impedances, count), 2.0e-12));
        }
    }
}

TEST_CASE("WaveguideJunction two-port result matches the closed-form pressure oracle",
          "[signal][waveguide][junction][oracle]") {
    pulp::signal::WaveguideJunction64 junction;
    junction.set_port_count(2);
    junction.set_impedance(0, 1.0);
    junction.set_impedance(1, 4.0);
    std::array<double, 2> waves{0.75, -0.25};
    REQUIRE(junction.scatter(waves.data(), waves.data(), waves.size()));
    const auto pressure = 2.0 * (0.75 / 1.0 - 0.25 / 4.0) / (1.0 / 1.0 + 1.0 / 4.0);
    CHECK_THAT(waves[0], WithinAbs(pressure - 0.75, 1.0e-15));
    CHECK_THAT(waves[1], WithinAbs(pressure + 0.25, 1.0e-15));
}

TEST_CASE("WaveguideJunction oracle rejects the unweighted planted reference",
          "[signal][waveguide][junction][negative-control]") {
    constexpr std::array<double, 3> incoming{0.7, -0.2, 0.4};
    constexpr std::array<double, 3> impedances{1.0, 2.0, 4.0};
    std::array<double, 3> wrong{};
    const auto wrong_pressure =
        2.0 * (incoming[0] + incoming[1] + incoming[2]) /
        (1.0 / impedances[0] + 1.0 / impedances[1] + 1.0 / impedances[2]);
    for (std::size_t i = 0; i < wrong.size(); ++i)
        wrong[i] = wrong_pressure - incoming[i];
    CHECK(std::abs(weighted_energy(incoming, impedances, 3) -
                   weighted_energy(wrong, impedances, 3)) > 0.1);
}

TEST_CASE("WaveguideJunction rejects invalid geometry and RT scatter allocates nothing",
          "[signal][waveguide][junction][rt]") {
    pulp::signal::WaveguideJunctionT<float, 4> junction;
    junction.set_port_count(3);
    junction.set_port_count(1);
    CHECK(junction.port_count() == 3);
    junction.set_impedance(0, 0.0f);
    CHECK(junction.impedance(0) == 1.0e-6f);
    junction.set_impedance(0, std::numeric_limits<float>::infinity());
    CHECK(junction.impedance(0) == 1.0e-6f);

    std::array<float, 3> incoming{0.25f, -0.5f, 0.75f};
    std::array<float, 3> outgoing{};
    REQUIRE(junction.scatter(incoming.data(), outgoing.data(), outgoing.size()));
    const auto previous = outgoing;
    incoming[1] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(junction.scatter(incoming.data(), outgoing.data(), outgoing.size()));
    CHECK(outgoing == previous);

    incoming[1] = -0.5f;
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 1024; ++i)
            REQUIRE(junction.scatter(incoming.data(), outgoing.data(), outgoing.size()));
        junction.reset();
        CHECK(probe.allocation_count() == 0);
    }
}

TEST_CASE("Waveguide line and passive boundaries compose into a decaying round trip",
          "[signal][waveguide][composition]") {
    pulp::signal::WaveguideLine64 line;
    REQUIRE(line.prepare(48000.0, 0.01));
    line.set_length_samples(4.0);
    pulp::signal::WaveguideReflectionFilter64 left_boundary;
    pulp::signal::WaveguideReflectionFilter64 right_boundary;
    left_boundary.set_reflection_gain(0.5);
    right_boundary.set_reflection_gain(0.5);
    left_boundary.set_loss_pole(0.0);
    right_boundary.set_loss_pole(0.0);

    std::array<double, 20> left_arrivals{};
    std::array<double, 20> right_arrivals{};
    for (std::size_t n = 0; n < left_arrivals.size(); ++n) {
        const auto excitation = n == 0 ? 1.0 : 0.0;
        line.read_outputs(left_arrivals[n], right_arrivals[n]);
        const auto left_feedback = left_boundary.process(left_arrivals[n]);
        const auto right_feedback = right_boundary.process(right_arrivals[n]);
        line.push_inputs(excitation + left_feedback, right_feedback);
    }
    CHECK(right_arrivals[4] == 1.0);
    CHECK(left_arrivals[8] == 0.5);
    CHECK(right_arrivals[12] == 0.25);
    CHECK(left_arrivals[16] == 0.125);
    CHECK_THAT(line.round_trip_seconds(), WithinAbs(8.0 / 48000.0, 1.0e-15));
}
