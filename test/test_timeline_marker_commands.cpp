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

constexpr std::int64_t kBar = 4 * kTicksPerQuarter;

// Ids: project 1, sequence 3, track 4, seeded annotations 10..15. next_item_id
// 20 leaves 20 and up free for marker and region inserts to claim.
Project annotated_project(std::vector<SequenceMarker> markers = {},
                          std::vector<SequenceRegion> regions = {}) {
    auto track = take(Track::create({4}, "track", {}));
    auto sequence =
        take(Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter}, std::nullopt,
                              {track}, std::move(markers), std::move(regions)));
    return take(Project::create({{1}, "project", 20, {3}, {}, {sequence}}));
}

const Sequence& annotated_sequence(const Project& project) {
    return *project.find_sequence({3});
}

} // namespace

TEST_CASE("Sequence markers and regions canonicalize order independent of authored order") {
    const auto project = annotated_project(
        {SequenceMarker{{12}, "outro", {2 * kBar}}, SequenceMarker{{10}, "intro", {0}},
         SequenceMarker{{11}, "verse", {kBar}}},
        {SequenceRegion{{15}, "second", {kBar}, {kBar}}, SequenceRegion{{13}, "whole", {0}, {kBar}},
         SequenceRegion{{14}, "first", {0}, {kBar / 2}}});
    const auto& sequence = annotated_sequence(project);
    REQUIRE(sequence.markers().size() == 3);
    REQUIRE(sequence.markers()[0].id == ItemId{10});
    REQUIRE(sequence.markers()[1].id == ItemId{11});
    REQUIRE(sequence.markers()[2].id == ItemId{12});
    REQUIRE(sequence.regions().size() == 3);
    // (position, duration, id): the two spans starting at 0 order by length.
    REQUIRE(sequence.regions()[0].id == ItemId{14});
    REQUIRE(sequence.regions()[1].id == ItemId{13});
    REQUIRE(sequence.regions()[2].id == ItemId{15});
    REQUIRE(sequence.find_marker({11})->name == "verse");
    REQUIRE(sequence.find_region({15})->name == "second");
    REQUIRE(sequence.find_marker({13}) == nullptr);
    REQUIRE(sequence.find_region({10}) == nullptr);
}

TEST_CASE("Sequence regions may overlap and nest") {
    // Overlap is a documented model allowance, not an accident: a section and
    // the sub-section inside it must both be expressible.
    auto sequence = Sequence::create(
        {3}, "sequence", TickDuration{8 * kTicksPerQuarter}, std::nullopt, {}, {},
        {SequenceRegion{{13}, "part b", {0}, {4 * kTicksPerQuarter}},
         SequenceRegion{{14}, "chorus", {kTicksPerQuarter}, {kTicksPerQuarter}},
         SequenceRegion{{15}, "straddles", {2 * kTicksPerQuarter}, {4 * kTicksPerQuarter}}});
    REQUIRE(sequence);
    REQUIRE(sequence.value().regions().size() == 3);
}

TEST_CASE("Sequence rejects malformed markers and regions fail closed") {
    const auto duration = TickDuration{8 * kTicksPerQuarter};

    auto duplicate_marker =
        Sequence::create({3}, "sequence", duration, std::nullopt, {},
                         {SequenceMarker{{10}, "a", {0}}, SequenceMarker{{10}, "b", {kBar}}}, {});
    REQUIRE_FALSE(duplicate_marker);
    REQUIRE(duplicate_marker.error().code == ModelErrorCode::DuplicateItemId);
    REQUIRE(duplicate_marker.error().item == ItemId{10});

    // Markers and regions share one identity space inside the sequence.
    auto duplicate_across =
        Sequence::create({3}, "sequence", duration, std::nullopt, {},
                         {SequenceMarker{{10}, "a", {0}}}, {SequenceRegion{{10}, "b", {0}, {8}}});
    REQUIRE_FALSE(duplicate_across);
    REQUIRE(duplicate_across.error().code == ModelErrorCode::DuplicateItemId);

    auto negative_marker = Sequence::create({3}, "sequence", duration, std::nullopt, {},
                                            {SequenceMarker{{10}, "before zero", {-1}}}, {});
    REQUIRE_FALSE(negative_marker);
    REQUIRE(negative_marker.error().code == ModelErrorCode::InvalidMarker);

    auto past_end =
        Sequence::create({3}, "sequence", duration, std::nullopt, {},
                         {SequenceMarker{{10}, "past end", {8 * kTicksPerQuarter + 1}}}, {});
    REQUIRE_FALSE(past_end);
    REQUIRE(past_end.error().code == ModelErrorCode::InvalidMarker);
    REQUIRE(past_end.error().item == ItemId{10});

    auto region_past_end = Sequence::create(
        {3}, "sequence", duration, std::nullopt, {}, {},
        {SequenceRegion{{13}, "overruns", {7 * kTicksPerQuarter}, {2 * kTicksPerQuarter}}});
    REQUIRE_FALSE(region_past_end);
    REQUIRE(region_past_end.error().code == ModelErrorCode::InvalidRegion);

    auto empty_region = Sequence::create({3}, "sequence", duration, std::nullopt, {}, {},
                                         {SequenceRegion{{13}, "zero length", {0}, {0}}});
    REQUIRE_FALSE(empty_region);
    REQUIRE(empty_region.error().code == ModelErrorCode::InvalidRegion);

    auto zero_id = Sequence::create({3}, "sequence", duration, std::nullopt, {},
                                    {SequenceMarker{{0}, "unnamed identity", {0}}}, {});
    REQUIRE_FALSE(zero_id);
    REQUIRE(zero_id.error().code == ModelErrorCode::InvalidItemId);

    // A marker at the exact end of the sequence is in bounds.
    auto at_end = Sequence::create({3}, "sequence", duration, std::nullopt, {},
                                   {SequenceMarker{{10}, "end", {8 * kTicksPerQuarter}}}, {});
    REQUIRE(at_end);
}

TEST_CASE("Annotation edits leave the prior sequence snapshot untouched") {
    const auto project = annotated_project();
    const auto& original = annotated_sequence(project);
    auto edited = take(original.insert_marker(SequenceMarker{{20}, "intro", {0}}));
    REQUIRE(edited.markers().size() == 1);
    REQUIRE(original.markers().empty());
    REQUIRE_FALSE(edited.shares_storage_with(original));
    REQUIRE(edited.tracks().size() == original.tracks().size());
    REQUIRE(edited.find_track({4}) != nullptr);

    auto reverted = take(edited.erase_marker({20}));
    REQUIRE(reverted.markers().empty());
    REQUIRE(edited.markers().size() == 1);

    auto missing = original.erase_marker({20});
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error().code == ModelErrorCode::MissingItem);

    // An edit that would violate the annotation invariants is rejected and the
    // prior snapshot is unchanged.
    auto rejected = edited.insert_marker(SequenceMarker{{20}, "duplicate", {kBar}});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ModelErrorCode::DuplicateItemId);
    REQUIRE(edited.markers().size() == 1);
}

TEST_CASE("Marker and region commands compare and retain their payloads") {
    const Command inserted = InsertMarker{{3}, SequenceMarker{{10}, "intro", {0}}};
    REQUIRE(equivalent(inserted, Command{InsertMarker{{3}, SequenceMarker{{10}, "intro", {0}}}}));
    REQUIRE_FALSE(
        equivalent(inserted, Command{InsertMarker{{3}, SequenceMarker{{10}, "outro", {0}}}}));
    REQUIRE_FALSE(
        equivalent(inserted, Command{InsertMarker{{3}, SequenceMarker{{10}, "intro", {kBar}}}}));
    REQUIRE(retained_size(inserted) >= sizeof(InsertMarker) + 5);

    const Command region = InsertRegion{{3}, SequenceRegion{{13}, "verse", {0}, {kBar}}};
    REQUIRE(
        equivalent(region, Command{InsertRegion{{3}, SequenceRegion{{13}, "verse", {0}, {kBar}}}}));
    REQUIRE_FALSE(equivalent(
        region, Command{InsertRegion{{3}, SequenceRegion{{13}, "verse", {0}, {kBar / 2}}}}));

    REQUIRE(equivalent(Command{RemoveMarker{{3}, {10}}}, Command{RemoveMarker{{3}, {10}}}));
    REQUIRE_FALSE(equivalent(Command{RemoveMarker{{3}, {10}}}, Command{RemoveMarker{{3}, {11}}}));
    REQUIRE(equivalent(Command{RemoveRegion{{3}, {13}}}, Command{RemoveRegion{{3}, {13}}}));
    REQUIRE_FALSE(equivalent(Command{RemoveRegion{{3}, {13}}}, Command{RemoveRegion{{3}, {14}}}));
}

TEST_CASE("Marker insert undo restores the prior document and tombstones the identity") {
    const auto initial = annotated_project();
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();
    auto committed = session->submit(
        writer, session_transaction(writer, {},
                                    {InsertMarker{{3}, SequenceMarker{{20}, "intro", {kBar}}}}));
    REQUIRE(committed);
    REQUIRE(committed->dirty.items()[0].flags ==
            (DirtyFlags::Structure | DirtyFlags::Marker | DirtyFlags::Added));
    REQUIRE(annotated_sequence(*session->snapshot()).find_marker({20}));

    REQUIRE(session->undo(writer));
    REQUIRE(annotated_sequence(*session->snapshot()).markers().empty());
    REQUIRE_FALSE(session->snapshot()->locate({20})->active);

    REQUIRE(session->redo(writer));
    const auto* restored = annotated_sequence(*session->snapshot()).find_marker({20});
    REQUIRE(restored);
    REQUIRE(restored->name == "intro");
    REQUIRE(restored->position == TickPosition{kBar});

    auto replayed = session->journal().replay(initial, {});
    REQUIRE(replayed);
    REQUIRE(replayed->find_sequence({3})->find_marker({20}));
}

TEST_CASE("Region remove undo restores the exact span") {
    const auto initial = annotated_project({}, {SequenceRegion{{13}, "chorus", {kBar}, {kBar}}});
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();
    auto committed =
        session->submit(writer, session_transaction(writer, {}, {RemoveRegion{{3}, {13}}}));
    REQUIRE(committed);
    REQUIRE(committed->dirty.items()[0].flags ==
            (DirtyFlags::Structure | DirtyFlags::Marker | DirtyFlags::Removed));
    REQUIRE(annotated_sequence(*session->snapshot()).regions().empty());

    REQUIRE(session->undo(writer));
    const auto* restored = annotated_sequence(*session->snapshot()).find_region({13});
    REQUIRE(restored);
    REQUIRE(restored->name == "chorus");
    REQUIRE(restored->position == TickPosition{kBar});
    REQUIRE(restored->duration == TickDuration{kBar});
}

TEST_CASE("Marker commands gate on the target sequence and the target annotation") {
    const auto initial = annotated_project({SequenceMarker{{10}, "intro", {0}}});
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();

    auto wrong_sequence = session->submit(
        writer, session_transaction(writer, {},
                                    {InsertMarker{{4}, SequenceMarker{{20}, "on a track", {0}}}}));
    REQUIRE_FALSE(wrong_sequence);
    REQUIRE(wrong_sequence.error().code == ConflictCode::WrongTargetKind);

    auto missing = session->submit(
        writer, session_transaction(writer, session->revision(), {RemoveMarker{{3}, {19}}}));
    REQUIRE_FALSE(missing);

    // A region id cannot be removed through the marker command and vice versa.
    auto wrong_kind = session->submit(
        writer, session_transaction(writer, session->revision(), {RemoveRegion{{3}, {10}}}));
    REQUIRE_FALSE(wrong_kind);
    REQUIRE(wrong_kind.error().code == ConflictCode::WrongTargetKind);

    // An out-of-bounds marker is rejected by the model, not silently clamped.
    auto out_of_bounds = session->submit(
        writer,
        session_transaction(
            writer, session->revision(),
            {InsertMarker{{3}, SequenceMarker{{20}, "past end", {8 * kTicksPerQuarter + 1}}}}));
    REQUIRE_FALSE(out_of_bounds);
    REQUIRE(out_of_bounds.error().model_error);
    REQUIRE(out_of_bounds.error().model_error->code == ModelErrorCode::InvalidMarker);
    REQUIRE_FALSE(session->snapshot()->locate({20}));
}
