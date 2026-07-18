#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timebase/compiled_meter_map.hpp>
#include <pulp/timebase/quantize.hpp>

#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

using namespace pulp::timebase;

static_assert(sizeof(MonotonicBeat) == sizeof(TickPosition));
static_assert(std::is_trivially_copyable_v<MonotonicBeat>);

namespace {

template <typename ExactSampleAtTick>
void verify_properties(const CompiledTempoMap& map, std::uint64_t seed, std::int64_t minimum_sample,
                       std::int64_t maximum_sample, std::int64_t minimum_tick,
                       std::int64_t maximum_tick, int cases,
                       ExactSampleAtTick&& exact_sample_at_tick) {
    std::mt19937_64 random(seed);
    std::uniform_int_distribution<std::int64_t> sample_distribution(minimum_sample, maximum_sample);
    std::uniform_int_distribution<std::int64_t> tick_distribution(minimum_tick, maximum_tick);
    for (int index = 0; index < cases; ++index) {
        const SamplePosition sample{sample_distribution(random)};
        const auto canonical_tick = map.samples_to_ticks(sample);
        REQUIRE(map.ticks_to_samples(canonical_tick) == sample);

        const TickPosition tick{tick_distribution(random)};
        const auto mapped_sample = map.ticks_to_samples(tick);
        const auto canonical = map.samples_to_ticks(mapped_sample);
        REQUIRE(map.ticks_to_samples(canonical) == mapped_sample);
        REQUIRE(map.ticks_to_samples({tick.value + 1}).value >= mapped_sample.value);

        // Arbitrary integer ticks cannot round-trip exactly through a coarser
        // integer sample grid. Their forward conversion must round the exact
        // analytic position by less than one sample.
        REQUIRE(std::abs(static_cast<long double>(mapped_sample.value) -
                         exact_sample_at_tick(tick.value)) < 1.0L);
    }
}

struct TempoOracle {
    std::vector<TempoPoint> points;
    std::vector<std::int64_t> sample_anchors;
    RationalRate rate;

    long double segment_samples(std::size_t index, std::int64_t delta_ticks) const {
        const auto start_bpm = static_cast<long double>(points[index].bpm);
        const auto scale =
            rate.as_long_double() * 60.0L / static_cast<long double>(kTicksPerQuarter);
        if (delta_ticks < 0 || index + 1 == points.size() ||
            points[index].curve_to_next == TempoCurve::Constant ||
            points[index + 1].bpm == points[index].bpm) {
            return static_cast<long double>(delta_ticks) * scale / start_bpm;
        }

        const auto length =
            static_cast<long double>(points[index + 1].tick.value - points[index].tick.value);
        const auto slope = (static_cast<long double>(points[index + 1].bpm) - start_bpm) / length;
        return scale * std::log1p(slope * static_cast<long double>(delta_ticks) / start_bpm) /
               slope;
    }

    long double samples_at_tick(std::int64_t tick) const {
        std::size_t index = 0;
        while (index + 1 < points.size() && tick >= points[index + 1].tick.value) {
            ++index;
        }
        return static_cast<long double>(sample_anchors[index]) +
               segment_samples(index, tick - points[index].tick.value);
    }
};

TempoOracle make_oracle(std::vector<TempoPoint> points, RationalRate rate) {
    TempoOracle oracle{std::move(points), {0}, rate.normalized()};
    for (std::size_t index = 0; index + 1 < oracle.points.size(); ++index) {
        const auto duration = oracle.segment_samples(index, oracle.points[index + 1].tick.value -
                                                                oracle.points[index].tick.value);
        oracle.sample_anchors.push_back(oracle.sample_anchors.back() +
                                        static_cast<std::int64_t>(std::llround(duration)));
    }
    return oracle;
}

MonotonicBeat beat_at_sample(const CompiledTempoMap& map, SamplePosition sample) {
    return {map.samples_to_ticks(sample)};
}

MonotonicBeat ceil_to_quantum(MonotonicBeat value, TickDuration quantum) {
    const auto remainder = value.position.value % quantum.value;
    if (remainder == 0)
        return value;
    const auto distance = remainder < 0 ? -remainder : quantum.value - remainder;
    return value + TickDuration{distance};
}

std::int64_t floor_divide(std::int64_t value, std::int64_t divisor) {
    const auto quotient = value / divisor;
    return value % divisor < 0 ? quotient - 1 : quotient;
}

std::int64_t non_negative_modulo(std::int64_t value, std::int64_t divisor) {
    const auto remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

std::optional<std::int64_t> resolve_launch(const CompiledTempoMap& map,
                                           SamplePosition block_start,
                                           std::int64_t request_offset,
                                           std::int64_t frames,
                                           TickDuration quantum) {
    const auto target =
        ceil_to_quantum(beat_at_sample(map, {block_start.value + request_offset}), quantum);
    if (beat_at_sample(map, {block_start.value + frames}) < target)
        return std::nullopt;

    std::int64_t low = request_offset;
    std::int64_t high = frames;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        if (beat_at_sample(map, {block_start.value + middle}) >= target)
            high = middle;
        else
            low = middle + 1;
    }
    return low;
}

std::optional<std::int64_t> resolve_launch_oracle(const CompiledTempoMap& map,
                                                  SamplePosition block_start,
                                                  std::int64_t request_offset,
                                                  std::int64_t frames,
                                                  TickDuration quantum) {
    const auto target =
        ceil_to_quantum(beat_at_sample(map, {block_start.value + request_offset}), quantum);
    for (auto offset = request_offset; offset <= frames; ++offset) {
        if (beat_at_sample(map, {block_start.value + offset}) >= target)
            return offset;
    }
    return std::nullopt;
}

struct LoopRange {
    std::int64_t sample_begin = 0;
    std::int64_t sample_end = 0;
    TickPosition timeline_tick_begin;
    auto operator<=>(const LoopRange&) const = default;
};

std::vector<LoopRange> split_loop(const CompiledTempoMap& map,
                                  SamplePosition block_start,
                                  std::int64_t frames,
                                  TickDuration loop_length) {
    const auto beat = [&](std::int64_t offset) {
        return beat_at_sample(map, {block_start.value + offset}).position.value;
    };
    const auto first_cycle = floor_divide(beat(0), loop_length.value);
    const auto last_cycle = floor_divide(beat(frames - 1), loop_length.value);
    if (first_cycle == last_cycle)
        return {{0, frames, {non_negative_modulo(beat(0), loop_length.value)}}};
    if (last_cycle != first_cycle + 1)
        return {};

    std::int64_t low = 1;
    std::int64_t high = frames;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        if (floor_divide(beat(middle), loop_length.value) > first_cycle)
            high = middle;
        else
            low = middle + 1;
    }
    return {{0, low, {non_negative_modulo(beat(0), loop_length.value)}},
            {low, frames, {non_negative_modulo(beat(low), loop_length.value)}}};
}

std::vector<LoopRange> split_loop_oracle(const CompiledTempoMap& map,
                                         SamplePosition block_start,
                                         std::int64_t frames,
                                         TickDuration loop_length) {
    const auto beat = [&](std::int64_t offset) {
        return beat_at_sample(map, {block_start.value + offset}).position.value;
    };
    std::vector<LoopRange> ranges;
    std::int64_t begin = 0;
    auto cycle = floor_divide(beat(0), loop_length.value);
    for (std::int64_t offset = 1; offset < frames; ++offset) {
        const auto next_cycle = floor_divide(beat(offset), loop_length.value);
        if (next_cycle != cycle) {
            ranges.push_back(
                {begin, offset, {non_negative_modulo(beat(begin), loop_length.value)}});
            begin = offset;
            cycle = next_cycle;
        }
    }
    ranges.push_back(
        {begin, frames, {non_negative_modulo(beat(begin), loop_length.value)}});
    return ranges.size() <= 2 ? ranges : std::vector<LoopRange>{};
}

} // namespace

TEST_CASE("Editable tempo and meter maps reject malformed document values") {
    const std::array invalid_tempo{TempoPoint{{0}, std::numeric_limits<double>::quiet_NaN()}};
    const auto tempo = TempoMap::create(invalid_tempo);
    REQUIRE_FALSE(tempo);
    REQUIRE(tempo.error() == TempoMapError::InvalidBpm);

    const std::array invalid_final_ramp{
        TempoPoint{{0}, 120.0, TempoCurve::LinearInTicks}};
    const auto final_ramp = TempoMap::create(invalid_final_ramp);
    REQUIRE_FALSE(final_ramp);
    REQUIRE(final_ramp.error() == TempoMapError::InvalidFinalCurve);

    const std::array invalid_meter{MeterPoint{{0}, {4, 3}}};
    const auto meter = MeterMap::create(invalid_meter);
    REQUIRE_FALSE(meter);
    REQUIRE(meter.error() == MeterMapError::InvalidSignature);
}

TEST_CASE("CompiledMeterMap converts exact zero-based bars ticks and discontinuities") {
    constexpr auto bar_4_4 = 4 * kTicksPerQuarter;
    const std::array points{
        MeterPoint{{0}, {4, 4}},
        MeterPoint{{2 * bar_4_4}, {3, 4}},
    };
    const auto editable = MeterMap::create(points);
    REQUIRE(editable);
    const auto compiled_result = CompiledMeterMap::compile(editable.value());
    REQUIRE(compiled_result);
    const auto& map = compiled_result.value();

    REQUIRE(map.tick_to_bar({-1}) == BarTickPosition{{-1}, {bar_4_4 - 1}});
    REQUIRE(map.tick_to_bar({2 * bar_4_4}) == BarTickPosition{{2}, {0}});
    REQUIRE(map.bar_to_tick({2}) == TickPosition{2 * bar_4_4});
    REQUIRE(map.bar_to_tick({3}, {17}) == TickPosition{2 * bar_4_4 + 3 * kTicksPerQuarter + 17});
    REQUIRE(map.tick_to_bar(map.bar_to_tick({20}, {123})) == BarTickPosition{{20}, {123}});
    REQUIRE(map.meter_at_tick({2 * bar_4_4 - 1}) == MeterSignature{4, 4});
    REQUIRE(map.meter_at_tick({2 * bar_4_4}) == MeterSignature{3, 4});

    const std::array off_boundary{
        MeterPoint{{0}, {4, 4}}, MeterPoint{{bar_4_4 + 1}, {3, 4}}};
    const auto rejected = CompiledMeterMap::compile(off_boundary);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error() == MeterMapError::ChangeNotOnBarBoundary);
}

TEST_CASE("TempoCursor matches cold lookup across ramps boundaries and discontinuities") {
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{2 * kTicksPerQuarter}, 180.0, TempoCurve::Constant},
        TempoPoint{{5 * kTicksPerQuarter}, 90.0, TempoCurve::LinearInTicks},
        TempoPoint{{8 * kTicksPerQuarter}, 120.0, TempoCurve::Constant},
    };
    const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});
    TempoCursor cursor(map);
    const auto end = map.ticks_to_samples({10 * kTicksPerQuarter}).value;
    std::mt19937_64 random(0x435552534f52ULL);
    std::uniform_int_distribution<std::int64_t> step(0, 4096);
    std::int64_t sample = -10'000;
    while (sample < end) {
        const auto streaming = cursor.advance({sample});
        const auto cold = map.resolve_sample({sample});
        REQUIRE(streaming.tick == cold.tick);
        REQUIRE(streaming.represented_sample == cold.represented_sample);
        REQUIRE(streaming.absolute_error_samples == cold.absolute_error_samples);
        REQUIRE(streaming.exact == cold.exact);
        sample += step(random);
    }

    for (const auto discontinuity : {map.ticks_to_samples({7 * kTicksPerQuarter}),
                                     map.ticks_to_samples({kTicksPerQuarter}),
                                     map.ticks_to_samples({8 * kTicksPerQuarter})}) {
        REQUIRE(cursor.advance(discontinuity).tick == map.resolve_sample(discontinuity).tick);
    }
}

TEST_CASE("Tempo and meter compilation remain mathematically independent") {
    const std::array tempos_a{TempoPoint{{0}, 60.0}};
    const std::array tempos_b{TempoPoint{{0}, 240.0}};
    const auto tempo_a = require_compiled_tempo_map(tempos_a, RationalRate{48'000, 1});
    const auto tempo_b = require_compiled_tempo_map(tempos_b, RationalRate{48'000, 1});
    const std::array meters_a{MeterPoint{{0}, {4, 4}}};
    const std::array meters_b{MeterPoint{{0}, {7, 8}}};
    const auto meter_a = CompiledMeterMap::compile(meters_a);
    const auto meter_b = CompiledMeterMap::compile(meters_b);
    REQUIRE(meter_a);
    REQUIRE(meter_b);

    REQUIRE(tempo_a.ticks_to_samples({kTicksPerQuarter}) == SamplePosition{48'000});
    REQUIRE(tempo_b.ticks_to_samples({kTicksPerQuarter}) == SamplePosition{12'000});
    REQUIRE(meter_a->bar_to_tick({1}) == TickPosition{4 * kTicksPerQuarter});
    REQUIRE(meter_b->bar_to_tick({1}) == TickPosition{7 * kTicksPerQuarter / 2});
}

TEST_CASE("CompiledTempoMap validates its immutable input") {
    auto empty = CompiledTempoMap::compile(std::span<const TempoPoint>{}, {48'000, 1});
    REQUIRE_FALSE(empty);
    REQUIRE(empty.error() == TempoMapError::Empty);
    const std::array valid_point{TempoPoint{{0}, 120.0}};
    auto invalid_rate = CompiledTempoMap::compile(valid_point, {0, 1});
    REQUIRE_FALSE(invalid_rate);
    REQUIRE(invalid_rate.error() == TempoMapError::InvalidSampleRate);
    const std::array missing_zero{TempoPoint{{1}, 120.0}};
    auto missing = CompiledTempoMap::compile(missing_zero, {48'000, 1});
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error() == TempoMapError::MissingTickZero);
    const std::array unordered{TempoPoint{{0}, 120.0}, TempoPoint{{0}, 130.0}};
    auto disorder = CompiledTempoMap::compile(unordered, {48'000, 1});
    REQUIRE_FALSE(disorder);
    REQUIRE(disorder.error() == TempoMapError::UnorderedPoints);
    const std::array cumulative_overflow{
        TempoPoint{{0}, 1.0},
        TempoPoint{{100'000'000'000'000'000}, 1.0},
        TempoPoint{{200'000'000'000'000'000}, 1.0},
    };
    REQUIRE_FALSE(CompiledTempoMap::compile(cumulative_overflow, {768'000, 1}));
    const std::array rounded_duration_overflow{
        TempoPoint{{0}, 60.0},
        TempoPoint{{std::numeric_limits<std::int64_t>::max()}, 60.0},
    };
    REQUIRE_FALSE(CompiledTempoMap::compile(
        rounded_duration_overflow,
        {18'446'744'073'709'224'001ULL, 26'143'344'775'665ULL}));
}

TEST_CASE("CompiledTempoMap reports nearest representation on a sparse tick grid") {
    const std::array points{TempoPoint{{0}, 1.0}};
    const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});

    REQUIRE(map.ticks_to_samples({0}) == SamplePosition{0});
    REQUIRE(map.ticks_to_samples({1}) == SamplePosition{4});
    const auto result = map.resolve_sample({2});
    REQUIRE_FALSE(result.exact);
    REQUIRE(result.represented_sample == map.ticks_to_samples(result.tick));
    REQUIRE(result.absolute_error_samples == 2);

    const std::array sparse_points{TempoPoint{{0}, 1.0}};
    const auto sparse_map = require_compiled_tempo_map(sparse_points, RationalRate{192'000, 1});
    const auto samples_per_tick = 192'000.0L * 60.0L / static_cast<long double>(kTicksPerQuarter);
    const auto nearest_error_bound =
        static_cast<std::uint64_t>(std::floor(std::ceil(samples_per_tick) / 2.0L));
    for (std::int64_t sample = -50'000; sample <= 50'000; ++sample) {
        const auto sparse_result = sparse_map.resolve_sample({sample});
        REQUIRE(sparse_result.represented_sample ==
                sparse_map.ticks_to_samples(sparse_result.tick));
        REQUIRE(sparse_result.absolute_error_samples <= nearest_error_bound);
        REQUIRE(sparse_result.exact == (sparse_result.absolute_error_samples == 0));
    }
}

TEST_CASE("CompiledTempoMap reports saturation outside the tick domain") {
    const std::array points{TempoPoint{{0}, 1'000.0}};
    const auto map = require_compiled_tempo_map(points, RationalRate{44'100, 1});

    const auto in_range = map.resolve_sample({0});
    REQUIRE(in_range.exact);
    REQUIRE(in_range.represented_sample == SamplePosition{0});
    REQUIRE(map.ticks_to_samples(in_range.tick) == in_range.represented_sample);

    const auto maximum_tick_sample =
        map.ticks_to_samples({std::numeric_limits<std::int64_t>::max()});
    const auto above = map.resolve_sample({std::numeric_limits<std::int64_t>::max()});
    REQUIRE(above.represented_sample == maximum_tick_sample);
    REQUIRE(map.ticks_to_samples(above.tick) == above.represented_sample);
    REQUIRE_FALSE(above.exact);
    REQUIRE(above.absolute_error_samples ==
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                static_cast<std::uint64_t>(maximum_tick_sample.value));

    const auto minimum_tick_sample =
        map.ticks_to_samples({std::numeric_limits<std::int64_t>::min()});
    const auto below = map.resolve_sample({std::numeric_limits<std::int64_t>::min()});
    REQUIRE(below.represented_sample == minimum_tick_sample);
    REQUIRE(map.ticks_to_samples(below.tick) == below.represented_sample);
    REQUIRE_FALSE(below.exact);
    REQUIRE(below.absolute_error_samples ==
            static_cast<std::uint64_t>(minimum_tick_sample.value) -
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::min()));
}

TEST_CASE("CompiledTempoMap canonicalizes saturated sample-domain edges") {
    const std::array points{TempoPoint{{0}, 1.0}};
    const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();

    const auto control = map.resolve_sample({0});
    REQUIRE(control.exact);
    REQUIRE(control.tick == TickPosition{0});

    const auto lower = map.resolve_sample({minimum});
    REQUIRE(lower.exact);
    REQUIRE(lower.tick == TickPosition{minimum});
    REQUIRE(lower.represented_sample == SamplePosition{minimum});

    const auto upper = map.resolve_sample({maximum});
    REQUIRE(upper.exact);
    REQUIRE(upper.represented_sample == SamplePosition{maximum});
    REQUIRE(map.ticks_to_samples(upper.tick) == SamplePosition{maximum});
    REQUIRE(upper.tick.value > minimum);
    REQUIRE(map.ticks_to_samples({upper.tick.value - 1}).value < maximum);
}

TEST_CASE("CompiledTempoMap adds rounded segment durations in the integer domain") {
    // At 768 kHz / 1 BPM this first segment ends above 2^53 samples. Apple
    // long double is double, so converting that integer anchor back to floating
    // point before adding the next one-sample segment can erase the increment.
    constexpr std::int64_t large_tick = 137'922'738'588'222;
    const std::array points{
        TempoPoint{{0}, 1.0, TempoCurve::Constant},
        TempoPoint{{large_tick}, 1'000.0, TempoCurve::Constant},
        TempoPoint{{large_tick + 15}, 1'000.0, TempoCurve::Constant},
    };
    const auto map = require_compiled_tempo_map(points, RationalRate{768'000, 1});
    const auto first_anchor = map.ticks_to_samples({large_tick});
    const auto second_anchor = map.ticks_to_samples({large_tick + 15});

    REQUIRE(first_anchor.value > (std::int64_t{1} << 53));
    REQUIRE(second_anchor.value == first_anchor.value + 1);
    REQUIRE(map.ticks_to_samples({large_tick + 14}).value <= second_anchor.value);
}

TEST_CASE("CompiledTempoMap keeps exact integer anchors across constant segments") {
    const std::array points{
        TempoPoint{{0}, 120.0, TempoCurve::Constant},
        TempoPoint{{4 * kTicksPerQuarter}, 90.0, TempoCurve::Constant},
        TempoPoint{{8 * kTicksPerQuarter}, 150.0, TempoCurve::Constant},
    };
    const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});

    REQUIRE(map.ticks_to_samples({0}) == SamplePosition{0});
    REQUIRE(map.ticks_to_samples({4 * kTicksPerQuarter}) == SamplePosition{96'000});
    REQUIRE(map.ticks_to_samples({8 * kTicksPerQuarter}) == SamplePosition{224'000});
    REQUIRE(map.ticks_to_samples(map.samples_to_ticks({96'000})) == SamplePosition{96'000});
    const auto exact_samples = [](std::int64_t tick) {
        const auto ticks = static_cast<long double>(tick);
        if (tick < 4 * kTicksPerQuarter) {
            return ticks * 48'000.0L * 60.0L /
                   (static_cast<long double>(kTicksPerQuarter) * 120.0L);
        }
        if (tick < 8 * kTicksPerQuarter) {
            return 96'000.0L + static_cast<long double>(tick - 4 * kTicksPerQuarter) * 48'000.0L *
                                   60.0L / (static_cast<long double>(kTicksPerQuarter) * 90.0L);
        }
        return 224'000.0L + static_cast<long double>(tick - 8 * kTicksPerQuarter) * 48'000.0L *
                                60.0L / (static_cast<long double>(kTicksPerQuarter) * 150.0L);
    };
    verify_properties(map, 0x43ab'0031, -100'000, 400'000, -2 * kTicksPerQuarter,
                      12 * kTicksPerQuarter, 1'000, exact_samples);
}

TEST_CASE("CompiledTempoMap analytically integrates and inverts linear tick ramps") {
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{8 * kTicksPerQuarter}, 180.0, TempoCurve::LinearInTicks},
        TempoPoint{{16 * kTicksPerQuarter}, 90.0, TempoCurve::Constant},
    };
    const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});

    const auto first_boundary = map.ticks_to_samples({8 * kTicksPerQuarter});
    const auto second_boundary = map.ticks_to_samples({16 * kTicksPerQuarter});
    REQUIRE(map.ticks_to_samples({-8 * kTicksPerQuarter}) == SamplePosition{-384'000});
    REQUIRE(map.ticks_to_samples(map.samples_to_ticks({-384'000})) == SamplePosition{-384'000});
    REQUIRE(first_boundary.value > 0);
    REQUIRE(second_boundary.value > first_boundary.value);
    REQUIRE(map.ticks_to_samples(map.samples_to_ticks(first_boundary)) == first_boundary);
    REQUIRE(map.ticks_to_samples(map.samples_to_ticks(second_boundary)) == second_boundary);
    REQUIRE(map.ticks_to_samples({8 * kTicksPerQuarter - 1}).value <= first_boundary.value);
    REQUIRE(map.ticks_to_samples({8 * kTicksPerQuarter + 1}).value >= first_boundary.value);
    const auto ramp_samples = [](long double ticks, long double length, long double start_bpm,
                                 long double end_bpm) {
        const auto slope = (end_bpm - start_bpm) / length;
        const auto scale = 48'000.0L * 60.0L / static_cast<long double>(kTicksPerQuarter);
        return scale * std::log1p(slope * ticks / start_bpm) / slope;
    };
    const auto exact_samples = [&](std::int64_t tick) {
        if (tick < 0) {
            return static_cast<long double>(tick) * 48'000.0L * 60.0L /
                   (static_cast<long double>(kTicksPerQuarter) * 60.0L);
        }
        if (tick < 8 * kTicksPerQuarter) {
            return ramp_samples(static_cast<long double>(tick), 8.0L * kTicksPerQuarter, 60.0L,
                                180.0L);
        }
        if (tick < 16 * kTicksPerQuarter) {
            return static_cast<long double>(first_boundary.value) +
                   ramp_samples(static_cast<long double>(tick - 8 * kTicksPerQuarter),
                                8.0L * kTicksPerQuarter, 180.0L, 90.0L);
        }
        return static_cast<long double>(second_boundary.value) +
               static_cast<long double>(tick - 16 * kTicksPerQuarter) * 48'000.0L * 60.0L /
                   (static_cast<long double>(kTicksPerQuarter) * 90.0L);
    };
    verify_properties(map, 0x43ab'0032, -100'000, second_boundary.value + 200'000,
                      -2 * kTicksPerQuarter, 20 * kTicksPerQuarter, 1'000, exact_samples);
}

TEST_CASE("CompiledTempoMap randomized maps cover rates ramps boundaries and large positions") {
    constexpr int kMapsPerRate = 10;
    constexpr int kPositionsPerMap = 20'000;
    constexpr int kInteriorCasesPerSegment = 4'000;
    constexpr int kWideCases = 4'000;
    constexpr int kNegativeCases = 2'000;
    constexpr int kPostMapCases = 2'000;
    constexpr std::int64_t kLargeTick = 1'000'000'000'000;
    static_assert(3 * kInteriorCasesPerSegment + kWideCases + kNegativeCases + kPostMapCases ==
                  kPositionsPerMap);
    const std::array rates{
        RationalRate{44'100, 1},  RationalRate{48'000, 1},         RationalRate{96'000, 1},
        RationalRate{192'000, 1}, RationalRate{48'000'000, 1'001},
    };
    std::mt19937_64 random(0x43ab'0040);
    std::uniform_int_distribution<int> bpm_distribution(30, 300);
    std::uniform_int_distribution<int> length_distribution(1, 64);

    std::uint64_t position_cases = 0;
    std::uint64_t ramp_interior_cases = 0;
    for (std::size_t rate_index = 0; rate_index < rates.size(); ++rate_index) {
        for (int map_index = 0; map_index < kMapsPerRate; ++map_index) {
            std::vector<TempoPoint> points;
            points.reserve(4);
            std::int64_t tick = 0;
            const auto curve =
                map_index % 2 == 0 ? TempoCurve::Constant : TempoCurve::LinearInTicks;
            for (int point_index = 0; point_index < 4; ++point_index) {
                points.push_back({{tick}, static_cast<double>(bpm_distribution(random)), curve});
                tick += static_cast<std::int64_t>(length_distribution(random)) * kTicksPerQuarter;
            }
            points.back().curve_to_next = TempoCurve::Constant;

            const auto oracle = make_oracle(points, rates[rate_index]);
            const auto map = require_compiled_tempo_map(points, rates[rate_index]);
            std::uint64_t range_index = 0;
            const auto run_range = [&](std::int64_t minimum_tick, std::int64_t maximum_tick,
                                       int cases) {
                const auto minimum_sample = map.ticks_to_samples({minimum_tick}).value;
                const auto maximum_sample = map.ticks_to_samples({maximum_tick}).value;
                verify_properties(
                    map,
                    0x43ab'1000 + rate_index * kMapsPerRate * 8 + map_index * 8 + range_index++,
                    minimum_sample, maximum_sample, minimum_tick, maximum_tick, cases,
                    [&](std::int64_t position) { return oracle.samples_at_tick(position); });
                position_cases += cases;
            };

            // Sixty percent of every map's randomized positions exercise the
            // three finite segment interiors. On the 25 ramp maps this is
            // 300,000 cases total, evenly divided across all ramp segments.
            for (std::size_t segment = 0; segment + 1 < points.size(); ++segment) {
                run_range(points[segment].tick.value, points[segment + 1].tick.value - 1,
                          kInteriorCasesPerSegment);
                if (curve == TempoCurve::LinearInTicks)
                    ramp_interior_cases += kInteriorCasesPerSegment;
            }
            run_range(-8 * kTicksPerQuarter, -1, kNegativeCases);
            run_range(points.back().tick.value, points.back().tick.value + 8 * kTicksPerQuarter,
                      kPostMapCases);
            run_range(-kLargeTick, kLargeTick, kWideCases);

            REQUIRE(map.sample_rate() == rates[rate_index].normalized());
            REQUIRE(map.segment_count() == points.size());
            REQUIRE(map.ticks_to_samples({-kLargeTick}).value <= 0);
            REQUIRE(map.ticks_to_samples({kLargeTick}).value >= 0);
            for (std::size_t boundary = 1; boundary < points.size(); ++boundary) {
                const auto boundary_tick = points[boundary].tick;
                const auto boundary_sample = map.ticks_to_samples(boundary_tick);
                REQUIRE(boundary_sample.value == static_cast<std::int64_t>(std::llround(
                                                     oracle.samples_at_tick(boundary_tick.value))));
                REQUIRE(map.ticks_to_samples({boundary_tick.value - 1}).value <=
                        boundary_sample.value);
                REQUIRE(map.ticks_to_samples({boundary_tick.value + 1}).value >=
                        boundary_sample.value);
                for (std::int64_t adjacent = -1; adjacent <= 1; ++adjacent) {
                    const SamplePosition sample{boundary_sample.value + adjacent};
                    const auto resolved = map.resolve_sample(sample);
                    REQUIRE(resolved.exact);
                    REQUIRE(resolved.represented_sample == sample);
                }
            }
        }
    }
    REQUIRE(position_cases == 1'000'000);
    REQUIRE(ramp_interior_cases == 300'000);
}

TEST_CASE("timebase quantization utilities preserve TransportQuantizer arithmetic") {
    REQUIRE(beats_per_bar(7, 8) == 3.5);
    REQUIRE(beats_per_bar(0, 4) == 0.0);
    REQUIRE(frames_to_beats(24'000.0, 48'000.0, 120.0) == 1.0);
    REQUIRE(beats_to_frames(1.0, 48'000.0, 120.0) == 24'000.0);
    REQUIRE(next_grid_boundary(1.0, 1.0) == 1.0);
    REQUIRE(next_grid_boundary(1.0001, 1.0) == 2.0);
}

TEST_CASE("CompiledTempoMap resolves quantized MonotonicBeat launches at exact samples") {
    constexpr TickDuration quantum{kTicksPerQuarter / 4};
    const std::array<std::int64_t, 9> blocks{1, 2, 7, 31, 64, 127, 256, 511, 1024};
    const std::array<double, 6> start_tempos{30.0, 45.0, 60.0, 90.0, 120.0, 180.0};
    const std::array<double, 5> end_tempos{35.0, 70.0, 120.0, 200.0, 300.0};
    const std::array<std::int64_t, 7> phases{
        0, 1, quantum.value - 1, quantum.value,
        quantum.value + 1, 2 * quantum.value - 1, 9 * quantum.value + 13};

    std::uint64_t cases = 0;
    for (const auto frames : blocks) {
        for (const auto start_tempo : start_tempos) {
            for (const auto end_tempo : end_tempos) {
                const std::array points{
                    TempoPoint{{0}, start_tempo, TempoCurve::LinearInTicks},
                    TempoPoint{{16 * kTicksPerQuarter}, end_tempo, TempoCurve::Constant},
                };
                const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});
                for (const auto phase : phases) {
                    const auto block_start = map.ticks_to_samples({phase});
                    for (const auto request : {std::int64_t{0}, frames / 3,
                                               frames / 2, frames - 1}) {
                        REQUIRE(resolve_launch(map, block_start, request, frames, quantum) ==
                                resolve_launch_oracle(map, block_start, request, frames, quantum));
                        ++cases;
                    }
                }
            }
        }
    }
    REQUIRE(cases == 7'560);

    SECTION("negative preroll launches ceil toward the next signed grid boundary") {
        const std::array points{TempoPoint{{0}, 120.0}};
        const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});
        for (const auto phase : {-2 * quantum.value - 1, -quantum.value - 1,
                                 -quantum.value + 1, std::int64_t{-1}}) {
            const auto block_start = map.ticks_to_samples({phase});
            REQUIRE(resolve_launch(map, block_start, 0, 1024, quantum) ==
                    resolve_launch_oracle(map, block_start, 0, 1024, quantum));
        }
    }
}

TEST_CASE("CompiledTempoMap projects at most one loop wrap into two exact ranges") {
    constexpr TickDuration loop_length{kTicksPerQuarter};
    const std::array<std::int64_t, 8> blocks{2, 3, 17, 63, 128, 255, 512, 1024};
    const std::array<double, 5> start_tempos{40.0, 70.0, 100.0, 160.0, 240.0};
    const std::array<double, 4> end_tempos{50.0, 120.0, 220.0, 400.0};
    std::uint64_t cases = 0;
    for (const auto frames : blocks) {
        for (const auto start_tempo : start_tempos) {
            for (const auto end_tempo : end_tempos) {
                const std::array points{
                    TempoPoint{{0}, start_tempo, TempoCurve::LinearInTicks},
                    TempoPoint{{16 * kTicksPerQuarter}, end_tempo, TempoCurve::Constant},
                };
                const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});
                const auto nominal_ticks_per_frame = static_cast<std::int64_t>(
                    std::ceil(start_tempo * static_cast<double>(kTicksPerQuarter) /
                              (48'000.0 * 60.0)));
                for (const auto distance : {std::int64_t{0}, std::int64_t{1},
                                            nominal_ticks_per_frame / 2,
                                            nominal_ticks_per_frame,
                                            nominal_ticks_per_frame * frames / 2}) {
                    const auto phase = loop_length.value - 1 -
                                       std::min(distance, loop_length.value - 1);
                    const auto block_start = map.ticks_to_samples({phase});
                    const auto actual = split_loop(map, block_start, frames, loop_length);
                    REQUIRE(actual ==
                            split_loop_oracle(map, block_start, frames, loop_length));
                    REQUIRE(actual.size() <= 2);
                    ++cases;
                }
            }
        }
    }
    REQUIRE(cases == 800);

    SECTION("a callback spanning more than one wrap fails closed") {
        const std::array points{TempoPoint{{0}, 1'000.0}};
        const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});
        constexpr TickDuration tiny_loop{1'000};
        const auto block_start = map.ticks_to_samples({tiny_loop.value - 1});
        REQUIRE(split_loop_oracle(map, block_start, 16, tiny_loop).empty());
        REQUIRE(split_loop(map, block_start, 16, tiny_loop).empty());
    }

    SECTION("negative preroll ranges use floor cycles and non-negative loop positions") {
        const std::array points{TempoPoint{{0}, 120.0}};
        const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});
        const std::array<std::int64_t, 4> phases{-2 * loop_length.value - 100,
                                                 -loop_length.value - 100,
                                                 -loop_length.value + 100,
                                                 -100};
        for (const auto phase : phases) {
            const auto block_start = map.ticks_to_samples({phase});
            for (const auto frames : {2, 31, 256, 1024}) {
                const auto ranges = split_loop(map, block_start, frames, loop_length);
                REQUIRE(ranges == split_loop_oracle(map, block_start, frames, loop_length));
                for (const auto& range : ranges) {
                    REQUIRE(range.timeline_tick_begin.value >= 0);
                    REQUIRE(range.timeline_tick_begin.value < loop_length.value);
                }
            }
        }
    }
}

TEST_CASE("CompiledTempoMap preserves MonotonicBeat over variable callbacks") {
    const std::array points{
        TempoPoint{{0}, 70.0, TempoCurve::LinearInTicks},
        TempoPoint{{16 * kTicksPerQuarter}, 220.0, TempoCurve::Constant},
    };
    const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});
    const std::array<std::int64_t, 12> blocks{1, 17, 64, 3, 255, 7,
                                              512, 31, 2, 127, 9, 1024};
    SamplePosition block_start{0};
    auto previous = beat_at_sample(map, block_start);
    for (const auto frames : blocks) {
        const auto end = beat_at_sample(map, {block_start.value + frames});
        REQUIRE(end >= previous);
        REQUIRE(resolve_launch(map, block_start, frames / 2, frames,
                               {kTicksPerQuarter / 8}) ==
                resolve_launch_oracle(map, block_start, frames / 2, frames,
                                      {kTicksPerQuarter / 8}));
        block_start.value += frames;
        previous = end;
    }
    REQUIRE(block_start == SamplePosition{2052});
}

TEST_CASE("timebase strong-position arithmetic saturates at its signed domain") {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();

    REQUIRE((TickPosition{maximum} + TickDuration{1}) == TickPosition{maximum});
    REQUIRE((TickPosition{minimum} + TickDuration{-1}) == TickPosition{minimum});
    REQUIRE((TickPosition{minimum} - TickDuration{1}) == TickPosition{minimum});
    REQUIRE((TickPosition{maximum} - TickDuration{-1}) == TickPosition{maximum});
    REQUIRE((TickPosition{maximum} - TickPosition{minimum}) == TickDuration{maximum});
    REQUIRE((TickPosition{minimum} - TickPosition{maximum}) == TickDuration{minimum});

    REQUIRE((MonotonicBeat{{maximum}} + TickDuration{1}) == MonotonicBeat{{maximum}});
    REQUIRE((MonotonicBeat{{minimum}} - TickDuration{1}) == MonotonicBeat{{minimum}});
    REQUIRE((MonotonicBeat{{maximum}} - MonotonicBeat{{minimum}}) == TickDuration{maximum});
}
