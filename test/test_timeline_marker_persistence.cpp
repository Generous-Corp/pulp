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

// Mirrors v4/sequence-markers.json: one coloured and one uncoloured annotation
// of each kind, so both encoder branches are exercised, plus a session origin.
Project annotated_project() {
    auto track = take(Track::create({3}, "root track", {}));
    auto sequence = take(Sequence::create(
        {2}, "root", TickDuration{8 * kTicksPerQuarter}, std::nullopt, {track},
        {SequenceMarker{{4}, "intro", {0}, 0xff3366ffu},
         SequenceMarker{{5}, "drop", {4 * kTicksPerQuarter}, std::nullopt}},
        {SequenceRegion{{6}, "verse", {0}, {4 * kTicksPerQuarter}, std::nullopt},
         SequenceRegion{{7}, "hook", {kTicksPerQuarter}, {2 * kTicksPerQuarter}, 0x00c08040u}}));
    ProjectInput input;
    input.id = {1};
    input.name = "v4-markers";
    input.next_item_id = 8;
    input.root_sequence_id = {2};
    input.sequences = {sequence};
    input.session_start = SessionStart{SamplePosition{172'800'000}, RationalRate{48'000, 1}};
    return take(Project::create(std::move(input)));
}

// A project whose only distinction from annotated_project() is that it declares
// no session origin, so the project envelope's optional member is absent.
Project project_without_session_start() {
    auto track = take(Track::create({3}, "root track", {}));
    auto sequence = take(Sequence::create({2}, "root", TickDuration{8 * kTicksPerQuarter},
                                          std::nullopt, {track}, {}, {}));
    return take(Project::create(ProjectInput{{1}, "no-origin", 4, {2}, {}, {sequence}}));
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

    // Colour is optional per annotation: a set value survives exactly and an
    // unset one stays unset rather than defaulting to an opaque black.
    REQUIRE(sequence->markers()[0].color == std::optional<std::uint32_t>{0xff3366ffu});
    REQUIRE_FALSE(sequence->markers()[1].color.has_value());
    REQUIRE_FALSE(sequence->regions()[0].color.has_value());
    REQUIRE(sequence->regions()[1].color == std::optional<std::uint32_t>{0x00c08040u});

    // The session origin is an exact sample offset on its own rational rate.
    REQUIRE(decoded.session_start().has_value());
    REQUIRE(decoded.session_start()->start == SamplePosition{172'800'000});
    REQUIRE(decoded.session_start()->sample_rate == RationalRate{48'000, 1});

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

TEST_CASE("Timeline marker fixture matches the combined current sequence schema") {
    const auto registry = builtins();
    const auto original = marker_fixture("v4/sequence-markers.json");
    const auto decoded = take(deserialize_project(original, registry));
    const auto resaved = take(serialize_project(decoded, registry)).json;
    REQUIRE(resaved == original);
    REQUIRE(resaved.find(R"("type_name":"pulp.timeline.sequence","version":4)") !=
            std::string::npos);
    REQUIRE(resaved.find(R"("groove":{"name":"","step":"0","steps":[])") != std::string::npos);
    REQUIRE(resaved.find(R"("scenes":[])") != std::string::npos);
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
    // The chain walks 4 -> 3 (the straight groove and empty scene list drop
    // cleanly), then 3 -> 2 (the empty chord lane drops cleanly), and finally
    // refuses at 2 -> 1, where the authored annotations would be lost.
    auto refused =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 4, 1, populated, limits);
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
    REQUIRE(resaved.find(R"("type_name":"pulp.timeline.sequence","version":4)") !=
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
    const auto version = mismatched.find(R"("type_name":"pulp.timeline.sequence","version":4)");
    REQUIRE(version != std::string::npos);
    mismatched.replace(
        version, std::string_view(R"("type_name":"pulp.timeline.sequence","version":4)").size(),
        R"("type_name":"pulp.timeline.sequence","version":1)");
    auto rejected_version = deserialize_project(mismatched, registry);
    REQUIRE_FALSE(rejected_version);
    REQUIRE(rejected_version.error().code == PersistenceErrorCode::InvalidSchema);
}

TEST_CASE("Timeline project upgrades and downgrades across the session-origin bump") {
    const auto registry = builtins();
    DecodeLimits limits;

    // v1/minimal.json is a whole project envelope that predates the session
    // origin, so it is the exact payload the upgrade has to accept.
    const auto legacy = marker_fixture("v1/minimal.json");
    REQUIRE(legacy.find(R"("type_name":"pulp.timeline.project","version":1)") != std::string::npos);

    const auto upgraded = take(
        registry.migrate(SchemaDomain::Document, "pulp.timeline.project", 1, 2, legacy, limits));
    REQUIRE(upgraded.find(R"("type_name":"pulp.timeline.project","version":2)") !=
            std::string::npos);
    // The session origin is optional, so an upgrade adds no member at all: only
    // the version moves, and every other byte is copied through.
    REQUIRE(upgraded.find(R"("session_start")") == std::string::npos);
    REQUIRE(upgraded.size() == legacy.size());

    const auto downgraded = take(
        registry.migrate(SchemaDomain::Document, "pulp.timeline.project", 2, 1, upgraded, limits));
    REQUIRE(downgraded == legacy);
}

TEST_CASE("Timeline project downgrade refuses to discard a session origin") {
    const auto registry = builtins();
    DecodeLimits limits;
    const auto populated = take(serialize_project(annotated_project(), registry)).json;
    REQUIRE(populated.find(R"("session_start")") != std::string::npos);
    auto refused =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.project", 2, 1, populated, limits);
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().code == PersistenceErrorCode::MigrationFailed);

    // A project that never declared an origin downgrades cleanly.
    const auto plain = take(serialize_project(project_without_session_start(), registry)).json;
    REQUIRE(plain.find(R"("session_start")") == std::string::npos);
    const auto lowered = take(
        registry.migrate(SchemaDomain::Document, "pulp.timeline.project", 2, 1, plain, limits));
    REQUIRE(lowered.find(R"("type_name":"pulp.timeline.project","version":1)") !=
            std::string::npos);
}

TEST_CASE("Timeline snapshots reject malformed session origins and colours") {
    const auto registry = builtins();
    const auto snapshot = take(serialize_project(annotated_project(), registry)).json;

    // A session origin on a version-one project is a contradiction.
    auto mismatched = snapshot;
    const auto version = mismatched.find(R"("type_name":"pulp.timeline.project","version":2)");
    REQUIRE(version != std::string::npos);
    mismatched.replace(
        version, std::string_view(R"("type_name":"pulp.timeline.project","version":2)").size(),
        R"("type_name":"pulp.timeline.project","version":1)");
    auto rejected_version = deserialize_project(mismatched, registry);
    REQUIRE_FALSE(rejected_version);
    REQUIRE(rejected_version.error().code == PersistenceErrorCode::InvalidSchema);

    // A negative origin is not a point on any clock.
    auto negative = snapshot;
    const auto start = negative.find(R"("start":"172800000")");
    REQUIRE(start != std::string::npos);
    negative.replace(start, std::string_view(R"("start":"172800000")").size(),
                     R"("start":"-172800000")");
    auto rejected_negative = deserialize_project(negative, registry);
    REQUIRE_FALSE(rejected_negative);
    REQUIRE(rejected_negative.error().code == PersistenceErrorCode::ModelRejected);
    REQUIRE(rejected_negative.error().model_error->code == ModelErrorCode::InvalidSessionStart);

    // Colour is a whole 32-bit number, not a string and not oversized.
    auto quoted_color = snapshot;
    const auto color = quoted_color.find(R"("color":4281558783)");
    REQUIRE(color != std::string::npos);
    quoted_color.replace(color, std::string_view(R"("color":4281558783)").size(),
                         R"("color":"4281558783")");
    auto rejected_quoted = deserialize_project(quoted_color, registry);
    REQUIRE_FALSE(rejected_quoted);

    auto oversized_color = snapshot;
    oversized_color.replace(color, std::string_view(R"("color":4281558783)").size(),
                            R"("color":4294967296)");
    auto rejected_oversized = deserialize_project(oversized_color, registry);
    REQUIRE_FALSE(rejected_oversized);
}

TEST_CASE("Timeline session origin normalizes its rate and rejects an invalid one") {
    ProjectInput input;
    input.id = {1};
    input.name = "origin";
    input.next_item_id = 3;
    input.root_sequence_id = {2};
    input.sequences = {take(Sequence::create({2}, "root", TickDuration{0}, {}))};

    // The same instant expressed at a scaled rate stores identically.
    input.session_start = SessionStart{SamplePosition{48'000}, RationalRate{96'000, 2}};
    const auto normalized = take(Project::create(input));
    REQUIRE(normalized.session_start()->sample_rate == RationalRate{48'000, 1});

    input.session_start = SessionStart{SamplePosition{0}, RationalRate{0, 1}};
    auto invalid_rate = Project::create(input);
    REQUIRE_FALSE(invalid_rate);
    REQUIRE(invalid_rate.error().code == ModelErrorCode::InvalidSessionStart);

    input.session_start = SessionStart{SamplePosition{-1}, RationalRate{48'000, 1}};
    auto negative = Project::create(input);
    REQUIRE_FALSE(negative);
    REQUIRE(negative.error().code == ModelErrorCode::InvalidSessionStart);
}
