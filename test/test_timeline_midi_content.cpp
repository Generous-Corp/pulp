#include "support/timeline_persistence_test_support.hpp"

#include <algorithm>
#include <optional>

namespace {

constexpr std::uint64_t kModifierSeed = 1'234'567'890'123'456'789ull;

// Addresses on two channels so the canonical lane ordering has something to
// order, with identities deliberately descending against that ordering: with
// ascending identities an assertion about lane order passes even when the code
// sorted by identity instead of by address.
constexpr MidiLaneAddress kExpression{0, 0, 11, 0, 74};
constexpr MidiLaneAddress kModulation{0, 1, 11, 0, 1};

MidiExpressionLane expression_lane() {
    return MidiExpressionLane{{11}, kExpression, {{{8}, {48}, 0xffffffff}, {{10}, {0}, 0}}};
}

MidiExpressionLane modulation_lane() {
    return MidiExpressionLane{{9}, kModulation, {{{5}, {24}, 0x80000000}}};
}

NoteModifier every_third_note_six() {
    NoteModifier modifier;
    modifier.note_id = {6};
    modifier.condition = NoteConditionKind::EveryNth;
    modifier.condition_period = 3;
    modifier.condition_offset = 1;
    modifier.probability = 0xc000;
    modifier.ratchet_count = 2;
    return modifier;
}

std::vector<NoteEvent> two_notes() {
    return {{{7}, {0}, {2}, 0xffff, 60, 0}, {{6}, {4}, {2}, 0x8000, 64, 1}};
}

MidiContent content_with_lanes() {
    return take(MidiContent::create(two_notes(), {every_third_note_six()}, kModifierSeed,
                                    {modulation_lane(), expression_lane()}));
}

Project project_with_lanes() {
    auto clip = take(Clip::create({4}, {0}, {100}, content_with_lanes()));
    auto track = take(Track::create({3}, "track", {clip}));
    auto sequence = take(Sequence::create({2}, "sequence", TickDuration{100}, {track}));
    return take(Project::create(ProjectInput{{1}, "project", 20, {2}, {}, {sequence}}));
}

const MidiContent& only_midi_content(const Project& project) {
    const auto* sequence = project.find_sequence({2});
    REQUIRE(sequence != nullptr);
    return std::get<MidiContent>(sequence->tracks()[0].clips()[0].content());
}

} // namespace

TEST_CASE("MidiContent orders lanes by address and points by position then identity",
          "[timeline][midi-content]") {
    const auto content = content_with_lanes();

    REQUIRE(content.lanes().size() == 2);
    // Supplied modulation-first; the canonical order is by address, and the
    // expression lane's channel sorts first while carrying the larger identity.
    CHECK(content.lanes()[0].address == kExpression);
    CHECK(content.lanes()[0].id == ItemId{11});
    CHECK(content.lanes()[1].address == kModulation);
    CHECK(content.lanes()[1].id == ItemId{9});

    REQUIRE(content.lanes()[0].points.size() == 2);
    CHECK(content.lanes()[0].points[0].id == ItemId{10});
    CHECK(content.lanes()[0].points[0].position == TickPosition{0});
    CHECK(content.lanes()[0].points[1].id == ItemId{8});
    CHECK(content.lanes()[0].points[1].position == TickPosition{48});

    const auto* found = content.lane_for(kModulation);
    REQUIRE(found != nullptr);
    CHECK(found->id == ItemId{9});
    CHECK(content.lane_for(MidiLaneAddress{0, 2, 11, 0, 1}) == nullptr);
}

TEST_CASE("MidiContent rejects two lanes claiming one address", "[timeline][midi-content]") {
    auto duplicate = expression_lane();
    duplicate.id = {12};
    duplicate.points.clear();
    auto created =
        MidiContent::create(two_notes(), {}, 0, {expression_lane(), std::move(duplicate)});
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code == ModelErrorCode::DuplicateMidiLaneAddress);
}

TEST_CASE("MidiContent rejects an identity shared between a note and a lane point",
          "[timeline][midi-content]") {
    auto collide = modulation_lane();
    collide.points[0].id = {7};
    auto created = MidiContent::create(two_notes(), {}, 0, {std::move(collide)});
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code == ModelErrorCode::DuplicateItemId);
    CHECK(created.error().item == ItemId{7});
}

TEST_CASE("MidiContent note replace preserves untouched lane storage",
          "[timeline][midi-content]") {
    const auto original = content_with_lanes();
    const auto replaced = take(original.replace_note({{6}, {4}, {2}, 0x1234, 64, 1}));

    REQUIRE(replaced.notes().size() == 2);
    const auto moved = std::find_if(replaced.notes().begin(), replaced.notes().end(),
                                    [](const NoteEvent& note) { return note.id == ItemId{6}; });
    REQUIRE(moved != replaced.notes().end());
    CHECK(moved->velocity == 0x1234);

    REQUIRE(replaced.lanes().size() == original.lanes().size());
    for (std::size_t index = 0; index < replaced.lanes().size(); ++index) {
        CHECK(replaced.lanes()[index].id == original.lanes()[index].id);
        CHECK(replaced.lanes()[index].address == original.lanes()[index].address);
        CHECK(replaced.lanes()[index].points == original.lanes()[index].points);
    }
    CHECK(replaced.modifier_seed() == original.modifier_seed());
    REQUIRE(replaced.modifiers().size() == 1);
    CHECK(replaced.modifiers()[0] == original.modifiers()[0]);
}

TEST_CASE("Copying a clip issues fresh identities for its lanes and points",
          "[timeline][midi-content]") {
    const auto original = content_with_lanes();
    const auto source = take(Clip::create({4}, {0}, {100}, original));
    ItemIdAllocator allocator(100);
    const auto copied = take(remap_ids(source, allocator));
    const auto& copy = std::get<MidiContent>(copied.clip.content());

    REQUIRE(copy.lanes().size() == 2);
    // Every identity the copy owns must be one the allocator just issued. A
    // lane the remap walk never visited comes back either missing or still
    // carrying the source's identity, and the second is worse: two live
    // objects would answer to one identity, which is what the two-pass
    // allocator exists to prevent.
    const auto issued = [&](ItemId id) {
        return id.valid() && id.value >= 100 && id.value < allocator.next_value();
    };
    std::vector<ItemId> copied_ids;
    for (std::size_t index = 0; index < copy.lanes().size(); ++index) {
        const auto& source_lane = original.lanes()[index];
        const auto& copy_lane = copy.lanes()[index];
        CHECK(copy_lane.address == source_lane.address);
        CHECK(copy_lane.id != source_lane.id);
        CHECK(issued(copy_lane.id));
        CHECK(copied.ids.find(source_lane.id) == std::optional<ItemId>(copy_lane.id));
        copied_ids.push_back(copy_lane.id);

        REQUIRE(copy_lane.points.size() == source_lane.points.size());
        for (std::size_t point = 0; point < copy_lane.points.size(); ++point) {
            CHECK(copy_lane.points[point].position == source_lane.points[point].position);
            CHECK(copy_lane.points[point].value == source_lane.points[point].value);
            CHECK(copy_lane.points[point].id != source_lane.points[point].id);
            CHECK(issued(copy_lane.points[point].id));
            CHECK(copied.ids.find(source_lane.points[point].id) ==
                  std::optional<ItemId>(copy_lane.points[point].id));
            copied_ids.push_back(copy_lane.points[point].id);
        }
    }
    for (const auto& note : copy.notes())
        copied_ids.push_back(note.id);
    // Distinctness is asserted separately from freshness: a walk that visited
    // every lane but reused one issued identity twice would satisfy every
    // check above.
    std::sort(copied_ids.begin(), copied_ids.end());
    CHECK(std::adjacent_find(copied_ids.begin(), copied_ids.end()) == copied_ids.end());
    // Two lanes, three points, two notes: the count is asserted so a walk that
    // silently stopped emitting one kind cannot pass the distinctness check by
    // having fewer identities to collide.
    CHECK(copied_ids.size() == 7);
}

TEST_CASE("A sequence copy and its carried-id transfer agree on the whole owned set",
          "[timeline][midi-content]") {
    // Remapping a subtree allocates one destination identity per identity the
    // clip owns, and the carried-id overload below re-derives that same set to
    // size-check a table it is handed. Both read visit_clip_owned_identities,
    // so a kind the traversal stops emitting drops out of the copy and out of
    // the expectation together and the two still agree — which is why the
    // hand-counted total is asserted rather than just the agreement.
    const auto clip = take(Clip::create({4}, {0}, {100}, content_with_lanes()));
    const auto track = take(Track::create({3}, "track", {clip}));
    const auto sequence = take(Sequence::create({2}, "sequence", TickDuration{100}, {track}));

    ItemIdAllocator allocator(100);
    const auto copied = take(remap_ids(sequence, allocator));
    // Sequence, track, clip, two notes, two lanes, three points.
    CHECK(copied.ids.entries().size() == 10);

    const auto carried = remap_ids(sequence, copied.ids.entries(), RemapIdFixups{});
    REQUIRE(carried.has_value());
    CHECK(carried.value().ids.entries().size() == copied.ids.entries().size());
}

TEST_CASE("MidiContent lanes survive a serialize and deserialize round trip",
          "[timeline][midi-content]") {
    const auto registry = builtins();
    const auto project = project_with_lanes();
    const auto serialized = take(serialize_project(project, registry));
    // Lanes and their points are document identities, so they appear in the
    // identity index under their own kinds; the reload below therefore also
    // exercises the ownership rules for those kinds.
    CHECK(serialized.json.find(R"("kind":"midi_lane")") != std::string::npos);
    CHECK(serialized.json.find(R"("kind":"midi_lane_point")") != std::string::npos);
    const auto reloaded = take(deserialize_project(serialized.json, registry));
    const auto& content = only_midi_content(reloaded);

    REQUIRE(content.lanes().size() == 2);
    CHECK(content.lanes()[0].address == kExpression);
    CHECK(content.lanes()[0].id == ItemId{11});
    REQUIRE(content.lanes()[0].points.size() == 2);
    CHECK(content.lanes()[0].points[0].value == 0u);
    CHECK(content.lanes()[0].points[1].value == 0xffffffffu);
    REQUIRE(content.lanes()[1].points.size() == 1);
    CHECK(content.lanes()[1].points[0].value == 0x80000000u);
    CHECK(content.lanes()[1].points[0].position == TickPosition{24});
    CHECK(content.modifier_seed() == kModifierSeed);
}

TEST_CASE("Note content without lanes loads as a degenerate MidiContent",
          "[timeline][midi-content]") {
    const auto registry = builtins();
    // A document written before lanes existed: the content envelope is pinned
    // at the version that had no lane member at all.
    const std::string legacy =
        R"({"data":{"assets":[],"id":"1","meter_map":[{"denominator":4,"numerator":4,"tick":"0"}],)"
        R"("name":"project","next_item_id":"20","root_sequence_id":"2","sequences":[{"data":)"
        R"({"absolute_duration":null,"id":"2","musical_duration":"100","name":"sequence",)"
        R"("tracks":[{"data":{"active_take_lane_id":"0","automation_lanes":[],"clips":[{"data":)"
        R"({"content":{"data":{"modifier_seed":"1234567890123456789","modifiers":[{"condition":)"
        R"("every_nth","condition_offset":1,"condition_period":3,"note_id":"6","probability":49152,)"
        R"("ratchet_count":2}],"notes":[{"channel":0,"duration_ticks":"2","id":"7","pitch":60,)"
        R"("start_ticks":"0","velocity":65535},{"channel":1,"duration_ticks":"2","id":"6",)"
        R"("pitch":64,"start_ticks":"4","velocity":32768}]},)"
        R"("type_name":"pulp.timeline.content.notes","version":2},"fade_in_duration":"0",)"
        R"("fade_out_duration":"0","gain_linear_bits":"1065353216","id":"4","time_range":)"
        R"({"duration_ticks":"100","kind":"musical","start_ticks":"0"}},)"
        R"("type_name":"pulp.timeline.clip","version":1}],"device_chain":[],"id":"3",)"
        R"("name":"track","record_armed":false,"take_lanes":[]},)"
        R"("type_name":"pulp.timeline.track","version":6}]},)"
        R"("type_name":"pulp.timeline.sequence","version":1}],)"
        R"("tempo_map":[{"bpm_bits":"4638144666238189568","curve":"constant","tick":"0"}]},)"
        R"("type_name":"pulp.timeline.project","version":1})";

    const auto loaded = take(deserialize_project(legacy, registry));
    const auto& content = only_midi_content(loaded);

    CHECK(content.lanes().empty());
    // Notes and modifiers cross the migration untouched, which is the whole
    // claim: a document that never authored a controller plays identically.
    REQUIRE(content.notes().size() == 2);
    CHECK(content.notes()[0].id == ItemId{7});
    CHECK(content.notes()[0].velocity == 0xffff);
    CHECK(content.notes()[1].id == ItemId{6});
    CHECK(content.notes()[1].velocity == 0x8000);
    CHECK(content.modifier_seed() == kModifierSeed);
    REQUIRE(content.modifiers().size() == 1);
    CHECK(content.modifiers()[0] == every_third_note_six());
}

TEST_CASE("Lane-bearing content has no older spelling to downgrade into",
          "[timeline][midi-content]") {
    const auto registry = builtins();
    const auto* release = find_builtin_timeline_schema_release("v0.750.0");
    REQUIRE(release != nullptr);
    const auto* target = release->find(SchemaDomain::Content, "pulp.timeline.content.notes");
    REQUIRE(target != nullptr);

    const auto project_from = [](MidiContent content) {
        auto clip = take(Clip::create({4}, {0}, {100}, std::move(content)));
        auto track = take(Track::create({3}, "track", {clip}));
        auto sequence = take(Sequence::create({2}, "sequence", TickDuration{100}, {track}));
        return take(Project::create(ProjectInput{{1}, "project", 20, {2}, {}, {sequence}}));
    };

    // The two projects differ only in whether a lane is authored, so the
    // refusal below can only be about the lane the older release cannot spell.
    const auto with_lane =
        project_from(take(MidiContent::create(two_notes(), {}, 0, {modulation_lane()})));
    const auto without_lane = project_from(take(MidiContent::create(two_notes())));

    CHECK_FALSE(serialize_project_for_release(with_lane, registry, *release).has_value());
    CHECK(serialize_project_for_release(without_lane, registry, *release).has_value());
}
