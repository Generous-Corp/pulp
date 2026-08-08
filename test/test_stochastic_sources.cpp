#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/dust.hpp>
#include <pulp/signal/lfsr.hpp>
#include <pulp/signal/noise_source.hpp>
#include <pulp/signal/noise_tilt.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

std::uint32_t reference_xorshift(std::uint32_t& state) {
    state ^= state << 13u;
    state ^= state >> 17u;
    state ^= state << 5u;
    return state;
}

double reference_unit(std::uint32_t& state) {
    return static_cast<double>(reference_xorshift(state)) / 4294967296.0;
}

template <typename Source>
std::vector<double> averaged_spectrum(Source& source, std::size_t frame_size, int frame_count) {
    std::vector<double> accumulated(frame_size / 2 + 1, 0.0);
    std::vector<double> frame(frame_size);
    for (int frame_index = 0; frame_index < frame_count; ++frame_index) {
        for (std::size_t sample = 0; sample < frame_size; ++sample) {
            const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(sample) /
                                                       static_cast<double>(frame_size - 1));
            frame[sample] = source.process() * window;
        }
        for (std::size_t bin = 0; bin < accumulated.size(); ++bin) {
            double real = 0.0;
            double imaginary = 0.0;
            const double omega =
                2.0 * kPi * static_cast<double>(bin) / static_cast<double>(frame_size);
            for (std::size_t sample = 0; sample < frame_size; ++sample) {
                real += frame[sample] * std::cos(omega * static_cast<double>(sample));
                imaginary -= frame[sample] * std::sin(omega * static_cast<double>(sample));
            }
            accumulated[bin] += real * real + imaginary * imaginary;
        }
    }
    for (double& value : accumulated)
        value /= static_cast<double>(frame_count);
    return accumulated;
}

double spectral_slope(const std::vector<double>& power, double sample_rate, double low_hz,
                      double high_hz) {
    const double bin_hz = 0.5 * sample_rate / static_cast<double>(power.size() - 1);
    std::vector<double> x;
    std::vector<double> y;
    for (double low = low_hz; low * 2.0 <= high_hz * 1.0001; low *= 2.0) {
        const double high = low * 2.0;
        const auto first = static_cast<std::size_t>(std::ceil(low / bin_hz));
        const auto last = static_cast<std::size_t>(std::floor(high / bin_hz));
        if (last <= first || last >= power.size())
            continue;
        double energy = 0.0;
        for (std::size_t bin = first; bin <= last; ++bin)
            energy += power[bin];
        x.push_back(std::log2(std::sqrt(low * high)));
        y.push_back(10.0 * std::log10(energy / static_cast<double>(last - first + 1) + 1e-30));
    }
    REQUIRE(x.size() >= 4);
    const double mean_x = std::accumulate(x.begin(), x.end(), 0.0) / x.size();
    const double mean_y = std::accumulate(y.begin(), y.end(), 0.0) / y.size();
    double numerator = 0.0;
    double denominator = 0.0;
    for (std::size_t index = 0; index < x.size(); ++index) {
        numerator += (x[index] - mean_x) * (y[index] - mean_y);
        denominator += (x[index] - mean_x) * (x[index] - mean_x);
    }
    return numerator / denominator;
}

double measured_tilt(double requested, double sample_rate) {
    pulp::signal::NoiseTilt64 source;
    REQUIRE(source.prepare(sample_rate));
    REQUIRE(source.set_tilt_db_per_octave(requested));
    source.reset();
    return spectral_slope(averaged_spectrum(source, 2048, 8), sample_rate, 187.5, 12000.0);
}

} // namespace

TEST_CASE("LFSR reaches the independently known maximal period", "[signal][stochastic][lfsr]") {
    pulp::signal::Lfsr64 audited_default;
    const auto initial = audited_default.state();
    std::array<bool, 256> default_visited{};
    default_visited[initial] = true;
    for (int step = 1; step < 255; ++step) {
        audited_default.clock();
        REQUIRE(audited_default.state() != 0u);
        REQUIRE_FALSE(default_visited[audited_default.state()]);
        default_visited[audited_default.state()] = true;
    }
    REQUIRE(audited_default.clock() == initial);

    pulp::signal::Lfsr64 source;
    REQUIRE(source.configure(4, 0b1001u, 1u));
    std::array<bool, 16> visited{};
    visited[source.state()] = true;
    for (int step = 1; step < 15; ++step) {
        source.clock();
        REQUIRE(source.state() != 0u);
        REQUIRE_FALSE(visited[source.state()]);
        visited[source.state()] = true;
    }
    REQUIRE(source.clock() == 1u);

    REQUIRE(source.configure(4, 0u, 1u));
    for (int step = 0; step < 4; ++step)
        source.clock();
    REQUIRE(source.state() == 0u);
}

TEST_CASE("LFSR exposes exact state, weighted range, and reset semantics",
          "[signal][stochastic][lfsr]") {
    pulp::signal::Lfsr64 source;
    REQUIRE(source.configure(4, 0b1001u, 0b1010u));
    REQUIRE(source.set_offset(-2.0));
    REQUIRE(source.set_weight(0, -0.5));
    REQUIRE(source.set_weight(1, 1.0));
    REQUIRE(source.set_weight(2, 2.0));
    REQUIRE(source.set_weight(3, 4.0));
    REQUIRE(source.weighted_output() == 3.0);
    REQUIRE(source.minimum_output() == -2.5);
    REQUIRE(source.maximum_output() == 5.0);

    const auto first = source.clock();
    source.reset();
    REQUIRE(source.state() == source.seed());
    REQUIRE(source.clock() == first);
    REQUIRE_FALSE(source.configure(1, 1u, 1u));
    REQUIRE_FALSE(source.configure(4, 0x10u, 1u));
    REQUIRE_FALSE(source.set_weight(4, 1.0));
    REQUIRE_FALSE(source.set_weight(0, std::numeric_limits<double>::quiet_NaN()));

    pulp::signal::Lfsr overflow_guard;
    REQUIRE(overflow_guard.set_weight(0, std::numeric_limits<float>::max()));
    REQUIRE_FALSE(overflow_guard.set_weight(1, std::numeric_limits<float>::max()));
    REQUIRE(std::isfinite(overflow_guard.maximum_output()));

    pulp::signal::Lfsr expansion_guard;
    REQUIRE(expansion_guard.set_weight(7, std::numeric_limits<float>::max()));
    REQUIRE(expansion_guard.set_length(2));
    REQUIRE(expansion_guard.set_weight(0, std::numeric_limits<float>::max()));
    REQUIRE_FALSE(expansion_guard.set_length(8));
    REQUIRE_FALSE(expansion_guard.configure(8, 0x8eu, 1u));
    REQUIRE(expansion_guard.length() == 2);
    REQUIRE(std::isfinite(expansion_guard.maximum_output()));

    REQUIRE(source.configure(4, 0b1001u, 0u));
    REQUIRE(source.clock() == 0u);
    REQUIRE(source.clock(true) == 1u);
    STATIC_REQUIRE(pulp::signal::Lfsr::latency_samples() == 0);
    STATIC_REQUIRE(pulp::signal::Lfsr64::tail_samples() == 0);
}

TEST_CASE("Dust matches an independent seeded fixture and every block partition",
          "[signal][stochastic][dust]") {
    constexpr std::uint32_t seed = 0x12345678u;
    pulp::signal::Dust64 source;
    REQUIRE(source.prepare(1000.0));
    REQUIRE(source.set_density_hz(250.0));
    source.set_seed(seed);
    source.reset();

    std::uint32_t reference_state = seed;
    std::array<double, 257> expected{};
    for (double& sample : expected)
        sample = reference_unit(reference_state) < 0.25 ? 1.0 : 0.0;
    for (double expected_sample : expected)
        REQUIRE(source.process() == expected_sample);

    source.reset();
    std::array<double, 257> partitioned{};
    source.process(partitioned.data(), 3);
    source.process(partitioned.data() + 3, 127);
    source.process(partitioned.data() + 130, 1);
    source.process(partitioned.data() + 131, 126);
    REQUIRE(partitioned == expected);
}

TEST_CASE("Dust event rate and amplitude distributions match their declared laws",
          "[signal][stochastic][dust]") {
    pulp::signal::Dust64 source;
    REQUIRE(source.prepare(48000.0));
    REQUIRE(source.set_density_hz(480.0));
    constexpr int trials = 1000000;
    int events = 0;
    for (int index = 0; index < trials; ++index) {
        source.process();
        events += source.triggered() ? 1 : 0;
    }
    const double expected = trials * 0.01;
    const double sigma = std::sqrt(trials * 0.01 * 0.99);
    REQUIRE(std::fabs(events - expected) < 5.0 * sigma);
    REQUIRE(std::fabs(events - trials * 0.02) > 50.0 * sigma);

    REQUIRE(source.set_density_hz(48000.0));
    source.set_distribution(pulp::signal::DustAmplitudeDistribution::uniform_unipolar);
    source.reset();
    double unipolar_sum = 0.0;
    for (int index = 0; index < 200000; ++index) {
        const double sample = source.process();
        REQUIRE(sample >= 0.0);
        REQUIRE(sample < 1.0);
        unipolar_sum += sample;
    }
    REQUIRE(std::fabs(unipolar_sum / 200000.0 - 0.5) < 0.005);

    source.set_distribution(pulp::signal::DustAmplitudeDistribution::uniform_bipolar);
    source.reset();
    double sum = 0.0;
    double square_sum = 0.0;
    for (int index = 0; index < 200000; ++index) {
        const double sample = source.process();
        REQUIRE(sample >= -1.0);
        REQUIRE(sample < 1.0);
        sum += sample;
        square_sum += sample * sample;
    }
    REQUIRE(std::fabs(sum / 200000.0) < 0.005);
    REQUIRE(std::fabs(square_sum / 200000.0 - 1.0 / 3.0) < 0.005);

    REQUIRE_FALSE(source.prepare(0.0));
    REQUIRE_FALSE(source.set_density_hz(-1.0));
    REQUIRE_FALSE(source.set_density_hz(48001.0));
    REQUIRE_FALSE(source.set_level(-0.01));
    REQUIRE_FALSE(source.set_level(1.01));
    REQUIRE_FALSE(
        source.set_distribution(static_cast<pulp::signal::DustAmplitudeDistribution>(255)));
    STATIC_REQUIRE(pulp::signal::Dust::latency_samples() == 0);
    STATIC_REQUIRE(pulp::signal::Dust64::tail_samples() == 0);
}

TEST_CASE("Dust event positions do not depend on amplitude law or level",
          "[signal][stochastic][dust]") {
    pulp::signal::Dust64 constant;
    pulp::signal::Dust64 unipolar;
    pulp::signal::Dust64 bipolar;
    for (auto* source : {&constant, &unipolar, &bipolar}) {
        REQUIRE(source->prepare(48000.0));
        REQUIRE(source->set_density_hz(733.0));
        source->set_seed(0xF00D1234u);
    }
    REQUIRE(unipolar.set_level(0.25));
    REQUIRE(bipolar.set_level(0.75));
    unipolar.set_distribution(pulp::signal::DustAmplitudeDistribution::uniform_unipolar);
    bipolar.set_distribution(pulp::signal::DustAmplitudeDistribution::uniform_bipolar);
    constant.reset();
    unipolar.reset();
    bipolar.reset();
    for (int index = 0; index < 100000; ++index) {
        constant.process();
        unipolar.process();
        bipolar.process();
        REQUIRE(unipolar.triggered() == constant.triggered());
        REQUIRE(bipolar.triggered() == constant.triggered());
    }
}

TEST_CASE("Zero-tilt noise is exactly the canonical white stream",
          "[signal][stochastic][noise-tilt]") {
    auto check = []<typename Sample>() {
        pulp::signal::NoiseTiltT<Sample> tilted;
        pulp::signal::NoiseSourceT<Sample> white;
        REQUIRE(tilted.prepare(48000.0));
        white.prepare(48000.0);
        tilted.set_seed(0xABCDEF01u);
        white.set_seed(0xABCDEF01u);
        tilted.reset();
        white.reset();
        for (int index = 0; index < 8192; ++index)
            REQUIRE(tilted.process() == white.white());
    };
    check.template operator()<float>();
    check.template operator()<double>();
}

TEST_CASE("Continuous noise tilt follows the requested PSD slope",
          "[signal][stochastic][noise-tilt]") {
    for (double requested : {-4.5, -1.5, 0.0, 1.5, 4.5}) {
        const double measured = measured_tilt(requested, 48000.0);
        REQUIRE(std::fabs(measured - requested) < 0.9);
    }
    REQUIRE(std::fabs(measured_tilt(3.0, 96000.0) - 3.0) < 0.9);
}

TEST_CASE("Tilt reset and block partition are exact for both public precisions",
          "[signal][stochastic][noise-tilt]") {
    auto check = []<typename Sample>() {
        pulp::signal::NoiseTiltT<Sample> source;
        REQUIRE(source.prepare(48000.0));
        REQUIRE(source.set_tilt_db_per_octave(3.25));
        source.set_seed(0x9137ACEDu);
        source.reset();
        std::array<Sample, 511> expected{};
        for (auto& sample : expected)
            sample = source.process();
        source.reset();
        std::array<Sample, 511> partitioned{};
        source.process(partitioned.data(), 17);
        source.process(partitioned.data() + 17, 233);
        source.process(partitioned.data() + 250, 261);
        REQUIRE(partitioned == expected);
        for (Sample sample : partitioned)
            REQUIRE(std::isfinite(static_cast<double>(sample)));
        REQUIRE(source.fault_count() == 0);
    };
    check.template operator()<float>();
    check.template operator()<double>();

    pulp::signal::NoiseTilt64 source;
    REQUIRE_FALSE(source.prepare(7999.0));
    REQUIRE_FALSE(source.prepare(384001.0));
    REQUIRE_FALSE(source.set_tilt_db_per_octave(-6.01));
    REQUIRE_FALSE(source.set_tilt_db_per_octave(6.01));
    REQUIRE_FALSE(source.set_level(std::numeric_limits<double>::infinity()));
    REQUIRE(source.prepare(384000.0));
    REQUIRE(source.set_tilt_db_per_octave(-6.0));
    REQUIRE(source.set_level(0.0));
    for (int index = 0; index < 262144; ++index)
        REQUIRE(source.process() == 0.0);
    REQUIRE(source.fault_count() == 0);
    REQUIRE(source.set_level(1.0));
    for (int index = 0; index < 4096; ++index)
        REQUIRE(std::isfinite(source.process()));
    REQUIRE(source.set_level(std::numeric_limits<double>::denorm_min()));
    for (int index = 0; index < 4096; ++index)
        REQUIRE_FALSE(pulp::signal::is_denormal(source.process()));
    STATIC_REQUIRE(pulp::signal::NoiseTilt::latency_samples() == 0);
    STATIC_REQUIRE(pulp::signal::NoiseTilt64::tail_samples() == 0);
}

TEST_CASE("Stochastic source process paths allocate nothing", "[signal][stochastic][rt-safety]") {
    pulp::signal::Lfsr64 lfsr;
    pulp::signal::Dust64 dust;
    pulp::signal::NoiseTilt64 tilt;
    REQUIRE(dust.prepare(48000.0));
    REQUIRE(dust.set_density_hz(1000.0));
    REQUIRE(tilt.prepare(48000.0));
    REQUIRE(tilt.set_tilt_db_per_octave(-5.0));
    double sink = 0.0;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int index = 0; index < 16384; ++index) {
            lfsr.clock((index & 127) == 0);
            sink += lfsr.weighted_output() + dust.process() + tilt.process();
        }
        lfsr.reset();
        dust.reset();
        tilt.reset();
        allocations = probe.allocation_count();
    }
    REQUIRE(std::isfinite(sink));
    REQUIRE(allocations == 0);
}
