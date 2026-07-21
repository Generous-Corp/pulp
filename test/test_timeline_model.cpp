#include "../core/timeline/src/identity_directory.hpp"
#include "../core/timeline/src/identity_transition.hpp"
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <type_traits>
#include <vector>

using namespace pulp::timeline;
namespace runtime = pulp::runtime;

TEST_CASE("Timeline private identity equality is semantic across insertion histories") {
    pulp::timeline::detail::IdentityDirectory ascending;
    pulp::timeline::detail::IdentityDirectory interleaved;
    const auto location = [](std::uint64_t id) {
        return ItemLocation{ItemKind::Clip, {11}, {10}, {11}, {id}, true};
    };
    for (std::uint64_t id = 1; id <= 7; ++id)
        REQUIRE(ascending.insert({id}, location(id)));
    for (const std::uint64_t id : {4, 2, 6, 1, 3, 5, 7})
        REQUIRE(interleaved.insert({id}, location(id)));
    REQUIRE(ascending.equivalent(interleaved));
    REQUIRE(interleaved.replace({7},
                                {ItemKind::Clip, {11}, {10}, {11}, {7}, false}));
    REQUIRE_FALSE(ascending.equivalent(interleaved));
}

TEST_CASE("Timeline item ownership is kind plus immediate parent") {
    STATIC_REQUIRE_FALSE(std::is_aggregate_v<ItemLocation>);
    STATIC_REQUIRE_FALSE(
        std::is_constructible_v<ItemLocation, ItemKind, ItemId, ItemId, ItemId, bool>);

    const ItemLocation first{ItemKind::Note, {50}, {10}, {20}, {50}, true};
    const ItemLocation same_owner{ItemKind::Note, {50}, {11}, {21}, {50}, false};
    const ItemLocation other_parent{ItemKind::Note, {51}, {10}, {20}, {50}, true};
    const ItemLocation other_kind{ItemKind::Clip, {50}, {10}, {20}, {50}, true};

    REQUIRE(first.has_same_owner(same_owner));
    REQUIRE_FALSE(first.has_same_owner(other_parent));
    REQUIRE_FALSE(first.has_same_owner(other_kind));

    REQUIRE(immediate_parent_id(ItemKind::Project, {1}, {2}, {3}, {4}) == ItemId{});
    REQUIRE(immediate_parent_id(ItemKind::Asset, {1}, {2}, {3}, {4}) == ItemId{1});
    REQUIRE(immediate_parent_id(ItemKind::Sequence, {1}, {2}, {3}, {4}) == ItemId{1});
    REQUIRE(immediate_parent_id(ItemKind::Track, {1}, {2}, {3}, {4}) == ItemId{2});
    REQUIRE(immediate_parent_id(ItemKind::Clip, {1}, {2}, {3}, {4}) == ItemId{3});
    REQUIRE(immediate_parent_id(ItemKind::Note, {1}, {2}, {3}, {4}) == ItemId{4});
    REQUIRE(immediate_parent_id(ItemKind::DevicePlacement, {1}, {2}, {3}, {4}) == ItemId{3});
}
using namespace pulp::timebase;

namespace {

runtime::Result<std::shared_ptr<const void>, PersistenceError>
decode_test_int(const JsonValue&, const void*) noexcept {
    std::shared_ptr<const void> value = std::make_shared<const int>(42);
    return runtime::Ok(std::move(value));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
encode_test_int(const std::shared_ptr<const void>& value, BoundedJsonSink& output,
                const void*) noexcept {
    output.append("{\"value\":");
    output.append(std::to_string(*static_cast<const int*>(value.get())));
    output.append("}");
    return runtime::Ok(SchemaWriteSuccess{});
}

std::size_t retained_test_int(const std::shared_ptr<const void>&, const void*) noexcept {
    return sizeof(int);
}

SchemaRegistry test_int_registry() {
    SchemaRegistryBuilder builder;
    TypeSchema schema;
    schema.type_name = "vendor.timeline.generator";
    schema.domain = SchemaDomain::Content;
    schema.current_version = 1;
    schema.fields = {{"value", SchemaValueKind::U32}};
    schema.codec = {{}, decode_test_int, encode_test_int, retained_test_int};
    REQUIRE(builder.register_type(std::move(schema)));
    auto registry = std::move(builder).build();
    REQUIRE(registry);
    return std::move(registry).value();
}

ContentHash content_hash(char digit = 'a') {
    return *ContentHash::from_hex(std::string(64, digit));
}

template <typename T> T take_value(pulp::runtime::Result<T, ModelError> result) {
    REQUIRE(result.has_value());
    return std::move(result).value();
}

NoteContent notes(std::vector<NoteEvent> events) {
    return take_value(NoteContent::create(std::move(events)));
}

Clip clip(ItemId id, std::int64_t start, std::int64_t duration,
          ClipContent content = EmptyContent{}) {
    return take_value(Clip::create(id, {start}, {duration}, std::move(content)));
}

Clip absolute_clip(ItemId id, std::int64_t start_sample, std::uint64_t sample_count,
                   ClipContent content = EmptyContent{}, RationalRate rate = {48'000, 1}) {
    return take_value(
        Clip::create_absolute(id, {start_sample}, sample_count, rate, std::move(content)));
}

Project make_project() {
    auto note_clip = clip(
        {5}, 200, 100, notes({{{8}, {20}, {10}, 0x8000, 64, 1}, {{7}, {10}, {10}, 0xffff, 60, 0}}));
    auto media_clip = clip({4}, 0, 100, MediaRef{{2}, {25}, 100});
    auto track = take_value(Track::create({6}, "track", {note_clip, media_clip}));
    auto sequence = take_value(Sequence::create({3}, "sequence", TickDuration{400}, {track}));
    return take_value(Project::create(ProjectInput{{1},
                                                   "project",
                                                   9,
                                                   {3},
                                                   {{{2},
                                                     "audio.wav",
                                                     1'000,
                                                     {48'000, 1},
                                                     content_hash(),
                                                     AssetStoragePolicy::External,
                                                     {},
                                                     {}}},
                                                   {sequence}}));
}

} // namespace

TEST_CASE("Timeline reactivation policy refreshes ancestor navigation caches") {
    const ItemLocation tombstone{ItemKind::Note, {5}, {2}, {3}, {5}, false};
    const ItemLocation requested{ItemKind::Note, {5}, {2}, {4}, {5}, false};

    const auto reactivated = pulp::timeline::detail::reactivated_location(tombstone, requested);
    REQUIRE(reactivated == ItemLocation{ItemKind::Note, {5}, {2}, {4}, {5}, true});

    REQUIRE_FALSE(pulp::timeline::detail::reactivated_location(
        ItemLocation{ItemKind::Note, {6}, {2}, {3}, {6}, false}, requested));
    REQUIRE_FALSE(pulp::timeline::detail::reactivated_location(
        ItemLocation{ItemKind::Note, {5}, {2}, {3}, {5}, true}, requested));
}

TEST_CASE("Timeline snapshots retain sorted indexes and immutable note content") {
    const auto project = make_project();
    REQUIRE(project.id() == ItemId{1});
    REQUIRE(project.next_item_id() == 9);
    REQUIRE(project.find_asset({2}) != nullptr);

    const auto* sequence = project.find_sequence({3});
    REQUIRE(sequence != nullptr);
    const auto* track = sequence->find_track({6});
    REQUIRE(track != nullptr);
    REQUIRE(track->clips().size() == 2);
    REQUIRE(track->clips()[0].id() == ItemId{4});
    REQUIRE(track->clips()[1].id() == ItemId{5});
    REQUIRE(track->find_clip({5}) == &track->clips()[1]);
    REQUIRE(track->find_clip({99}) == nullptr);

    const auto& note_content = std::get<NoteContent>(track->find_clip({5})->content());
    REQUIRE(note_content.notes().size() == 2);
    REQUIRE(note_content.notes()[0].id == ItemId{7});
    REQUIRE(note_content.notes()[1].id == ItemId{8});

    const auto snapshot_copy = project;
    auto allocator = project.item_id_allocator();
    REQUIRE(take_value(allocator.allocate()) == ItemId{9});
    REQUIRE(snapshot_copy.next_item_id() == 9);
}

TEST_CASE("Timeline construction rejects invalid ranges identities and references") {
    auto overlap_a = clip({10}, 0, 100);
    auto overlap_b = clip({11}, 99, 10);
    auto overlap = Track::create({12}, "overlap", {overlap_a, overlap_b});
    REQUIRE_FALSE(overlap.has_value());
    REQUIRE(overlap.error().code == ModelErrorCode::OverlappingClips);

    auto bad_note = NoteContent::create({{{1}, {0}, {1}, 0xffff, 128, 0}});
    REQUIRE_FALSE(bad_note.has_value());
    REQUIRE(bad_note.error().code == ModelErrorCode::InvalidNote);

    auto missing_media = clip({4}, 0, 10, MediaRef{{99}, {0}, 10});
    auto track = take_value(Track::create({5}, "track", {missing_media}));
    auto sequence = take_value(Sequence::create({3}, "sequence", TickDuration{100}, {track}));
    auto missing = Project::create(ProjectInput{{1}, "project", 100, {3}, {}, {sequence}});
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error().code == ModelErrorCode::MissingAsset);

    auto out_of_asset = clip({4}, 0, 10, MediaRef{{2}, {95}, 10});
    auto range_track = take_value(Track::create({5}, "track", {out_of_asset}));
    auto range_sequence =
        take_value(Sequence::create({3}, "sequence", TickDuration{100}, {range_track}));
    auto invalid_range = Project::create(ProjectInput{{1},
                                                      "project",
                                                      6,
                                                      {3},
                                                      {{{2},
                                                        "short.wav",
                                                        100,
                                                        {48'000, 1},
                                                        content_hash(),
                                                        AssetStoragePolicy::External,
                                                        {},
                                                        {}}},
                                                      {range_sequence}});
    REQUIRE_FALSE(invalid_range.has_value());
    REQUIRE(invalid_range.error().code == ModelErrorCode::InvalidMediaRange);

    auto duplicate_track = take_value(Track::create({1}, "track", {}));
    auto duplicate_sequence =
        take_value(Sequence::create({3}, "sequence", TickDuration{100}, {duplicate_track}));
    auto duplicate =
        Project::create(ProjectInput{{1}, "project", 4, {3}, {}, {duplicate_sequence}});
    REQUIRE_FALSE(duplicate.has_value());
    REQUIRE(duplicate.error().code == ModelErrorCode::DuplicateItemId);

    auto nonmonotonic = Project::create(
        ProjectInput{{1},
                     "project",
                     3,
                     {3},
                     {},
                     {take_value(Sequence::create({3}, "sequence", TickDuration{0}, {}))}});
    REQUIRE_FALSE(nonmonotonic.has_value());
    REQUIRE(nonmonotonic.error().code == ModelErrorCode::NextItemIdNotMonotonic);
}

TEST_CASE("Timeline assets separate content identity from resolution hints") {
    MediaAsset asset{{2},
                     "audio",
                     1'000,
                     {48'000, 1},
                     content_hash('b'),
                     AssetStoragePolicy::PreferEmbedded,
                     {{AssetLocatorKind::ExternalUri, "file:///music/audio.wav"}},
                     {{"proxy",
                       content_hash('c'),
                       AssetStoragePolicy::Embedded,
                       {{AssetLocatorKind::PackageRelative, "media/proxy.wav"}}}}};
    auto sequence = take_value(Sequence::create({3}, "sequence", TickDuration{100}, {}));
    auto project = Project::create(ProjectInput{{1}, "project", 4, {3}, {asset}, {sequence}});
    REQUIRE(project.has_value());
    REQUIRE(project.value().assets()[0].content_hash == content_hash('b'));
    REQUIRE(project.value().assets()[0].representations[0].role == "proxy");

    asset.locators[0].hint.clear();
    auto bad_locator = Project::create(ProjectInput{{1}, "project", 4, {3}, {asset}, {sequence}});
    REQUIRE_FALSE(bad_locator.has_value());
    REQUIRE(bad_locator.error().code == ModelErrorCode::InvalidAssetLocator);

    asset.locators = {};
    asset.representations.push_back(asset.representations.front());
    auto duplicate = Project::create(ProjectInput{{1}, "project", 4, {3}, {asset}, {sequence}});
    REQUIRE_FALSE(duplicate.has_value());
    REQUIRE(duplicate.error().code == ModelErrorCode::DuplicateAssetRepresentation);
}

TEST_CASE("Timeline registered content remaps while opaque content fails closed") {
    const auto registry = test_int_registry();
    auto created = registry.create_registered_no_owned_ids({"vendor.timeline.generator", 1},
                                                           std::make_shared<const int>(42), 1024);
    REQUIRE(created);
    const auto registered = std::move(created).value();
    const auto registered_clip = clip({10}, 0, 100, registered);
    ItemIdAllocator allocator(100);
    auto registered_remap = remap_ids(registered_clip, allocator);
    REQUIRE(registered_remap.has_value());
    const auto& remapped_registered =
        std::get<RegisteredContent>(registered_remap.value().clip.content());
    REQUIRE(*remapped_registered.value_as<int>() == 42);
    REQUIRE(allocator.next_value() == 101);

    const auto opaque = take_value(OpaqueContent::create(
        {"vendor.timeline.future", 7},
        R"({"data":{"owned_id":"999"},"type_name":"vendor.timeline.future","version":7})"));
    const auto opaque_clip = clip({11}, 0, 100, opaque);
    const auto before = allocator.next_value();
    auto opaque_remap = remap_ids(opaque_clip, allocator);
    REQUIRE_FALSE(opaque_remap.has_value());
    REQUIRE(opaque_remap.error().code == ModelErrorCode::OpaqueContentCannotRemap);
    REQUIRE(allocator.next_value() == before);

    const auto opaque_track = take_value(Track::create({12}, "opaque", {opaque_clip}));
    ItemIdAllocator track_allocator(std::numeric_limits<std::uint64_t>::max() - 1);
    const auto track_before = track_allocator.next_value();
    auto track_remap = remap_ids(opaque_track, track_allocator);
    REQUIRE_FALSE(track_remap.has_value());
    REQUIRE(track_remap.error().code == ModelErrorCode::OpaqueContentCannotRemap);
    REQUIRE(track_allocator.next_value() == track_before);

    const auto opaque_sequence =
        take_value(Sequence::create({13}, "opaque", TickDuration{100}, {opaque_track}));
    ItemIdAllocator sequence_allocator(500);
    const auto sequence_before = sequence_allocator.next_value();
    auto sequence_remap = remap_ids(opaque_sequence, sequence_allocator);
    REQUIRE_FALSE(sequence_remap.has_value());
    REQUIRE(sequence_remap.error().code == ModelErrorCode::OpaqueContentCannotRemap);
    REQUIRE(sequence_allocator.next_value() == sequence_before);
}

TEST_CASE("Timeline opaque content validates and retains its exact admission boundary") {
    const SchemaIdentity identity{"vendor.timeline.future", 7};
    const std::string raw =
        R"({"data":{"text":"future"},"type_name":"vendor.timeline.future","version":7})";
    OpaqueContentLimits limits;
    limits.max_input_bytes = raw.size();
    limits.max_opaque_bytes = raw.size();
    limits.max_depth = 4;
    limits.max_total_values = 8;
    limits.max_array_elements = 2;
    limits.max_object_members = 3;
    limits.max_string_bytes = 64;
    auto accepted = OpaqueContent::create(identity, raw, limits);
    REQUIRE(accepted.has_value());
    REQUIRE(accepted.value().raw_json() == raw);
    REQUIRE(accepted.value().validation_limits() == limits);

    auto too_small = limits;
    --too_small.max_input_bytes;
    auto bounded = OpaqueContent::create(identity, raw, too_small);
    REQUIRE_FALSE(bounded.has_value());
    REQUIRE(bounded.error().code == ModelErrorCode::OpaqueContentLimitExceeded);

    REQUIRE_FALSE(
        OpaqueContent::create(
            identity, R"({"data":{},"extra":0,"type_name":"vendor.timeline.future","version":7})")
            .has_value());
    REQUIRE_FALSE(
        OpaqueContent::create(identity, R"({"type_name":"vendor.timeline.future","version":7})")
            .has_value());
    REQUIRE_FALSE(OpaqueContent::create(
                      identity, R"({"data":0,"type_name":"vendor.timeline.future","version":7})")
                      .has_value());
    REQUIRE_FALSE(OpaqueContent::create(
                      identity, R"({"data":{},"type_name":"vendor.timeline.other","version":7})")
                      .has_value());
    REQUIRE_FALSE(OpaqueContent::create(
                      identity, R"({"data":{},"type_name":"vendor.timeline.future","version":8})")
                      .has_value());
    REQUIRE_FALSE(
        OpaqueContent::create(
            identity, R"({"data":{},"data":{},"type_name":"vendor.timeline.future","version":7})")
            .has_value());
    std::string invalid_utf8 = R"({"data":{"text":")";
    invalid_utf8.push_back(static_cast<char>(0xc0));
    invalid_utf8 += R"("},"type_name":"vendor.timeline.future","version":7})";
    REQUIRE_FALSE(OpaqueContent::create(identity, std::move(invalid_utf8)).has_value());
}

TEST_CASE("Timeline ID allocation is monotonic and fails closed at exhaustion") {
    ItemIdAllocator allocator(41);
    REQUIRE(take_value(allocator.allocate()) == ItemId{41});
    REQUIRE(take_value(allocator.allocate()) == ItemId{42});
    REQUIRE(allocator.next_value() == 43);

    ItemIdAllocator exhausted(std::numeric_limits<std::uint64_t>::max());
    auto result = exhausted.allocate();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == ModelErrorCode::ItemIdExhausted);

    ItemIdAllocator last(std::numeric_limits<std::uint64_t>::max() - 1);
    REQUIRE(take_value(last.allocate()) == ItemId{std::numeric_limits<std::uint64_t>::max() - 1});
    REQUIRE_FALSE(last.allocate().has_value());
    REQUIRE_FALSE(ItemId{std::numeric_limits<std::uint64_t>::max()}.valid());

    const ItemId terminal_id{std::numeric_limits<std::uint64_t>::max() - 1};
    auto max_next = Project::create(
        ProjectInput{{1},
                     "project",
                     std::numeric_limits<std::uint64_t>::max(),
                     terminal_id,
                     {},
                     {take_value(Sequence::create(terminal_id, "sequence", TickDuration{0}, {}))}});
    REQUIRE(max_next.has_value());
    REQUIRE_FALSE(max_next.value().item_id_allocator().allocate().has_value());
}

TEST_CASE("Timeline anchors model tempo-following and fixed absolute ranges") {
    const auto musical = clip({10}, 0, 100);
    const auto absolute = absolute_clip({11}, 0, 48'000, EmptyContent{}, {96'000, 2});
    REQUIRE(musical.time_anchor() == ClipTimeAnchor::Musical);
    REQUIRE(absolute.time_anchor() == ClipTimeAnchor::Absolute);
    REQUIRE(absolute.absolute_duration_samples() == 48'000);
    REQUIRE(absolute.absolute_sample_rate() == RationalRate{48'000, 1});

    // A Track cannot compare positions from different clock domains without a
    // context-owned tempo/rate projection, so the model rejects the mixture.
    auto mixed = Track::create({12}, "mixed", {absolute, musical});
    REQUIRE_FALSE(mixed.has_value());
    REQUIRE(mixed.error().code == ModelErrorCode::MixedTimeAnchors);

    auto absolute_overlap =
        Track::create({13}, "bad", {absolute_clip({14}, 0, 100), absolute_clip({15}, 99, 100)});
    REQUIRE_FALSE(absolute_overlap.has_value());
    REQUIRE(absolute_overlap.error().code == ModelErrorCode::OverlappingClips);

    auto incompatible_rates = Track::create(
        {13}, "rates",
        {absolute_clip({14}, 0, 100), absolute_clip({15}, 200, 100, EmptyContent{}, {44'100, 1})});
    REQUIRE_FALSE(incompatible_rates.has_value());
    REQUIRE(incompatible_rates.error().code == ModelErrorCode::IncompatibleSampleRate);

    auto absolute_track = take_value(Track::create({12}, "absolute", {absolute}));
    auto bounded =
        Sequence::create({16}, "bounded", TickDuration{100},
                         AbsoluteTimelineDuration{47'999, {48'000, 1}}, {absolute_track});
    REQUIRE_FALSE(bounded.has_value());
    REQUIRE(bounded.error().code == ModelErrorCode::InvalidDuration);

    auto musical_track = take_value(Track::create({13}, "musical", {musical}));
    auto valid = Sequence::create({16}, "bounded", TickDuration{100},
                                  AbsoluteTimelineDuration{48'000, {96'000, 2}},
                                  {absolute_track, musical_track});
    REQUIRE(valid.has_value());
    REQUIRE(valid.value().duration()->value == 100);
    REQUIRE(valid.value().absolute_duration()->sample_count == 48'000);
    REQUIRE(valid.value().absolute_duration()->sample_rate == RationalRate{48'000, 1});
}

TEST_CASE("Timeline clip edits path-copy bounded nodes and reclaim snapshots") {
    const auto before = Track::index_stats();
    {
        std::vector<Clip> clips;
        clips.reserve(10'000);
        for (std::uint64_t index = 0; index < 10'000; ++index)
            clips.push_back(clip({index + 1}, static_cast<std::int64_t>(index * 2), 1));
        const auto original = take_value(Track::create({20'000}, "large", std::move(clips)));
        const auto after_build = Track::index_stats();
        REQUIRE(after_build.live_nodes - before.live_nodes == 20'000);

        const auto edited = take_value(original.replace_clip(clip({5'001}, 20'001, 1)));
        const auto after_edit = Track::index_stats();
        REQUIRE(after_edit.nodes_created - after_build.nodes_created < 256);
        REQUIRE(original.find_clip({5'001})->start().value == 10'000);
        REQUIRE(edited.find_clip({5'001})->start().value == 20'001);
        REQUIRE(original.shared_index_nodes_with(edited) > 19'800);
        REQUIRE(edited.clips().size() == 10'000);
    }
    REQUIRE(Track::index_stats().live_nodes == before.live_nodes);
}

TEST_CASE("Timeline subtree remap is two-pass atomic and fixes external references") {
    const auto source = clip({10}, 0, 100, MediaRef{{77}, {0}, 100});
    struct FixupState {
        bool fail = false;
    } state;
    ExternalIdFixup fixup{
        &state, [](void* raw, ItemId id) noexcept -> pulp::runtime::Result<ItemId, ModelError> {
            const auto* state = static_cast<FixupState*>(raw);
            if (state->fail)
                return pulp::runtime::Result<ItemId, ModelError>(
                    pulp::runtime::Err(ModelError{ModelErrorCode::MissingAsset, {}, id}));
            return pulp::runtime::Result<ItemId, ModelError>(
                pulp::runtime::Ok(ItemId{id.value + 1}));
        }};

    ItemIdAllocator allocator(100);
    auto remapped_clip = remap_ids(source, allocator, fixup);
    REQUIRE(remapped_clip.has_value());
    REQUIRE(remapped_clip.value().clip.id() == ItemId{100});
    REQUIRE(std::get<MediaRef>(remapped_clip.value().clip.content()).asset_id == ItemId{78});
    REQUIRE(allocator.next_value() == 101);

    const auto track = take_value(Track::create({20}, "track", {source}));
    auto remapped_track = remap_ids(track, allocator, fixup);
    REQUIRE(remapped_track.has_value());
    REQUIRE(remapped_track.value().ids.entries().size() == 2);
    REQUIRE(allocator.next_value() == 103);

    const auto sequence =
        take_value(Sequence::create({30}, "sequence", TickDuration{100}, {track}));
    auto remapped_sequence = remap_ids(sequence, allocator, fixup);
    REQUIRE(remapped_sequence.has_value());
    REQUIRE(remapped_sequence.value().ids.entries().size() == 3);
    REQUIRE(allocator.next_value() == 106);

    state.fail = true;
    const auto before_failure = allocator.next_value();
    auto failed = remap_ids(source, allocator, fixup);
    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error().code == ModelErrorCode::MissingAsset);
    REQUIRE(allocator.next_value() == before_failure);
}

TEST_CASE("Timeline subtree remap preflights closure-wide identity uniqueness") {
    ItemIdAllocator allocator(100);

    const auto colliding_clip = clip({10}, 0, 100, notes({{{10}, {0}, {10}, 0xffff, 60, 0}}));
    auto clip_result = remap_ids(colliding_clip, allocator);
    REQUIRE_FALSE(clip_result.has_value());
    REQUIRE(clip_result.error().code == ModelErrorCode::DuplicateItemId);
    REQUIRE(allocator.next_value() == 100);

    auto track_result = Track::create({20}, "track", {clip({20}, 0, 10)});
    REQUIRE_FALSE(track_result.has_value());
    REQUIRE(track_result.error().code == ModelErrorCode::DuplicateItemId);
    REQUIRE(allocator.next_value() == 100);

    const auto track_a = take_value(Track::create({30}, "a", {clip({40}, 0, 10)}));
    const auto track_b = take_value(Track::create({31}, "b", {clip({40}, 20, 10)}));
    const auto colliding_sequence =
        take_value(Sequence::create({32}, "sequence", TickDuration{100}, {track_a, track_b}));
    auto sequence_result = remap_ids(colliding_sequence, allocator);
    REQUIRE_FALSE(sequence_result.has_value());
    REQUIRE(sequence_result.error().code == ModelErrorCode::DuplicateItemId);
    REQUIRE(allocator.next_value() == 100);
}

TEST_CASE("Timeline remap allocates first then fixes internal references") {
    const auto original = make_project();
    const auto remapped = take_value(remap_ids(original, 100));
    REQUIRE(remapped.ids.entries().size() == 8);
    REQUIRE(remapped.project.id() == *remapped.ids.find({1}));
    REQUIRE(remapped.project.root_sequence_id() == *remapped.ids.find({3}));
    REQUIRE(remapped.project.next_item_id() == 108);

    const auto* sequence = remapped.project.find_sequence(*remapped.ids.find({3}));
    REQUIRE(sequence != nullptr);
    const auto* track = sequence->find_track(*remapped.ids.find({6}));
    REQUIRE(track != nullptr);
    const auto* media_clip = track->find_clip(*remapped.ids.find({4}));
    REQUIRE(media_clip != nullptr);
    REQUIRE(std::get<MediaRef>(media_clip->content()).asset_id == *remapped.ids.find({2}));

    const auto* note_clip = track->find_clip(*remapped.ids.find({5}));
    REQUIRE(note_clip != nullptr);
    const auto& remapped_notes = std::get<NoteContent>(note_clip->content()).notes();
    REQUIRE(remapped_notes[0].id == *remapped.ids.find({7}));
    REQUIRE(remapped_notes[1].id == *remapped.ids.find({8}));

    REQUIRE(original.find_asset({2}) != nullptr);
    REQUIRE(original.find_sequence({3}) != nullptr);

    const auto terminal =
        take_value(remap_ids(original, std::numeric_limits<std::uint64_t>::max() - 8));
    REQUIRE(terminal.project.next_item_id() == std::numeric_limits<std::uint64_t>::max());
    REQUIRE_FALSE(terminal.project.item_id_allocator().allocate().has_value());
}
