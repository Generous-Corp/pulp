#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

#include <pulp/timeline_editor/edit_intent.hpp>
#include <pulp/timeline_editor/hit_test.hpp>
#include <pulp/timeline_editor/snap_grid.hpp>
#include <pulp/timeline_editor/viewport_projection.hpp>

using namespace pulp;
using namespace pulp::timebase;
using namespace pulp::timeline_editor;

namespace {

CompiledMeterMap compile(std::span<const MeterPoint> points) {
    auto result = CompiledMeterMap::compile(points);
    REQUIRE(result);
    return std::move(result).value();
}

struct ProjectedGesture {
    ProjectedHit hit;
    timeline::Transaction transaction;
};

ProjectedGesture projected_drag(float tolerance_px) {
    constexpr auto quarter = kTicksPerQuarter;
    const std::array meter_points{MeterPoint{{0}, {4, 4}}};
    const auto meter = compile(meter_points);
    auto grid = SnapGrid::create({quarter});
    auto projection = TickProjection::create({0}, {4 * quarter}, {0.0f, 400.0f});
    REQUIRE(grid);
    REQUIRE(projection);

    const std::array candidates{ProjectedHitCandidate{
        {5}, projection->x_at({quarter}), 20.0f, projection->x_at({2 * quarter}), 40.0f}};
    const auto hit = hit_test_projected_items(candidates, 150.0f, 30.0f, tolerance_px);
    REQUIRE(hit);

    EditIntent intent;
    intent.kind =
        hit->region == ProjectedHitRegion::Body ? EditIntentKind::Move : EditIntentKind::Resize;
    intent.sequence_id = {3};
    intent.track_id = {4};
    intent.clip_id = hit->item;
    intent.expected_range = timeline::MusicalTimeRange{{quarter}, {quarter}};
    intent.replacement_range =
        timeline::MusicalTimeRange{grid->snap(meter, projection->tick_at(270.0f)), {quarter}};

    EditIntentIdentity identity;
    identity.transaction_id = timeline::TransactionId{timeline::WriterId{1}, 1};
    identity.command_id = timeline::CommandId{timeline::WriterId{1}, 1};
    auto lowered = lower_edit_intent(intent, identity);
    REQUIRE(lowered);
    return {*hit, std::move(lowered).value()};
}

} // namespace

TEST_CASE("Projected timeline hits keep resolved pointer geometry below the view floor",
          "[timeline-editor][hit-test]") {
    const auto mouse = projected_drag(4.0f);
    const auto touch = projected_drag(22.0f);
    const ProjectedHit expected{{5}, ProjectedHitRegion::Body};

    REQUIRE(mouse.hit == expected);
    REQUIRE(touch.hit == expected);
    REQUIRE(timeline::equivalent(mouse.transaction, touch.transaction));
    REQUIRE(mouse.transaction.commands.size() == 1);
    const auto& move = std::get<timeline::MoveClip>(mouse.transaction.commands.front().command);
    REQUIRE(move.clip_id == timeline::ItemId{5});
    REQUIRE(std::get<timeline::MusicalTimeRange>(move.replacement_range).start ==
            TickPosition{3 * kTicksPerQuarter});
}

TEST_CASE("Resolved touch tolerance reaches outside a mouse-sized projected target",
          "[timeline-editor][hit-test]") {
    const std::array candidates{ProjectedHitCandidate{{5}, 100.0f, 20.0f, 200.0f, 40.0f}};

    REQUIRE_FALSE(hit_test_projected_items(candidates, 215.0f, 30.0f, 4.0f));
    const auto touch = hit_test_projected_items(candidates, 215.0f, 30.0f, 22.0f);
    REQUIRE((touch == ProjectedHit{{5}, ProjectedHitRegion::TrailingEdge}));
}

TEST_CASE("Projected timeline hits resolve half-open boundaries and geometric ties",
          "[timeline-editor][hit-test]") {
    const std::array adjacent{
        ProjectedHitCandidate{{10}, 0.0f, 0.0f, 100.0f, 20.0f},
        ProjectedHitCandidate{{11}, 100.0f, 0.0f, 200.0f, 20.0f},
    };
    REQUIRE((hit_test_projected_items(adjacent, 100.0f, 10.0f, 0.0f) ==
             ProjectedHit{{11}, ProjectedHitRegion::LeadingEdge}));

    const std::array adjacent_front_order{
        ProjectedHitCandidate{{11}, 100.0f, 0.0f, 200.0f, 20.0f},
        ProjectedHitCandidate{{10}, 0.0f, 0.0f, 100.0f, 20.0f},
    };
    REQUIRE((hit_test_projected_items(adjacent_front_order, 100.0f, 10.0f, 4.0f) ==
             ProjectedHit{{11}, ProjectedHitRegion::LeadingEdge}));

    const std::array single{ProjectedHitCandidate{{12}, 0.0f, 0.0f, 100.0f, 20.0f}};
    REQUIRE(hit_test_projected_items(single, 0.0f, 0.0f, 0.0f));
    REQUIRE_FALSE(hit_test_projected_items(single, 100.0f, 10.0f, 0.0f));
    REQUIRE_FALSE(hit_test_projected_items(single, 50.0f, 20.0f, 0.0f));
    REQUIRE(hit_test_projected_items(single, -4.0f, 10.0f, 4.0f));
    REQUIRE_FALSE(hit_test_projected_items(single, 104.0f, 10.0f, 4.0f));
    REQUIRE_FALSE(hit_test_projected_items(single, 50.0f, 24.0f, 4.0f));

    const std::array overlap{
        ProjectedHitCandidate{{13}, 0.0f, 0.0f, 100.0f, 20.0f},
        ProjectedHitCandidate{{14}, 0.0f, 0.0f, 100.0f, 20.0f},
    };
    REQUIRE((hit_test_projected_items(overlap, 50.0f, 10.0f, 0.0f) ==
             ProjectedHit{{14}, ProjectedHitRegion::Body}));

    const std::array unequal_distance{
        ProjectedHitCandidate{{20}, 0.0f, 0.0f, 100.0f, 20.0f},
        ProjectedHitCandidate{{21}, 110.0f, 0.0f, 210.0f, 20.0f},
    };
    REQUIRE((hit_test_projected_items(unequal_distance, 102.0f, 10.0f, 10.0f) ==
             ProjectedHit{{20}, ProjectedHitRegion::TrailingEdge}));
    REQUIRE((hit_test_projected_items(unequal_distance, 105.0f, 10.0f, 10.0f) ==
             ProjectedHit{{21}, ProjectedHitRegion::LeadingEdge}));

    const std::array narrow{ProjectedHitCandidate{{30}, 0.0f, 0.0f, 10.0f, 20.0f}};
    REQUIRE((hit_test_projected_items(narrow, 3.0f, 10.0f, 5.0f) ==
             ProjectedHit{{30}, ProjectedHitRegion::LeadingEdge}));
    REQUIRE((hit_test_projected_items(narrow, 5.0f, 10.0f, 5.0f) ==
             ProjectedHit{{30}, ProjectedHitRegion::Body}));
    REQUIRE((hit_test_projected_items(narrow, 7.0f, 10.0f, 5.0f) ==
             ProjectedHit{{30}, ProjectedHitRegion::TrailingEdge}));
    REQUIRE((hit_test_projected_items(narrow, 12.0f, 10.0f, 5.0f) ==
             ProjectedHit{{30}, ProjectedHitRegion::TrailingEdge}));

    const std::array euclidean_distance{
        ProjectedHitCandidate{{40}, 4.0f, 4.0f, 5.0f, 5.0f},
        ProjectedHitCandidate{{41}, -1.0f, 6.0f, 1.0f, 7.0f},
    };
    REQUIRE((hit_test_projected_items(euclidean_distance, 0.0f, 0.0f, 10.0f) ==
             ProjectedHit{{40}, ProjectedHitRegion::LeadingEdge}));
}

TEST_CASE("Projected timeline hits fail closed on invalid input", "[timeline-editor][hit-test]") {
    constexpr auto nan = std::numeric_limits<float>::quiet_NaN();
    constexpr auto infinity = std::numeric_limits<float>::infinity();
    const std::array valid{ProjectedHitCandidate{{5}, 0.0f, 0.0f, 100.0f, 20.0f}};
    const std::array invalid{
        ProjectedHitCandidate{{}, 0.0f, 0.0f, 100.0f, 20.0f},
        ProjectedHitCandidate{{6}, 100.0f, 0.0f, 0.0f, 20.0f},
        ProjectedHitCandidate{{7}, 0.0f, nan, 100.0f, 20.0f},
    };
    const std::array mixed{
        invalid[0],
        ProjectedHitCandidate{{8}, 0.0f, 0.0f, 100.0f, 20.0f},
        invalid[1],
    };

    REQUIRE_FALSE(hit_test_projected_items(valid, nan, 10.0f, 4.0f));
    REQUIRE_FALSE(hit_test_projected_items(valid, 10.0f, infinity, 4.0f));
    REQUIRE_FALSE(hit_test_projected_items(valid, 10.0f, 10.0f, nan));
    REQUIRE_FALSE(hit_test_projected_items(valid, 10.0f, 10.0f, -1.0f));
    REQUIRE_FALSE(hit_test_projected_items(invalid, 10.0f, 10.0f, 4.0f));
    REQUIRE((hit_test_projected_items(mixed, 10.0f, 10.0f, 4.0f) ==
             ProjectedHit{{8}, ProjectedHitRegion::Body}));
    static_assert(noexcept(hit_test_projected_items(valid, 10.0f, 10.0f, 4.0f)));
}

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

TEST_CASE("Swing restarts from a meter change whose bar is off the tick-zero grid",
          "[timeline-editor][snap-grid]") {
    constexpr auto first_bar = 3 * kTicksPerQuarter / 2;
    const std::array points{
        MeterPoint{{0}, {3, 8}},
        MeterPoint{{first_bar}, {4, 4}},
    };
    const auto meter = compile(points);
    constexpr TickDuration eighth{kTicksPerQuarter / 2};
    auto grid = SnapGrid::create(eighth, kTripletSwing);
    REQUIRE(grid);

    constexpr auto local_offbeat = 2 * kTicksPerQuarter / 3;
    constexpr TickPosition offbeat{first_bar + local_offbeat};
    CHECK(grid->snap(meter, offbeat) == offbeat);
    CHECK(grid->snap(meter, {offbeat.value - 1}, SnapDirection::AtOrBefore) ==
          TickPosition{first_bar});
    CHECK(grid->snap(meter, {offbeat.value - 1}, SnapDirection::AtOrAfter) == offbeat);
    CHECK(grid->snap(meter, {offbeat.value + 1}, SnapDirection::AtOrBefore) == offbeat);
    CHECK(grid->snap(meter, {offbeat.value + 1}, SnapDirection::AtOrAfter) ==
          TickPosition{first_bar + kTicksPerQuarter});
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
    const auto first_boundary = grid->snap(meter, {minimum}, SnapDirection::AtOrAfter);
    const auto last_boundary = grid->snap(meter, {maximum}, SnapDirection::AtOrBefore);
    REQUIRE(first_boundary.value > minimum);
    REQUIRE(last_boundary.value < maximum);
    CHECK(grid->snap(meter, first_boundary, SnapDirection::AtOrBefore) == first_boundary);
    CHECK(grid->snap(meter, first_boundary, SnapDirection::AtOrAfter) == first_boundary);
    CHECK(grid->snap(meter, last_boundary, SnapDirection::AtOrBefore) == last_boundary);
    CHECK(grid->snap(meter, last_boundary, SnapDirection::AtOrAfter) == last_boundary);
    CHECK(grid->snap(meter, {minimum}) == first_boundary);
    CHECK(grid->snap(meter, {minimum + 1}) == first_boundary);
    CHECK(grid->snap(meter, {minimum + 2}) == first_boundary);
    CHECK(grid->snap(meter, {maximum}) == last_boundary);
    CHECK(grid->snap(meter, {maximum - 1}) == last_boundary);
    CHECK(grid->snap(meter, {maximum - 2}) == last_boundary);
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
