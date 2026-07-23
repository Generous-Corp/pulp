#include "support/timeline_persistence_test_support.hpp"

#include <pulp/timeline/document_session.hpp>

#include <array>

namespace {

const SchemaReleaseMap& release(std::string_view label) {
    const auto* found = find_builtin_timeline_schema_release(label);
    REQUIRE(found != nullptr);
    return *found;
}

Project project_with_devices() {
    auto track =
        take(Track::create(TrackInput{.id = {3}, .name = "track", .device_chain = {{{4}}}}));
    auto sequence = take(Sequence::create({2}, "root", TickDuration{100}, {track}));
    return take(Project::create(ProjectInput{{1}, "devices", 5, {2}, {}, {sequence}}));
}

Project project_with_automation() {
    auto curve =
        take(AutomationCurve::create({{{6}, {0}, 0.25f, AutomationInterpolation::Continuous}}));
    auto lane = take(AutomationLane::create({5}, DeviceParameterTarget{{4}, 7}, std::move(curve)));
    auto track = take(Track::create(TrackInput{.id = {3},
                                               .name = "track",
                                               .device_chain = {{{4}}},
                                               .automation_lanes = {std::move(lane)}}));
    auto sequence = take(Sequence::create({2}, "root", TickDuration{100}, {track}));
    return take(Project::create(ProjectInput{{1}, "automation", 7, {2}, {}, {sequence}}));
}

Project project_with_takes() {
    auto recorded = take(Take::create({6}, MediaRef{{5}, {0}, 100}, {0}, RationalRate{48'000, 1}));
    auto lane = take(TakeLane::create({4}, "comp", {recorded}));
    auto track = take(Track::create(
        TrackInput{.id = {3}, .name = "track", .take_lanes = {lane}, .record_armed = true}));
    auto sequence = take(Sequence::create({2}, "root", TickDuration{100}, {track}));
    MediaAsset asset{{5}, "audio.wav", 100, {48'000, 1}, hash('a'), AssetStoragePolicy::External,
                     {},  {}};
    return take(Project::create(ProjectInput{{1}, "takes", 7, {2}, {asset}, {sequence}}));
}

} // namespace

TEST_CASE("Timeline release maps name exact shipped schema sets") {
    const auto releases = builtin_timeline_schema_releases();
    REQUIRE(releases.size() == 3);
    REQUIRE(releases[0].release_label == "v0.736.0");
    REQUIRE(releases[1].release_label == "v0.744.0");
    REQUIRE(releases[2].release_label == "v0.748.0");
    REQUIRE(releases[0].versions.size() == 9);
    REQUIRE(releases[1].versions.size() == 10);
    REQUIRE(releases[2].versions.size() == 12);

    REQUIRE(releases[0].find(SchemaDomain::Document, "pulp.timeline.track")->version == 1);
    REQUIRE(releases[1].find(SchemaDomain::Document, "pulp.timeline.track")->version == 2);
    REQUIRE(releases[2].find(SchemaDomain::Document, "pulp.timeline.track")->version == 3);
    REQUIRE(releases[0].find(SchemaDomain::Document, "pulp.timeline.device_placement") == nullptr);
    REQUIRE(releases[1].find(SchemaDomain::Document, "pulp.timeline.automation_lane") == nullptr);
    REQUIRE(releases[2].find(SchemaDomain::Document, "pulp.timeline.take_lane") == nullptr);
    REQUIRE(find_builtin_timeline_schema_release("v0.747.0") == nullptr);
}

TEST_CASE("Timeline snapshots target each shipped release canonically") {
    const auto registry = builtins();
    const auto project = project_with();
    struct Expectation {
        std::string_view label;
        std::uint32_t track_version;
    };
    constexpr std::array expectations{
        Expectation{"v0.736.0", 1},
        Expectation{"v0.744.0", 2},
        Expectation{"v0.748.0", 3},
    };
    for (const auto& expectation : expectations) {
        INFO(expectation.label);
        const auto snapshot =
            take(serialize_project_for_release(project, registry, release(expectation.label)));
        REQUIRE(snapshot.json.find("\"type_name\":\"pulp.timeline.track\",\"version\":" +
                                   std::to_string(expectation.track_version)) != std::string::npos);
        const auto decoded = take(deserialize_project(snapshot.json, registry));
        REQUIRE(decoded.name() == project.name());
        REQUIRE(take(serialize_project_for_release(decoded, registry, release(expectation.label)))
                    .json == snapshot.json);
    }
}

TEST_CASE("Timeline release export rejects lossy feature downgrades") {
    const auto registry = builtins();
    const auto devices =
        serialize_project_for_release(project_with_devices(), registry, release("v0.736.0"));
    REQUIRE_FALSE(devices.has_value());
    REQUIRE(devices.error().code == PersistenceErrorCode::MigrationFailed);

    const auto automation =
        serialize_project_for_release(project_with_automation(), registry, release("v0.744.0"));
    REQUIRE_FALSE(automation.has_value());
    REQUIRE(automation.error().code == PersistenceErrorCode::MigrationFailed);

    const auto takes =
        serialize_project_for_release(project_with_takes(), registry, release("v0.748.0"));
    REQUIRE_FALSE(takes.has_value());
    REQUIRE(takes.error().code == PersistenceErrorCode::MigrationFailed);
}

TEST_CASE("Timeline release export removes only unsupported inactive identity tombstones") {
    const auto registry = builtins();
    auto session = take(DocumentSession::create(project_with_automation()));
    auto writer = take(session->register_writer());
    Transaction remove;
    remove.id = writer.allocate_transaction_id();
    remove.expected_revision = session->revision();
    remove.commands.push_back({writer.allocate_command_id(), RemoveAutomationLane{{2}, {3}, {5}}});
    REQUIRE(session->submit(writer, std::move(remove)).has_value());
    REQUIRE_FALSE(session->snapshot()->locate({5})->active);
    REQUIRE_FALSE(session->snapshot()->locate({6})->active);

    const auto snapshot =
        take(serialize_project_for_release(*session->snapshot(), registry, release("v0.744.0")));
    REQUIRE(snapshot.json.find("\"kind\":\"automation_lane\"") == std::string::npos);
    REQUIRE(snapshot.json.find("\"kind\":\"automation_point\"") == std::string::npos);
    const auto decoded = take(deserialize_project(snapshot.json, registry));
    REQUIRE(decoded.next_item_id() == session->snapshot()->next_item_id());
    REQUIRE_FALSE(decoded.locate({5}).has_value());
    REQUIRE_FALSE(decoded.locate({6}).has_value());
}

TEST_CASE("Timeline release export requires an explicit target for encountered extensions") {
    const auto registry = builtins();
    const std::string raw = R"({"data":{"value":"kept"},"type_name":"vendor.future","version":7})";
    auto opaque = take(OpaqueContent::create({"vendor.future", 7}, raw));
    auto result = serialize_project_for_release(project_with(std::move(opaque)), registry,
                                                release("v0.748.0"));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == PersistenceErrorCode::UnsupportedStructuralType);
    REQUIRE(result.error().path.find("/content/type_name") != std::string::npos);
}

TEST_CASE("Timeline release export validates caller-defined maps") {
    const auto registry = builtins();
    std::vector<SchemaVersionTarget> current_versions;
    for (const auto& schema : registry.types())
        current_versions.push_back({schema.domain, schema.type_name, schema.current_version});
    const SchemaReleaseMap current{"current-test", current_versions};
    REQUIRE(take(serialize_project_for_release(project_with_takes(), registry, current)).json ==
            take(serialize_project(project_with_takes(), registry)).json);

    constexpr std::array future{
        SchemaVersionTarget{SchemaDomain::Document, "pulp.timeline.project", 2},
    };
    auto future_result =
        serialize_project_for_release(project_with(), registry, SchemaReleaseMap{"future", future});
    REQUIRE_FALSE(future_result.has_value());
    REQUIRE(future_result.error().code == PersistenceErrorCode::UnsupportedSchemaVersion);

    constexpr std::array duplicate{
        SchemaVersionTarget{SchemaDomain::Document, "pulp.timeline.project", 1},
        SchemaVersionTarget{SchemaDomain::Document, "pulp.timeline.project", 1},
    };
    auto duplicate_result = serialize_project_for_release(project_with(), registry,
                                                          SchemaReleaseMap{"duplicate", duplicate});
    REQUIRE_FALSE(duplicate_result.has_value());
    REQUIRE(duplicate_result.error().code == PersistenceErrorCode::InvalidSchema);

    SchemaRegistryBuilder incomplete_builder;
    for (const auto& schema : registry.types()) {
        auto incomplete = schema;
        if (incomplete.type_name == "pulp.timeline.track")
            incomplete.downgrades.clear();
        REQUIRE(incomplete_builder.register_type(std::move(incomplete)).has_value());
    }
    const auto incomplete_registry = take(std::move(incomplete_builder).build());
    auto missing_path =
        serialize_project_for_release(project_with(), incomplete_registry, release("v0.748.0"));
    REQUIRE_FALSE(missing_path.has_value());
    REQUIRE(missing_path.error().code == PersistenceErrorCode::MigrationPathMissing);
}
