#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/transient_designer.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using pulp::signal::EnvelopeFollower64;
using pulp::signal::TransientDesigner64;

namespace {

constexpr double kSampleRate = 1000.0;

double advance_reference_envelope(double input, double& state, double attack_ms,
                                  double release_ms) {
    const double coefficient = EnvelopeFollower64::coefficient_for_time_ms(
        input > state ? attack_ms : release_ms, kSampleRate);
    state += coefficient * (std::abs(input) - state);
    return state;
}

TransientDesigner64 configured(double attack_db, double sustain_db) {
    TransientDesigner64 designer;
    designer.set_fast_attack_ms(1.0);
    designer.set_fast_release_ms(10.0);
    designer.set_slow_attack_ms(10.0);
    designer.set_slow_release_ms(100.0);
    designer.set_attack_db(attack_db);
    designer.set_sustain_db(sustain_db);
    designer.prepare(kSampleRate);
    return designer;
}

} // namespace

TEST_CASE("Transient designer follows an independent fast-slow envelope oracle",
          "[signal][transient-designer][oracle]") {
    auto designer = configured(12.0, 6.0);
    double fast = 0.0;
    double slow = 0.0;
    const std::array<double, 12> input{1.0,  1.0, 1.0, 0.25, 0.25, 0.25,
                                       0.25, 0.0, 0.0, 0.0,  0.0,  0.0};

    for (double sample : input) {
        const double fast_envelope = advance_reference_envelope(sample, fast, 1.0, 10.0);
        const double slow_envelope = advance_reference_envelope(sample, slow, 10.0, 100.0);
        const double scale = std::max({fast_envelope, slow_envelope, 1.0e-12});
        const double contrast = std::clamp((fast_envelope - slow_envelope) / scale, -1.0, 1.0);
        const double expected_db = 12.0 * std::max(contrast, 0.0) + 6.0 * std::max(-contrast, 0.0);
        const double expected = sample * std::pow(10.0, expected_db / 20.0);
        REQUIRE_THAT(designer.process(sample), WithinRel(expected, 1.0e-12));
        REQUIRE_THAT(designer.contrast(), WithinAbs(contrast, 1.0e-12));
    }
}

TEST_CASE("Attack and sustain controls shape different regions",
          "[signal][transient-designer][behavior]") {
    auto attack_only = configured(12.0, 0.0);
    const double enhanced_onset = attack_only.process(1.0);
    REQUIRE(enhanced_onset > 1.5);

    auto sustain_only = configured(0.0, 12.0);
    for (int i = 0; i < 200; ++i)
        sustain_only.process(1.0);
    const double enhanced_body = sustain_only.process(0.25);
    REQUIRE(enhanced_body > 0.25);
    REQUIRE(sustain_only.contrast() < 0.0);

    auto attack_cut = configured(-12.0, 0.0);
    REQUIRE(attack_cut.process(1.0) < 1.0);
    auto sustain_cut = configured(0.0, -12.0);
    for (int i = 0; i < 200; ++i)
        sustain_cut.process(1.0);
    REQUIRE(sustain_cut.process(0.25) < 0.25);
}

TEST_CASE("Neutral transient designer is sample-exact while its detector advances",
          "[signal][transient-designer][neutral]") {
    auto designer = configured(0.0, 0.0);
    for (double sample : {-1.0, -0.0, 0.125, 0.75, -0.5})
        REQUIRE(designer.process(sample) == sample);
    REQUIRE(designer.contrast() != 0.0);
    STATIC_REQUIRE(TransientDesigner64::latency_samples() == 0);
}

TEST_CASE("Transient designer reset and block partitioning are deterministic",
          "[signal][transient-designer][partition]") {
    std::array<double, 257> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = i < 31 ? 0.0 : (i < 97 ? 0.8 : 0.2);
    std::array<double, 257> whole{};
    std::array<double, 257> split{};

    auto a = configured(9.0, -4.0);
    auto b = configured(9.0, -4.0);
    a.process(input.data(), whole.data(), static_cast<int>(input.size()));
    b.process(input.data(), split.data(), 73);
    b.process(input.data() + 73, split.data() + 73, 184);
    REQUIRE(whole == split);

    a.reset();
    std::array<double, 257> replay{};
    a.process(input.data(), replay.data(), static_cast<int>(input.size()));
    REQUIRE(whole == replay);
}

TEST_CASE("Transient designer processing allocates no memory", "[signal][transient-designer][rt]") {
    auto designer = configured(8.0, 5.0);
    std::array<double, 2048> input{};
    std::array<double, 2048> output{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = std::sin(0.071 * static_cast<double>(i));

    pulp::test::RtAllocationProbe probe;
    designer.process(input.data(), output.data(), static_cast<int>(input.size()));
    REQUIRE(probe.allocation_count() == 0);
}

TEST_CASE("Transient designer clamps controls and fails closed on hostile audio",
          "[signal][transient-designer][safety]") {
    auto designer = configured(100.0, -100.0);
    REQUIRE(designer.attack_db() == 24.0);
    REQUIRE(designer.sustain_db() == -24.0);
    designer.set_attack_db(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(designer.attack_db() == 24.0);

    REQUIRE(designer.process(std::numeric_limits<double>::infinity()) == 0.0);
    auto fresh = configured(24.0, -24.0);
    for (double sample : {0.1, 0.2, 0.0, -0.4})
        REQUIRE(designer.process(sample) == fresh.process(sample));

    REQUIRE(designer.process(std::numeric_limits<double>::max()) == 0.0);
    fresh.reset();
    for (double sample : {0.1, 0.2, 0.0, -0.4})
        REQUIRE(designer.process(sample) == fresh.process(sample));
}
