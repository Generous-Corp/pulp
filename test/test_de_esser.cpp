#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/de_esser.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::DeEsser64;

namespace {

constexpr double kSampleRate = 48000.0;

std::vector<double> sine(double frequency_hz, double amplitude, std::size_t sample_count) {
    std::vector<double> result(sample_count);
    for (std::size_t i = 0; i < sample_count; ++i) {
        result[i] = amplitude * std::sin(2.0 * std::numbers::pi * frequency_hz *
                                         static_cast<double>(i) / kSampleRate);
    }
    return result;
}

// Independent least-squares amplitude oracle. It does not share filter,
// envelope, or gain-computer arithmetic with the processor.
double sine_amplitude(std::span<const double> signal, std::size_t begin, double frequency_hz) {
    double ss = 0.0;
    double cc = 0.0;
    double sc = 0.0;
    double sy = 0.0;
    double cy = 0.0;
    for (std::size_t i = begin; i < signal.size(); ++i) {
        const double phase =
            2.0 * std::numbers::pi * frequency_hz * static_cast<double>(i) / kSampleRate;
        const double s = std::sin(phase);
        const double c = std::cos(phase);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        sy += s * signal[i];
        cy += c * signal[i];
    }
    const double determinant = ss * cc - sc * sc;
    const double a = (sy * cc - cy * sc) / determinant;
    const double b = (cy * ss - sy * sc) / determinant;
    return std::hypot(a, b);
}

std::vector<double> render(DeEsser64& de_esser, const std::vector<double>& input) {
    std::vector<double> output(input.size());
    de_esser.process(input.data(), output.data(), output.size());
    return output;
}

double ideal_reduction_db(double detector_level_db, double threshold_db, double range_db) {
    return std::clamp(detector_level_db - threshold_db, 0.0, range_db);
}

bool reduction_matches(double measured, double expected, double tolerance = 0.15) {
    return std::abs(measured - expected) <= tolerance;
}

} // namespace

TEST_CASE("de-esser rejects malformed configuration without mutation",
          "[signal][de-esser][configuration][negative]") {
    DeEsser64 de_esser;
    REQUIRE(de_esser.prepare(kSampleRate));
    const auto original = de_esser.params();

    auto invalid = original;
    invalid.detector_frequency_hz = kSampleRate;
    REQUIRE_FALSE(de_esser.set_params(invalid));
    REQUIRE(de_esser.params().detector_frequency_hz == original.detector_frequency_hz);

    invalid = original;
    invalid.range_db = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(de_esser.set_params(invalid));
    REQUIRE_FALSE(DeEsser64{}.prepare(0.0));
}

TEST_CASE("de-esser detector listen isolates the configured sibilance band",
          "[signal][de-esser][frequency][oracle]") {
    DeEsser64::Params params;
    params.threshold_db = 24.0;
    params.detector_frequency_hz = 6500.0;
    params.detector_q = 2.0;

    DeEsser64 center;
    DeEsser64 low;
    REQUIRE(center.prepare(kSampleRate, params));
    REQUIRE(low.prepare(kSampleRate, params));
    center.set_output_mode(DeEsser64::OutputMode::detector_listen);
    low.set_output_mode(DeEsser64::OutputMode::detector_listen);

    const auto center_output = render(center, sine(6500.0, 0.5, 48000));
    const auto low_output = render(low, sine(500.0, 0.5, 48000));
    const double center_amplitude = sine_amplitude(center_output, 4096, 6500.0);
    const double low_amplitude = sine_amplitude(low_output, 4096, 500.0);

    REQUIRE(center_amplitude > 0.45);
    REQUIRE(low_amplitude < 0.03);
    REQUIRE(center_amplitude > low_amplitude * 15.0);

    // Planted negative control: swapping the measurements must make this same
    // frequency-discrimination verdict fail.
    const auto separates_band = [](double pass, double reject) {
        return pass > 0.4 && reject < 0.05 && pass > reject * 10.0;
    };
    REQUIRE(separates_band(center_amplitude, low_amplitude));
    REQUIRE_FALSE(separates_band(low_amplitude, center_amplitude));
}

TEST_CASE("de-esser unity gain recombines with flat sine magnitude",
          "[signal][de-esser][recombination][oracle]") {
    DeEsser64::Params params;
    params.threshold_db = 24.0;
    DeEsser64 de_esser;
    REQUIRE(de_esser.prepare(kSampleRate, params));

    for (const double frequency : {200.0, 3000.0, 12000.0}) {
        de_esser.reset();
        const auto input = sine(frequency, 0.4, 48000);
        const auto output = render(de_esser, input);
        const double measured = sine_amplitude(output, 8192, frequency);
        REQUIRE_THAT(measured, WithinAbs(0.4, 2.0e-4));
        REQUIRE_THAT(de_esser.gain_reduction_db(), WithinAbs(0.0, 1.0e-12));
    }
}

TEST_CASE("de-esser applies bounded analytic reduction only to sibilance",
          "[signal][de-esser][dynamics][oracle]") {
    DeEsser64::Params params;
    params.threshold_db = -18.0;
    params.range_db = 9.0;
    params.attack_ms = 0.1;
    params.release_ms = 1000.0;
    params.detector_frequency_hz = 6500.0;
    params.detector_q = 2.0;
    params.split_frequency_hz = 4000.0;

    DeEsser64 sibilance;
    REQUIRE(sibilance.prepare(kSampleRate, params));
    const auto hot_input = sine(6500.0, 0.5, 96000);
    const auto hot_output = render(sibilance, hot_input);

    const double expected =
        ideal_reduction_db(sibilance.detector_level_db(), params.threshold_db, params.range_db);
    REQUIRE(reduction_matches(sibilance.gain_reduction_db(), expected));
    REQUIRE(sibilance.gain_reduction_db() > 8.8);
    // The LR low branch still contributes above the split, so this is a bound
    // on the recombined signal rather than the 9 dB high-band gain alone.
    REQUIRE(sine_amplitude(hot_output, 48000, 6500.0) < 0.25);

    // Planted gain-computer mutation: a 3 dB range cap must be rejected by the
    // same oracle that accepts the real 9 dB result.
    const double wrong =
        ideal_reduction_db(sibilance.detector_level_db(), params.threshold_db, 3.0);
    REQUIRE_FALSE(reduction_matches(sibilance.gain_reduction_db(), wrong));

    DeEsser64 nonsibilant;
    REQUIRE(nonsibilant.prepare(kSampleRate, params));
    const auto low_output = render(nonsibilant, sine(500.0, 0.5, 96000));
    REQUIRE(nonsibilant.gain_reduction_db() < 0.05);
    REQUIRE_THAT(sine_amplitude(low_output, 48000, 500.0), WithinAbs(0.5, 3.0e-4));
}

TEST_CASE("de-esser bypass is sample exact while detector state advances",
          "[signal][de-esser][bypass]") {
    DeEsser64 de_esser;
    REQUIRE(de_esser.prepare(kSampleRate));
    de_esser.set_bypassed(true);
    de_esser.set_output_mode(DeEsser64::OutputMode::detector_listen);
    const auto input = sine(6500.0, 0.37, 4096);
    const auto output = render(de_esser, input);

    REQUIRE(output == input);
    REQUIRE(de_esser.gain_reduction_db() > 0.0);
}

TEST_CASE("de-esser nonfinite recovery and reset preserve configuration",
          "[signal][de-esser][nonfinite][reset]") {
    DeEsser64 de_esser;
    REQUIRE(de_esser.prepare(kSampleRate));
    for (const double sample : sine(6500.0, 0.8, 4096))
        (void)de_esser.process(sample);
    REQUIRE(de_esser.gain_reduction_db() > 0.0);

    const auto params = de_esser.params();
    REQUIRE(de_esser.process(std::numeric_limits<double>::infinity()) == 0.0);
    REQUIRE_FALSE(de_esser.healthy());
    REQUIRE(de_esser.fault_count() == 1);
    REQUIRE(de_esser.params().threshold_db == params.threshold_db);
    REQUIRE(de_esser.gain_reduction_db() == 0.0);

    REQUIRE(std::isfinite(de_esser.process(0.25)));
    REQUIRE(de_esser.healthy());
}

TEST_CASE("de-esser block partitions and reset are sample deterministic",
          "[signal][de-esser][partition][reset]") {
    const auto input = sine(6500.0, 0.6, 8192);
    DeEsser64 whole;
    DeEsser64 partitioned;
    REQUIRE(whole.prepare(kSampleRate));
    REQUIRE(partitioned.prepare(kSampleRate));

    const auto whole_output = render(whole, input);
    std::vector<double> partitioned_output(input.size());
    std::size_t position = 0;
    for (const std::size_t length : {1u, 17u, 64u, 511u, 2048u, 4096u, 1455u}) {
        partitioned.process(input.data() + position, partitioned_output.data() + position, length);
        position += length;
    }
    REQUIRE(position == input.size());
    REQUIRE(partitioned_output == whole_output);

    whole.reset();
    const auto after_reset = render(whole, input);
    REQUIRE(after_reset == whole_output);
}

TEST_CASE("de-esser realtime process path is allocation free", "[signal][de-esser][realtime]") {
    // Confirm the probe can report a real allocation before trusting its zero.
    std::size_t planted_allocations = 0;
    void* planted = nullptr;
    {
        pulp::test::RtAllocationProbe probe;
        // Call the replaceable function directly: an otherwise non-escaping
        // new-expression may legally be elided at -O3.
        planted = ::operator new(sizeof(double) * 64);
        static_cast<double*>(planted)[0] = 1.0;
        planted_allocations = probe.allocation_count();
    }
    REQUIRE(static_cast<double*>(planted)[0] == 1.0);
    ::operator delete(planted);
    REQUIRE(planted_allocations > 0);

    DeEsser64 de_esser;
    REQUIRE(de_esser.prepare(kSampleRate));
    std::array<double, 256> block{};
    for (std::size_t i = 0; i < block.size(); ++i)
        block[i] =
            0.5 * std::sin(2.0 * std::numbers::pi * 6500.0 * static_cast<double>(i) / kSampleRate);

    pulp::test::RtAllocationProbe probe;
    de_esser.process(block.data(), block.size());
    de_esser.set_bypassed(true);
    de_esser.process(block.data(), block.size());
    de_esser.set_output_mode(DeEsser64::OutputMode::detector_listen);
    de_esser.process(block.data(), block.size());
    de_esser.reset();
    REQUIRE(probe.allocation_count() == 0);
}
