#include <pulp/timeline/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <utility>

using namespace pulp::timeline;

namespace {

template <typename T> T take(pulp::runtime::Result<T, ModelError> result) {
    REQUIRE(result.has_value());
    return std::move(result).value();
}

Clip make_clip(ItemId id, std::int64_t start = 0) {
    return take(Clip::create(id, {start}, {10}, EmptyContent{}));
}

Track make_track() {
    return take(Track::create(TrackInput{.id = {10},
                                         .name = "track",
                                         .clips = {make_clip({30})},
                                         .device_chain = {{{21}}, {{20}}}}));
}

} // namespace

TEST_CASE("Timeline tracks own an ordered device placement chain") {
    const auto track = make_track();
    REQUIRE(track.device_chain().size() == 2);
    REQUIRE(track.device_chain()[0].id == ItemId{21});
    REQUIRE(track.device_chain()[1].id == ItemId{20});
    REQUIRE(track.find_device_placement({20}) == &track.device_chain()[1]);
    REQUIRE(track.find_device_placement({99}) == nullptr);

    auto invalid = Track::create(TrackInput{.id = {10}, .name = "track", .device_chain = {{{0}}}});
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error().code == ModelErrorCode::InvalidItemId);

    auto duplicate =
        Track::create(TrackInput{.id = {10}, .name = "track", .device_chain = {{{20}}, {{20}}}});
    REQUIRE_FALSE(duplicate.has_value());
    REQUIRE(duplicate.error().code == ModelErrorCode::DuplicateItemId);
    REQUIRE(duplicate.error().item == ItemId{20});
}

TEST_CASE("Timeline clip edits retain device chain storage") {
    const auto original = make_track();
    const auto* storage = original.device_chain().data();

    const auto replaced = take(original.replace_clip(make_clip({30}, 20)));
    const auto inserted = take(original.insert_clip(make_clip({31}, 40)));
    const auto erased = take(original.erase_clip({30}));

    REQUIRE(replaced.device_chain().data() == storage);
    REQUIRE(inserted.device_chain().data() == storage);
    REQUIRE(erased.device_chain().data() == storage);
    REQUIRE(original.find_clip({30})->start().value == 0);
}

TEST_CASE("Timeline projects register device placement ownership globally") {
    const auto track = make_track();
    const auto sequence =
        take(Sequence::create({3}, "sequence", pulp::timebase::TickDuration{100}, {track}));
    const auto project =
        take(Project::create(ProjectInput{{1}, "project", 31, {3}, {}, {sequence}}));

    REQUIRE(project.next_item_id() == 31);
    const auto location = project.locate({21});
    REQUIRE(location.has_value());
    REQUIRE(location->kind == ItemKind::DevicePlacement);
    REQUIRE(location->sequence_id == ItemId{3});
    REQUIRE(location->track_id == ItemId{10});
    REQUIRE_FALSE(location->clip_id.valid());

    auto collision = Track::create(TrackInput{
        .id = {10}, .name = "track", .clips = {make_clip({20})}, .device_chain = {{{20}}}});
    REQUIRE_FALSE(collision.has_value());
    REQUIRE(collision.error().code == ModelErrorCode::DuplicateItemId);
    REQUIRE(collision.error().item == ItemId{20});
}

TEST_CASE("Timeline remap treats device placements as owned identities") {
    const auto original = make_track();
    ItemIdAllocator allocator(100);
    const auto remapped = take(remap_ids(original, allocator));

    REQUIRE(remapped.ids.entries().size() == 4);
    REQUIRE(remapped.track.id() == *remapped.ids.find({10}));
    REQUIRE(remapped.track.device_chain()[0].id == *remapped.ids.find({21}));
    REQUIRE(remapped.track.device_chain()[1].id == *remapped.ids.find({20}));
    REQUIRE(remapped.track.find_clip(*remapped.ids.find({30})) != nullptr);
    REQUIRE(allocator.next_value() == 104);

    const auto sequence =
        take(Sequence::create({3}, "sequence", pulp::timebase::TickDuration{100}, {original}));
    const auto project =
        take(Project::create(ProjectInput{{1}, "project", 31, {3}, {}, {sequence}}));
    const auto remapped_project = take(remap_ids(project, 200));
    REQUIRE(remapped_project.ids.entries().size() == 6);
    const auto* project_track =
        remapped_project.project.sequences()[0].find_track(*remapped_project.ids.find({10}));
    REQUIRE(project_track != nullptr);
    REQUIRE(project_track->device_chain()[0].id == *remapped_project.ids.find({21}));
    REQUIRE(project_track->device_chain()[1].id == *remapped_project.ids.find({20}));
    REQUIRE(remapped_project.project.locate(*remapped_project.ids.find({21}))->kind ==
            ItemKind::DevicePlacement);

    ItemIdAllocator exhausted(std::numeric_limits<std::uint64_t>::max() - 2);
    const auto before = exhausted.next_value();
    auto failed = remap_ids(original, exhausted);
    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error().code == ModelErrorCode::ItemIdExhausted);
    REQUIRE(exhausted.next_value() == before);
}
