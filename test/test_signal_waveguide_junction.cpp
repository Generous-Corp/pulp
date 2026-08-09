#include <pulp/signal/waveguide_junction.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

using Catch::Matchers::WithinAbs;
using pulp::signal::WaveguideJunctionT;

namespace {

template <std::size_t Capacity>
std::array<long double, Capacity> matrix_oracle(
    const std::array<long double, Capacity>& incoming,
    const std::array<long double, Capacity>& impedances, std::size_t count) {
    std::array<long double, Capacity> outgoing{};
    long double total_admittance = 0.0L;
    for (std::size_t column = 0; column < count; ++column)
        total_admittance += 1.0L / impedances[column];

    for (std::size_t row = 0; row < count; ++row) {
        for (std::size_t column = 0; column < count; ++column) {
            const auto coefficient =
                2.0L / (impedances[column] * total_admittance) -
                (row == column ? 1.0L : 0.0L);
            outgoing[row] += coefficient * incoming[column];
        }
    }
    return outgoing;
}

template <std::size_t Capacity>
long double acoustic_power(const std::array<long double, Capacity>& waves,
                           const std::array<long double, Capacity>& impedances,
                           std::size_t count) {
    long double power = 0.0L;
    for (std::size_t port = 0; port < count; ++port)
        power += waves[port] * waves[port] / impedances[port];
    return power;
}

std::uint32_t next_bits(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

double uniform(std::uint32_t& state, double low, double high) {
    const auto fraction = static_cast<double>(next_bits(state)) * 0x1.0p-32;
    return low + (high - low) * fraction;
}

} // namespace

static_assert(std::is_nothrow_default_constructible_v<WaveguideJunctionT<float, 4>>);
static_assert(noexcept(std::declval<WaveguideJunctionT<float, 4>&>().set_port_count(4)));
static_assert(noexcept(std::declval<WaveguideJunctionT<float, 4>&>().set_impedance(0, 1.0f)));
static_assert(noexcept(std::declval<const WaveguideJunctionT<float, 4>&>().scatter(
    static_cast<const float*>(nullptr), static_cast<float*>(nullptr), 0)));

TEST_CASE("waveguide junction matches an independent high-precision scattering matrix",
          "[signal][waveguide][junction]") {
    constexpr std::size_t capacity = 4;
    WaveguideJunctionT<double, capacity> junction;
    std::uint32_t state = 0x57474A54U;

    for (std::size_t count = 2; count <= capacity; ++count) {
        junction.set_port_count(count);
        for (std::size_t trial = 0; trial < 256; ++trial) {
            std::array<double, capacity> incoming{};
            std::array<double, capacity> outgoing{};
            std::array<long double, capacity> oracle_in{};
            std::array<long double, capacity> impedances{};
            for (std::size_t port = 0; port < count; ++port) {
                const auto impedance = std::exp(uniform(state, std::log(1.0e-3), std::log(1.0e3)));
                incoming[port] = uniform(state, -8.0, 8.0);
                oracle_in[port] = static_cast<long double>(incoming[port]);
                impedances[port] = static_cast<long double>(impedance);
                junction.set_impedance(port, impedance);
            }

            junction.scatter(incoming.data(), outgoing.data(), count);
            const auto expected = matrix_oracle(oracle_in, impedances, count);
            for (std::size_t port = 0; port < count; ++port)
                CHECK_THAT(outgoing[port], WithinAbs(static_cast<double>(expected[port]), 2.0e-12));

            std::array<long double, capacity> actual_out{};
            for (std::size_t port = 0; port < count; ++port)
                actual_out[port] = static_cast<long double>(outgoing[port]);
            const auto incoming_power = acoustic_power(oracle_in, impedances, count);
            const auto outgoing_power = acoustic_power(actual_out, impedances, count);
            const auto tolerance = 4.0e-12L * std::max(1.0L, incoming_power);
            CHECK(std::abs(outgoing_power - incoming_power) <= tolerance);
        }
    }
}

TEST_CASE("waveguide junction reduces to known equal-impedance scattering cases",
          "[signal][waveguide][junction]") {
    WaveguideJunctionT<double, 4> junction;
    std::array<double, 4> incoming{0.75, -0.25, 0.0, 0.0};
    std::array<double, 4> outgoing{};

    junction.scatter(incoming.data(), outgoing.data(), 2);
    CHECK_THAT(outgoing[0], WithinAbs(incoming[1], 2.0e-15));
    CHECK_THAT(outgoing[1], WithinAbs(incoming[0], 2.0e-15));

    junction.set_port_count(3);
    incoming = {1.0, 0.0, 0.0, 0.0};
    junction.scatter(incoming.data(), outgoing.data(), 3);
    CHECK_THAT(outgoing[0], WithinAbs(-1.0 / 3.0, 2.0e-15));
    CHECK_THAT(outgoing[1], WithinAbs(2.0 / 3.0, 2.0e-15));
    CHECK_THAT(outgoing[2], WithinAbs(2.0 / 3.0, 2.0e-15));

    junction.scatter(outgoing.data(), outgoing.data(), 3);
    CHECK_THAT(outgoing[0], WithinAbs(1.0, 3.0e-15));
    CHECK_THAT(outgoing[1], WithinAbs(0.0, 3.0e-15));
    CHECK_THAT(outgoing[2], WithinAbs(0.0, 3.0e-15));
}

TEST_CASE("waveguide junction configuration and sample faults fail closed",
          "[signal][waveguide][junction][fault]") {
    WaveguideJunctionT<double, 4> junction;
    junction.set_port_count(3);
    junction.set_port_count(1);
    junction.set_port_count(5);
    CHECK(junction.port_count() == 3);

    junction.set_impedance(0, 0.0);
    CHECK(junction.impedance(0) == junction.minimum_impedance);
    junction.set_impedance(1, 3.0);
    junction.set_impedance(1, std::numeric_limits<double>::quiet_NaN());
    junction.set_impedance(4, 7.0);
    CHECK(junction.impedance(1) == 3.0);
    CHECK(junction.impedance(4) == 0.0);

    std::array<double, 4> incoming{0.5, -0.25, 0.125, 0.0};
    std::array<double, 4> outgoing{9.0, 9.0, 9.0, 9.0};
    junction.scatter(incoming.data(), outgoing.data(), 2);
    CHECK(outgoing[0] == 0.0);
    CHECK(outgoing[1] == 0.0);
    CHECK(outgoing[2] == 9.0);

    incoming[1] = std::numeric_limits<double>::infinity();
    junction.scatter(incoming.data(), outgoing.data(), 3);
    CHECK(outgoing[0] == 0.0);
    CHECK(outgoing[1] == 0.0);
    CHECK(outgoing[2] == 0.0);

    incoming = {0.5, -0.25, 0.125, 0.0};
    junction.scatter(incoming.data(), outgoing.data(), 3);
    CHECK(std::all_of(outgoing.begin(), outgoing.begin() + 3,
                      [](double sample) { return std::isfinite(sample); }));
}

TEST_CASE("waveguide junction remains finite near the scalar range limit",
          "[signal][waveguide][junction][fault]") {
    WaveguideJunctionT<double> junction;
    const auto level = std::numeric_limits<double>::max() * 0.25;
    junction.set_impedance(0, std::numeric_limits<double>::max());
    junction.set_impedance(1, std::numeric_limits<double>::max());
    std::array incoming{level, -level};
    std::array<double, 2> outgoing{};
    junction.scatter(incoming.data(), outgoing.data(), 2);
    CHECK(std::isfinite(outgoing[0]));
    CHECK(std::isfinite(outgoing[1]));
    CHECK_THAT(outgoing[0], WithinAbs(-level, level * 4.0e-15));
    CHECK_THAT(outgoing[1], WithinAbs(level, level * 4.0e-15));

    incoming = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    junction.scatter(incoming.data(), outgoing.data(), 2);
    CHECK(outgoing[0] == std::numeric_limits<double>::max());
    CHECK(outgoing[1] == std::numeric_limits<double>::max());

    if constexpr (std::numeric_limits<long double>::max_exponent >
                  std::numeric_limits<double>::max_exponent) {
        WaveguideJunctionT<long double> wide_junction;
        const auto wide_level = std::numeric_limits<double>::max() * 2.0L;
        wide_junction.set_impedance(0, wide_level);
        wide_junction.set_impedance(1, wide_level);
        std::array wide_incoming{wide_level, wide_level};
        std::array<long double, 2> wide_outgoing{};
        wide_junction.scatter(wide_incoming.data(), wide_outgoing.data(), 2);
        CHECK(wide_outgoing[0] == wide_level);
        CHECK(wide_outgoing[1] == wide_level);
    }
}

TEST_CASE("waveguide junction configuration and scattering allocate no memory",
          "[signal][waveguide][junction][rt-safety]") {
    WaveguideJunctionT<float, 4> junction;
    std::array<float, 4> incoming{0.5f, -0.25f, 0.125f, -0.0625f};
    std::array<float, 4> outgoing{};

    pulp::test::RtAllocationProbe probe;
    for (std::size_t count = 2; count <= 4; ++count) {
        junction.set_port_count(count);
        for (std::size_t port = 0; port < count; ++port)
            junction.set_impedance(port, 0.25f + static_cast<float>(port));
        for (std::size_t iteration = 0; iteration < 4096; ++iteration)
            junction.scatter(incoming.data(), outgoing.data(), count);
    }
    CHECK(probe.allocation_count() == 0);
}
