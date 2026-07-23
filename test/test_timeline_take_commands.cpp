#include "timeline_command_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <utility>
#include <vector>

using namespace timeline_test;

namespace {

template <typename T, typename E> T take(pulp::runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

MediaAsset recorded_asset(ItemId id = {20}, std::uint64_t frame_count = 1'000) {
    return MediaAsset{id,
                      "captured.wav",
                      frame_count,
                      {48'000, 1},
                      content_hash(),
                      AssetStoragePolicy::External,
                      {},
                      {}};
}

Take recorded_take(ItemId id, ItemId asset_id = {20}, std::uint64_t frames = 100,
                   std::int64_t source_start = 0, std::int64_t placement = 0) {
    return take(Take::create(id, MediaRef{asset_id, {source_start}, frames}, {placement},
                             RationalRate{48'000, 1}));
}

TakeLane take_lane(ItemId lane_id = {31}, ItemId take_id = {32}, std::string name = "comp") {
    return take(TakeLane::create(lane_id, std::move(name), {recorded_take(take_id)}));
}

std::vector<TakeCompSegment> comp(ItemId take_id = {32}, std::int64_t start = 0,
                                  std::uint64_t count = 50) {
    return {{.take_id = take_id, .range = {{start}, count, {48'000, 1}}}};
}

// Ids: project 1, sequence 3, track 10, asset 20. next_item_id 31 leaves the
// lane/take ids (31, 32) available for InsertTakeLane to allocate.
Project record_project(bool armed = false) {
    TrackInput track_input;
    track_input.id = {10};
    track_input.name = "track";
    track_input.record_armed = armed;
    auto track = take(Track::create(std::move(track_input)));
    auto sequence =
        take(Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter}, {track}));
    return take(Project::create({{1}, "project", 31, {3}, {recorded_asset()}, {sequence}}));
}

const Track& record_track(const Project& project) {
    return *project.find_sequence({3})->find_track({10});
}

} // namespace

TEST_CASE("Take commands compare and retain complete lane payloads") {
    const Command inserted = InsertTakeLane{{3}, {10}, take_lane()};
    const Command same = InsertTakeLane{{3}, {10}, take_lane()};
    const Command renamed = InsertTakeLane{{3}, {10}, take_lane({31}, {32}, "other")};
    REQUIRE(equivalent(inserted, same));
    REQUIRE_FALSE(equivalent(inserted, renamed));
    REQUIRE(retained_size(inserted) >= sizeof(InsertTakeLane) + sizeof(Take));

    REQUIRE(equivalent(Command{RemoveTakeLane{{3}, {10}, {31}}},
                       Command{RemoveTakeLane{{3}, {10}, {31}}}));
    REQUIRE_FALSE(equivalent(Command{RemoveTakeLane{{3}, {10}, {31}}},
                             Command{RemoveTakeLane{{3}, {10}, {32}}}));

    REQUIRE(equivalent(Command{SetRecordArm{{3}, {10}, false, true}},
                       Command{SetRecordArm{{3}, {10}, false, true}}));
    REQUIRE_FALSE(equivalent(Command{SetRecordArm{{3}, {10}, false, true}},
                             Command{SetRecordArm{{3}, {10}, true, true}}));

    REQUIRE(equivalent(Command{InsertTake{{3}, {10}, {31}, recorded_take({33})}},
                       Command{InsertTake{{3}, {10}, {31}, recorded_take({33})}}));
    REQUIRE_FALSE(equivalent(Command{InsertTake{{3}, {10}, {31}, recorded_take({33})}},
                             Command{InsertTake{{3}, {10}, {31}, recorded_take({34})}}));
    REQUIRE(retained_size(Command{InsertTake{{3}, {10}, {31}, recorded_take({33})}}) >=
            sizeof(InsertTake));
    REQUIRE(equivalent(Command{RemoveTake{{3}, {10}, {31}, {33}}},
                       Command{RemoveTake{{3}, {10}, {31}, {33}}}));
    REQUIRE_FALSE(equivalent(Command{RemoveTake{{3}, {10}, {31}, {33}}},
                             Command{RemoveTake{{3}, {10}, {31}, {34}}}));
    REQUIRE(equivalent(Command{SetActiveTakeLane{{3}, {10}, {}, {31}}},
                       Command{SetActiveTakeLane{{3}, {10}, {}, {31}}}));
    REQUIRE_FALSE(equivalent(Command{SetActiveTakeLane{{3}, {10}, {}, {31}}},
                             Command{SetActiveTakeLane{{3}, {10}, {31}, {}}}));
    REQUIRE(equivalent(Command{SetTakeComp{{3}, {10}, {31}, {}, comp()}},
                       Command{SetTakeComp{{3}, {10}, {31}, {}, comp()}}));
    REQUIRE_FALSE(equivalent(Command{SetTakeComp{{3}, {10}, {31}, {}, comp()}},
                             Command{SetTakeComp{{3}, {10}, {31}, {}, comp({32}, 1)}}));
    REQUIRE(retained_size(Command{SetTakeComp{{3}, {10}, {31}, comp(), comp()}}) >=
            sizeof(SetTakeComp) + 2 * sizeof(TakeCompSegment));
}

TEST_CASE("Take-lane insert is atomic and publishes owned identities") {
    const auto original = record_project();
    const auto tx = transaction({1}, 1, 1, {}, {InsertTakeLane{{3}, {10}, take_lane()}});
    auto reduced = reduce_transaction(original, tx);
    REQUIRE(reduced);
    REQUIRE(record_track(original).take_lanes().empty());

    const auto* lane = record_track(reduced->project).find_take_lane({31});
    REQUIRE(lane != nullptr);
    REQUIRE(equivalent(*lane, take_lane()));
    REQUIRE(reduced->project.next_item_id() == 33);
    REQUIRE(reduced->dirty.items().size() == 1);
    REQUIRE(
        reduced->dirty.items()[0] ==
        DirtyItem{{31}, {10}, {3}, DirtyFlags::Structure | DirtyFlags::Take | DirtyFlags::Added});
    REQUIRE(std::holds_alternative<RemoveTakeLane>(reduced->inverses[0]));

    // The take lane's parent is its track; the take's parent is its lane.
    const auto lane_location = reduced->project.locate({31});
    const auto take_location = reduced->project.locate({32});
    REQUIRE(lane_location->active);
    REQUIRE(lane_location->kind == ItemKind::TakeLane);
    REQUIRE(lane_location->parent_id == ItemId{10});
    REQUIRE(take_location->active);
    REQUIRE(take_location->kind == ItemKind::Take);
    REQUIRE(take_location->parent_id == ItemId{31});
    REQUIRE(take_location->clip_id == ItemId{});

    // Re-inserting the same identities is rejected (not silently duplicated).
    auto rejected = reduce_transaction(
        original, transaction({1}, 2, 2, {}, {InsertTakeLane{{3}, {10}, take_lane()}}));
    REQUIRE(rejected);
    auto colliding = reduce_transaction(
        reduced->project,
        transaction({1}, 3, 3, {}, {InsertTakeLane{{3}, {10}, take_lane({31}, {40})}}));
    REQUIRE_FALSE(colliding);
    REQUIRE(colliding.error().code == ConflictCode::IdentityNotAvailable);
}

TEST_CASE("Take-lane insert rejects a take referencing a missing or out-of-range asset") {
    const auto original = record_project();
    // Missing asset: take references {99}, project only owns {20}.
    auto missing = reduce_transaction(
        original,
        transaction(
            {1}, 1, 1, {},
            {InsertTakeLane{
                {3}, {10}, take(TakeLane::create({31}, "comp", {recorded_take({32}, {99})}))}}));
    REQUIRE_FALSE(missing);

    // Out of range: asset {20} has 1000 frames; take wants source_start 980 + 100.
    auto out_of_range = reduce_transaction(
        original,
        transaction({1}, 2, 2, {},
                    {InsertTakeLane{{3},
                                    {10},
                                    take(TakeLane::create(
                                        {31}, "comp", {recorded_take({32}, {20}, 100, 980)}))}}));
    REQUIRE_FALSE(out_of_range);
}

TEST_CASE("Take-lane insert undo redo and replay preserve tombstone ownership") {
    const auto initial = record_project();
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();
    auto insert = session_transaction(writer, {}, {InsertTakeLane{{3}, {10}, take_lane()}});
    REQUIRE(session->submit(writer, std::move(insert)));
    REQUIRE(record_track(*session->snapshot()).find_take_lane({31}));

    REQUIRE(session->undo(writer));
    REQUIRE_FALSE(record_track(*session->snapshot()).find_take_lane({31}));
    REQUIRE_FALSE(session->snapshot()->locate({31})->active);
    REQUIRE_FALSE(session->snapshot()->locate({32})->active);
    REQUIRE(session->snapshot()->locate({32})->parent_id == ItemId{31});

    // The tombstoned identities cannot be reused by a fresh insert.
    auto reuse =
        session_transaction(writer, session->revision(), {InsertTakeLane{{3}, {10}, take_lane()}});
    auto rejected = session->submit(writer, std::move(reuse));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::IdentityNotAvailable);

    REQUIRE(session->redo(writer));
    const auto* restored = record_track(*session->snapshot()).find_take_lane({31});
    REQUIRE(restored);
    REQUIRE(equivalent(*restored, take_lane()));
    REQUIRE(session->snapshot()->locate({31})->active);
    REQUIRE(session->snapshot()->locate({32})->active);

    auto replayed = session->journal().replay(initial, {});
    REQUIRE(replayed);
    REQUIRE(equivalent(*record_track(*replayed).find_take_lane({31}), take_lane()));
}

TEST_CASE("Take-lane remove undo redo and replay retain the removed lane") {
    auto initial = record_project();
    // Seed the project with a take lane by committing an insert first.
    auto seed_session = std::move(DocumentSession::create(initial)).value();
    auto seed_writer = std::move(seed_session->register_writer()).value();
    REQUIRE(seed_session->submit(
        seed_writer,
        session_transaction(seed_writer, {}, {InsertTakeLane{{3}, {10}, take_lane()}})));
    const auto seeded = *seed_session->snapshot();

    auto session = std::move(DocumentSession::create(seeded)).value();
    auto writer = std::move(session->register_writer()).value();
    auto remove = session_transaction(writer, {}, {RemoveTakeLane{{3}, {10}, {31}}});
    auto committed = session->submit(writer, std::move(remove));
    REQUIRE(committed);
    REQUIRE(committed->dirty.items()[0].flags ==
            (DirtyFlags::Structure | DirtyFlags::Take | DirtyFlags::Removed));
    REQUIRE_FALSE(record_track(*session->snapshot()).find_take_lane({31}));
    REQUIRE_FALSE(session->snapshot()->locate({31})->active);
    REQUIRE_FALSE(session->snapshot()->locate({32})->active);

    REQUIRE(session->undo(writer));
    REQUIRE(equivalent(*record_track(*session->snapshot()).find_take_lane({31}), take_lane()));
    REQUIRE(session->snapshot()->locate({31})->active);
    REQUIRE(session->snapshot()->locate({32})->active);
    REQUIRE(session->redo(writer));
    REQUIRE_FALSE(record_track(*session->snapshot()).find_take_lane({31}));

    auto replayed = session->journal().replay(seeded, {});
    REQUIRE(replayed);
    REQUIRE_FALSE(record_track(*replayed).find_take_lane({31}));
    REQUIRE_FALSE(replayed->locate({31})->active);
}

TEST_CASE("Record-arm set gates on expected value and round-trips through undo and replay") {
    const auto initial = record_project(/*armed=*/false);
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();

    // A stale expected value is rejected without mutating the document.
    auto stale = session_transaction(writer, {}, {SetRecordArm{{3}, {10}, true, true}});
    auto rejected = session->submit(writer, std::move(stale));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ExpectedValueMismatch);
    REQUIRE_FALSE(record_track(*session->snapshot()).record_armed());

    auto arm =
        session_transaction(writer, session->revision(), {SetRecordArm{{3}, {10}, false, true}});
    auto committed = session->submit(writer, std::move(arm));
    REQUIRE(committed);
    REQUIRE(committed->dirty.items()[0].flags == DirtyFlags::Take);
    REQUIRE(record_track(*session->snapshot()).record_armed());

    REQUIRE(session->undo(writer));
    REQUIRE_FALSE(record_track(*session->snapshot()).record_armed());
    REQUIRE(session->redo(writer));
    REQUIRE(record_track(*session->snapshot()).record_armed());

    auto replayed = session->journal().replay(initial, {});
    REQUIRE(replayed);
    REQUIRE(record_track(*replayed).record_armed());
}

TEST_CASE("Take insert and remove publish one owned identity and round trip") {
    const auto original = record_project();
    auto lane_inserted = reduce_transaction(
        original, transaction({1}, 1, 1, {}, {InsertTakeLane{{3}, {10}, take_lane()}}));
    REQUIRE(lane_inserted);
    const auto seeded = lane_inserted->project;

    auto session = std::move(DocumentSession::create(seeded)).value();
    auto writer = std::move(session->register_writer()).value();
    auto insert = session_transaction(
        writer, {}, {InsertTake{{3}, {10}, {31}, recorded_take({33}, {20}, 80, 200, 400)}});
    auto committed = session->submit(writer, std::move(insert));
    REQUIRE(committed);
    REQUIRE(
        committed->dirty.items()[0] ==
        DirtyItem{{33}, {10}, {3}, DirtyFlags::Structure | DirtyFlags::Take | DirtyFlags::Added});
    const auto* lane = record_track(*session->snapshot()).find_take_lane({31});
    REQUIRE(lane != nullptr);
    REQUIRE(lane->takes().size() == 2);
    REQUIRE(lane->find_take({33}) != nullptr);
    REQUIRE(session->snapshot()->locate({33})->parent_id == ItemId{31});
    REQUIRE(session->snapshot()->next_item_id() == 34);

    REQUIRE(session->undo(writer));
    REQUIRE(record_track(*session->snapshot()).find_take_lane({31})->find_take({33}) == nullptr);
    REQUIRE_FALSE(session->snapshot()->locate({33})->active);
    REQUIRE(session->redo(writer));
    REQUIRE(record_track(*session->snapshot()).find_take_lane({31})->find_take({33}) != nullptr);

    auto remove =
        session_transaction(writer, session->revision(), {RemoveTake{{3}, {10}, {31}, {33}}});
    auto removed = session->submit(writer, std::move(remove));
    REQUIRE(removed);
    REQUIRE(removed->dirty.items()[0].flags ==
            (DirtyFlags::Structure | DirtyFlags::Take | DirtyFlags::Removed));
    REQUIRE(record_track(*session->snapshot()).find_take_lane({31})->find_take({33}) == nullptr);
    REQUIRE_FALSE(session->snapshot()->locate({33})->active);
    REQUIRE(session->undo(writer));
    REQUIRE(record_track(*session->snapshot()).find_take_lane({31})->find_take({33}) != nullptr);

    auto replayed = session->journal().replay(seeded, {});
    REQUIRE(replayed);
    REQUIRE(record_track(*replayed).find_take_lane({31})->find_take({33}) != nullptr);
}

TEST_CASE("Take insert rejects wrong lane ownership and invalid media atomically") {
    const auto original = record_project();
    auto lane_inserted = reduce_transaction(
        original, transaction({1}, 1, 1, {}, {InsertTakeLane{{3}, {10}, take_lane()}}));
    REQUIRE(lane_inserted);
    const auto seeded = lane_inserted->project;

    auto missing_lane = reduce_transaction(
        seeded, transaction({1}, 2, 2, {}, {InsertTake{{3}, {10}, {99}, recorded_take({33})}}));
    REQUIRE_FALSE(missing_lane);
    REQUIRE(missing_lane.error().code == ConflictCode::TargetMissing);
    REQUIRE(seeded.locate({33}) == std::nullopt);

    auto missing_asset = reduce_transaction(
        seeded,
        transaction({1}, 3, 3, {}, {InsertTake{{3}, {10}, {31}, recorded_take({33}, {99})}}));
    REQUIRE_FALSE(missing_asset);
    REQUIRE(missing_asset.error().model_error->code == ModelErrorCode::MissingAsset);
    REQUIRE(seeded.locate({33}) == std::nullopt);

    auto wrong_parent =
        reduce_transaction(seeded, transaction({1}, 4, 4, {}, {RemoveTake{{3}, {10}, {99}, {32}}}));
    REQUIRE_FALSE(wrong_parent);
    REQUIRE(wrong_parent.error().code == ConflictCode::ParentMismatch);
}

TEST_CASE("Asset removal is rejected while a take still references the asset") {
    const auto original = record_project();
    auto lane_inserted = reduce_transaction(
        original, transaction({1}, 1, 1, {}, {InsertTakeLane{{3}, {10}, take_lane()}}));
    REQUIRE(lane_inserted);

    auto rejected =
        reduce_transaction(lane_inserted->project, transaction({1}, 2, 2, {}, {RemoveAsset{{20}}}));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ModelInvariant);
    REQUIRE(rejected.error().model_error);
    REQUIRE(rejected.error().model_error->code == ModelErrorCode::MissingAsset);
    REQUIRE(rejected.error().model_error->item == ItemId{32});
    REQUIRE(rejected.error().model_error->related_item == ItemId{20});
    REQUIRE(lane_inserted->project.find_asset({20}) != nullptr);
}

TEST_CASE("Active take-lane selection is explicit and prevents dangling removal") {
    const auto original = record_project();
    auto lane_inserted = reduce_transaction(
        original, transaction({1}, 1, 1, {}, {InsertTakeLane{{3}, {10}, take_lane()}}));
    REQUIRE(lane_inserted);
    auto session = std::move(DocumentSession::create(lane_inserted->project)).value();
    auto writer = std::move(session->register_writer()).value();

    auto stale = session_transaction(writer, {}, {SetActiveTakeLane{{3}, {10}, {31}, {}}});
    auto stale_result = session->submit(writer, std::move(stale));
    REQUIRE_FALSE(stale_result);
    REQUIRE(stale_result.error().code == ConflictCode::ExpectedValueMismatch);

    auto missing = session_transaction(writer, {}, {SetActiveTakeLane{{3}, {10}, {}, {99}}});
    auto missing_result = session->submit(writer, std::move(missing));
    REQUIRE_FALSE(missing_result);
    REQUIRE(missing_result.error().code == ConflictCode::TargetMissing);

    const ItemId exhausted{std::numeric_limits<std::uint64_t>::max()};
    auto invalid = session_transaction(writer, {}, {SetActiveTakeLane{{3}, {10}, {}, exhausted}});
    auto invalid_result = session->submit(writer, std::move(invalid));
    REQUIRE_FALSE(invalid_result);
    REQUIRE(invalid_result.error().code == ConflictCode::InvalidIdentifier);

    REQUIRE(session->submit(
        writer, session_transaction(writer, {}, {SetActiveTakeLane{{3}, {10}, {}, {31}}})));
    REQUIRE(record_track(*session->snapshot()).active_take_lane_id() == ItemId{31});

    auto selected_remove =
        session_transaction(writer, session->revision(), {RemoveTakeLane{{3}, {10}, {31}}});
    auto selected_remove_result = session->submit(writer, std::move(selected_remove));
    REQUIRE_FALSE(selected_remove_result);
    REQUIRE(selected_remove_result.error().code == ConflictCode::ExpectedValueMismatch);

    auto clear_and_remove = session_transaction(
        writer, session->revision(),
        {SetActiveTakeLane{{3}, {10}, {31}, {}}, RemoveTakeLane{{3}, {10}, {31}}});
    REQUIRE(session->submit(writer, std::move(clear_and_remove)));
    REQUIRE(record_track(*session->snapshot()).active_take_lane_id() == ItemId{});
    REQUIRE(record_track(*session->snapshot()).find_take_lane({31}) == nullptr);

    REQUIRE(session->undo(writer));
    REQUIRE(record_track(*session->snapshot()).active_take_lane_id() == ItemId{31});
    REQUIRE(record_track(*session->snapshot()).find_take_lane({31}) != nullptr);
    REQUIRE(session->redo(writer));
    REQUIRE(record_track(*session->snapshot()).active_take_lane_id() == ItemId{});
    REQUIRE(record_track(*session->snapshot()).find_take_lane({31}) == nullptr);
}

TEST_CASE("Journal checkpoint equivalence includes takes and active selection") {
    const auto original = record_project();
    auto lane_inserted = reduce_transaction(
        original, transaction({1}, 1, 1, {}, {InsertTakeLane{{3}, {10}, take_lane()}}));
    REQUIRE(lane_inserted);
    const auto seeded = lane_inserted->project;

    auto session = std::move(DocumentSession::create(seeded)).value();
    auto writer = std::move(session->register_writer()).value();
    REQUIRE(session->submit(
        writer, session_transaction(writer, {}, {SetRecordArm{{3}, {10}, false, true}})));

    auto renamed_lane = reduce_transaction(
        original, transaction({1}, 2, 2, {},
                              {InsertTakeLane{{3}, {10}, take_lane({31}, {32}, "different")}}));
    REQUIRE(renamed_lane);
    auto content_mismatch = session->journal().replay(renamed_lane->project, {});
    REQUIRE_FALSE(content_mismatch);
    REQUIRE(content_mismatch.error().code == ConflictCode::ModelInvariant);

    auto selected = reduce_transaction(
        seeded, transaction({1}, 3, 3, {}, {SetActiveTakeLane{{3}, {10}, {}, {31}}}));
    REQUIRE(selected);
    auto selection_mismatch = session->journal().replay(selected->project, {});
    REQUIRE_FALSE(selection_mismatch);
    REQUIRE(selection_mismatch.error().code == ConflictCode::ModelInvariant);
}

TEST_CASE("Segment comp gates, validates, undoes, redoes, and replays exactly") {
    const auto original = record_project();
    auto inserted = reduce_transaction(
        original, transaction({1}, 1, 1, {}, {InsertTakeLane{{3}, {10}, take_lane()}}));
    REQUIRE(inserted);
    const auto seeded = inserted->project;
    auto session = std::move(DocumentSession::create(seeded)).value();
    auto writer = std::move(session->register_writer()).value();

    auto stale = session->submit(
        writer, session_transaction(writer, {}, {SetTakeComp{{3}, {10}, {31}, comp(), {}}}));
    REQUIRE_FALSE(stale);
    REQUIRE(stale.error().code == ConflictCode::ExpectedValueMismatch);

    auto invalid = session->submit(
        writer,
        session_transaction(writer, {}, {SetTakeComp{{3}, {10}, {31}, {}, comp({99})}}));
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error().model_error);
    REQUIRE(invalid.error().model_error->code == ModelErrorCode::InvalidTakeComp);

    const auto selection = comp();
    REQUIRE(session->submit(
        writer,
        session_transaction(writer, {}, {SetTakeComp{{3}, {10}, {31}, {}, selection}})));
    const auto selected =
        record_track(*session->snapshot()).find_take_lane({31})->comp_segments();
    REQUIRE(std::vector<TakeCompSegment>(selected.begin(), selected.end()) == selection);

    auto referenced_remove =
        session_transaction(writer, session->revision(), {RemoveTake{{3}, {10}, {31}, {32}}});
    auto remove_result = session->submit(writer, std::move(referenced_remove));
    REQUIRE_FALSE(remove_result);
    REQUIRE(remove_result.error().model_error);
    REQUIRE(remove_result.error().model_error->code == ModelErrorCode::ActiveCompTakeRemoval);

    REQUIRE(session->undo(writer));
    REQUIRE(record_track(*session->snapshot()).find_take_lane({31})->comp_segments().empty());
    REQUIRE(session->redo(writer));
    const auto redone = record_track(*session->snapshot()).find_take_lane({31})->comp_segments();
    REQUIRE(std::vector<TakeCompSegment>(redone.begin(), redone.end()) == selection);

    auto replayed = session->journal().replay(seeded, {});
    REQUIRE(replayed);
    const auto replayed_comp = record_track(*replayed).find_take_lane({31})->comp_segments();
    REQUIRE(std::vector<TakeCompSegment>(replayed_comp.begin(), replayed_comp.end()) == selection);
}

TEST_CASE("Segment comp undo gates against the canonical replacement") {
    const auto original = record_project();
    auto lane_inserted = reduce_transaction(
        original, transaction({1}, 1, 1, {}, {InsertTakeLane{{3}, {10}, take_lane()}}));
    REQUIRE(lane_inserted);
    auto take_inserted =
        reduce_transaction(lane_inserted->project,
                           transaction({1}, 2, 2, {},
                                       {InsertTake{{3}, {10}, {31}, recorded_take({33})}}));
    REQUIRE(take_inserted);

    auto session = std::move(DocumentSession::create(take_inserted->project)).value();
    auto writer = std::move(session->register_writer()).value();
    const std::vector<TakeCompSegment> unordered{
        {.take_id = {33}, .range = {{25}, 25, {48'000, 1}}},
        {.take_id = {32}, .range = {{0}, 25, {48'000, 1}}},
    };
    REQUIRE(session->submit(
        writer, session_transaction(writer, {}, {SetTakeComp{{3}, {10}, {31}, {}, unordered}})));
    const auto stored = record_track(*session->snapshot()).find_take_lane({31})->comp_segments();
    REQUIRE(stored[0].take_id == ItemId{32});
    REQUIRE(stored[1].take_id == ItemId{33});
    REQUIRE(session->undo(writer));
    REQUIRE(record_track(*session->snapshot()).find_take_lane({31})->comp_segments().empty());
    REQUIRE(session->redo(writer));
    REQUIRE(record_track(*session->snapshot()).find_take_lane({31})->comp_segments().size() == 2);
}
