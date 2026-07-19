#include <pulp/playback/track_automation_program.hpp>

#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;
using namespace pulp::timeline;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    if (!result)
        std::abort();
    return std::move(result).value();
}

std::shared_ptr<const CompiledTempoMap> tempo_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return shared_compiled_tempo_map(points, RationalRate{48'000, 1});
}

std::shared_ptr<const AutomationProgram>
program(ItemId lane_id, DeviceParameterTarget target,
        const std::shared_ptr<const CompiledTempoMap>& map, ProgramGeneration generation) {
    auto curve = take(AutomationCurve::create({AutomationPoint{{lane_id.value + 1}, {0}, 0.5f}}));
    auto lane = take(AutomationLane::create(lane_id, target, std::move(curve)));
    return take(AutomationProgram::compile(lane, map, generation));
}

} // namespace

TEST_CASE("track automation program canonicalizes lane ownership and lookup") {
    const auto map = tempo_map();
    const auto lane_30 = program({30}, {{100}, 3}, map, 4);
    const auto lane_10 = program({10}, {{100}, 1}, map, 4);
    const auto lane_20 = program({20}, {{100}, 2}, map, 4);

    const auto aggregate = take(TrackAutomationProgram::create(
        {5}, map, {lane_30, lane_10, lane_20}));

    REQUIRE(aggregate->track_id() == ItemId{5});
    REQUIRE(aggregate->tempo_map_owner() == map);
    REQUIRE(aggregate->programs().size() == 3);
    REQUIRE(aggregate->programs()[0] == lane_10);
    REQUIRE(aggregate->programs()[1] == lane_20);
    REQUIRE(aggregate->programs()[2] == lane_30);
    REQUIRE(aggregate->find_lane({20}) == lane_20.get());
    REQUIRE(aggregate->find_lane({99}) == nullptr);
}

TEST_CASE("track automation program preserves mixed-generation lane reuse") {
    const auto map = tempo_map();
    const auto unchanged = program({10}, {{100}, 1}, map, 2);
    const auto changed = program({20}, {{100}, 2}, map, 7);
    const auto unchanged_token = unchanged->instance_token();

    const auto first = take(TrackAutomationProgram::create({5}, map, {unchanged}));
    const auto rebuilt = take(TrackAutomationProgram::create({5}, map, {changed, unchanged}));

    REQUIRE(first->find_lane({10}) == unchanged.get());
    REQUIRE(rebuilt->find_lane({10}) == unchanged.get());
    REQUIRE(rebuilt->find_lane({10})->generation() == 2);
    REQUIRE(rebuilt->find_lane({10})->instance_token() == unchanged_token);
    REQUIRE(rebuilt->find_lane({20})->generation() == 7);
}

TEST_CASE("empty track automation program retains its exact tempo map") {
    const auto map = tempo_map();
    const auto aggregate = take(TrackAutomationProgram::create({5}, map, {}));

    REQUIRE(aggregate->programs().empty());
    REQUIRE(aggregate->tempo_map_owner().get() == map.get());
}

TEST_CASE("track automation program rejects missing ownership inputs") {
    const auto map = tempo_map();
    const auto lane = program({10}, {{100}, 1}, map, 1);

    const auto invalid_track = TrackAutomationProgram::create({}, map, {lane});
    REQUIRE_FALSE(invalid_track);
    REQUIRE(invalid_track.error().code == TrackAutomationProgramErrorCode::InvalidTrackId);

    const auto missing_map = TrackAutomationProgram::create({5}, {}, {lane});
    REQUIRE_FALSE(missing_map);
    REQUIRE(missing_map.error().code == TrackAutomationProgramErrorCode::MissingTempoMap);

    const auto missing_program = TrackAutomationProgram::create({5}, map, {lane, {}});
    REQUIRE_FALSE(missing_program);
    REQUIRE(missing_program.error().code == TrackAutomationProgramErrorCode::MissingProgram);

    const auto colliding_identity = program({5}, {{100}, 2}, map, 1);
    const auto invalid_lane = TrackAutomationProgram::create({5}, map, {colliding_identity});
    REQUIRE_FALSE(invalid_lane);
    REQUIRE(invalid_lane.error().code == TrackAutomationProgramErrorCode::InvalidLaneId);
    REQUIRE(invalid_lane.error().lane == ItemId{5});
}

TEST_CASE("track automation program requires exact tempo map ownership") {
    const auto owning_map = tempo_map();
    const auto equivalent_map = tempo_map();
    const auto lane = program({10}, {{100}, 1}, owning_map, 1);

    const auto mismatch = TrackAutomationProgram::create({5}, equivalent_map, {lane});
    REQUIRE_FALSE(mismatch);
    REQUIRE(mismatch.error().code == TrackAutomationProgramErrorCode::TempoMapMismatch);
    REQUIRE(mismatch.error().lane == ItemId{10});

    const auto colliding_identity = program({5}, {{100}, 2}, equivalent_map, 1);
    for (const auto& input :
         std::array{std::vector{lane, colliding_identity},
                    std::vector{colliding_identity, lane}}) {
        const auto deterministic = TrackAutomationProgram::create({5}, owning_map, input);
        REQUIRE_FALSE(deterministic);
        REQUIRE(deterministic.error().code == TrackAutomationProgramErrorCode::InvalidLaneId);
        REQUIRE(deterministic.error().lane == ItemId{5});
    }
}

TEST_CASE("track automation program rejects duplicate lanes deterministically") {
    const auto map = tempo_map();
    const auto first = program({10}, {{100}, 1}, map, 1);
    const auto second = program({10}, {{100}, 2}, map, 2);

    for (const auto& input : std::array{std::vector{first, second}, std::vector{second, first}}) {
        const auto duplicate = TrackAutomationProgram::create({5}, map, input);
        REQUIRE_FALSE(duplicate);
        REQUIRE(duplicate.error().code == TrackAutomationProgramErrorCode::DuplicateLane);
        REQUIRE(duplicate.error().lane == ItemId{10});
        REQUIRE(duplicate.error().related_lane == ItemId{10});
    }
}

TEST_CASE("track automation program rejects duplicate targets but preserves device scope") {
    const auto map = tempo_map();
    const auto first = program({10}, {{100}, 7}, map, 1);
    const auto duplicate_target = program({20}, {{100}, 7}, map, 3);

    for (const auto& input :
         std::array{std::vector{first, duplicate_target}, std::vector{duplicate_target, first}}) {
        const auto duplicate = TrackAutomationProgram::create({5}, map, input);
        REQUIRE_FALSE(duplicate);
        REQUIRE(duplicate.error().code == TrackAutomationProgramErrorCode::DuplicateTarget);
        REQUIRE(duplicate.error().lane == ItemId{20});
        REQUIRE(duplicate.error().related_lane == ItemId{10});
        REQUIRE(duplicate.error().target == DeviceParameterTarget{{100}, 7});
    }

    const auto other_device = program({30}, {{200}, 7}, map, 4);
    const auto distinct = TrackAutomationProgram::create({5}, map, {first, other_device});
    REQUIRE(distinct);
}
