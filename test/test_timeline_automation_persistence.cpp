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
    REQUIRE(decoded.locate({5})->parent_id == ItemId{3});
    REQUIRE(decoded.locate({6})->parent_id == ItemId{5});
    REQUIRE(take(serialize_project(decoded, builtins())).json == encoded.json);
}

TEST_CASE("Timeline automation decode reports exact fields and preserves model failures") {
    auto missing_points = take(serialize_project(automation_project(), builtins())).json;
    const auto points_begin = missing_points.find("\"points\":[");
    const auto points_end = missing_points.find("],\"target\":", points_begin);
    REQUIRE(points_begin != std::string::npos);
    REQUIRE(points_end != std::string::npos);
    missing_points.erase(points_begin, points_end + 2 - points_begin);
    auto missing = deserialize_project(missing_points, builtins());
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error().code == PersistenceErrorCode::MissingField);
    REQUIRE(missing.error().path ==
            "/data/sequences/0/data/tracks/0/data/automation_lanes/0/data/points");

    auto duplicate_point = take(serialize_project(automation_project(), builtins())).json;
    const auto points = duplicate_point.find("\"points\":[");
    const auto second_id = duplicate_point.find("\"id\":\"7\"", points);
    REQUIRE(points != std::string::npos);
    REQUIRE(second_id != std::string::npos);
    duplicate_point.replace(second_id, std::string_view("\"id\":\"7\"").size(),
                            "\"id\":\"6\"");
    auto rejected = deserialize_project(duplicate_point, builtins());
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == PersistenceErrorCode::ModelRejected);
    REQUIRE(rejected.error().model_error.has_value());
    REQUIRE(rejected.error().model_error->code == ModelErrorCode::DuplicateItemId);
    REQUIRE(rejected.error().model_error->item == ItemId{6});
}

TEST_CASE("Timeline permanent Track v3 automation fixture re-saves byte-identically") {
    const auto original = fixture("v3/automation-lane.json");
    auto decoded_result = deserialize_project(original, builtins());
    const auto decoded_code = decoded_result ? -1 : static_cast<int>(decoded_result.error().code);
    const auto decoded_path = decoded_result ? std::string{} : decoded_result.error().path;
    INFO("code=" << decoded_code << " path=" << decoded_path);
    REQUIRE(decoded_result);
    const auto decoded = std::move(decoded_result).value();
    const auto& track = decoded.sequences()[0].tracks()[0];
    REQUIRE(track.automation_lanes().size() == 1);
    REQUIRE(track.automation_lanes()[0].curve().points()[1].interpolation ==
            AutomationInterpolation::Hold);
    // The permanent fixture is canonical: re-saving it reproduces the exact bytes,
    // and the automation point's lane ownership survives via parent_id.
    REQUIRE(decoded.locate({6})->parent_id == ItemId{5});
    REQUIRE(take(serialize_project(decoded, builtins())).json == original);
}

TEST_CASE("Timeline v3 decode rejects an automation point whose parent_id is stripped") {
    // A hand-edited v3 file that drops an automation point's parent_id must fail
    // closed: an AutomationPoint's lane is not derivable from its coordinates, so
    // the immediate_parent_id fallback yields no owner and the point would be
    // orphaned. Deserialize must reject it rather than silently load it.
    auto canonical = take(serialize_project(automation_project(), builtins())).json;
    // Point {6}'s identity entry carries "parent_id":"5"; remove exactly that pair.
    constexpr std::string_view point_parent = R"("id":"6","kind":"automation_point","parent_id":"5",)";
    const auto position = canonical.find(point_parent);
    REQUIRE(position != std::string::npos);
    const std::string_view without_parent = R"("id":"6","kind":"automation_point",)";
    canonical.replace(position, point_parent.size(), without_parent);
    auto rejected = deserialize_project(canonical, builtins());
    REQUIRE_FALSE(rejected.has_value());
    // The orphaned point is caught by the model's identity ownership validation.
    REQUIRE(rejected.error().code == PersistenceErrorCode::ModelRejected);
}

TEST_CASE("Timeline v2 track document upgrades clean under the bumped v3 policy") {
    // Bumping current_version to 3 must not reject existing v2 documents:
    // requires_automation(2) is false, so a v2 track carries no automation and
    // migrates to v3 (gaining an empty automation_lanes array) without error.
    const auto registry = builtins();
    const std::string v2 =
        R"({"data":{"clips":[],"device_chain":[],"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":2})";
    const auto upgraded =
        take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 2, 3, v2));
    REQUIRE(upgraded.find("\"automation_lanes\":[]") != std::string::npos);
    REQUIRE(upgraded.find("\"version\":3") != std::string::npos);
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

TEST_CASE("Timeline Track migrations validate each source version's field shape") {
    const auto registry = builtins();
    const auto reject = [&](std::uint32_t from, std::uint32_t to, std::string_view source) {
        auto result = registry.migrate(SchemaDomain::Document, "pulp.timeline.track", from, to,
                                       source);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == PersistenceErrorCode::MigrationFailed);
    };

    reject(1, 2,
           R"({"data":{"clips":[],"device_chain":[],"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":1})");
    reject(2, 3,
           R"({"data":{"clips":[],"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":2})");
    reject(2, 3,
           R"({"data":{"automation_lanes":[],"clips":[],"device_chain":[],"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":2})");
    reject(2, 1,
           R"({"data":{"automation_lanes":[],"clips":[],"device_chain":[],"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":2})");
    reject(3, 2,
           R"({"data":{"automation_lanes":[],"clips":[],"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":3})");
    reject(3, 2,
           R"({"data":{"automation_lanes":[],"clips":[],"device_chain":null,"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":3})");
    reject(3, 2,
           R"({"data":{"clips":[],"device_chain":[],"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":3})");
    reject(3, 2,
           R"({"data":{"automation_lanes":null,"clips":[],"device_chain":[],"id":"3","name":"track"},"type_name":"pulp.timeline.track","version":3})");
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

TEST_CASE("Timeline automation decode preserves exact child parse errors") {
    const auto snapshot = take(serialize_project(automation_project(), builtins())).json;
    const auto reject_replacement = [&](std::string_view needle, std::string_view replacement,
                                        PersistenceErrorCode code, std::string_view path) {
        auto malformed = snapshot;
        const auto position = malformed.find(needle);
        REQUIRE(position != std::string::npos);
        malformed.replace(position, needle.size(), replacement);
        auto rejected = deserialize_project(malformed, builtins());
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error().code == code);
        REQUIRE(rejected.error().path == path);
    };

    reject_replacement(
        R"("id":"5","points")", R"("id":"05","points")",
        PersistenceErrorCode::InvalidNumber,
        "/data/sequences/0/data/tracks/0/data/automation_lanes/0/data/id");
    reject_replacement(
        R"("device_placement_id":"4")", R"("device_placement_id":"04")",
        PersistenceErrorCode::InvalidNumber,
        "/data/sequences/0/data/tracks/0/data/automation_lanes/0/data/target/data/device_placement_id");
    reject_replacement(
        R"("parameter_id":7)", R"("parameter_id":4294967296)",
        PersistenceErrorCode::InvalidNumber,
        "/data/sequences/0/data/tracks/0/data/automation_lanes/0/data/target/data/parameter_id");
}
