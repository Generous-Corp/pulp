#include <pulp/timeline/model.hpp>

#include "../core/timeline/src/project_state_access.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <utility>
#include <vector>

using namespace pulp::timeline;

namespace {

template <typename T, typename E> T take(pulp::runtime::Result<T, E> result) {
    REQUIRE(result.has_value());
    return std::move(result).value();
}

AutomationLane lane(ItemId id, ItemId placement, std::uint32_t parameter,
                    std::vector<AutomationPoint> points) {
    return take(AutomationLane::create(id, DeviceParameterTarget{placement, parameter},
                                       take(AutomationCurve::create(std::move(points)))));
}

Track track_with_automation() {
    return take(Track::create(TrackInput{
        .id = {10},
        .name = "track",
        .device_chain = {{{20}}},
        .automation_lanes =
            {
                lane({32}, {20}, 8, {{{42}, {10}, 0.75f}}),
                lane({31}, {20}, 7, {{{41}, {0}, 0.25f}}),
            },
    }));
}

Project project_with_automation(std::uint64_t next = 43) {
    auto sequence = take(Sequence::create({3}, "sequence", pulp::timebase::TickDuration{100},
                                          {track_with_automation()}));
    return take(Project::create(ProjectInput{{1}, "project", next, {3}, {}, {sequence}}));
}

} // namespace

TEST_CASE("Track owns canonical validated automation lanes") {
    const auto track = track_with_automation();
    REQUIRE(track.automation_lanes().size() == 2);
    REQUIRE(track.automation_lanes()[0].id() == ItemId{31});
    REQUIRE(track.automation_lanes()[1].id() == ItemId{32});
    REQUIRE(track.find_automation_lane({32}) == &track.automation_lanes()[1]);

    auto missing_target = Track::create(TrackInput{
        .id = {10},
        .name = "track",
        .device_chain = {{{20}}},
        .automation_lanes = {lane({31}, {21}, 7, {})},
    });
    REQUIRE_FALSE(missing_target.has_value());
    REQUIRE(missing_target.error().code == ModelErrorCode::MissingAutomationTarget);

    auto duplicate_target = Track::create(TrackInput{
        .id = {10},
        .name = "track",
        .device_chain = {{{20}}},
        .automation_lanes = {lane({31}, {20}, 7, {}), lane({32}, {20}, 7, {})},
    });
    REQUIRE_FALSE(duplicate_target.has_value());
    REQUIRE(duplicate_target.error().code == ModelErrorCode::DuplicateAutomationTarget);

    auto colliding_point = Track::create(TrackInput{
        .id = {10},
        .name = "track",
        .device_chain = {{{20}}},
        .automation_lanes = {lane({31}, {20}, 7, {{{20}, {0}, 0.5f}})},
    });
    REQUIRE_FALSE(colliding_point.has_value());
    REQUIRE(colliding_point.error().code == ModelErrorCode::DuplicateItemId);
}

TEST_CASE("Automation lane edits preserve immutable Track snapshots") {
    const auto original = track_with_automation();
    const auto* original_storage = original.automation_lanes().data();
    const auto inserted = take(original.insert_automation_lane(lane({33}, {20}, 9, {})));
    const auto erased = take(inserted.erase_automation_lane({31}));
    const auto clip_inserted = take(original.insert_clip(take(Clip::create(
        {50}, pulp::timebase::TickPosition{0}, pulp::timebase::TickDuration{10}, EmptyContent{}))));

    REQUIRE(original.automation_lanes().data() == original_storage);
    REQUIRE(clip_inserted.automation_lanes().data() == original_storage);
    REQUIRE(original.find_automation_lane({33}) == nullptr);
    REQUIRE(inserted.find_automation_lane({33}) != nullptr);
    REQUIRE(erased.find_automation_lane({31}) == nullptr);
}

TEST_CASE("Clip edits check only changed identities against attached automation") {
    const auto original = track_with_automation();

    auto colliding_clip = original.insert_clip(take(Clip::create(
        {41}, pulp::timebase::TickPosition{0}, pulp::timebase::TickDuration{10}, EmptyContent{})));
    REQUIRE_FALSE(colliding_clip.has_value());
    REQUIRE(colliding_clip.error().code == ModelErrorCode::DuplicateItemId);
    REQUIRE(colliding_clip.error().item == ItemId{41});

    const auto notes = take(NoteContent::create(
        {{{32}, pulp::timebase::TickPosition{0}, pulp::timebase::TickDuration{1}}}));
    auto colliding_note = original.insert_clip(take(Clip::create(
        {50}, pulp::timebase::TickPosition{0}, pulp::timebase::TickDuration{10}, notes)));
    REQUIRE_FALSE(colliding_note.has_value());
    REQUIRE(colliding_note.error().code == ModelErrorCode::DuplicateItemId);
    REQUIRE(colliding_note.error().item == ItemId{32});

    const auto inserted = take(original.insert_clip(take(Clip::create(
        {50}, pulp::timebase::TickPosition{0}, pulp::timebase::TickDuration{10}, EmptyContent{}))));
    auto replacement = inserted.replace_clip(take(Clip::create(
        {50}, pulp::timebase::TickPosition{0}, pulp::timebase::TickDuration{10}, notes)));
    REQUIRE_FALSE(replacement.has_value());
    REQUIRE(replacement.error().code == ModelErrorCode::DuplicateItemId);
    REQUIRE(replacement.error().item == ItemId{32});

    const auto erased = take(original.erase_automation_lane({31}));
    REQUIRE(erased.insert_clip(take(Clip::create(
        {41}, pulp::timebase::TickPosition{0}, pulp::timebase::TickDuration{10}, EmptyContent{}))));
}

TEST_CASE("Project identities describe automation lane and point ownership") {
    const auto project = project_with_automation();
    const auto lane_location = project.locate({31});
    const auto point_location = project.locate({41});
    REQUIRE(lane_location->kind == ItemKind::AutomationLane);
    REQUIRE(lane_location->sequence_id == ItemId{3});
    REQUIRE(lane_location->track_id == ItemId{10});
    REQUIRE(lane_location->parent_id == ItemId{10});
    REQUIRE(point_location->kind == ItemKind::AutomationPoint);
    REQUIRE(point_location->parent_id == ItemId{31});
}

TEST_CASE("Automation remap owns lane and point IDs but preserves parameter IDs") {
    const auto original = track_with_automation();
    ItemIdAllocator allocator(100);
    const auto remapped = take(remap_ids(original, allocator));
    REQUIRE(remapped.ids.entries().size() == 6);
    REQUIRE(allocator.next_value() == 106);

    const auto* mapped_lane = remapped.track.find_automation_lane(*remapped.ids.find({31}));
    REQUIRE(mapped_lane != nullptr);
    const auto& target = std::get<DeviceParameterTarget>(mapped_lane->target());
    REQUIRE(target.device_placement_id == *remapped.ids.find({20}));
    REQUIRE(target.param_id == 7);
    REQUIRE(mapped_lane->curve().points()[0].id == *remapped.ids.find({41}));

    ItemIdAllocator exhausted(std::numeric_limits<std::uint64_t>::max() - 4);
    const auto before = exhausted.next_value();
    auto failed = remap_ids(original, exhausted);
    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error().code == ModelErrorCode::ItemIdExhausted);
    REQUIRE(exhausted.next_value() == before);
}

TEST_CASE("Project remap preserves automation tombstones and their owners") {
    auto project = project_with_automation(53);
    auto identities = detail::ProjectStateAccess::identity_entries(project);
    identities.push_back({{51}, {ItemKind::AutomationLane, {10}, {3}, {10}, {}, false}});
    identities.push_back({{52}, {ItemKind::AutomationPoint, {51}, {3}, {10}, {}, false}});
    project = take(
        detail::ProjectStateAccess::restore_identities(std::move(project), std::move(identities)));

    const auto remapped = take(remap_ids(project, 100));
    const auto lane_tombstone = remapped.project.locate(*remapped.ids.find({51}));
    const auto point_tombstone = remapped.project.locate(*remapped.ids.find({52}));
    REQUIRE(lane_tombstone.has_value());
    REQUIRE_FALSE(lane_tombstone->active);
    REQUIRE(lane_tombstone->parent_id == *remapped.ids.find({10}));
    REQUIRE(point_tombstone.has_value());
    REQUIRE_FALSE(point_tombstone->active);
    REQUIRE(point_tombstone->parent_id == *remapped.ids.find({51}));
}

TEST_CASE("Restore rejects an automation point whose parent is not a lane") {
    auto project = project_with_automation(53);
    auto identities = detail::ProjectStateAccess::identity_entries(project);
    // Point {52} names the device placement {20} as its parent instead of a lane.
    // Its coordinate shape is well formed, so only the ownership walk can reject it.
    identities.push_back({{52}, {ItemKind::AutomationPoint, {20}, {3}, {10}, {}, false}});
    auto restored =
        detail::ProjectStateAccess::restore_identities(std::move(project), std::move(identities));
    REQUIRE_FALSE(restored.has_value());
    REQUIRE(restored.error().code == ModelErrorCode::InvalidSchemaIdentity);
    REQUIRE(restored.error().item == ItemId{52});
}
