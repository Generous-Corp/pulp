#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

#include <pulp/timeline_editor/snap_grid.hpp>

using namespace pulp;
using namespace pulp::timebase;
using namespace pulp::timeline_editor;

namespace {

CompiledMeterMap compile(std::span<const MeterPoint> points) {
    auto result = CompiledMeterMap::compile(points);
    REQUIRE(result);
    return std::move(result).value();
}

} // namespace

TEST_CASE("Tick snapping is inclusive and resolves a nearest tie later",
          "[timeline-editor][snap-grid]") {
    const std::array points{MeterPoint{{0}, {4, 4}}};
    const auto meter = compile(points);
    auto grid = SnapGrid::create({kTicksPerQuarter / 2});
    REQUIRE(grid);

    CHECK(grid->interval() == TickDuration{kTicksPerQuarter / 2});
    CHECK(grid->swing() == kStraightSwing);
    CHECK(grid->snap(meter, {kTicksPerQuarter / 2}, SnapDirection::AtOrBefore) ==
          TickPosition{kTicksPerQuarter / 2});
    CHECK(grid->snap(meter, {kTicksPerQuarter / 2}, SnapDirection::AtOrAfter) ==
          TickPosition{kTicksPerQuarter / 2});
    CHECK(grid->snap(meter, {kTicksPerQuarter / 4}) == TickPosition{kTicksPerQuarter / 2});
    CHECK(grid->snap(meter, {kTicksPerQuarter / 4 - 1}) == TickPosition{0});
}

TEST_CASE("Snap phase restarts at a meter change instead of inheriting tick zero",
          "[timeline-editor][snap-grid]") {
    constexpr auto first_bar = 3 * kTicksPerQuarter / 2;
    const std::array points{
        MeterPoint{{0}, {3, 8}},
        MeterPoint{{first_bar}, {4, 4}},
    };
    const auto meter = compile(points);
    auto grid = SnapGrid::create({kTicksPerQuarter});
    REQUIRE(grid);

    CHECK(grid->snap(meter, {first_bar + kTicksPerQuarter / 10}) == TickPosition{first_bar});
    CHECK(grid->snap(meter, {first_bar + 3 * kTicksPerQuarter / 5}) ==
          TickPosition{first_bar + kTicksPerQuarter});
    CHECK(grid->snap(meter, {-kTicksPerQuarter / 10}) == TickPosition{0});
}

TEST_CASE("A partial final cell snaps to the authored bar boundary",
          "[timeline-editor][snap-grid]") {
    constexpr auto bar_length = 5 * kTicksPerQuarter / 2;
    const std::array points{MeterPoint{{0}, {5, 8}}};
    const auto meter = compile(points);
    auto grid = SnapGrid::create({kTicksPerQuarter});
    REQUIRE(grid);

    CHECK(grid->snap(meter, {bar_length - kTicksPerQuarter / 10}) == TickPosition{bar_length});
    CHECK(grid->snap(meter, {bar_length - 1}, SnapDirection::AtOrBefore) ==
          TickPosition{2 * kTicksPerQuarter});
    CHECK(grid->snap(meter, {bar_length - 1}, SnapDirection::AtOrAfter) ==
          TickPosition{bar_length});
}

TEST_CASE("Swing moves the interior snap boundary without moving the bar",
          "[timeline-editor][snap-grid]") {
    const std::array points{MeterPoint{{0}, {4, 4}}};
    const auto meter = compile(points);
    constexpr TickDuration eighth{kTicksPerQuarter / 2};
    auto grid = SnapGrid::create(eighth, kTripletSwing);
    REQUIRE(grid);

    const auto swung_offbeat = swing_position({eighth.value}, eighth, kTripletSwing);
    REQUIRE(swung_offbeat == TickPosition{2 * kTicksPerQuarter / 3});
    CHECK(grid->snap(meter, swung_offbeat) == swung_offbeat);
    CHECK(grid->snap(meter, {kTicksPerQuarter / 3}) == swung_offbeat);
    CHECK(grid->snap(meter, {4 * kTicksPerQuarter}) == TickPosition{4 * kTicksPerQuarter});
}

TEST_CASE("Tick snapping remains total at both signed endpoints", "[timeline-editor][snap-grid]") {
    const std::array points{MeterPoint{{0}, {4, 4}}};
    const auto meter = compile(points);
    auto grid = SnapGrid::create({kTicksPerQuarter / 4}, kTripletSwing);
    REQUIRE(grid);

    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    CHECK(grid->snap(meter, {minimum}, SnapDirection::AtOrBefore) == TickPosition{minimum});
    CHECK(grid->snap(meter, {maximum}, SnapDirection::AtOrAfter) == TickPosition{maximum});
    CHECK(grid->snap(meter, {minimum}).value >= minimum);
    CHECK(grid->snap(meter, {maximum}).value <= maximum);
    static_assert(noexcept(std::declval<const SnapGrid&>().snap(
        std::declval<const CompiledMeterMap&>(), TickPosition{})));
}

TEST_CASE("Invalid snap grids fail before gesture math", "[timeline-editor][snap-grid]") {
    auto zero = SnapGrid::create({0});
    REQUIRE_FALSE(zero);
    CHECK(zero.error() == SnapGridError::InvalidInterval);

    auto oversized = SnapGrid::create({kMaxSwingGridTicks + 1});
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error() == SnapGridError::InvalidInterval);

    auto collapsed = SnapGrid::create({kTicksPerQuarter / 2}, {2, 2});
    REQUIRE_FALSE(collapsed);
    CHECK(collapsed.error() == SnapGridError::InvalidSwingRatio);
}
