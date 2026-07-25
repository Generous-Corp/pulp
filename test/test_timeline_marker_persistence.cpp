#include "support/timeline_persistence_test_support.hpp"

#include <fstream>
#include <string_view>

namespace {

std::string marker_fixture(std::string_view relative_path) {
    std::ifstream stream(std::string(PULP_TIMELINE_FIXTURE_DIR) + "/" + std::string(relative_path),
                         std::ios::binary);
    REQUIRE(stream.good());
    std::string contents{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    while (!contents.empty() && (contents.back() == '\n' || contents.back() == '\r'))
        contents.pop_back();
    return contents;
}

Project annotated_project() {
    auto track = take(Track::create({3}, "root track", {}));
    auto sequence = take(Sequence::create(
        {2}, "root", TickDuration{8 * kTicksPerQuarter}, std::nullopt, {track},
        {SequenceMarker{{4}, "intro", {0}}, SequenceMarker{{5}, "drop", {4 * kTicksPerQuarter}}},
        {SequenceRegion{{6}, "verse", {0}, {4 * kTicksPerQuarter}},
         SequenceRegion{{7}, "hook", {kTicksPerQuarter}, {2 * kTicksPerQuarter}}}));
    return take(Project::create(ProjectInput{{1}, "v4-markers", 8, {2}, {}, {sequence}}));
}

// The lone sequence envelope inside a whole-project snapshot, as raw bytes.
std::string sequence_envelope(const std::string& snapshot) {
    const auto parsed = take(parse_json(snapshot));
    const auto* data = parsed->root().find("data");
    REQUIRE(data != nullptr);
    const auto* sequences = data->find("sequences");
    REQUIRE(sequences != nullptr);
    REQUIRE(sequences->array.size() == 1);
    return std::string(parsed->raw(sequences->array[0]));
}

} // namespace

TEST_CASE("Timeline markers and regions round trip through a snapshot") {
    const auto registry = builtins();
    const auto encoded = take(serialize_project(annotated_project(), registry));
    const auto decoded = take(deserialize_project(encoded.json, registry));

    const auto* sequence = decoded.find_sequence({2});
    REQUIRE(sequence != nullptr);
    REQUIRE(sequence->markers().size() == 2);
    REQUIRE(sequence->markers()[0].id == ItemId{4});
    REQUIRE(sequence->markers()[0].name == "intro");
    REQUIRE(sequence->markers()[0].position == TickPosition{0});
    REQUIRE(sequence->markers()[1].name == "drop");
    REQUIRE(sequence->markers()[1].position == TickPosition{4 * kTicksPerQuarter});
    REQUIRE(sequence->regions().size() == 2);
    REQUIRE(sequence->regions()[0].id == ItemId{6});
    REQUIRE(sequence->regions()[0].duration == TickDuration{4 * kTicksPerQuarter});
    REQUIRE(sequence->regions()[1].name == "hook");
    REQUIRE(sequence->regions()[1].position == TickPosition{kTicksPerQuarter});

    // The overlapping pair survives: "hook" lies inside "verse".
    REQUIRE(sequence->regions()[1].position.value >= sequence->regions()[0].position.value);
    REQUIRE(sequence->regions()[1].position.value + sequence->regions()[1].duration.value <=
            sequence->regions()[0].position.value + sequence->regions()[0].duration.value);

    // Every annotation is a registered identity the project can locate.
    for (const auto id : {ItemId{4}, ItemId{5}}) {
        REQUIRE(decoded.locate(id));
        REQUIRE(decoded.locate(id)->kind == ItemKind::Marker);
        REQUIRE(decoded.locate(id)->parent_id == ItemId{2});
    }
    for (const auto id : {ItemId{6}, ItemId{7}}) {
        REQUIRE(decoded.locate(id));
        REQUIRE(decoded.locate(id)->kind == ItemKind::Region);
    }
    REQUIRE(take(serialize_project(decoded, registry)).json == encoded.json);
}

TEST_CASE("Timeline marker fixture re-saves byte for byte") {
    const auto registry = builtins();
    const auto original = marker_fixture("v4/sequence-markers.json");
    const auto decoded = take(deserialize_project(original, registry));
    REQUIRE(take(serialize_project(decoded, registry)).json == original);
}

TEST_CASE("Timeline sequence upgrades to markers and regions and downgrades back") {
    const auto registry = builtins();
    DecodeLimits limits;

    // A version-one project predates markers entirely; its sequence envelope is
    // the exact payload the upgrade has to handle.
    const auto legacy = sequence_envelope(marker_fixture("v1/minimal.json"));
    REQUIRE(legacy.find("\"markers\"") == std::string::npos);
    REQUIRE(legacy.find("\"version\":1") != std::string::npos);

    const auto upgraded = take(
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 1, 2, legacy, limits));
    REQUIRE(upgraded.find(R"("markers":[])") != std::string::npos);
    REQUIRE(upgraded.find(R"("regions":[])") != std::string::npos);
    REQUIRE(upgraded.find("\"version\":2") != std::string::npos);
    // The upgrade is a pure insertion: canonical member order is preserved.
    REQUIRE(upgraded.find(R"("id":"2","markers":[],"musical_duration")") != std::string::npos);
    REQUIRE(upgraded.find(R"("name":"root","regions":[],"tracks")") != std::string::npos);

    const auto downgraded = take(
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 2, 1, upgraded, limits));
    REQUIRE(downgraded == legacy);
}

TEST_CASE("Timeline sequence downgrade refuses to discard authored annotations") {
    const auto registry = builtins();
    DecodeLimits limits;
    const auto populated =
        sequence_envelope(take(serialize_project(annotated_project(), registry)).json);
    auto refused =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 2, 1, populated, limits);
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().code == PersistenceErrorCode::MigrationFailed);
}

TEST_CASE("Timeline version-one sequences decode with no markers or regions") {
    const auto registry = builtins();
    const auto decoded = take(deserialize_project(marker_fixture("v1/minimal.json"), registry));
    const auto* sequence = decoded.find_sequence({2});
    REQUIRE(sequence != nullptr);
    REQUIRE(sequence->markers().empty());
    REQUIRE(sequence->regions().empty());
    // Re-saving lifts the sequence to the current schema version.
    const auto resaved = take(serialize_project(decoded, registry)).json;
    REQUIRE(resaved.find(R"("markers":[],"musical_duration")") != std::string::npos);
    REQUIRE(resaved.find(R"("type_name":"pulp.timeline.sequence","version":2)") !=
            std::string::npos);
}

TEST_CASE("Timeline snapshots reject malformed marker and region payloads") {
    const auto registry = builtins();
    const auto snapshot = take(serialize_project(annotated_project(), registry)).json;

    // A duplicated marker identity must be rejected, not silently deduplicated.
    auto duplicate = snapshot;
    const auto drop = duplicate.find(R"({"data":{"id":"5","name":"drop","position":)");
    REQUIRE(drop != std::string::npos);
    duplicate.replace(drop, std::string_view(R"({"data":{"id":"5")").size(),
                      R"({"data":{"id":"4")");
    auto rejected_duplicate = deserialize_project(duplicate, registry);
    REQUIRE_FALSE(rejected_duplicate);
    REQUIRE(rejected_duplicate.error().code == PersistenceErrorCode::ModelRejected);
    REQUIRE(rejected_duplicate.error().model_error->code == ModelErrorCode::DuplicateItemId);

    // A marker beyond the sequence's musical duration must be rejected.
    auto out_of_bounds = snapshot;
    const auto position = out_of_bounds.find(R"("name":"drop","position":"2822400")");
    REQUIRE(position != std::string::npos);
    out_of_bounds.replace(position,
                          std::string_view(R"("name":"drop","position":"2822400")").size(),
                          R"("name":"drop","position":"9999999")");
    auto rejected_bounds = deserialize_project(out_of_bounds, registry);
    REQUIRE_FALSE(rejected_bounds);
    REQUIRE(rejected_bounds.error().code == PersistenceErrorCode::ModelRejected);
    REQUIRE(rejected_bounds.error().model_error->code == ModelErrorCode::InvalidMarker);

    // A version-one sequence carrying markers is a contradiction, not a hint.
    auto mismatched = snapshot;
    const auto version = mismatched.find(R"("type_name":"pulp.timeline.sequence","version":2)");
    REQUIRE(version != std::string::npos);
    mismatched.replace(
        version, std::string_view(R"("type_name":"pulp.timeline.sequence","version":2)").size(),
        R"("type_name":"pulp.timeline.sequence","version":1)");
    auto rejected_version = deserialize_project(mismatched, registry);
    REQUIRE_FALSE(rejected_version);
    REQUIRE(rejected_version.error().code == PersistenceErrorCode::InvalidSchema);
}
