#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>
#include <vector>

using namespace pulp::timeline;
using namespace pulp::timebase;
namespace runtime = pulp::runtime;

namespace {

template <typename T> T ok(runtime::Result<T, ModelError> result) {
    REQUIRE(result.has_value());
    return std::move(result).value();
}

ContentHash audio_hash() {
    return *ContentHash::from_hex(std::string(64, 'a'));
}

MediaAsset audio_asset(ItemId id, std::uint64_t frame_count = 1'000) {
    return MediaAsset{
        id, "audio.wav", frame_count, {48'000, 1}, audio_hash(), AssetStoragePolicy::External,
        {}, {}};
}

Take make_take(ItemId id, ItemId asset_id, std::uint64_t frames = 100,
               std::int64_t source_start = 0, std::int64_t placement = 0) {
    return ok(Take::create(id, MediaRef{asset_id, {source_start}, frames}, {placement},
                           RationalRate{48'000, 1}));
}

// Ids: project 1, sequence 2, track 3, take lane 4, take 5, asset 6. The take
// references asset {6}, which the project owns.
Project project_with_take(bool record_armed = false, ItemId active_take_lane_id = {}) {
    auto lane = ok(TakeLane::create({4}, "comp A", {make_take({5}, {6})}));
    auto track = ok(Track::create(TrackInput{.id = {3},
                                             .name = "track",
                                             .take_lanes = {lane},
                                             .record_armed = record_armed,
                                             .active_take_lane_id = active_take_lane_id}));
    auto sequence = ok(Sequence::create({2}, "sequence", TickDuration{100}, {track}));
    return ok(
        Project::create(ProjectInput{{1}, "project", 7, {2}, {audio_asset({6})}, {sequence}}));
}

} // namespace

TEST_CASE("Take-lane and take immediate parents follow the ownership rule") {
    // A take lane is track-owned and recomputable from coordinates.
    REQUIRE(immediate_parent_id(ItemKind::TakeLane, {1}, {2}, {3}, {4}) == ItemId{3});
    // A take's parent is its lane, supplied via lane_id — never re-derived from
    // (sequence, track, clip). Without a lane_id it has no derivable parent.
    REQUIRE(immediate_parent_id(ItemKind::Take, {1}, {2}, {3}, {4}, {9}) == ItemId{9});
    REQUIRE(immediate_parent_id(ItemKind::Take, {1}, {2}, {3}, {4}) == ItemId{});
}

TEST_CASE("Take construction rejects malformed regions fail-closed") {
    REQUIRE(Take::create({5}, MediaRef{{6}, {0}, 100}, {0}, RationalRate{48'000, 1}).has_value());
    REQUIRE_FALSE(Take::create({0}, MediaRef{{6}, {0}, 100}, {0}, RationalRate{48'000, 1}));
    REQUIRE_FALSE(Take::create({5}, MediaRef{{0}, {0}, 100}, {0}, RationalRate{48'000, 1}));
    REQUIRE_FALSE(Take::create({5}, MediaRef{{6}, {0}, 0}, {0}, RationalRate{48'000, 1}));
    REQUIRE_FALSE(Take::create({5}, MediaRef{{6}, {0}, 100}, {-1}, RationalRate{48'000, 1}));
    REQUIRE_FALSE(Take::create({5}, MediaRef{{6}, {0}, 100}, {0}, RationalRate{0, 1}));
}

TEST_CASE("Take lane orders takes by identity and rejects duplicates") {
    auto lane = ok(TakeLane::create({4}, "lane", {make_take({7}, {6}), make_take({5}, {6})}));
    REQUIRE(lane.takes().size() == 2);
    REQUIRE(lane.takes()[0].id() == ItemId{5});
    REQUIRE(lane.takes()[1].id() == ItemId{7});
    REQUIRE(lane.find_take({5}) != nullptr);
    REQUIRE(lane.find_take({7}) != nullptr);
    REQUIRE(lane.find_take({99}) == nullptr);
    REQUIRE_FALSE(TakeLane::create({4}, "dup", {make_take({5}, {6}), make_take({5}, {6})}));
    REQUIRE_FALSE(TakeLane::create({0}, "bad", {}));
}

TEST_CASE("Take comp canonicalizes exact in-bounds non-overlapping segments") {
    const auto take_a = make_take({5}, {6}, 100, 0, 100);
    const auto take_b = make_take({7}, {6}, 100, 100, 100);
    auto lane = ok(TakeLane::create(
        {4}, "comp", {take_b, take_a},
        {{.take_id = {7}, .range = {{150}, 25, {48'000, 1}}},
         {.take_id = {5}, .range = {{100}, 50, {48'000, 1}}}}));
    REQUIRE(lane.comp_segments().size() == 2);
    REQUIRE(lane.comp_segments()[0].take_id == ItemId{5});
    REQUIRE(lane.comp_segments()[1].take_id == ItemId{7});

    REQUIRE_FALSE(TakeLane::create(
        {4}, "missing", {take_a},
        {{.take_id = {99}, .range = {{100}, 10, {48'000, 1}}}}));
    REQUIRE_FALSE(TakeLane::create(
        {4}, "before", {take_a},
        {{.take_id = {5}, .range = {{99}, 10, {48'000, 1}}}}));
    REQUIRE_FALSE(TakeLane::create(
        {4}, "past-end", {take_a},
        {{.take_id = {5}, .range = {{190}, 11, {48'000, 1}}}}));
    REQUIRE_FALSE(TakeLane::create(
        {4}, "empty", {take_a},
        {{.take_id = {5}, .range = {{100}, 0, {48'000, 1}}}}));
    REQUIRE_FALSE(TakeLane::create(
        {4}, "noncanonical-rate", {take_a},
        {{.take_id = {5}, .range = {{100}, 10, {96'000, 2}}}}));
    REQUIRE_FALSE(TakeLane::create(
        {4}, "wrong-rate", {take_a},
        {{.take_id = {5}, .range = {{100}, 10, {44'100, 1}}}}));
    REQUIRE_FALSE(TakeLane::create(
        {4}, "overlap", {take_a, take_b},
        {{.take_id = {5}, .range = {{100}, 60, {48'000, 1}}},
         {.take_id = {7}, .range = {{150}, 25, {48'000, 1}}}}));

    const auto other_rate =
        ok(Take::create({8}, MediaRef{{6}, {0}, 100}, {200}, RationalRate{44'100, 1}));
    REQUIRE_FALSE(TakeLane::create(
        {4}, "mixed-rate", {take_a, other_rate},
        {{.take_id = {5}, .range = {{100}, 50, {48'000, 1}}},
         {.take_id = {8}, .range = {{200}, 50, {44'100, 1}}}}));

    const auto near_limit =
        ok(Take::create({9}, MediaRef{{6}, {0}, 10},
                        {std::numeric_limits<std::int64_t>::max() - 5},
                        RationalRate{48'000, 1}));
    REQUIRE_FALSE(TakeLane::create(
        {4}, "unrepresentable-end", {near_limit},
        {{.take_id = {9},
          .range = {{std::numeric_limits<std::int64_t>::max() - 5}, 10, {48'000, 1}}}}));
}

TEST_CASE("Take comp replacement is immutable and referenced take deletion fails closed") {
    auto lane = ok(TakeLane::create({4}, "comp", {make_take({5}, {6}, 100, 0, 100)}));
    auto selected = ok(lane.with_comp_segments(
        {{.take_id = {5}, .range = {{120}, 40, {48'000, 1}}}}));
    REQUIRE(lane.comp_segments().empty());
    REQUIRE(selected.comp_segments().size() == 1);
    auto rejected = selected.erase_take({5});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ModelErrorCode::ActiveCompTakeRemoval);
    REQUIRE(rejected.error().item == ItemId{5});
    REQUIRE(ok(selected.with_comp_segments({})).erase_take({5}));
}

TEST_CASE("Track owns take lanes, record-arm intent, and active playlist selection") {
    const auto project = project_with_take(true, {4});
    const auto* sequence = project.find_sequence({2});
    REQUIRE(sequence != nullptr);
    const auto* track = sequence->find_track({3});
    REQUIRE(track != nullptr);
    REQUIRE(track->record_armed());
    REQUIRE(track->active_take_lane_id() == ItemId{4});
    REQUIRE(track->take_lanes().size() == 1);
    const auto* lane = track->find_take_lane({4});
    REQUIRE(lane != nullptr);
    REQUIRE(lane->find_take({5}) != nullptr);
    REQUIRE(track->find_take_lane({99}) == nullptr);

    // A takeless track defaults to not armed with no lanes.
    const auto plain = project_with_take(false);
    REQUIRE_FALSE(plain.find_sequence({2})->find_track({3})->record_armed());
    REQUIRE(plain.find_sequence({2})->find_track({3})->active_take_lane_id() == ItemId{});

    auto selectable_lane = ok(TakeLane::create({4}, "lane", {make_take({5}, {6})}));
    auto missing = Track::create(TrackInput{
        .id = {3}, .name = "track", .take_lanes = {selectable_lane}, .active_take_lane_id = {99}});
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error().code == ModelErrorCode::MissingItem);

    const ItemId exhausted{std::numeric_limits<std::uint64_t>::max()};
    auto invalid = Track::create(TrackInput{.id = {3},
                                            .name = "track",
                                            .take_lanes = {selectable_lane},
                                            .active_take_lane_id = exhausted});
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error().code == ModelErrorCode::InvalidItemId);
    REQUIRE_FALSE(track->with_active_take_lane(exhausted));
}

TEST_CASE("Take and take-lane identities record lane ownership") {
    const auto project = project_with_take();

    const auto lane_loc = project.locate({4});
    REQUIRE(lane_loc.has_value());
    REQUIRE(lane_loc->kind == ItemKind::TakeLane);
    REQUIRE(lane_loc->sequence_id == ItemId{2});
    REQUIRE(lane_loc->track_id == ItemId{3});
    // A take lane's parent is recomputable from its coordinates (its track).
    REQUIRE(lane_loc->parent_id == ItemId{3});
    REQUIRE(lane_loc->parent_id == immediate_parent_id(ItemKind::TakeLane, project.id(),
                                                       lane_loc->sequence_id, lane_loc->track_id,
                                                       lane_loc->clip_id));

    const auto take_loc = project.locate({5});
    REQUIRE(take_loc.has_value());
    REQUIRE(take_loc->kind == ItemKind::Take);
    REQUIRE(take_loc->sequence_id == ItemId{2});
    REQUIRE(take_loc->track_id == ItemId{3});
    REQUIRE(take_loc->clip_id == ItemId{}); // takes are lane-owned, never clip-owned
    // A take's parent is its lane, carried in parent_id — not its track.
    REQUIRE(take_loc->parent_id == ItemId{4});
    REQUIRE(take_loc->parent_id == immediate_parent_id(ItemKind::Take, project.id(),
                                                       take_loc->sequence_id, take_loc->track_id,
                                                       take_loc->clip_id, ItemId{4}));
}

TEST_CASE("A take referencing a missing or out-of-range asset is rejected") {
    // Missing: the take references {6} but the project only owns asset {99}.
    {
        auto lane = ok(TakeLane::create({4}, "lane", {make_take({5}, {6})}));
        auto track = ok(Track::create(TrackInput{.id = {3}, .name = "t", .take_lanes = {lane}}));
        auto sequence = ok(Sequence::create({2}, "seq", TickDuration{100}, {track}));
        REQUIRE_FALSE(
            Project::create(ProjectInput{{1}, "p", 7, {2}, {audio_asset({99})}, {sequence}}));
    }
    // Out of range: asset {6} has 100 frames; the take wants source_start 50 + 100 frames.
    {
        auto big = make_take({5}, {6}, /*frames=*/100, /*source_start=*/50);
        auto lane = ok(TakeLane::create({4}, "lane", {big}));
        auto track = ok(Track::create(TrackInput{.id = {3}, .name = "t", .take_lanes = {lane}}));
        auto sequence = ok(Sequence::create({2}, "seq", TickDuration{100}, {track}));
        REQUIRE_FALSE(Project::create(
            ProjectInput{{1}, "p", 7, {2}, {audio_asset({6}, /*frame_count=*/100)}, {sequence}}));
    }
}

TEST_CASE("A take identity cannot collide with any other track-owned id") {
    // A take reusing a device-placement id is rejected at track construction.
    auto lane = ok(TakeLane::create({4}, "lane", {make_take({8}, {6})}));
    auto colliding = Track::create(TrackInput{
        .id = {3}, .name = "t", .device_chain = {DevicePlacement{{8}}}, .take_lanes = {lane}});
    REQUIRE_FALSE(colliding.has_value());
}

TEST_CASE("Remap threads take and take-lane identities and fixes the asset reference") {
    const auto project = project_with_take(true, {4});
    const auto remapped = ok(remap_ids(project, 100));

    const auto sequence_id = *remapped.ids.find({2});
    const auto track_id = *remapped.ids.find({3});
    const auto lane_id = *remapped.ids.find({4});
    const auto take_id = *remapped.ids.find({5});
    const auto asset_id = *remapped.ids.find({6});
    REQUIRE(lane_id != ItemId{4});
    REQUIRE(take_id != ItemId{5});

    const auto* sequence = remapped.project.find_sequence(sequence_id);
    REQUIRE(sequence != nullptr);
    const auto* track = sequence->find_track(track_id);
    REQUIRE(track != nullptr);
    REQUIRE(track->record_armed());
    REQUIRE(track->active_take_lane_id() == lane_id);
    const auto* lane = track->find_take_lane(lane_id);
    REQUIRE(lane != nullptr);
    const auto* take = lane->find_take(take_id);
    REQUIRE(take != nullptr);
    REQUIRE(take->media().asset_id == asset_id);

    // The remapped take still points at the remapped lane, not its track.
    const auto take_loc = remapped.project.locate(take_id);
    REQUIRE(take_loc.has_value());
    REQUIRE(take_loc->parent_id == lane_id);
    REQUIRE(take_loc->track_id == track_id);
}

TEST_CASE("Remap fixes take identities embedded in comp selections") {
    auto lane = ok(TakeLane::create(
        {4}, "comp", {make_take({5}, {6})},
        {{.take_id = {5}, .range = {{0}, 50, {48'000, 1}}}}));
    auto track = ok(Track::create(
        TrackInput{.id = {3}, .name = "track", .take_lanes = {lane}}));
    auto sequence = ok(Sequence::create({2}, "sequence", TickDuration{100}, {track}));
    auto project = ok(
        Project::create(ProjectInput{{1}, "project", 7, {2}, {audio_asset({6})}, {sequence}}));
    const auto remapped = ok(remap_ids(project, 100));
    const auto lane_id = *remapped.ids.find({4});
    const auto take_id = *remapped.ids.find({5});
    const auto* comp_lane =
        remapped.project.find_sequence(*remapped.ids.find({2}))
            ->find_track(*remapped.ids.find({3}))
            ->find_take_lane(lane_id);
    REQUIRE(comp_lane->comp_segments().size() == 1);
    REQUIRE(comp_lane->comp_segments()[0].take_id == take_id);
}
