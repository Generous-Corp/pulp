#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/sub_oscillator.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::SubOscillator;
using pulp::signal::SubOscillator64;
using pulp::signal::SubOscillatorWaveform;

namespace {

std::vector<double> render(std::size_t block_size, int octave, SubOscillatorWaveform waveform) {
    constexpr int parent_period = 37;
    constexpr int sample_count = parent_period * 17 + 11;
    std::vector<double> output(static_cast<std::size_t>(sample_count));
    SubOscillator64 oscillator;
    REQUIRE(oscillator.set_octave(octave));
    oscillator.set_waveform(waveform);

    for (std::size_t begin = 0; begin < output.size(); begin += block_size) {
        const std::size_t end = std::min(output.size(), begin + block_size);
        for (std::size_t i = begin; i < end; ++i) {
            const int sample = static_cast<int>(i);
            const bool edge = sample != 0 && (sample % parent_period) == 0;
            const double phase = static_cast<double>(sample % parent_period) / parent_period;
            output[i] = oscillator.next(phase, edge);
        }
    }
    return output;
}

} // namespace

TEST_CASE("SubOscillator divides explicit parent cycles by two and four",
          "[signal][sub-oscillator]") {
    constexpr int parent_period = 29;
    for (const int octave : {1, 2}) {
        SubOscillator64 oscillator;
        REQUIRE(oscillator.set_octave(octave));
        const int divisor = 1 << octave;
        for (int sample = 0; sample < parent_period * divisor * 3; ++sample) {
            const bool edge = sample != 0 && (sample % parent_period) == 0;
            const double parent_phase = static_cast<double>(sample % parent_period) / parent_period;
            const int parent_cycle = sample / parent_period;
            const double sub_phase = (static_cast<double>(parent_cycle % divisor) + parent_phase) /
                                     static_cast<double>(divisor);
            const double expected = sub_phase < 0.5 ? 1.0 : -1.0;
            REQUIRE(oscillator.next(parent_phase, edge) == expected);
            REQUIRE_THAT(oscillator.phase(), WithinAbs(sub_phase, 1.0e-14));
        }
    }
}

TEST_CASE("SubOscillator sine is the analytic divided parent phase", "[signal][sub-oscillator]") {
    SubOscillator64 oscillator;
    REQUIRE(oscillator.set_octave(2));
    oscillator.set_waveform(SubOscillatorWaveform::sine);
    for (int cycle = 0; cycle < 4; ++cycle) {
        if (cycle != 0)
            oscillator.on_parent_cycle();
        for (int step = 0; step < 31; ++step) {
            const double parent_phase = static_cast<double>(step) / 31.0;
            const double expected_phase = (static_cast<double>(cycle) + parent_phase) / 4.0;
            REQUIRE_THAT(oscillator.next(parent_phase),
                         WithinAbs(std::sin(2.0 * std::numbers::pi * expected_phase), 1.0e-14));
        }
    }
}

TEST_CASE("SubOscillator float follows the same divided-phase contract",
          "[signal][sub-oscillator]") {
    SubOscillator oscillator;
    REQUIRE(oscillator.set_octave(1));
    oscillator.set_waveform(SubOscillatorWaveform::sine);
    oscillator.on_parent_cycle();
    const float parent_phase = 0.375f;
    const float expected_phase = (1.0f + parent_phase) * 0.5f;
    REQUIRE_THAT(oscillator.next(parent_phase),
                 WithinAbs(std::sin(2.0f * std::numbers::pi_v<float> * expected_phase), 2.0e-6));
}

TEST_CASE("SubOscillator preserves parent history when switching octaves",
          "[signal][sub-oscillator][regression]") {
    SubOscillator64 oscillator;
    REQUIRE(oscillator.set_octave(1));
    oscillator.on_parent_cycle();
    oscillator.on_parent_cycle();
    oscillator.on_parent_cycle();
    REQUIRE(oscillator.next(0.0) == -1.0);

    REQUIRE(oscillator.set_octave(2));
    REQUIRE_THAT(oscillator.phase(), WithinAbs(0.5, 1.0e-14));
    REQUIRE(oscillator.next(0.0) == -1.0);
    REQUIRE_THAT(oscillator.phase(), WithinAbs(0.75, 1.0e-14));
}

TEST_CASE("SubOscillator follows parent reset and phase jump instead of free running",
          "[signal][sub-oscillator][negative-control]") {
    SubOscillator64 oscillator;
    oscillator.set_waveform(SubOscillatorWaveform::sine);
    REQUIRE(oscillator.set_octave(1));

    for (int i = 0; i < 13; ++i)
        (void)oscillator.next(static_cast<double>(i) / 16.0);

    oscillator.reset();
    const double jumped_parent_phase = 0.0;
    const double locked = oscillator.next(jumped_parent_phase);
    const double expected = std::sin(std::numbers::pi * jumped_parent_phase);
    REQUIRE_THAT(locked, WithinAbs(expected, 1.0e-14));

    // A sample-count divider retains its old phase through the parent reset.
    const double stale_free_running_phase = 13.0 / 32.0;
    const double stale = std::sin(2.0 * std::numbers::pi * stale_free_running_phase);
    REQUIRE(std::fabs(stale - expected) > 0.5);
}

TEST_CASE("SubOscillator rendering is invariant to irregular block partitions",
          "[signal][sub-oscillator]") {
    const auto reference = render(1, 2, SubOscillatorWaveform::sine);
    for (const std::size_t block : {std::size_t{7}, std::size_t{31}, std::size_t{113}})
        REQUIRE(render(block, 2, SubOscillatorWaveform::sine) == reference);
}

TEST_CASE("SubOscillator rejects invalid controls and absorbs invalid parent phase",
          "[signal][sub-oscillator]") {
    SubOscillator64 oscillator;
    REQUIRE(oscillator.set_octave(2));
    REQUIRE_FALSE(oscillator.set_octave(0));
    REQUIRE_FALSE(oscillator.set_octave(3));
    REQUIRE(oscillator.octave() == 2);
    REQUIRE(oscillator.next(std::numeric_limits<double>::quiet_NaN()) == 0.0);
    REQUIRE(oscillator.phase() == 0.0);
    REQUIRE(oscillator.next(-0.1) == 0.0);
    REQUIRE(oscillator.next(1.0) == 0.0);
}

TEST_CASE("SubOscillator audio-thread operations allocate no memory",
          "[signal][sub-oscillator][rt]") {
    SubOscillator64 oscillator;
    REQUIRE(oscillator.set_octave(2));
    oscillator.set_waveform(SubOscillatorWaveform::sine);
    double checksum = 0.0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 4096; ++i) {
            const bool edge = i != 0 && (i % 47) == 0;
            checksum += oscillator.next(static_cast<double>(i % 47) / 47.0, edge);
            if (i == 2048)
                oscillator.reset();
        }
        CHECK_FALSE(probe.saw_allocation());
    }
    CHECK(std::isfinite(checksum));
}
