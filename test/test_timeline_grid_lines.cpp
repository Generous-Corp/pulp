#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <span>

#include <pulp/timeline_editor/grid_lines.hpp>

using namespace pulp;

namespace {

timebase::CompiledMeterMap meter_map(std::initializer_list<timebase::MeterPoint> points) {
    auto map = timebase::MeterMap::create(std::span<const timebase::MeterPoint>(
        points.begin(), points.size()));
    REQUIRE(map);
    auto compiled = timebase::CompiledMeterMap::compile(*map);
    REQUIRE(compiled);
    return *compiled;
}

} // namespace

TEST_CASE("grid lines share one meter-aware viewport kernel", "[timeline-editor][grid-lines]") {
    const auto meter = meter_map({{{0}, {4, 4}}, {{4 * timebase::kTicksPerQuarter}, {3, 4}}});
    const auto projection = timeline_editor::TickProjection::create(
        {0}, {8 * timebase::kTicksPerQuarter}, {10.0f, 800.0f});
    REQUIRE(projection);

    std::array<timeline_editor::GridLine, 32> lines{};
    const auto result = timeline_editor::generate_grid_lines(*projection, meter, 80.0f, lines);
    REQUIRE(result);
    REQUIRE(result.count == 9);
    CHECK(lines[0] == timeline_editor::GridLine{{0}, 10.0f,
                                                   timeline_editor::GridLineLevel::Bar});
    CHECK(lines[1].tick == timebase::TickPosition{timebase::kTicksPerQuarter});
    CHECK(lines[1].level == timeline_editor::GridLineLevel::Beat);
    CHECK(lines[4].tick == timebase::TickPosition{4 * timebase::kTicksPerQuarter});
    CHECK(lines[4].level == timeline_editor::GridLineLevel::Bar);
    CHECK(lines[6].tick == timebase::TickPosition{6 * timebase::kTicksPerQuarter});
    CHECK(lines[7].tick == timebase::TickPosition{7 * timebase::kTicksPerQuarter});
    CHECK(lines[7].level == timeline_editor::GridLineLevel::Bar);
    CHECK(lines[8].tick == timebase::TickPosition{8 * timebase::kTicksPerQuarter});
    CHECK(lines[8].level == timeline_editor::GridLineLevel::Beat);
}

TEST_CASE("grid lines reject invalid spacing and bounded output", "[timeline-editor][grid-lines]") {
    const auto meter = meter_map({{{0}, {4, 4}}});
    const auto projection = timeline_editor::TickProjection::create(
        {0}, {4 * timebase::kTicksPerQuarter}, {0.0f, 400.0f});
    REQUIRE(projection);
    std::array<timeline_editor::GridLine, 8> lines{};

    CHECK(timeline_editor::generate_grid_lines(
              *projection, meter, 0.0f, lines).error ==
          timeline_editor::GridLineError::NonPositiveSpacing);
    CHECK(timeline_editor::generate_grid_lines(
              *projection, meter, std::numeric_limits<float>::quiet_NaN(), lines).error ==
          timeline_editor::GridLineError::NonFiniteSpacing);

    std::array<timeline_editor::GridLine, 1> too_small{};
    const auto bounded = timeline_editor::generate_grid_lines(*projection, meter, 1.0f, too_small);
    CHECK(bounded.error == timeline_editor::GridLineError::OutputTooSmall);
    CHECK(bounded.count == 1);
}

TEST_CASE("grid lines omit the containing bar before a partial viewport",
          "[timeline-editor][grid-lines]") {
    const auto meter = meter_map({{{0}, {4, 4}}});
    const auto projection = timeline_editor::TickProjection::create(
        {timebase::kTicksPerQuarter}, {4 * timebase::kTicksPerQuarter}, {0.0f, 400.0f});
    REQUIRE(projection);

    std::array<timeline_editor::GridLine, 8> lines{};
    const auto result = timeline_editor::generate_grid_lines(*projection, meter, 1.0f, lines);

    REQUIRE(result);
    REQUIRE(result.count == 5);
    CHECK(lines[0].tick == timebase::TickPosition{timebase::kTicksPerQuarter});
    CHECK(lines[0].level == timeline_editor::GridLineLevel::Beat);
    CHECK(lines[3].tick == timebase::TickPosition{4 * timebase::kTicksPerQuarter});
    CHECK(lines[3].level == timeline_editor::GridLineLevel::Bar);
    CHECK(lines[4].tick == timebase::TickPosition{5 * timebase::kTicksPerQuarter});
    CHECK(lines[4].level == timeline_editor::GridLineLevel::Beat);
}
