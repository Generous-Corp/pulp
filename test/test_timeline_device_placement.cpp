#include <pulp/timeline/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <limits>
#include <string>
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
    DevicePlacement first{
        .id = {21},
        .configuration = {.position = DeviceChainPosition::PreFader,
                          .slot_kind = DeviceSlotKind::AudioToAudio,
                          .device_kind = DeviceKind::BuiltIn,
                          .binding_key = "pulp.test.first"},
        .state_ref = *ContentHash::from_hex(std::string(64, 'a')),
    };
    DevicePlacement second{
        .id = {20},
        .configuration = {.position = DeviceChainPosition::PreFader,
                          .slot_kind = DeviceSlotKind::AudioToAudio,
                          .device_kind = DeviceKind::External,
                          .binding_key = "com.example.second",
                          .bypassed = true,
                          .wet_dry_bits = std::bit_cast<std::uint32_t>(0.5f)},
    };
    return take(Track::create(TrackInput{
        .id = {10},
        .name = "track",
        .clips = {make_clip({30})},
        .device_chain = {std::move(first), std::move(second)},
    }));
}

DevicePlacement device(ItemId id, DeviceSlotKind slot, DeviceChainPosition position,
                       std::string binding = "pulp.test.device") {
    return DevicePlacement{
        .id = id,
        .configuration = {.position = position,
                          .slot_kind = slot,
                          .device_kind = DeviceKind::BuiltIn,
                          .binding_key = std::move(binding)},
    };
}

ContentHash state_hash(char digit) {
    return *ContentHash::from_hex(std::string(64, digit));
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

TEST_CASE("Timeline typed device chains reject malformed declarations and domain crossings") {
    const auto valid = Track::create(TrackInput{
        .id = {10},
        .name = "typed",
        .device_chain = {device({20}, DeviceSlotKind::EventToEvent,
                                DeviceChainPosition::PreFader),
                         device({21}, DeviceSlotKind::EventToAudio,
                                DeviceChainPosition::PreFader),
                         device({22}, DeviceSlotKind::AudioToAudio,
                                DeviceChainPosition::PostFader)},
    });
    REQUIRE(valid);

    auto direct_audio = Track::create(TrackInput{
        .id = {10},
        .name = "invalid",
        .device_chain = {device({20}, DeviceSlotKind::EventToEvent,
                                DeviceChainPosition::PreFader),
                         device({21}, DeviceSlotKind::AudioToAudio,
                                DeviceChainPosition::PreFader)},
    });
    REQUIRE_FALSE(direct_audio);
    REQUIRE(direct_audio.error().code == ModelErrorCode::InvalidIdentityTransition);
    REQUIRE(direct_audio.error().item == ItemId{21});

    auto post_event = Track::create(TrackInput{
        .id = {10},
        .name = "invalid",
        .device_chain = {device({20}, DeviceSlotKind::EventToAudio,
                                DeviceChainPosition::PostFader)},
    });
    REQUIRE_FALSE(post_event);
    REQUIRE(post_event.error().code == ModelErrorCode::InvalidIdentityTransition);

    auto malformed = device({20}, DeviceSlotKind::AudioToAudio,
                            DeviceChainPosition::PreFader);
    malformed.configuration.device_kind = DeviceKind::Unresolved;
    REQUIRE_FALSE(Track::create(TrackInput{
        .id = {10}, .name = "invalid", .device_chain = {malformed}}));
    malformed.configuration.device_kind = DeviceKind::BuiltIn;
    malformed.configuration.wet_dry_bits = std::bit_cast<std::uint32_t>(
        std::numeric_limits<float>::quiet_NaN());
    REQUIRE_FALSE(Track::create(TrackInput{
        .id = {10}, .name = "invalid", .device_chain = {malformed}}));
}

TEST_CASE("Timeline device chain operations preserve authored order and validate replacements") {
    auto original = take(Track::create(TrackInput{
        .id = {10},
        .name = "typed",
        .device_chain = {device({20}, DeviceSlotKind::EventToAudio,
                                DeviceChainPosition::PreFader),
                         device({22}, DeviceSlotKind::AudioToAudio,
                                DeviceChainPosition::PreFader)},
    }));
    auto inserted = take(original.insert_device(
        device({21}, DeviceSlotKind::AudioToAudio, DeviceChainPosition::PreFader),
        std::optional<ItemId>{ItemId{22}}));
    REQUIRE(inserted.device_chain()[0].id == ItemId{20});
    REQUIRE(inserted.device_chain()[1].id == ItemId{21});
    REQUIRE(inserted.device_chain()[2].id == ItemId{22});

    auto moved = take(inserted.move_device({21}, std::nullopt));
    REQUIRE(moved.device_chain()[1].id == ItemId{22});
    REQUIRE(moved.device_chain()[2].id == ItemId{21});
    auto replacement = moved.device_chain()[2];
    replacement.configuration.position = DeviceChainPosition::PostFader;
    replacement.state_ref = state_hash('a');
    auto replaced = take(moved.replace_device(replacement));
    REQUIRE(replaced.device_chain()[2] == replacement);
    auto erased = take(replaced.erase_device({22}));
    REQUIRE(erased.device_chain().size() == 2);
    REQUIRE(erased.device_chain()[1] == replacement);
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
    REQUIRE(location->parent_id == ItemId{10});
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
    REQUIRE(remapped.track.device_chain()[0].configuration ==
            original.device_chain()[0].configuration);
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
    REQUIRE(project_track->device_chain()[0].state_ref == original.device_chain()[0].state_ref);
    REQUIRE(remapped_project.project.locate(*remapped_project.ids.find({21}))->kind ==
            ItemKind::DevicePlacement);
    REQUIRE(remapped_project.project.locate(*remapped_project.ids.find({21}))->parent_id ==
            *remapped_project.ids.find({10}));

    ItemIdAllocator exhausted(std::numeric_limits<std::uint64_t>::max() - 2);
    const auto before = exhausted.next_value();
    auto failed = remap_ids(original, exhausted);
    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error().code == ModelErrorCode::ItemIdExhausted);
    REQUIRE(exhausted.next_value() == before);
}
