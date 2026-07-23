#include "support/timeline_persistence_test_support.hpp"

namespace {

// Ids: project 1, sequence 2, track 4, take lane 5, takes 7 and 8, asset 6.
// Two takes so the round trip must preserve both take identities and the
// take -> lane parent under canonical ordering.
Project project_with_takes(bool record_armed = true) {
    auto take_a = take(Take::create({7}, MediaRef{{6}, {0}, 100}, {0}, RationalRate{48'000, 1}));
    auto take_b = take(Take::create({8}, MediaRef{{6}, {100}, 200}, {480}, RationalRate{48'000, 1}));
    auto lane = take(TakeLane::create({5}, "comp", {take_a, take_b}));
    auto track = take(Track::create(
        TrackInput{.id = {4}, .name = "track", .take_lanes = {lane}, .record_armed = record_armed}));
    auto sequence = take(Sequence::create({2}, "seq", TickDuration{100}, {track}));
    MediaAsset asset{{6},          "audio.wav", 1'000, {48'000, 1}, hash('a'),
                     AssetStoragePolicy::External, {}, {}};
    return take(Project::create(ProjectInput{{1}, "project", 9, {2}, {asset}, {sequence}}));
}

} // namespace

TEST_CASE("Take/comp snapshots round trip canonically and preserve lane parent") {
    const auto registry = builtins();
    const auto original = project_with_takes();

    auto first = serialize_project(original, registry);
    REQUIRE(first.has_value());
    REQUIRE(first.value().json.find("\"take_lanes\":[") != std::string::npos);
    REQUIRE(first.value().json.find("\"record_armed\":true") != std::string::npos);
    REQUIRE(first.value().json.find("\"type_name\":\"pulp.timeline.track\",\"version\":4") !=
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
    const auto* lane = track->find_take_lane({5});
    REQUIRE(lane != nullptr);
    REQUIRE(lane->takes().size() == 2);
    REQUIRE(lane->find_take({8})->media().asset_id == ItemId{6});
    REQUIRE(lane->find_take({8})->media().frame_count == 200);
    REQUIRE(lane->find_take({8})->placement_start() == SamplePosition{480});

    // Re-encoding the decoded project reproduces the exact canonical bytes.
    const auto second = take(serialize_project(decoded, registry));
    REQUIRE(second.json == first.value().json);
}

TEST_CASE("Track v3 to v4 migration adds empty take state and round trips") {
    const auto registry = builtins();
    const std::string v3 =
        R"({"data":{"automation_lanes":[],"clips":[],"device_chain":[],"id":"3","name":"track"},)"
        R"("type_name":"pulp.timeline.track","version":3})";
    const auto v4 =
        take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 3, 4, v3));
    REQUIRE(v4.find("\"record_armed\":false") != std::string::npos);
    REQUIRE(v4.find("\"take_lanes\":[]") != std::string::npos);
    REQUIRE(v4.find("\"version\":4") != std::string::npos);
    // The empty-take v4 form downgrades back to the exact v3 bytes.
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 4, 3, v4)) == v3);
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
