#include <pulp/timebase/compiled_tempo_map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <utility>
#include <vector>

using namespace pulp::timebase;

namespace {

CompiledTempoMap compile(std::span<const TempoPoint> points,
                         RationalRate rate = {48'000, 1}) {
    auto compiled = CompiledTempoMap::compile(points, rate);
    REQUIRE(compiled);
    return std::move(compiled).value();
}

} // namespace

TEST_CASE("fractional sample conversion analytically inverts tempo ramps") {
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{8 * kTicksPerQuarter}, 180.0, TempoCurve::LinearInTicks},
        TempoPoint{{16 * kTicksPerQuarter}, 90.0},
    };
    const auto map = compile(points);
    for (const auto tick : {
             -static_cast<long double>(kTicksPerQuarter) + 0.25L,
             static_cast<long double>(3 * kTicksPerQuarter) + 0.5L,
             static_cast<long double>(8 * kTicksPerQuarter) + 0.25L,
             static_cast<long double>(12 * kTicksPerQuarter) + 0.5L,
             static_cast<long double>(18 * kTicksPerQuarter) + 0.75L,
         }) {
        const auto sample = map.fractional_ticks_to_samples(tick);
        REQUIRE(std::abs(map.fractional_samples_to_ticks(sample) - tick) < 1e-8L);
    }
}

TEST_CASE("fractional tempo cursors consume forward segment crossings once") {
    constexpr std::size_t segment_count = 256;
    std::vector<TempoPoint> points;
    points.reserve(segment_count);
    for (std::size_t index = 0; index < segment_count; ++index) {
        points.push_back({{static_cast<std::int64_t>(index * kTicksPerQuarter)},
                          index % 2 == 0 ? 60.0 : 180.0,
                          index + 1 < segment_count ? TempoCurve::LinearInTicks
                                                    : TempoCurve::Constant});
    }
    const auto map = compile(points);
    TempoCursor cursor(map);
    std::size_t previous_segment = 0;
    const auto final_sample = map.ticks_to_samples(
        {static_cast<std::int64_t>((segment_count - 1) * kTicksPerQuarter)});
    for (std::int64_t sample = 0; sample <= final_sample.value; sample += 17) {
        const auto tick = cursor.advance_fractional(static_cast<long double>(sample) + 0.25L);
        REQUIRE(tick >= 0.0L);
        REQUIRE(cursor.segment_index() >= previous_segment);
        previous_segment = cursor.segment_index();
    }
    REQUIRE(previous_segment == segment_count - 1);

    TempoTickCursor tick_cursor(map);
    previous_segment = 0;
    for (std::size_t segment = 0; segment < segment_count; ++segment) {
        const auto tick = static_cast<long double>(segment * kTicksPerQuarter) + 0.25L;
        REQUIRE(std::abs(tick_cursor.advance_fractional(tick) -
                         map.fractional_ticks_to_samples(tick)) < 1e-8L);
        REQUIRE(tick_cursor.segment_index() >= previous_segment);
        previous_segment = tick_cursor.segment_index();
    }
    REQUIRE(previous_segment == segment_count - 1);
}

TEST_CASE("TempoCursor future tempo queries preserve forward sample advancement") {
    constexpr auto rate = RationalRate{48'000, 1};
    constexpr std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::Constant},
        TempoPoint{{2 * kTicksPerQuarter}, 120.0, TempoCurve::Constant},
        TempoPoint{{4 * kTicksPerQuarter}, 240.0, TempoCurve::Constant},
    };
    const auto map = compile(points, rate);

    // Independent exact oracle for the first 60 BPM section at 48 kHz:
    // one quarter note is 48,000 samples.
    constexpr TickPosition first_tick{kTicksPerQuarter / 2};
    constexpr SamplePosition first_sample{24'000};
    constexpr TickPosition next_tick{kTicksPerQuarter};
    constexpr SamplePosition next_sample{48'000};
    constexpr TickPosition future_tick{5 * kTicksPerQuarter};

    TempoCursor integer_cursor(map);
    const auto first_integer_result = integer_cursor.advance(first_sample);
    REQUIRE(first_integer_result.exact);
    REQUIRE(first_integer_result.represented_sample == first_sample);
    REQUIRE(integer_cursor.tempo_at_tick(future_tick) == 240.0);
    REQUIRE(integer_cursor.segment_index() == 0);
    const auto integer_result = integer_cursor.advance(next_sample);
    REQUIRE(integer_result.exact);
    REQUIRE(integer_result.represented_sample == next_sample);

    TempoCursor fractional_cursor(map);
    REQUIRE(fractional_cursor.advance_fractional(first_sample.value) ==
            static_cast<long double>(first_tick.value));
    REQUIRE(fractional_cursor.tempo_at_tick(future_tick) == 240.0);
    REQUIRE(fractional_cursor.segment_index() == 0);
    REQUIRE(fractional_cursor.advance_fractional(next_sample.value) ==
            static_cast<long double>(next_tick.value));
}

TEST_CASE("compiled tempo map reports interior ramp tempo extrema") {
    const std::array points{
        TempoPoint{{0}, 120.0, TempoCurve::LinearInTicks},
        TempoPoint{{kTicksPerQuarter}, 900.0, TempoCurve::LinearInTicks},
        TempoPoint{{2 * kTicksPerQuarter}, 120.0},
    };
    const auto map = compile(points);
    REQUIRE(map.maximum_tempo_between({0}, {2 * kTicksPerQuarter}) == 900.0);
    REQUIRE(map.maximum_tempo_between({0}, {kTicksPerQuarter / 2}) == 510.0);
    REQUIRE(map.maximum_tempo_between({3 * kTicksPerQuarter / 2}, {2 * kTicksPerQuarter}) ==
            510.0);
}
