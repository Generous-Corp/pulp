#include "support/timeline_persistence_test_support.hpp"

#include <fstream>

namespace {

std::string fixture(std::string_view relative_path) {
    std::ifstream stream(std::string(PULP_TIMELINE_FIXTURE_DIR) + "/" + std::string(relative_path),
                         std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

Project automation_project() {
    auto curve =
        take(AutomationCurve::create({{{6}, {0}, 0.25f, AutomationInterpolation::Continuous, 0.0f},
                                      {{7}, {10}, 0.75f, AutomationInterpolation::Hold, 0.0f}}));
    auto lane = take(AutomationLane::create({5}, DeviceParameterTarget{{4}, 7}, std::move(curve)));
    auto track = take(Track::create(TrackInput{.id = {3},
                                               .name = "track",
                                               .device_chain = {{{4}}},
                                               .automation_lanes = {std::move(lane)}}));
    auto sequence = take(Sequence::create({2}, "root", TickDuration{100}, {track}));
    return take(Project::create(ProjectInput{{1}, "v3-automation", 8, {2}, {}, {sequence}}));
}

} // namespace

TEST_CASE("Timeline Track v3 automation is canonical and round trips") {
    const auto encoded = take(serialize_project(automation_project(), builtins()));
    REQUIRE(encoded.json.find("\"type_name\":\"pulp.timeline.track\",\"version\":3") !=
            std::string::npos);
    REQUIRE(encoded.json.find("pulp.timeline.automation_target.device_parameter") !=
            std::string::npos);

    auto decoded_result = deserialize_project(encoded.json, builtins());
    const auto decoded_code = decoded_result ? -1 : static_cast<int>(decoded_result.error().code);
    const auto decoded_path = decoded_result ? std::string{} : decoded_result.error().path;
    INFO("code=" << decoded_code << " path=" << decoded_path);
    REQUIRE(decoded_result);
    const auto decoded = std::move(decoded_result).value();
    const auto& lane = decoded.sequences()[0].tracks()[0].automation_lanes()[0];
    REQUIRE(lane.id() == ItemId{5});
    REQUIRE(lane.curve().points().size() == 2);
    REQUIRE(std::get<DeviceParameterTarget>(lane.target()).param_id == 7);
    REQUIRE(decoded.locate({5})->automation_lane_id == ItemId{5});
    REQUIRE(decoded.locate({6})->automation_lane_id == ItemId{5});
    REQUIRE(take(serialize_project(decoded, builtins())).json == encoded.json);
}

TEST_CASE("Timeline identity decoding accepts snapshots predating automation owners") {
    auto encoded = take(serialize_project(project_with(), builtins())).json;
    constexpr std::string_view field = R"("automation_lane_id":"0",)";
    for (auto position = encoded.find(field); position != std::string::npos;
         position = encoded.find(field))
        encoded.erase(position, field.size());
    const auto decoded = take(deserialize_project(encoded, builtins()));
    REQUIRE_FALSE(decoded.locate(decoded.id())->automation_lane_id.valid());
}

TEST_CASE("Timeline permanent Track v3 automation fixture remains readable") {
    auto decoded_result = deserialize_project(fixture("v3/automation-lane.json"), builtins());
    const auto decoded_code = decoded_result ? -1 : static_cast<int>(decoded_result.error().code);
    const auto decoded_path = decoded_result ? std::string{} : decoded_result.error().path;
    INFO("code=" << decoded_code << " path=" << decoded_path);
    REQUIRE(decoded_result);
    const auto decoded = std::move(decoded_result).value();
    const auto& track = decoded.sequences()[0].tracks()[0];
    REQUIRE(track.automation_lanes().size() == 1);
    REQUIRE(track.automation_lanes()[0].curve().points()[1].interpolation ==
            AutomationInterpolation::Hold);
}

TEST_CASE("Timeline Track v2 and v3 migrations fail closed on lossy downgrade") {
    const auto registry = builtins();
    const std::string v2 =
        R"({"data":{"clips":[],"device_chain":[],"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":2})";
    const auto v3 = take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 2, 3, v2));
    REQUIRE(v3.find("\"automation_lanes\":[]") != std::string::npos);
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 3, 2, v3)) == v2);

    const std::string populated =
        R"({"data":{"automation_lanes":[{}],"clips":[],"device_chain":[],"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":3})";
    auto rejected =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 3, 2, populated);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == PersistenceErrorCode::MigrationFailed);

    for (const std::string_view malformed_chain : {"null", "{}"}) {
        const auto malformed =
            std::string(R"({"data":{"clips":[],"device_chain":)") +
            std::string(malformed_chain) +
            R"(,"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":2})";
        auto malformed_result =
            registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 2, 3, malformed);
        REQUIRE_FALSE(malformed_result.has_value());
        REQUIRE(malformed_result.error().code == PersistenceErrorCode::MigrationFailed);
    }
}

TEST_CASE("Timeline automation quotas reject lanes and points during preflight") {
    const auto snapshot = take(serialize_project(automation_project(), builtins())).json;
    DecodeLimits lane_limits;
    lane_limits.max_automation_lanes = 0;
    auto lane_rejected = preflight_timeline_structure(snapshot, lane_limits);
    REQUIRE_FALSE(lane_rejected.has_value());
    REQUIRE(lane_rejected.error().code == PersistenceErrorCode::LimitExceeded);
    REQUIRE(lane_rejected.error().path == "/data/sequences/0/data/tracks/0/data/automation_lanes");

    DecodeLimits point_limits;
    point_limits.max_automation_points = 1;
    auto point_rejected = preflight_timeline_structure(snapshot, point_limits);
    REQUIRE_FALSE(point_rejected.has_value());
    REQUIRE(point_rejected.error().code == PersistenceErrorCode::LimitExceeded);
    REQUIRE(point_rejected.error().path ==
            "/data/sequences/0/data/tracks/0/data/automation_lanes/0/data/points");

    const auto web = DecodeLimits::web_defaults();
    REQUIRE(web.max_automation_lanes < DecodeLimits{}.max_automation_lanes);
    REQUIRE(web.max_automation_points < DecodeLimits{}.max_automation_points);
}
