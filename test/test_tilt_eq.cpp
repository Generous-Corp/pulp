#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/tilt_eq.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>

using Catch::Matchers::WithinAbs;

namespace {

struct ReferenceSection {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    double s1 = 0.0;
    double s2 = 0.0;
};

ReferenceSection reference_high_shelf(double frequency_hz, double gain_db, double sample_rate) {
    const double pi = std::acos(-1.0);
    const double a = std::pow(10.0, gain_db / 40.0);
    const double omega = 2.0 * pi * frequency_hz / sample_rate;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / std::sqrt(2.0);
    const double two_sqrt_a_alpha = 2.0 * std::sqrt(a) * alpha;
    const double a0 = (a + 1.0) - (a - 1.0) * cosine + two_sqrt_a_alpha;
    return {
        a * ((a + 1.0) + (a - 1.0) * cosine + two_sqrt_a_alpha) / a0,
        -2.0 * a * ((a - 1.0) + (a + 1.0) * cosine) / a0,
        a * ((a + 1.0) + (a - 1.0) * cosine - two_sqrt_a_alpha) / a0,
        2.0 * ((a - 1.0) - (a + 1.0) * cosine) / a0,
        ((a + 1.0) - (a - 1.0) * cosine - two_sqrt_a_alpha) / a0,
    };
}

double reference_magnitude(const ReferenceSection& section, double frequency_hz,
                           double sample_rate) {
    const double omega = 2.0 * std::acos(-1.0) * frequency_hz / sample_rate;
    const std::complex<double> z1 = std::polar(1.0, -omega);
    const std::complex<double> z2 = z1 * z1;
    return std::abs(section.b0 + section.b1 * z1 + section.b2 * z2) /
           std::abs(1.0 + section.a1 * z1 + section.a2 * z2);
}

struct ReferenceTilt {
    std::array<ReferenceSection, 9> sections{};
    double normalization = 1.0;
};

ReferenceTilt make_reference(double pivot_hz, double slope_db_per_octave, double sample_rate) {
    ReferenceTilt result;
    constexpr double low_hz = 31.25;
    const double high_hz = std::min(16000.0, 0.4 * sample_rate);
    const double octaves = std::log2(high_hz / low_hz);
    const double ratio = high_hz / low_hz;
    const auto design = [&](double stage_gain) {
        for (std::size_t i = 0; i < result.sections.size(); ++i) {
            const double position = (static_cast<double>(i) + 0.5) / 9.0;
            result.sections[i] =
                reference_high_shelf(low_hz * std::pow(ratio, position), stage_gain, sample_rate);
        }
    };
    const auto endpoint_delta = [&] {
        double low_magnitude = 1.0;
        double high_magnitude = 1.0;
        for (const auto& section : result.sections) {
            low_magnitude *= reference_magnitude(section, low_hz, sample_rate);
            high_magnitude *= reference_magnitude(section, high_hz, sample_rate);
        }
        return 20.0 * std::log10(high_magnitude / low_magnitude);
    };

    const double target_delta = slope_db_per_octave * octaves;
    if (target_delta == 0.0) {
        design(0.0);
    } else {
        const double direction = target_delta > 0.0 ? 1.0 : -1.0;
        const double target_magnitude = std::abs(target_delta);
        double low_gain = 0.0;
        double high_gain = target_magnitude * 2.0 / 9.0 + 0.5;
        for (int iteration = 0; iteration < 60; ++iteration) {
            const double candidate = (low_gain + high_gain) * 0.5;
            design(direction * candidate);
            if (std::abs(endpoint_delta()) < target_magnitude)
                low_gain = candidate;
            else
                high_gain = candidate;
        }
        design(direction * (low_gain + high_gain) * 0.5);
    }
    for (const auto& section : result.sections)
        result.normalization /= reference_magnitude(section, pivot_hz, sample_rate);
    return result;
}

double reference_response(const ReferenceTilt& reference, double frequency_hz, double sample_rate) {
    double result = reference.normalization;
    for (const auto& section : reference.sections)
        result *= reference_magnitude(section, frequency_hz, sample_rate);
    return result;
}

double process_reference(ReferenceTilt& reference, double input) {
    for (auto& section : reference.sections) {
        const double output = section.b0 * input + section.s1;
        section.s1 = section.b1 * input - section.a1 * output + section.s2;
        section.s2 = section.b2 * input - section.a2 * output;
        input = output;
    }
    return input * reference.normalization;
}

} // namespace

TEST_CASE("TiltEq defines a pivot-normalized signed dB-per-octave slope", "[signal][eq][tilt]") {
    pulp::signal::TiltEqT<double, 1> eq;
    REQUIRE(eq.prepare(48000.0));
    REQUIRE(eq.set_config({800.0, 4.0}));

    REQUIRE_THAT(eq.magnitude_db(800.0), WithinAbs(0.0, 2e-12));
    constexpr double low_hz = 31.25;
    constexpr double high_hz = 16000.0;
    REQUIRE(eq.magnitude_db(high_hz) > eq.magnitude_db(low_hz));
    REQUIRE_THAT(eq.magnitude_db(high_hz) - eq.magnitude_db(low_hz),
                 WithinAbs(4.0 * std::log2(high_hz / low_hz), 2e-10));
    REQUIRE(eq.latency_samples() == 0);
    REQUIRE(eq.tail_samples() == -1);
}

TEST_CASE("TiltEq neutral processing is exact and configuration rejects transactionally",
          "[signal][eq][tilt]") {
    pulp::signal::TiltEqT<double, 1> eq;
    REQUIRE(eq.prepare(48000.0));
    const std::array<double, 6> input{0.0, -0.0, 0.125, -0.75, 1.0, -1.0};
    for (double sample : input)
        REQUIRE(std::bit_cast<std::uint64_t>(eq.process(sample)) ==
                std::bit_cast<std::uint64_t>(sample));
    REQUIRE(eq.tail_samples() == 0);

    REQUIRE(eq.set_config({1200.0, -3.5}));
    static_cast<void>(eq.process(0.5));
    auto reference = eq;
    const auto config = eq.config();
    const auto coefficients = eq.coefficients(3);
    REQUIRE_FALSE(eq.set_config({std::numeric_limits<double>::quiet_NaN(), 1.0}));
    REQUIRE_FALSE(eq.set_config({1000.0, 6.01}));
    REQUIRE_FALSE(eq.set_config({20000.0, 1.0}));
    REQUIRE(eq.config().pivot_hz == config.pivot_hz);
    REQUIRE(eq.config().tilt_db_per_octave == config.tilt_db_per_octave);
    REQUIRE(eq.coefficients(3).b0 == coefficients.b0);
    REQUIRE(eq.process(0.25) == reference.process(0.25));

    auto unchanged = eq;
    REQUIRE(eq.set_config(eq.config()));
    REQUIRE(eq.process(-0.125) == unchanged.process(-0.125));
    REQUIRE_FALSE(eq.prepare(7999.0));
    REQUIRE(eq.process(0.375) == unchanged.process(0.375));
}

TEST_CASE("TiltEq response matches an independent shelf oracle with a negative control",
          "[signal][eq][tilt][response]") {
    pulp::signal::TiltEqT<double, 1> eq;
    REQUIRE(eq.prepare(48000.0));
    REQUIRE(eq.set_config({750.0, 3.25}));
    const auto reference = make_reference(750.0, 3.25, 48000.0);
    const auto wrong_sign = make_reference(750.0, -3.25, 48000.0);

    double negative_control_error = 0.0;
    for (double frequency : {40.0, 125.0, 750.0, 3000.0, 12000.0}) {
        const double expected = reference_response(reference, frequency, 48000.0);
        REQUIRE_THAT(eq.magnitude(frequency), WithinAbs(expected, 2e-12));
        negative_control_error = std::max(
            negative_control_error,
            std::abs(eq.magnitude(frequency) - reference_response(wrong_sign, frequency, 48000.0)));
    }
    REQUIRE(negative_control_error > 0.25);
}

TEST_CASE("TiltEq validates stable finite designs at the public extremes",
          "[signal][eq][tilt][stability]") {
    using Eq = pulp::signal::TiltEqT<float, 1>;
    Eq eq;
    for (double sample_rate : {Eq::min_sample_rate, Eq::max_sample_rate}) {
        REQUIRE(eq.prepare(sample_rate));
        const double high_hz = std::min(Eq::max_design_hz, 0.4 * sample_rate);
        for (double pivot_hz : {Eq::min_pivot_hz, high_hz}) {
            for (double slope : {Eq::min_tilt_db_per_octave, Eq::max_tilt_db_per_octave}) {
                REQUIRE(eq.set_config({pivot_hz, slope}));
                for (std::size_t i = 0; i < Eq::shelf_count; ++i) {
                    const auto coefficients = eq.coefficients(i);
                    REQUIRE(pulp::signal::biquad_is_stable(coefficients));
                    REQUIRE(std::isfinite(coefficients.b0));
                    REQUIRE(std::isfinite(coefficients.b1));
                    REQUIRE(std::isfinite(coefficients.b2));
                    REQUIRE(std::isfinite(coefficients.a1));
                    REQUIRE(std::isfinite(coefficients.a2));
                }
                for (int i = 0; i < 512; ++i)
                    REQUIRE(std::isfinite(eq.process(i == 0 ? 1.0f : 0.0f)));
            }
        }
    }
}

TEST_CASE("TiltEq impulse matches an independent time-domain oracle and rejects a planted fault",
          "[signal][eq][tilt][oracle]") {
    pulp::signal::TiltEqT<double, 1> eq;
    REQUIRE(eq.prepare(48000.0));
    REQUIRE(eq.set_config({1400.0, -4.5}));
    auto reference = make_reference(1400.0, -4.5, 48000.0);
    auto planted = reference;
    planted.sections[4].b0 *= 1.02;

    double planted_error = 0.0;
    for (std::size_t i = 0; i < 256; ++i) {
        const double input = i == 0 ? 1.0 : 0.0;
        const double actual = eq.process(input);
        REQUIRE_THAT(actual, WithinAbs(process_reference(reference, input), 3e-12));
        planted_error =
            std::max(planted_error, std::abs(actual - process_reference(planted, input)));
    }
    REQUIRE(planted_error > 1e-4);
}

TEST_CASE("TiltEq block in-place partition channel and reset paths are deterministic",
          "[signal][eq][tilt][block]") {
    using Eq = pulp::signal::TiltEqT<float, 2>;
    Eq seed;
    REQUIRE(seed.prepare(44100.0));
    REQUIRE(seed.set_config({1000.0, 2.5}));

    std::array<float, 257> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>((static_cast<int>(i * 37 % 101) - 50) / 50.0);

    auto whole = seed;
    auto partitioned = seed;
    auto in_place = seed;
    std::array<float, 257> whole_output{};
    std::array<float, 257> partitioned_output{};
    auto in_place_output = input;
    REQUIRE(whole.process_block(input.data(), whole_output.data(), input.size(), 0));
    REQUIRE(partitioned.process_block(input.data(), partitioned_output.data(), 73, 0));
    REQUIRE(partitioned.process_block(input.data() + 73, partitioned_output.data() + 73,
                                      input.size() - 73, 0));
    REQUIRE(in_place.process_block(in_place_output.data(), in_place_output.size(), 0));
    REQUIRE(whole_output == partitioned_output);
    REQUIRE(whole_output == in_place_output);

    auto other_channel = seed;
    std::array<float, 257> channel_one{};
    REQUIRE(other_channel.process_block(input.data(), channel_one.data(), input.size(), 1));
    REQUIRE(channel_one == whole_output);
    REQUIRE_FALSE(other_channel.process_block(input.data(), input.size(), 2));
    REQUIRE_FALSE(other_channel.process_block(nullptr, 1, 0));
    REQUIRE(other_channel.process_block(static_cast<float*>(nullptr), 0, 0));

    seed.process(1.0f, 0);
    seed.reset();
    auto fresh = seed;
    for (std::size_t i = 0; i < 64; ++i) {
        const float sample = i == 0 ? 1.0f : 0.0f;
        REQUIRE(seed.process(sample, 0) == fresh.process(sample, 0));
    }
}

TEST_CASE("TiltEq recovers from non-finite input without poisoning later output",
          "[signal][eq][tilt][fault]") {
    pulp::signal::TiltEqT<double, 1> eq;
    REQUIRE(eq.prepare(48000.0));
    REQUIRE(eq.set_config({1000.0, 6.0}));
    static_cast<void>(eq.process(1.0));
    REQUIRE(eq.process(std::numeric_limits<double>::infinity()) == 0.0);
    REQUIRE(eq.fault_count() == 1);
    for (int i = 0; i < 32; ++i)
        REQUIRE(eq.process(0.0) == 0.0);
    eq.reset();
    REQUIRE(eq.fault_count() == 0);
}

TEST_CASE("TiltEq control response process and reset paths allocate nothing",
          "[signal][eq][tilt][rt-safety]") {
    pulp::signal::TiltEq eq;
    std::array<float, 64> block{};
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(eq.prepare(96000.0));
        REQUIRE(eq.set_config({2000.0, -5.0}));
        REQUIRE(eq.process_block(block.data(), block.size(), 0));
        static_cast<void>(eq.magnitude_db(1000.0));
        eq.reset();
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}
