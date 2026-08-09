#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/dynamic_eq.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using pulp::signal::DynamicEqBand64;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kFrequency = 1000.0;
constexpr double kPi = 3.141592653589793238462643383279502884;

DynamicEqBand64 make_band(double range_db) {
    DynamicEqBand64 band;
    band.prepare(kSampleRate);
    REQUIRE(band.set_parameters({.frequency_hz = kFrequency,
                                 .q = 2.3,
                                 .threshold_db = -40.0,
                                 .range_db = range_db,
                                 .transition_db = 6.0,
                                 .attack_ms = 2.0,
                                 .release_ms = 50.0}));
    return band;
}

struct ReferenceBiquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double s1 = 0.0, s2 = 0.0;

    double process(double input) {
        const double output = b0 * input + s1;
        s1 = b1 * input - a1 * output + s2;
        s2 = b2 * input - a2 * output;
        return output;
    }
};

} // namespace

TEST_CASE("Dynamic EQ matches independent band, envelope, and gain oracles",
          "[signal][dynamic-eq][oracle]") {
    auto band = make_band(-9.0);
    const double w0 = 2.0 * kPi * kFrequency / kSampleRate;
    const double alpha = std::sin(w0) / (2.0 * 2.3);
    const double a0 = 1.0 + alpha;
    ReferenceBiquad reference{alpha / a0, 0.0, -alpha / a0, -2.0 * std::cos(w0) / a0,
                              (1.0 - alpha) / a0};
    const auto actual = band.band_coefficients();
    REQUIRE_THAT(actual.b0, WithinAbs(reference.b0, 1.0e-15));
    REQUIRE_THAT(actual.b1, WithinAbs(reference.b1, 1.0e-15));
    REQUIRE_THAT(actual.b2, WithinAbs(reference.b2, 1.0e-15));
    REQUIRE_THAT(actual.a1, WithinAbs(reference.a1, 1.0e-15));
    REQUIRE_THAT(actual.a2, WithinAbs(reference.a2, 1.0e-15));
    double envelope = 0.0;
    const double attack = 1.0 - std::exp(-std::log(9.0) / (2.0e-3 * kSampleRate));
    const double release = 1.0 - std::exp(-std::log(9.0) / (50.0e-3 * kSampleRate));

    for (int i = 0; i < 1200; ++i) {
        const double input = 0.7 * std::sin(2.0 * kPi * kFrequency * i / kSampleRate) +
                             0.1 * std::sin(2.0 * kPi * 7000.0 * i / kSampleRate);
        const double filtered = reference.process(input);
        const double magnitude = std::abs(filtered);
        envelope += (magnitude > envelope ? attack : release) * (magnitude - envelope);
        const double level_db = envelope > 0.0 ? 20.0 * std::log10(envelope) : -160.0;
        const double activity = std::clamp((level_db + 40.0) / 6.0, 0.0, 1.0);
        const double gain_db = -9.0 * activity;
        const double expected = input + filtered * (std::pow(10.0, gain_db / 20.0) - 1.0);
        REQUIRE_THAT(band.process(input), WithinRel(expected, 2.0e-12));
        REQUIRE_THAT(band.dynamic_gain_db(), WithinAbs(gain_db, 2.0e-12));
    }
}

TEST_CASE("Dynamic EQ boost and cut converge to their centre-frequency ranges",
          "[signal][dynamic-eq][behavior]") {
    for (double range_db : {-12.0, 9.0}) {
        auto band = make_band(range_db);
        double input_energy = 0.0;
        double output_energy = 0.0;
        constexpr int total = 48000;
        constexpr int measure = 4800;
        for (int i = 0; i < total; ++i) {
            const double input = 0.5 * std::sin(2.0 * kPi * kFrequency * i / kSampleRate);
            const double output = band.process(input);
            if (i >= total - measure) {
                input_energy += input * input;
                output_energy += output * output;
            }
        }
        const double measured_db = 10.0 * std::log10(output_energy / input_energy);
        REQUIRE_THAT(measured_db, WithinAbs(range_db, 0.12));
        REQUIRE_THAT(band.dynamic_gain_db(), WithinAbs(range_db, 1.0e-6));
        REQUIRE_THAT(band.gain_reduction().db(),
                     WithinAbs(range_db < 0.0 ? -range_db : 0.0, 1.0e-6));
    }
}

TEST_CASE("Dynamic EQ is exactly transparent below threshold and at neutral range",
          "[signal][dynamic-eq][transparent]") {
    auto below = make_band(-18.0);
    REQUIRE(below.set_parameters({.frequency_hz = kFrequency,
                                  .q = 1.0,
                                  .threshold_db = -6.0,
                                  .range_db = -18.0,
                                  .transition_db = 6.0,
                                  .attack_ms = 2.0,
                                  .release_ms = 50.0}));
    auto neutral = make_band(0.0);
    for (int i = 0; i < 4096; ++i) {
        const double sample = 0.01 * std::sin(0.17 * i);
        REQUIRE(below.process(sample) == sample);
        REQUIRE(neutral.process(sample) == sample);
    }
    REQUIRE(neutral.detector_level() > 0.0);
    STATIC_REQUIRE(DynamicEqBand64::latency_samples() == 0);
}

TEST_CASE("Dynamic EQ reset and block partitions reproduce exactly",
          "[signal][dynamic-eq][partition]") {
    std::array<double, 513> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = 0.6 * std::sin(2.0 * kPi * kFrequency * i / kSampleRate);
    auto whole = input;
    auto split = input;
    auto replay = input;
    auto a = make_band(-7.0);
    auto b = make_band(-7.0);
    REQUIRE(a.process_block(whole.data(), whole.size()));
    REQUIRE(b.process_block(split.data(), 1));
    REQUIRE(b.process_block(split.data() + 1, 7));
    REQUIRE(b.process_block(split.data() + 8, 64));
    REQUIRE(b.process_block(split.data() + 72, split.size() - 72));
    REQUIRE(whole == split);
    a.reset();
    REQUIRE(a.process_block(replay.data(), replay.size()));
    REQUIRE(whole == replay);
}

TEST_CASE("Dynamic EQ processing is allocation free", "[signal][dynamic-eq][rt]") {
    auto band = make_band(8.0);
    std::array<double, 2048> audio{};
    for (std::size_t i = 0; i < audio.size(); ++i)
        audio[i] = std::sin(0.13 * static_cast<double>(i));
    pulp::test::RtAllocationProbe probe;
    REQUIRE(band.process_block(audio.data(), audio.size()));
    REQUIRE(probe.allocation_count() == 0);
}

TEST_CASE("Dynamic EQ parameter updates are atomic and hostile audio fails closed",
          "[signal][dynamic-eq][safety]") {
    auto band = make_band(-6.0);
    const auto before = band.parameters();
    auto invalid = before;
    invalid.q = std::numeric_limits<double>::quiet_NaN();
    invalid.range_db = 24.0;
    REQUIRE_FALSE(band.set_parameters(invalid));
    REQUIRE(band.parameters().q == before.q);
    REQUIRE(band.parameters().range_db == before.range_db);

    auto clamped = before;
    clamped.frequency_hz = 1.0e9;
    clamped.q = 1.0e9;
    clamped.range_db = -1.0e9;
    REQUIRE(band.set_parameters(clamped));
    REQUIRE(band.parameters().frequency_hz == kSampleRate * 0.45);
    REQUIRE(band.parameters().q == 12.0);
    REQUIRE(band.parameters().range_db == -24.0);
    REQUIRE_FALSE(band.process_block(nullptr, 1));
    REQUIRE(band.process_block(nullptr, 0));

    REQUIRE(band.process(std::numeric_limits<double>::infinity()) == 0.0);
    auto fresh = band;
    fresh.reset();
    for (double sample : {0.1, -0.2, 0.3, 0.0})
        REQUIRE(band.process(sample) == fresh.process(sample));
    REQUIRE(std::isfinite(band.process(std::numeric_limits<double>::max())));

    auto overflow = before;
    overflow.threshold_db = -120.0;
    overflow.range_db = 24.0;
    overflow.transition_db = 0.1;
    overflow.attack_ms = 0.01;
    REQUIRE(band.set_parameters(overflow));
    band.reset();
    REQUIRE(band.process(std::numeric_limits<double>::max()) == 0.0);

    DynamicEqBand64 hostile_rate;
    hostile_rate.prepare(std::numeric_limits<double>::max());
    REQUIRE(hostile_rate.sample_rate() == 48000.0);
    REQUIRE(std::isfinite(hostile_rate.process(0.5)));
}
