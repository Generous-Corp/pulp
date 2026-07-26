#include "support/timeline_persistence_test_support.hpp"

#include <fstream>

namespace {

Project session_project(std::string scene_name = "verse") {
    auto notes = take(NoteContent::create({}));
    auto clip = take(
        Clip::create({4}, TickPosition{0}, TickDuration{kTicksPerQuarter}, ClipContent{notes}));
    auto track = take(Track::create({3}, "drums", {clip}));
    FollowActionSet follow;
    follow.choices[0] = FollowAction{FollowActionKind::Jump, ItemId{7}, 3};
    follow.choice_count = 1;
    follow.repetitions = 2;
    follow.grid = TickDuration{2 * kTicksPerQuarter};
    Scene scene{
        {5},
        std::move(scene_name),
        {Slot{{6}, {4}, launch_every_quarters(1), follow}, Slot{{7}, {}, launch_immediate(), {}}}};
    auto lane = take(ChordScaleLane::create({}));
    auto sequence = take(Sequence::create(SequenceInput{
        .id = {2},
        .name = "root",
        .musical_duration = TickDuration{8 * kTicksPerQuarter},
        .tracks = {track},
        .chord_scale_lane = std::move(lane),
        .scenes = {scene},
    }));
    return take(Project::create(ProjectInput{{1}, "session", 8, {2}, {}, {sequence}}));
}

Project all_follow_kinds_project() {
    constexpr std::array kinds{
        FollowActionKind::None,     FollowActionKind::Stop, FollowActionKind::Again,
        FollowActionKind::Previous, FollowActionKind::Next, FollowActionKind::First,
        FollowActionKind::Last,     FollowActionKind::Any,  FollowActionKind::Other,
        FollowActionKind::Jump,
    };
    auto notes = take(NoteContent::create({}));
    auto clip = take(
        Clip::create({4}, TickPosition{0}, TickDuration{kTicksPerQuarter}, ClipContent{notes}));
    auto track = take(Track::create({3}, "drums", {clip}));
    std::vector<Slot> slots;
    for (std::size_t index = 0; index < kinds.size(); ++index) {
        auto follow = follow_action(kinds[index], TickDuration{kTicksPerQuarter}, 2);
        if (kinds[index] == FollowActionKind::Jump)
            follow.choices[0].target = ItemId{6};
        slots.push_back(Slot{ItemId{6 + index}, {4}, launch_every_quarters(1), follow});
    }
    FollowActionSet weighted;
    weighted.choice_count = 4;
    for (std::size_t index = 0; index < weighted.choice_count; ++index)
        weighted.choices[index] =
            FollowAction{kinds[index], {}, static_cast<std::uint16_t>(index + 1)};
    weighted.repetitions = 3;
    weighted.grid = TickDuration{2 * kTicksPerQuarter};
    slots.push_back(Slot{
        {16}, {4}, LaunchQuantize{TickDuration{kTicksPerQuarter}, TickPosition{123}}, weighted});
    auto lane = take(ChordScaleLane::create({}));
    auto sequence = take(Sequence::create(SequenceInput{
        .id = {2},
        .name = "root",
        .musical_duration = TickDuration{8 * kTicksPerQuarter},
        .tracks = {track},
        .chord_scale_lane = std::move(lane),
        .scenes = {Scene{{5}, "all follows", std::move(slots)}},
    }));
    return take(Project::create(ProjectInput{{1}, "all follows", 17, {2}, {}, {sequence}}));
}

std::string only_sequence(const std::string& snapshot) {
    const auto parsed = take(parse_json(snapshot));
    return std::string(parsed->raw(parsed->root().find("data")->find("sequences")->array[0]));
}

std::string fixture(std::string_view relative_path) {
    std::ifstream stream(std::string(PULP_TIMELINE_FIXTURE_DIR) + "/" + std::string(relative_path),
                         std::ios::binary);
    REQUIRE(stream.good());
    std::string contents{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    while (!contents.empty() && (contents.back() == '\n' || contents.back() == '\r'))
        contents.pop_back();
    return contents;
}

} // namespace

TEST_CASE("Timeline session scenes slots quantization and follow actions round trip") {
    const auto registry = builtins();
    const auto encoded = take(serialize_project(session_project(), registry));
    const auto decoded = take(deserialize_project(encoded.json, registry));
    const auto* sequence = decoded.find_sequence({2});
    REQUIRE(sequence);
    REQUIRE(sequence->scenes().size() == 1);
    REQUIRE(sequence->scenes()[0].name == "verse");
    REQUIRE(sequence->scenes()[0].slots.size() == 2);
    const auto& slot = sequence->scenes()[0].slots[0];
    REQUIRE(slot.clip_id == ItemId{4});
    REQUIRE(slot.launch_quantize == launch_every_quarters(1));
    REQUIRE(slot.follow.repetitions == 2);
    REQUIRE(slot.follow.grid == TickDuration{2 * kTicksPerQuarter});
    REQUIRE(slot.follow.active()[0] == FollowAction{FollowActionKind::Jump, ItemId{7}, 3});
    REQUIRE(decoded.locate({5})->kind == ItemKind::Scene);
    REQUIRE(decoded.locate({6})->kind == ItemKind::Slot);
    REQUIRE(decoded.locate({6})->parent_id == ItemId{5});
    REQUIRE(take(serialize_project(decoded, registry)).json == encoded.json);

    std::vector<SchemaVersionTarget> current_versions;
    for (const auto& schema : registry.types())
        current_versions.push_back({schema.domain, schema.type_name, schema.current_version});
    const SchemaReleaseMap current{"session-current", current_versions};
    REQUIRE(take(serialize_project_for_release(decoded, registry, current)).json == encoded.json);

    const auto summary = take(peek_project_summary(encoded.json, registry));
    REQUIRE(summary.counts.scenes == 1);
    REQUIRE(summary.counts.slots == 2);

    DecodeLimits scene_limit;
    scene_limit.max_scenes = 0;
    REQUIRE_FALSE(deserialize_project(encoded.json, registry, scene_limit));
    DecodeLimits slot_limit;
    slot_limit.max_slots = 1;
    REQUIRE_FALSE(deserialize_project(encoded.json, registry, slot_limit));
}

TEST_CASE("Timeline session persistence pins every follow kind and weighted choice") {
    constexpr std::array kinds{
        FollowActionKind::None,     FollowActionKind::Stop, FollowActionKind::Again,
        FollowActionKind::Previous, FollowActionKind::Next, FollowActionKind::First,
        FollowActionKind::Last,     FollowActionKind::Any,  FollowActionKind::Other,
        FollowActionKind::Jump,
    };
    const auto registry = builtins();
    const auto encoded = take(serialize_project(all_follow_kinds_project(), registry));
    const auto decoded = take(deserialize_project(encoded.json, registry));
    const auto& slots = decoded.find_sequence({2})->scenes()[0].slots;
    REQUIRE(slots.size() == kinds.size() + 1);
    for (std::size_t index = 0; index < kinds.size(); ++index)
        REQUIRE(slots[index].follow.active()[0].kind == kinds[index]);
    REQUIRE(slots[9].follow.active()[0].target == ItemId{6});

    const auto& weighted = slots.back();
    REQUIRE(weighted.launch_quantize.phase == TickPosition{123});
    REQUIRE(weighted.follow.active().size() == 4);
    for (std::size_t index = 0; index < weighted.follow.active().size(); ++index) {
        REQUIRE(weighted.follow.active()[index].kind == kinds[index]);
        REQUIRE(weighted.follow.active()[index].weight == index + 1);
    }
    REQUIRE(take(serialize_project(decoded, registry)).json == encoded.json);
}

TEST_CASE("Timeline sequence v4 scene migration is lossless only for an empty scene list") {
    const auto registry = builtins();
    DecodeLimits limits;
    const auto legacy_fixture = fixture("v4/sequence-before-scenes.json");
    const auto fixture_upgrade = take(registry.migrate(
        SchemaDomain::Document, "pulp.timeline.sequence", 4, 5, legacy_fixture, limits));
    REQUIRE(fixture_upgrade.find(R"("regions":[],"scenes":[],"tracks":[])") != std::string::npos);
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 5, 4,
                                  fixture_upgrade, limits)) == legacy_fixture);

    const auto current = only_sequence(take(serialize_project(session_project(), registry)).json);
    auto refused =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 5, 4, current, limits);
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().code == PersistenceErrorCode::MigrationFailed);

    auto empty = current;
    const auto begin = empty.find("\"scenes\":[");
    REQUIRE(begin != std::string::npos);
    const auto end = empty.find("],\"tracks\"", begin);
    REQUIRE(end != std::string::npos);
    empty.erase(begin + 10, end - (begin + 10));
    auto legacy_result =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 5, 4, empty, limits);
    REQUIRE(legacy_result);
    const auto legacy = std::move(legacy_result).value();
    REQUIRE(legacy.find("\"scenes\"") == std::string::npos);
    auto restored_result =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 4, 5, legacy, limits);
    REQUIRE(restored_result);
    const auto restored = std::move(restored_result).value();
    REQUIRE(restored == empty);
}

TEST_CASE("Timeline session model rejects a slot that names a missing clip") {
    auto project = session_project();
    const auto* sequence = project.find_sequence({2});
    auto scenes = std::vector<Scene>(sequence->scenes().begin(), sequence->scenes().end());
    scenes[0].slots[0].clip_id = ItemId{99};
    auto rejected = Sequence::create(SequenceInput{
        .id = sequence->id(),
        .name = sequence->name(),
        .musical_duration = sequence->duration(),
        .absolute_duration = sequence->absolute_duration(),
        .tracks = std::vector<Track>(sequence->tracks().begin(), sequence->tracks().end()),
        .chord_scale_lane = sequence->chord_scale_lane(),
        .scenes = std::move(scenes),
    });
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ModelErrorCode::MissingItem);
    REQUIRE(rejected.error().item == ItemId{99});

    const ItemId exhausted{std::numeric_limits<std::uint64_t>::max()};
    scenes = std::vector<Scene>(sequence->scenes().begin(), sequence->scenes().end());
    scenes[0].slots[0].clip_id = exhausted;
    auto invalid_clip_sentinel = Sequence::create(SequenceInput{
        .id = sequence->id(),
        .name = sequence->name(),
        .musical_duration = sequence->duration(),
        .absolute_duration = sequence->absolute_duration(),
        .tracks = std::vector<Track>(sequence->tracks().begin(), sequence->tracks().end()),
        .chord_scale_lane = sequence->chord_scale_lane(),
        .scenes = std::move(scenes),
    });
    REQUIRE_FALSE(invalid_clip_sentinel);
    REQUIRE(invalid_clip_sentinel.error().code == ModelErrorCode::InvalidItemId);
    REQUIRE(invalid_clip_sentinel.error().item == exhausted);

    scenes = std::vector<Scene>(sequence->scenes().begin(), sequence->scenes().end());
    scenes[0].slots[0].follow.choices[0] = FollowAction{FollowActionKind::Next, exhausted, 3};
    auto invalid_target_sentinel = Sequence::create(SequenceInput{
        .id = sequence->id(),
        .name = sequence->name(),
        .musical_duration = sequence->duration(),
        .absolute_duration = sequence->absolute_duration(),
        .tracks = std::vector<Track>(sequence->tracks().begin(), sequence->tracks().end()),
        .chord_scale_lane = sequence->chord_scale_lane(),
        .scenes = std::move(scenes),
    });
    REQUIRE_FALSE(invalid_target_sentinel);
    REQUIRE(invalid_target_sentinel.error().code == ModelErrorCode::InvalidItemId);
    REQUIRE(invalid_target_sentinel.error().item == exhausted);

    scenes = std::vector<Scene>(sequence->scenes().begin(), sequence->scenes().end());
    scenes[0].slots[0].follow.choices[0].kind = static_cast<FollowActionKind>(255);
    auto invalid_follow = Sequence::create(SequenceInput{
        .id = sequence->id(),
        .name = sequence->name(),
        .musical_duration = sequence->duration(),
        .absolute_duration = sequence->absolute_duration(),
        .tracks = std::vector<Track>(sequence->tracks().begin(), sequence->tracks().end()),
        .chord_scale_lane = sequence->chord_scale_lane(),
        .scenes = std::move(scenes),
    });
    REQUIRE_FALSE(invalid_follow);
    REQUIRE(invalid_follow.error().code == ModelErrorCode::InvalidSchemaIdentity);

    scenes = std::vector<Scene>(sequence->scenes().begin(), sequence->scenes().end());
    scenes[0].slots[0].follow.choices[1] = FollowAction{FollowActionKind::Stop, {}, 2};
    auto noncanonical_inactive_choice = Sequence::create(SequenceInput{
        .id = sequence->id(),
        .name = sequence->name(),
        .musical_duration = sequence->duration(),
        .absolute_duration = sequence->absolute_duration(),
        .tracks = std::vector<Track>(sequence->tracks().begin(), sequence->tracks().end()),
        .chord_scale_lane = sequence->chord_scale_lane(),
        .scenes = std::move(scenes),
    });
    REQUIRE_FALSE(noncanonical_inactive_choice);
    REQUIRE(noncanonical_inactive_choice.error().code == ModelErrorCode::InvalidSchemaIdentity);
}

TEST_CASE("Timeline session persistence rejects invalid scene text and stored references") {
    const auto registry = builtins();

    auto invalid_name = std::string("verse");
    invalid_name[0] = static_cast<char>(0xc0);
    auto invalid_utf8 = serialize_project(session_project(std::move(invalid_name)), registry);
    REQUIRE_FALSE(invalid_utf8);
    REQUIRE(invalid_utf8.error().code == PersistenceErrorCode::InvalidUtf8);
    REQUIRE(invalid_utf8.error().path == "/data/sequences/0/data/scenes/0/data/name");

    const auto canonical = take(serialize_project(session_project(), registry)).json;
    const auto parsed = take(parse_json(canonical));
    const auto* project_data = parsed->root().find("data");
    REQUIRE(project_data);
    const auto* sequences = project_data->find("sequences");
    REQUIRE(sequences);
    const auto* sequence_data = sequences->array[0].find("data");
    REQUIRE(sequence_data);
    const auto* scenes = sequence_data->find("scenes");
    REQUIRE(scenes);
    const auto* scene_data = scenes->array[0].find("data");
    REQUIRE(scene_data);
    const auto* slots = scene_data->find("slots");
    REQUIRE(slots);

    auto malformed_slots = canonical;
    malformed_slots.replace(slots->begin, slots->end - slots->begin, "null");
    auto rejected_slots = deserialize_project(malformed_slots, registry);
    REQUIRE_FALSE(rejected_slots);
    REQUIRE(rejected_slots.error().code == PersistenceErrorCode::InvalidSchema);
    REQUIRE(rejected_slots.error().path == "/data/sequences/0/data/scenes/0/data/slots");

    const auto* slot_data = slots->array[0].find("data");
    REQUIRE(slot_data);
    const auto* stored_clip = slot_data->find("clip_id");
    REQUIRE(stored_clip);
    auto noncanonical_clip = canonical;
    noncanonical_clip.replace(stored_clip->begin, stored_clip->end - stored_clip->begin, R"("04")");
    auto rejected_clip = deserialize_project(noncanonical_clip, registry);
    REQUIRE_FALSE(rejected_clip);
    REQUIRE(rejected_clip.error().code == PersistenceErrorCode::InvalidNumber);
    REQUIRE(rejected_clip.error().path ==
            "/data/sequences/0/data/scenes/0/data/slots/0/data/clip_id");

    const auto* stored_launch = slot_data->find("launch_quantize");
    REQUIRE(stored_launch);
    const auto* stored_phase = stored_launch->find("phase");
    REQUIRE(stored_phase);
    auto noncanonical_phase = canonical;
    noncanonical_phase.replace(stored_phase->begin, stored_phase->end - stored_phase->begin,
                               R"("00")");
    auto rejected_phase = deserialize_project(noncanonical_phase, registry);
    REQUIRE_FALSE(rejected_phase);
    REQUIRE(rejected_phase.error().code == PersistenceErrorCode::InvalidNumber);
    REQUIRE(rejected_phase.error().path ==
            "/data/sequences/0/data/scenes/0/data/slots/0/data/launch_quantize/phase");

    auto missing_clip = canonical;
    missing_clip.replace(stored_clip->begin, stored_clip->end - stored_clip->begin, R"("99")");
    REQUIRE_FALSE(deserialize_project(missing_clip, registry));

    auto missing_jump_target = canonical;
    const auto target = missing_jump_target.find(R"("target":"7")");
    REQUIRE(target != std::string::npos);
    missing_jump_target.replace(target, std::string_view(R"("target":"7")").size(),
                                R"("target":"99")");
    REQUIRE_FALSE(deserialize_project(missing_jump_target, registry));

    auto invalid_follow_kind = canonical;
    const auto invalid_kind = invalid_follow_kind.find(R"("kind":"jump")");
    REQUIRE(invalid_kind != std::string::npos);
    invalid_follow_kind.replace(invalid_kind, std::string_view(R"("kind":"jump")").size(),
                                R"("kind":"bogus")");
    auto rejected_kind = deserialize_project(invalid_follow_kind, registry);
    REQUIRE_FALSE(rejected_kind);
    REQUIRE(rejected_kind.error().code == PersistenceErrorCode::InvalidSchema);
    REQUIRE(rejected_kind.error().path ==
            "/data/sequences/0/data/scenes/0/data/slots/0/data/follow/choices/0/kind");

    auto invalid_non_jump_target = canonical;
    const auto jump = invalid_non_jump_target.find(R"("kind":"jump")");
    REQUIRE(jump != std::string::npos);
    invalid_non_jump_target.replace(jump, std::string_view(R"("kind":"jump")").size(),
                                    R"("kind":"next")");
    REQUIRE_FALSE(deserialize_project(invalid_non_jump_target, registry));
}

TEST_CASE("Timeline session remap preserves scene ownership and launch references") {
    const auto remapped = take(remap_ids(session_project(), 100));
    const auto sequence_id = *remapped.ids.find({2});
    const auto scene_id = *remapped.ids.find({5});
    const auto slot_id = *remapped.ids.find({6});
    const auto jump_target = *remapped.ids.find({7});
    const auto clip_id = *remapped.ids.find({4});

    const auto* sequence = remapped.project.find_sequence(sequence_id);
    REQUIRE(sequence);
    const auto* scene = sequence->find_scene(scene_id);
    REQUIRE(scene);
    REQUIRE(scene->slots.size() == 2);
    REQUIRE(scene->slots[0].id == slot_id);
    REQUIRE(scene->slots[0].clip_id == clip_id);
    REQUIRE(scene->slots[0].follow.active()[0].target == jump_target);
    REQUIRE(remapped.project.locate(slot_id)->parent_id == scene_id);
}
