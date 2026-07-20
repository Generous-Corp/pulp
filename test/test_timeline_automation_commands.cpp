#include "timeline_command_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

using namespace timeline_test;

namespace {

template <typename T, typename E> T take(pulp::runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

AutomationLane automation_lane(float final_value = 0.75f) {
    auto curve = take(AutomationCurve::create({
        {{32}, {0}, 0.25f, AutomationInterpolation::Continuous, 0.0f},
        {{33}, {kTicksPerQuarter}, final_value, AutomationInterpolation::Hold, 0.5f},
    }));
    return take(AutomationLane::create({31}, DeviceParameterTarget{{20}, 7}, std::move(curve)));
}

Project automation_project(bool with_lane, float final_value = 0.75f) {
    TrackInput track_input;
    track_input.id = {10};
    track_input.name = "track";
    track_input.device_chain = {{{20}}};
    if (with_lane)
        track_input.automation_lanes.push_back(automation_lane(final_value));
    auto track = take(Track::create(std::move(track_input)));
    auto sequence = take(
        Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter}, {track}));
    return take(Project::create(
        {{1}, "project", with_lane ? 40u : 31u, {3}, {}, {sequence}}));
}

const Track& automation_track(const Project& project) {
    return *project.find_sequence({3})->find_track({10});
}

} // namespace

TEST_CASE("Automation commands compare and retain complete lane payloads") {
    const Command inserted = InsertAutomationLane{{3}, {10}, automation_lane()};
    const Command same = InsertAutomationLane{{3}, {10}, automation_lane()};
    const Command changed = InsertAutomationLane{{3}, {10}, automation_lane(0.5f)};
    REQUIRE(equivalent(inserted, same));
    REQUIRE_FALSE(equivalent(inserted, changed));
    REQUIRE(retained_size(inserted) >=
            sizeof(InsertAutomationLane) + 2 * sizeof(AutomationPoint));

    REQUIRE(equivalent(Command{RemoveAutomationLane{{3}, {10}, {31}}},
                       Command{RemoveAutomationLane{{3}, {10}, {31}}}));
    REQUIRE_FALSE(equivalent(Command{RemoveAutomationLane{{3}, {10}, {31}}},
                             Command{RemoveAutomationLane{{3}, {10}, {32}}}));
}

TEST_CASE("Automation insert is atomic and publishes owned identities") {
    const auto original = automation_project(false);
    const auto tx = transaction({1}, 1, 1, {},
                                {InsertAutomationLane{{3}, {10}, automation_lane()}});
    auto reduced = reduce_transaction(original, tx);
    REQUIRE(reduced);
    REQUIRE(automation_track(original).automation_lanes().empty());
    REQUIRE(equivalent(*automation_track(reduced->project).find_automation_lane({31}),
                       automation_lane()));
    REQUIRE(reduced->project.next_item_id() == 34);
    REQUIRE(reduced->dirty.items().size() == 1);
    REQUIRE(reduced->dirty.items()[0] ==
            DirtyItem{{31}, {10}, {3}, DirtyFlags::Structure | DirtyFlags::Automation |
                                               DirtyFlags::Added});
    REQUIRE(std::holds_alternative<RemoveAutomationLane>(reduced->inverses[0]));

    const auto lane_location = reduced->project.locate({31});
    const auto first_point_location = reduced->project.locate({32});
    const auto second_point_location = reduced->project.locate({33});
    REQUIRE(lane_location->active);
    REQUIRE(lane_location->kind == ItemKind::AutomationLane);
    REQUIRE(lane_location->automation_lane_id == ItemId{31});
    REQUIRE(first_point_location->active);
    REQUIRE(first_point_location->kind == ItemKind::AutomationPoint);
    REQUIRE(first_point_location->automation_lane_id == ItemId{31});
    REQUIRE(second_point_location->active);

    auto colliding_curve = take(AutomationCurve::create({{{10}, {0}, 0.5f}}));
    auto colliding_lane = take(
        AutomationLane::create({31}, DeviceParameterTarget{{20}, 8}, std::move(colliding_curve)));
    auto rejected = reduce_transaction(
        original, transaction({1}, 2, 2, {},
                              {InsertAutomationLane{{3}, {10}, std::move(colliding_lane)}}));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::IdentityNotAvailable);
    REQUIRE(automation_track(original).automation_lanes().empty());
    REQUIRE(original.next_item_id() == 31);
}

TEST_CASE("Automation insert undo redo and replay preserve tombstone ownership") {
    const auto initial = automation_project(false);
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();
    auto insert = session_transaction(
        writer, {}, {InsertAutomationLane{{3}, {10}, automation_lane()}});
    REQUIRE(session->submit(writer, std::move(insert)));
    REQUIRE(automation_track(*session->snapshot()).find_automation_lane({31}));

    REQUIRE(session->undo(writer));
    REQUIRE_FALSE(automation_track(*session->snapshot()).find_automation_lane({31}));
    REQUIRE_FALSE(session->snapshot()->locate({31})->active);
    REQUIRE_FALSE(session->snapshot()->locate({32})->active);
    REQUIRE(session->snapshot()->locate({32})->automation_lane_id == ItemId{31});

    auto reuse = session_transaction(
        writer, session->revision(), {InsertAutomationLane{{3}, {10}, automation_lane()}});
    auto rejected = session->submit(writer, std::move(reuse));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::IdentityNotAvailable);

    REQUIRE(session->redo(writer));
    const auto* restored = automation_track(*session->snapshot()).find_automation_lane({31});
    REQUIRE(restored);
    REQUIRE(equivalent(*restored, automation_lane()));
    REQUIRE(session->snapshot()->locate({31})->active);
    REQUIRE(session->snapshot()->locate({32})->active);

    auto replayed = session->journal().replay(initial, {});
    REQUIRE(replayed);
    REQUIRE(equivalent(*automation_track(*replayed).find_automation_lane({31}), automation_lane()));
}

TEST_CASE("Automation remove undo redo and replay retain the removed lane") {
    const auto initial = automation_project(true);
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();
    auto remove = session_transaction(writer, {}, {RemoveAutomationLane{{3}, {10}, {31}}});
    auto committed = session->submit(writer, std::move(remove));
    REQUIRE(committed);
    REQUIRE(committed->dirty.items()[0].flags ==
            (DirtyFlags::Structure | DirtyFlags::Automation | DirtyFlags::Removed));
    REQUIRE_FALSE(automation_track(*session->snapshot()).find_automation_lane({31}));
    REQUIRE_FALSE(session->snapshot()->locate({31})->active);
    REQUIRE_FALSE(session->snapshot()->locate({33})->active);

    REQUIRE(session->undo(writer));
    REQUIRE(equivalent(*automation_track(*session->snapshot()).find_automation_lane({31}),
                       automation_lane()));
    REQUIRE(session->snapshot()->locate({31})->active);
    REQUIRE(session->snapshot()->locate({33})->active);
    REQUIRE(session->redo(writer));
    REQUIRE_FALSE(automation_track(*session->snapshot()).find_automation_lane({31}));

    auto replayed = session->journal().replay(initial, {});
    REQUIRE(replayed);
    REQUIRE_FALSE(automation_track(*replayed).find_automation_lane({31}));
    REQUIRE_FALSE(replayed->locate({31})->active);
    REQUIRE_FALSE(replayed->locate({33})->active);
}

TEST_CASE("Journal checkpoint equivalence includes automation lane content") {
    const auto initial = automation_project(true);
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();
    auto edit = session_transaction(
        writer, {}, {SetTempoMap{initial.tempo_map(), make_tempo_map(91.0)}});
    REQUIRE(session->submit(writer, std::move(edit)));

    auto rejected = session->journal().replay(automation_project(true, 0.5f), {});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ModelInvariant);
}
