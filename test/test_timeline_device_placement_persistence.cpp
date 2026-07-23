#include "support/timeline_persistence_test_support.hpp"

#include <fstream>

namespace {

std::string fixture(std::string_view relative_path) {
    std::ifstream stream(std::string(PULP_TIMELINE_FIXTURE_DIR) + "/" + std::string(relative_path),
                         std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string replace_once(std::string value, std::string_view before, std::string_view after) {
    const auto position = value.find(before);
    REQUIRE(position != std::string::npos);
    value.replace(position, before.size(), after);
    return value;
}

Project project_with_devices() {
    const auto track =
        take(Track::create(TrackInput{.id = {3}, .name = "track", .device_chain = {{{5}}, {{4}}}}));
    const auto sequence = take(Sequence::create({2}, "root", TickDuration{0}, {track}));
    return take(Project::create(ProjectInput{{1}, "devices", 6, {2}, {}, {sequence}}));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
rewrite_track_identity(std::string_view, BoundedJsonSink& output, const void*) noexcept {
    output.append(
        R"({"data":{"clips":[],"device_chain":[],"id":"99","name":"rewritten"},"type_name":"pulp.timeline.track","version":2})");
    return runtime::Ok(SchemaWriteSuccess{});
}

SchemaRegistry registry_with_replaced_track_migration() {
    const auto source = builtins();
    SchemaRegistryBuilder builder;
    for (const auto& original : source.types()) {
        auto copy = original;
        if (copy.type_name == "pulp.timeline.track") {
            REQUIRE(copy.upgrades.size() == 4);
            copy.upgrades[0].migrate = rewrite_track_identity;
        }
        REQUIRE(builder.register_type(std::move(copy)).has_value());
    }
    return take(std::move(builder).build());
}

void require_invalid_identity(const std::string& snapshot) {
    auto rejected = deserialize_project(snapshot, builtins());
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == PersistenceErrorCode::ModelRejected);
    REQUIRE(rejected.error().model_error.has_value());
    REQUIRE(rejected.error().model_error->code == ModelErrorCode::InvalidSchemaIdentity);
}

} // namespace

TEST_CASE("Timeline device chains round trip in authored order") {
    const auto registry = builtins();
    const auto encoded = take(serialize_project(project_with_devices(), registry));
    REQUIRE(encoded.json.find("\"type_name\":\"pulp.timeline.track\",\"version\":5") !=
            std::string::npos);
    REQUIRE(encoded.json.find("\"type_name\":\"pulp.timeline.device_placement\"") !=
            std::string::npos);

    const auto decoded = take(deserialize_project(encoded.json, registry));
    const auto& track = decoded.sequences()[0].tracks()[0];
    REQUIRE(track.device_chain().size() == 2);
    REQUIRE(track.device_chain()[0].id == ItemId{5});
    REQUIRE(track.device_chain()[1].id == ItemId{4});
    REQUIRE(take(serialize_project(decoded, registry)).json == encoded.json);
}

TEST_CASE("Timeline track migrations preserve device-placement identity or fail closed") {
    const auto registry = builtins();
    const std::string v1 =
        R"({"data":{"clips":[],"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":1})";
    const auto upgraded =
        take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 1, 2, v1));
    REQUIRE(upgraded.find("\"device_chain\":[]") != std::string::npos);
    REQUIRE(upgraded.find("\"version\":2") != std::string::npos);
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 2, 1, upgraded)) ==
            v1);

    const std::string opaque_raw =
        R"({ "version" : 7, "data" : {"escaped":"\u0061"}, "type_name" : "vendor.future" })";
    const auto opaque_v1 =
        R"({"data":{"clips":[{"data":{"content":)" + opaque_raw +
        R"(,"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1065353216","id":"4","time_range":{"duration_ticks":"10","kind":"musical","start_ticks":"0"}},"type_name":"pulp.timeline.clip","version":1}],"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":1})";
    const auto opaque_v2 =
        take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 1, 2, opaque_v1));
    REQUIRE(opaque_v2.find(opaque_raw) != std::string::npos);
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 2, 1,
                                  opaque_v2)) == opaque_v1);

    const std::string malformed =
        R"({"data":{"clips":"wrong","id":"3","name":"track"},"type_name":"pulp.timeline.track","version":1})";
    auto malformed_result =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 1, 2, malformed);
    REQUIRE_FALSE(malformed_result.has_value());
    REQUIRE(malformed_result.error().code == PersistenceErrorCode::MigrationFailed);

    const std::string populated =
        R"({"data":{"clips":[],"device_chain":[{"data":{"id":"4"},"type_name":"pulp.timeline.device_placement","version":1}],"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":2})";
    auto rejected =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 2, 1, populated);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == PersistenceErrorCode::MigrationFailed);
}

TEST_CASE("Timeline Track migration preserves raw member order and whitespace") {
    struct Case {
        std::string_view v2;
        std::string_view v1;
    };
    const std::array cases{
        Case{
            R"({"version":2, "data":{ "device_chain":[] , "clips":[],"id":"3","name":"track"}, "type_name":"pulp.timeline.track"})",
            R"({"version":1, "data":{ "clips":[],"id":"3","name":"track"}, "type_name":"pulp.timeline.track"})"},
        Case{
            R"({"data":{"clips":[],  "device_chain":[],  "id":"3","name":"track"},"type_name":"pulp.timeline.track","version":2})",
            R"({"data":{"clips":[],  "id":"3","name":"track"},"type_name":"pulp.timeline.track","version":1})"},
        Case{
            R"({"data":{"clips":[],"id":"3","name":"track",  "device_chain":[] },"type_name":"pulp.timeline.track","version":2})",
            R"({"data":{"clips":[],"id":"3","name":"track" },"type_name":"pulp.timeline.track","version":1})"},
    };
    const auto registry = builtins();
    for (const auto& item : cases) {
        INFO(item.v2);
        const auto downgraded =
            take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 2, 1, item.v2));
        REQUIRE(downgraded == item.v1);
        REQUIRE(parse_json(downgraded).has_value());
        const auto upgraded =
            take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 1, 2, downgraded));
        REQUIRE(parse_json(upgraded).has_value());
        REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 2, 1,
                                      upgraded)) == downgraded);
    }
}

TEST_CASE("Timeline permanent fixtures cover Track versions one and two") {
    const auto registry = builtins();
    const auto old_project = take(deserialize_project(fixture("v1/track.json"), registry));
    REQUIRE(old_project.sequences()[0].tracks()[0].device_chain().empty());
    const auto upgraded = take(serialize_project(old_project, registry));
    REQUIRE(upgraded.json.find("\"type_name\":\"pulp.timeline.track\",\"version\":5") !=
            std::string::npos);

    const auto current = take(deserialize_project(fixture("v2/device-chain.json"), registry));
    const auto chain = current.sequences()[0].tracks()[0].device_chain();
    REQUIRE(chain.size() == 2);
    REQUIRE(chain[0].id == ItemId{5});
    REQUIRE(chain[1].id == ItemId{4});

    const auto opaque_project =
        take(deserialize_project(fixture("v1/track-opaque.json"), registry));
    const auto& content = opaque_project.sequences()[0].tracks()[0].clips()[0].content();
    REQUIRE(std::holds_alternative<OpaqueContent>(content));
    const std::string raw =
        R"({ "version" : 7, "data" : {"escaped":"\u0061","owned_id":"99"}, "type_name" : "vendor.future" })";
    REQUIRE(std::get<OpaqueContent>(content).raw_json() == raw);
    REQUIRE(take(serialize_project(opaque_project, registry)).json.find(raw) != std::string::npos);
}

TEST_CASE("Timeline structural decode does not execute replaceable Track migrations") {
    const auto decoded = take(
        deserialize_project(fixture("v1/track.json"), registry_with_replaced_track_migration()));
    REQUIRE(decoded.sequences()[0].tracks()[0].id() == ItemId{3});
    REQUIRE(decoded.sequences()[0].tracks()[0].name() == "track");
}

TEST_CASE("Timeline device placement quota rejects before model construction") {
    const auto snapshot = take(serialize_project(project_with_devices(), builtins()));
    DecodeLimits limits;
    limits.max_device_placements = 1;
    auto rejected = preflight_timeline_structure(snapshot.json, limits);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == PersistenceErrorCode::LimitExceeded);
    REQUIRE(rejected.error().path == "/data/sequences/0/data/tracks/0/data/device_chain");
    REQUIRE(rejected.error().actual == 2);
    REQUIRE(rejected.error().limit == 1);
}

TEST_CASE("Timeline device chain envelopes reject mislabeled structural versions") {
    const auto current = fixture("v2/device-chain.json");
    const auto reject = [](const std::string& json, PersistenceErrorCode expected) {
        auto decoded = deserialize_project(json, builtins());
        REQUIRE_FALSE(decoded.has_value());
        REQUIRE(decoded.error().code == expected);
    };

    reject(replace_once(current, "pulp.timeline.track\",\"version\":2",
                        "pulp.timeline.track\",\"version\":6"),
           PersistenceErrorCode::UnsupportedSchemaVersion);
    reject(replace_once(current, "pulp.timeline.track\",\"version\":2",
                        "pulp.timeline.track\",\"version\":1"),
           PersistenceErrorCode::InvalidSchema);
    reject(replace_once(current, "pulp.timeline.device_placement\",\"version\":1",
                        "pulp.timeline.device_placement\",\"version\":2"),
           PersistenceErrorCode::UnsupportedSchemaVersion);
    reject(replace_once(current, "pulp.timeline.device_placement", "pulp.timeline.unknown_device"),
           PersistenceErrorCode::InvalidSchema);
}

TEST_CASE("Timeline persistence restores device placement tombstones with Track ownership") {
    auto snapshot = take(serialize_project(project_with_devices(), builtins())).json;
    const auto next = snapshot.find("\"next_item_id\":\"6\"");
    REQUIRE(next != std::string::npos);
    snapshot.replace(next, std::string("\"next_item_id\":\"6\"").size(), "\"next_item_id\":\"7\"");
    const auto identities_end = snapshot.find("],\"meter_map\":");
    REQUIRE(identities_end != std::string::npos);
    snapshot.insert(
        identities_end,
        R"(,{"active":false,"clip_id":"0","id":"6","kind":"device_placement","parent_id":"3","sequence_id":"2","track_id":"3"})");

    const auto legacy_snapshot = replace_once(
        snapshot,
        R"({"active":false,"clip_id":"0","id":"6","kind":"device_placement","parent_id":"3","sequence_id":"2","track_id":"3"})",
        R"({"active":false,"clip_id":"0","id":"6","kind":"device_placement","sequence_id":"2","track_id":"3"})");
    const auto restored = take(deserialize_project(legacy_snapshot, builtins()));
    const auto tombstone = restored.locate({6});
    REQUIRE(tombstone.has_value());
    REQUIRE(tombstone->kind == ItemKind::DevicePlacement);
    REQUIRE(tombstone->parent_id == ItemId{3});
    REQUIRE(tombstone->sequence_id == ItemId{2});
    REQUIRE(tombstone->track_id == ItemId{3});
    REQUIRE_FALSE(tombstone->active);
    REQUIRE(take(serialize_project(restored, builtins())).json == snapshot);

    const auto remapped = take(remap_ids(restored, 100));
    const auto remapped_tombstone_id = remapped.ids.find({6});
    REQUIRE(remapped_tombstone_id.has_value());
    const auto remapped_tombstone = remapped.project.locate(*remapped_tombstone_id);
    REQUIRE(remapped_tombstone.has_value());
    REQUIRE(remapped_tombstone->kind == ItemKind::DevicePlacement);
    REQUIRE(remapped_tombstone->parent_id == *remapped.ids.find({3}));
    REQUIRE(remapped_tombstone->sequence_id == *remapped.ids.find({2}));
    REQUIRE(remapped_tombstone->track_id == *remapped.ids.find({3}));
    REQUIRE_FALSE(remapped_tombstone->active);
    REQUIRE(remapped.project.next_item_id() == 106);

    require_invalid_identity(
        replace_once(snapshot, "\"clip_id\":\"0\",\"id\":\"6\"", "\"clip_id\":\"4\",\"id\":\"6\""));
    require_invalid_identity(replace_once(snapshot,
                                          "\"id\":\"6\",\"kind\":\"device_placement\",\"parent_"
                                          "id\":\"3\",\"sequence_id\":\"2\",\"track_id\":\"3\"",
                                          "\"id\":\"6\",\"kind\":\"device_placement\",\"parent_"
                                          "id\":\"3\",\"sequence_id\":\"2\",\"track_id\":\"99\""));
    require_invalid_identity(replace_once(snapshot, "\"parent_id\":\"3\",\"sequence_id\":\"2\"",
                                          "\"parent_id\":\"4\",\"sequence_id\":\"2\""));

    const auto base = project_with_devices();
    const auto second_track = take(Track::create({7}, "second", {}));
    const auto second_sequence =
        take(Sequence::create({6}, "second", TickDuration{0}, {second_track}));
    const auto two_sequences = take(Project::create(
        ProjectInput{{1}, "devices", 9, {2}, {}, {base.sequences()[0], second_sequence}}));
    auto cross_sequence = take(serialize_project(two_sequences, builtins())).json;
    const auto cross_identities_end = cross_sequence.find("],\"meter_map\":");
    REQUIRE(cross_identities_end != std::string::npos);
    cross_sequence.insert(
        cross_identities_end,
        R"(,{"active":false,"clip_id":"0","id":"8","kind":"device_placement","parent_id":"3","sequence_id":"6","track_id":"3"})");
    require_invalid_identity(cross_sequence);
}
