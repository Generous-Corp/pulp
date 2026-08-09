#include <pulp/timebase/compiled_tempo_map.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <utility>

using namespace pulp::timebase;

namespace {

CompiledTempoMap make_capability_map() {
    constexpr std::array points{
        TempoPoint{{0}, 72.0, TempoCurve::LinearInTicks},
        TempoPoint{{8 * kTicksPerQuarter}, 132.0, TempoCurve::LinearInTicks},
        TempoPoint{{16 * kTicksPerQuarter}, 96.0, TempoCurve::Constant},
    };
    auto result = CompiledTempoMap::compile(points, RationalRate{48'000, 1});
    REQUIRE(result);
    return std::move(result).value();
}

} // namespace

TEST_CASE("tempo-map capability is deterministic within one build") {
    const auto first = make_capability_map();
    const auto second = make_capability_map();

    for (std::int64_t tick = -2 * kTicksPerQuarter;
         tick <= 20 * kTicksPerQuarter; tick += 997) {
        const TickPosition position{tick};
        REQUIRE(first.ticks_to_samples(position) == second.ticks_to_samples(position));
    }

    for (std::int64_t sample = -64'000; sample <= 480'000; sample += 991) {
        const SamplePosition position{sample};
        const auto first_result = first.resolve_sample(position);
        const auto second_result = second.resolve_sample(position);
        REQUIRE(first_result.tick == second_result.tick);
        REQUIRE(first_result.represented_sample == second_result.represented_sample);
        REQUIRE(first_result.absolute_error_samples == second_result.absolute_error_samples);
        REQUIRE(first_result.exact == second_result.exact);
    }
}

TEST_CASE("tempo-map capability lookups allocate no memory") {
    const auto map = make_capability_map();
    std::int64_t checksum = 0;
    std::size_t allocations = 1;
    {
        pulp::test::RtAllocationProbe probe;
        for (std::int64_t index = 0; index < 100'000; ++index) {
            checksum += map.ticks_to_samples(TickPosition{index * 97}).value;
            checksum += map.resolve_sample(SamplePosition{index * 83}).tick.value;
        }
        allocations = probe.allocation_count();
    }

    REQUIRE(checksum != 0);
    REQUIRE(allocations == 0);
}
