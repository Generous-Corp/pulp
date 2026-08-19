#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/tempo_delay.hpp>
#include <pulp/timebase/tick.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

using Catch::Matchers::WithinAbs;
using pulp::signal::TempoDelayError;
using pulp::signal::TempoDelayTime;
using pulp::timebase::BeatDivision;

namespace {

constexpr std::array<std::int64_t, 21> kDivisionTicks{
    2'822'400, 4'233'600, 1'881'600, 1'411'200, 2'116'800, 940'800, 705'600,
    1'058'400, 470'400,   352'800,   529'200,   235'200,   176'400, 264'600,
    117'600,   88'200,    132'300,   58'800,    44'100,    66'150,  29'400,
};

double tick_sample_oracle(std::int64_t ticks, double bpm, double sample_rate) {
    return static_cast<double>(ticks) * 60.0 * sample_rate /
           (static_cast<double>(pulp::timebase::kTicksPerQuarter) * bpm);
}

std::array<double, 40> render_partitioned(const std::array<std::size_t, 5>& partitions) {
    TempoDelayTime time;
    REQUIRE(time.prepare(48000.0, 96000.0, 8) == TempoDelayError::none);
    REQUIRE(time.set_delay_samples(12000.0) == TempoDelayError::none);
    std::array<double, 40> output{};
    std::size_t cursor = 0;
    std::size_t block = 0;
    while (cursor < output.size()) {
        if (cursor == 7)
            REQUIRE(time.set_tempo(BeatDivision::Quarter, 120.0) == TempoDelayError::none);
        if (cursor == 23)
            REQUIRE(time.set_tempo(BeatDivision::Eighth, 90.0) == TempoDelayError::none);
        const std::size_t boundary = cursor < 7 ? 7 : (cursor < 23 ? 23 : output.size());
        const std::size_t count = std::min(
            {partitions[block++ % partitions.size()], output.size() - cursor, boundary - cursor});
        time.render(output.data() + cursor, count);
        cursor += count;
    }
    return output;
}

} // namespace

static_assert(noexcept(std::declval<TempoDelayTime&>().next()));
static_assert(noexcept(std::declval<TempoDelayTime&>().render(nullptr, 0)));
static_assert(noexcept(std::declval<TempoDelayTime&>().reset()));
static_assert(TempoDelayTime::latency_samples() == 0);
static_assert(TempoDelayTime::tail_samples() == 0);

TEST_CASE("tempo delay converts every division with an independent tick/sample oracle",
          "[signal][tempo-delay][division][oracle]") {
    constexpr double sample_rate = 48000.0;
    constexpr double bpm = 137.0;
    for (std::size_t index = 0; index < kDivisionTicks.size(); ++index) {
        const auto result =
            pulp::signal::tempo_delay_samples(static_cast<BeatDivision>(index), bpm, sample_rate);
        REQUIRE(result);
        const double expected = tick_sample_oracle(kDivisionTicks[index], bpm, sample_rate);
        REQUIRE_THAT(result.samples, WithinAbs(expected, 2.0e-12));
    }

    const auto dotted =
        pulp::signal::tempo_delay_samples(BeatDivision::QuarterDotted, 120.0, sample_rate);
    const auto triplet =
        pulp::signal::tempo_delay_samples(BeatDivision::EighthTriplet, 120.0, sample_rate);
    REQUIRE(dotted.samples == 36000.0);
    REQUIRE(triplet.samples == 8000.0);

    // Planted vocabulary controls: treating dotted as straight or triplet as
    // binary subdivision must not satisfy the independent tick oracle.
    REQUIRE(dotted.samples != tick_sample_oracle(705'600, 120.0, sample_rate));
    REQUIRE(triplet.samples != tick_sample_oracle(352'800, 120.0, sample_rate));
}

TEST_CASE("tempo delay conversion follows compiled tempo-map validation bounds",
          "[signal][tempo-delay][validation]") {
    constexpr double sr = 48000.0;
    REQUIRE(pulp::signal::tempo_delay_samples(BeatDivision::Quarter, 1.0, sr));
    REQUIRE(pulp::signal::tempo_delay_samples(BeatDivision::Quarter, 1000.0, sr));
    REQUIRE_FALSE(pulp::signal::tempo_delay_samples(BeatDivision::Quarter, 0.999, sr));
    REQUIRE_FALSE(pulp::signal::tempo_delay_samples(BeatDivision::Quarter, 1000.001, sr));
    REQUIRE_FALSE(pulp::signal::tempo_delay_samples(BeatDivision::Quarter,
                                                    std::numeric_limits<double>::quiet_NaN(), sr));
    REQUIRE_FALSE(pulp::signal::tempo_delay_samples(BeatDivision::Quarter, 120.0, 0.0));
    REQUIRE_FALSE(pulp::signal::tempo_delay_samples(
        BeatDivision::Quarter, 120.0, pulp::signal::kMaximumTempoDelaySampleRate + 1.0));
    REQUIRE_FALSE(pulp::signal::tempo_delay_samples(static_cast<BeatDivision>(255), 120.0, sr));

    const auto too_short =
        pulp::signal::tempo_delay_samples(BeatDivision::SixtyFourthTriplet, 1000.0, 0.01);
    REQUIRE(too_short.error == TempoDelayError::out_of_range);
}

TEST_CASE("tempo delay prepare and updates are transactional",
          "[signal][tempo-delay][configuration]") {
    TempoDelayTime time;
    REQUIRE(time.set_delay_samples(10.0) == TempoDelayError::not_prepared);
    REQUIRE(time.set_tempo(BeatDivision::Quarter, 120.0) == TempoDelayError::not_prepared);
    REQUIRE(time.prepare(48000.0, 96000.0, 16) == TempoDelayError::none);
    REQUIRE(time.set_delay_samples(1000.0) == TempoDelayError::none);
    REQUIRE(time.set_delay_samples(2000.0) == TempoDelayError::none);
    (void)time.next();
    const auto current = time.current_delay_samples();
    const auto target = time.target_delay_samples();
    const auto remaining = time.remaining_transition_samples();

    REQUIRE(time.set_delay_samples(std::numeric_limits<double>::quiet_NaN()) ==
            TempoDelayError::invalid_delay);
    REQUIRE(time.set_delay_samples(96001.0) == TempoDelayError::out_of_range);
    REQUIRE(time.set_tempo(static_cast<BeatDivision>(255), 120.0) ==
            TempoDelayError::invalid_division);
    REQUIRE(time.set_tempo(BeatDivision::Quarter, std::numeric_limits<double>::infinity()) ==
            TempoDelayError::invalid_tempo);
    REQUIRE(time.prepare(0.0, 96000.0) == TempoDelayError::invalid_sample_rate);
    REQUIRE(time.prepare(48000.0, std::numeric_limits<double>::infinity()) ==
            TempoDelayError::invalid_delay);
    REQUIRE(time.current_delay_samples() == current);
    REQUIRE(time.target_delay_samples() == target);
    REQUIRE(time.remaining_transition_samples() == remaining);
    REQUIRE(time.prepared());
}

TEST_CASE("tempo delay transition has exact sample-count and retarget semantics",
          "[signal][tempo-delay][transition]") {
    TempoDelayTime time;
    REQUIRE(time.prepare(48000.0, 1000.0, 4) == TempoDelayError::none);
    REQUIRE(time.set_delay_samples(100.0) == TempoDelayError::none);
    REQUIRE(time.next() == 100.0); // First setting is immediate.
    REQUIRE(time.set_delay_samples(200.0) == TempoDelayError::none);
    REQUIRE(time.conservative_delay_samples() == 200.0);
    REQUIRE(time.next() == 125.0);
    REQUIRE(time.next() == 150.0);
    REQUIRE(time.set_delay_samples(110.0) == TempoDelayError::none);
    REQUIRE_THAT(time.next(), WithinAbs(140.0, 1.0e-12));
    REQUIRE_THAT(time.next(), WithinAbs(130.0, 1.0e-12));
    REQUIRE_THAT(time.next(), WithinAbs(120.0, 1.0e-12));
    REQUIRE_THAT(time.next(), WithinAbs(110.0, 1.0e-12));
    REQUIRE_FALSE(time.transition_active());
    REQUIRE(time.current_delay_samples() == 110.0);
    REQUIRE(time.conservative_delay_samples() == 110.0);

    REQUIRE(time.set_delay_samples(300.0) == TempoDelayError::none);
    REQUIRE(time.next() == 157.5);
    REQUIRE(time.set_delay_samples(time.current_delay_samples()) == TempoDelayError::none);
    REQUIRE_FALSE(time.transition_active());
    REQUIRE(time.target_delay_samples() == 157.5);
    REQUIRE(time.conservative_delay_samples() == 157.5);

    REQUIRE(time.set_delay_samples(300.0) == TempoDelayError::none);
    REQUIRE(time.next() == 193.125);
    time.reset();
    REQUIRE_FALSE(time.transition_active());
    REQUIRE(time.current_delay_samples() == 300.0);
    REQUIRE(time.next() == 300.0);

    TempoDelayTime immediate;
    REQUIRE(immediate.prepare(48000.0, 1000.0, 0) == TempoDelayError::none);
    REQUIRE(immediate.set_delay_samples(10.0) == TempoDelayError::none);
    REQUIRE(immediate.set_delay_samples(20.0) == TempoDelayError::none);
    REQUIRE(immediate.current_delay_samples() == 20.0);
    REQUIRE(immediate.conservative_delay_samples() == 20.0);
    REQUIRE_FALSE(immediate.transition_active());
}

TEST_CASE("tempo changes use the new BPM and catch a planted stale-tempo control",
          "[signal][tempo-delay][tempo-change][negative-control]") {
    TempoDelayTime time;
    REQUIRE(time.prepare(48000.0, 96000.0, 8) == TempoDelayError::none);
    REQUIRE(time.set_tempo(BeatDivision::Quarter, 120.0) == TempoDelayError::none);
    REQUIRE(time.target_delay_samples() == 24000.0);
    REQUIRE(time.set_tempo(BeatDivision::Eighth, 90.0) == TempoDelayError::none);
    const double expected = tick_sample_oracle(352'800, 90.0, 48000.0);
    const double stale_tempo_control = tick_sample_oracle(352'800, 120.0, 48000.0);
    REQUIRE(time.target_delay_samples() == expected);
    REQUIRE(time.target_delay_samples() != stale_tempo_control);
    for (int i = 0; i < 8; ++i)
        (void)time.next();
    REQUIRE(time.current_delay_samples() == expected);
}

TEST_CASE("tempo delay block rendering is sample-partition deterministic",
          "[signal][tempo-delay][block][partition]") {
    const auto scalar = render_partitioned({1, 1, 1, 1, 1});
    const auto blocked = render_partitioned({9, 2, 13, 1, 7});
    REQUIRE(scalar == blocked);
}

TEST_CASE("tempo delay controller realtime paths allocate no memory",
          "[signal][tempo-delay][realtime]") {
    std::size_t planted_allocations = 0;
    void* planted = nullptr;
    {
        pulp::test::RtAllocationProbe probe;
        planted = ::operator new(sizeof(double) * 32);
        planted_allocations = probe.allocation_count();
    }
    ::operator delete(planted);
    REQUIRE(planted_allocations > 0);

    TempoDelayTime time;
    REQUIRE(time.prepare(48000.0, 96000.0, 32) == TempoDelayError::none);
    std::array<double, 128> output{};
    pulp::test::RtAllocationProbe probe;
    REQUIRE(time.set_tempo(BeatDivision::QuarterDotted, 123.0) == TempoDelayError::none);
    time.render(output.data(), output.size());
    REQUIRE(time.set_tempo(BeatDivision::SixteenthTriplet, 87.0) == TempoDelayError::none);
    time.render(output.data(), output.size());
    time.reset();
    REQUIRE(probe.allocation_count() == 0);
}

TEST_CASE("tempo delay controller metadata reflects control-only ownership",
          "[signal][tempo-delay][metadata]") {
    TempoDelayTime time;
    REQUIRE_FALSE(time.prepared());
    REQUIRE(time.next() == 0.0);
    REQUIRE(time.current_delay_samples() == 0.0);
    REQUIRE(time.target_delay_samples() == 0.0);
    REQUIRE(time.conservative_delay_samples() == 0.0);
    REQUIRE(time.latency_samples() == 0);
    REQUIRE(time.tail_samples() == 0);
    time.render(nullptr, 128);
}
