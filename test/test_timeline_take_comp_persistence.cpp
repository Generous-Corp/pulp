#include "support/timeline_persistence_test_support.hpp"

namespace {

// Ids: project 1, sequence 2, track 4, take lane 5, takes 7 and 8, asset 6.
// Two takes so the round trip must preserve both take identities and the
// take -> lane parent under canonical ordering.
Project project_with_takes(bool record_armed = true, ItemId active_take_lane_id = {5}) {
    auto take_a = take(Take::create({7}, MediaRef{{6}, {0}, 100}, {0}, RationalRate{48'000, 1}));
    auto take_b =
        take(Take::create({8}, MediaRef{{6}, {100}, 200}, {480}, RationalRate{48'000, 1}));
    auto lane = take(TakeLane::create(
        {5}, "comp", {take_a, take_b},
        {{.take_id = {7}, .range = {{20}, 40, {48'000, 1}}},
         {.take_id = {8}, .range = {{500}, 100, {48'000, 1}}}}));
    auto track = take(Track::create(TrackInput{.id = {4},
                                               .name = "track",
                                               .take_lanes = {lane},
                                               .record_armed = record_armed,
                                               .active_take_lane_id = active_take_lane_id}));
    auto sequence = take(Sequence::create({2}, "seq", TickDuration{100}, {track}));
    MediaAsset asset{{6}, "audio.wav", 1'000, {48'000, 1}, hash('a'), AssetStoragePolicy::External,
                     {},  {}};
    return take(Project::create(ProjectInput{{1}, "project", 9, {2}, {asset}, {sequence}}));
}

} // namespace

TEST_CASE("Take/comp snapshots round trip canonically and preserve lane parent") {
    const auto registry = builtins();
    const auto original = project_with_takes();

    auto first = serialize_project(original, registry);
    REQUIRE(first.has_value());
    REQUIRE(first.value().json.find("\"take_lanes\":[") != std::string::npos);
    REQUIRE(first.value().json.find("\"comp_segments\":[") != std::string::npos);
    REQUIRE(first.value().json.find(
                "\"type_name\":\"pulp.timeline.take_lane\",\"version\":2") !=
            std::string::npos);
    REQUIRE(first.value().json.find("\"record_armed\":true") != std::string::npos);
    REQUIRE(first.value().json.find("\"active_take_lane_id\":\"5\"") != std::string::npos);
    REQUIRE(first.value().json.find("\"type_name\":\"pulp.timeline.track\",\"version\":5") !=
            std::string::npos);

    const auto decoded = take(deserialize_project(first.value().json, registry));

    // The take -> lane parent identity survives decode.
    const auto take_loc = decoded.locate({7});
    REQUIRE(take_loc.has_value());
    REQUIRE(take_loc->kind == ItemKind::Take);
    REQUIRE(take_loc->parent_id == ItemId{5});
    const auto lane_loc = decoded.locate({5});
    REQUIRE(lane_loc.has_value());
    REQUIRE(lane_loc->kind == ItemKind::TakeLane);
    REQUIRE(lane_loc->parent_id == ItemId{4});

    const auto* track = decoded.find_sequence({2})->find_track({4});
    REQUIRE(track != nullptr);
    REQUIRE(track->record_armed());
    REQUIRE(track->active_take_lane_id() == ItemId{5});
    const auto* lane = track->find_take_lane({5});
    REQUIRE(lane != nullptr);
    REQUIRE(lane->takes().size() == 2);
    REQUIRE(lane->comp_segments().size() == 2);
    REQUIRE(lane->comp_segments()[0].take_id == ItemId{7});
    REQUIRE(lane->comp_segments()[1].range.start == SamplePosition{500});
    REQUIRE(lane->find_take({8})->media().asset_id == ItemId{6});
    REQUIRE(lane->find_take({8})->media().frame_count == 200);
    REQUIRE(lane->find_take({8})->placement_start() == SamplePosition{480});

    // Re-encoding the decoded project reproduces the exact canonical bytes.
    const auto second = take(serialize_project(decoded, registry));
    REQUIRE(second.json == first.value().json);
}

TEST_CASE("Take-lane v1 upgrades to an empty comp and downgrades only when empty") {
    const auto registry = builtins();
    const std::string v1 =
        R"({"data":{"id":"5","name":"lane","takes":[]},"type_name":"pulp.timeline.take_lane","version":1})";
    const auto v2 =
        take(registry.migrate(SchemaDomain::Document, "pulp.timeline.take_lane", 1, 2, v1));
    REQUIRE(v2 ==
            R"({"data":{"comp_segments":[],"id":"5","name":"lane","takes":[]},"type_name":"pulp.timeline.take_lane","version":2})");
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.take_lane", 2, 1,
                                  v2)) == v1);
    const std::string reordered_v1 =
        R"({"version":1,"type_name":"pulp.timeline.take_lane","data":{"id":"5","name":"lane","takes":[]}})";
    const auto reordered_v2 = take(registry.migrate(
        SchemaDomain::Document, "pulp.timeline.take_lane", 1, 2, reordered_v1));
    REQUIRE(reordered_v2 ==
            R"({"version":2,"type_name":"pulp.timeline.take_lane","data":{"comp_segments":[],"id":"5","name":"lane","takes":[]}})");
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.take_lane", 2, 1,
                                  reordered_v2)) == reordered_v1);

    const std::string populated =
        R"({"data":{"comp_segments":[{"sample_count":"1","sample_rate":{"denominator":"1","numerator":"48000"},"start":"0","take_id":"7"}],"id":"5","name":"lane","takes":[]},"type_name":"pulp.timeline.take_lane","version":2})";
    REQUIRE_FALSE(registry.migrate(SchemaDomain::Document, "pulp.timeline.take_lane", 2, 1,
                                   populated));
}

TEST_CASE("Take comp segment quota rejects the second segment during structural preflight") {
    const auto registry = builtins();
    const auto json = take(serialize_project(project_with_takes(), registry)).json;
    DecodeLimits limits;
    limits.max_take_comp_segments = 1;
    auto rejected = deserialize_project(json, registry, limits);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == PersistenceErrorCode::LimitExceeded);
    REQUIRE(rejected.error().path ==
            "/data/sequences/0/data/tracks/0/data/take_lanes/0/data/comp_segments");
    REQUIRE(rejected.error().actual == 2);
    REQUIRE(rejected.error().limit == 1);
}

TEST_CASE("Track v3 to v4 migration adds empty take state and round trips") {
    const auto registry = builtins();
    const std::string v3 =
        R"({"data":{"automation_lanes":[],"clips":[],"device_chain":[],"id":"3","name":"track"},)"
        R"("type_name":"pulp.timeline.track","version":3})";
    const auto v4 = take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 3, 4, v3));
    REQUIRE(v4.find("\"record_armed\":false") != std::string::npos);
    REQUIRE(v4.find("\"take_lanes\":[]") != std::string::npos);
    REQUIRE(v4.find("\"version\":4") != std::string::npos);
    // The empty-take v4 form downgrades back to the exact v3 bytes.
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 4, 3, v4)) == v3);
}

TEST_CASE("Track v4 to v5 migration adds arrangement selection and round trips") {
    const auto registry = builtins();
    const std::string v4 =
        R"({"data":{"automation_lanes":[],"clips":[],"device_chain":[],"id":"3","name":"track",)"
        R"("record_armed":false,"take_lanes":[]},"type_name":"pulp.timeline.track","version":4})";
    const auto v5 = take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 4, 5, v4));
    REQUIRE(v5.find("\"active_take_lane_id\":\"0\"") != std::string::npos);
    REQUIRE(v5.find("\"version\":5") != std::string::npos);
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 5, 4, v5)) == v4);
}

TEST_CASE("Track v5 to v4 migration removes an arrangement selection in any member position") {
    const auto registry = builtins();
    const std::string v5 =
        R"({"data":{"automation_lanes":[],"active_take_lane_id":"0","clips":[],)"
        R"("device_chain":[],"id":"3","name":"track","record_armed":false,"take_lanes":[]},)"
        R"("type_name":"pulp.timeline.track","version":5})";
    const std::string v4 =
        R"({"data":{"automation_lanes":[],"clips":[],"device_chain":[],"id":"3","name":"track",)"
        R"("record_armed":false,"take_lanes":[]},"type_name":"pulp.timeline.track","version":4})";
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 5, 4, v5)) == v4);
}

TEST_CASE("Track v5 to v4 downgrade fails closed on active take-lane selection") {
    const auto registry = builtins();
    const std::string selected =
        R"({"data":{"active_take_lane_id":"5","automation_lanes":[],"clips":[],)"
        R"("device_chain":[],"id":"3","name":"track","record_armed":false,"take_lanes":[]},)"
        R"("type_name":"pulp.timeline.track","version":5})";
    REQUIRE_FALSE(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 5, 4, selected));
}

TEST_CASE("Track v5 decode requires an active selection that names a local take lane") {
    const auto registry = builtins();
    auto canonical = take(serialize_project(project_with_takes(), registry)).json;
    constexpr std::string_view active = R"("active_take_lane_id":"5",)";
    const auto active_position = canonical.find(active);
    REQUIRE(active_position != std::string::npos);

    auto missing = canonical;
    missing.erase(active_position, active.size());
    auto missing_result = deserialize_project(missing, registry);
    REQUIRE_FALSE(missing_result);
    REQUIRE(missing_result.error().code == PersistenceErrorCode::MissingField);

    auto dangling = canonical;
    dangling.replace(active_position, active.size(), R"("active_take_lane_id":"99",)");
    auto dangling_result = deserialize_project(dangling, registry);
    REQUIRE_FALSE(dangling_result);
    REQUIRE(dangling_result.error().code == PersistenceErrorCode::ModelRejected);
    REQUIRE(dangling_result.error().model_error->code == ModelErrorCode::MissingItem);
}

TEST_CASE("Track v4 to v3 downgrade fails closed on non-empty take state") {
    const auto registry = builtins();
    const std::string armed =
        R"({"data":{"automation_lanes":[],"clips":[],"device_chain":[],"id":"3","name":"track",)"
        R"("record_armed":true,"take_lanes":[]},"type_name":"pulp.timeline.track","version":4})";
    REQUIRE_FALSE(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 4, 3, armed));
    const std::string populated =
        R"({"data":{"automation_lanes":[],"clips":[],"device_chain":[],"id":"3","name":"track",)"
        R"("record_armed":false,"take_lanes":[{}]},"type_name":"pulp.timeline.track","version":4})";
    REQUIRE_FALSE(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 4, 3, populated));
}
