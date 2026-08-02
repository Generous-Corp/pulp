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

TEST_CASE("Timeline v4 marker fixture upgrades without requiring scenes") {
    const auto registry = builtins();
    const auto original = marker_fixture("v4/sequence-markers.json");
    const auto decoded = take(deserialize_project(original, registry));
    const auto resaved = take(serialize_project(decoded, registry)).json;
    REQUIRE(original.find(R"("type_name":"pulp.timeline.sequence","version":4)") !=
            std::string::npos);
    REQUIRE(original.find(R"("scenes")") == std::string::npos);
    REQUIRE(resaved.find(R"("type_name":"pulp.timeline.sequence","version":7)") !=
            std::string::npos);
    REQUIRE(resaved.find(R"("groove":{"name":"","step":"0","steps":[])") != std::string::npos);
    REQUIRE(resaved.find(R"("scenes":[])") != std::string::npos);
    // The fixture recorded no authored order, so re-saving states the identity
    // order of its track list rather than leaving the field empty.
    REQUIRE(resaved.find(R"("scenes":[],"track_order":["3"],"tracks":[)") != std::string::npos);
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
    // The chain walks 7 -> 6 (the section roles drop, and the chord lane states
    // no detail to refuse over), then 6 -> 5 (the authored order is the identity
    // order, so it drops cleanly), then 5 -> 4 (the empty scene list drops
    // cleanly), then 4 -> 3 (the straight groove drops cleanly), then 3 -> 2
    // (the empty chord lane drops cleanly), and finally refuses at 2 -> 1,
    // where the authored annotations would be lost.
    auto refused =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 7, 1, populated, limits);
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
    REQUIRE(resaved.find(R"("type_name":"pulp.timeline.sequence","version":7)") !=
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
    const auto version = mismatched.find(R"("type_name":"pulp.timeline.sequence","version":7)");
    REQUIRE(version != std::string::npos);
    mismatched.replace(
        version, std::string_view(R"("type_name":"pulp.timeline.sequence","version":7)").size(),
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
        registry.migrate(SchemaDomain::Document, "pulp.timeline.project", 3, 1, populated, limits);
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().code == PersistenceErrorCode::MigrationFailed);

    // A project that never declared an origin downgrades cleanly.
    const auto plain = take(serialize_project(project_without_session_start(), registry)).json;
    REQUIRE(plain.find(R"("session_start")") == std::string::npos);
    const auto lowered = take(
        registry.migrate(SchemaDomain::Document, "pulp.timeline.project", 3, 1, plain, limits));
    REQUIRE(lowered.find(R"("type_name":"pulp.timeline.project","version":1)") !=
            std::string::npos);
}

TEST_CASE("Timeline snapshots reject malformed session origins and colours") {
    const auto registry = builtins();
    const auto snapshot = take(serialize_project(annotated_project(), registry)).json;

    // A session origin on a version-one project is a contradiction.
    auto mismatched = snapshot;
    const auto version = mismatched.find(R"("type_name":"pulp.timeline.project","version":3)");
    REQUIRE(version != std::string::npos);
    mismatched.replace(
        version, std::string_view(R"("type_name":"pulp.timeline.project","version":3)").size(),
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

TEST_CASE("a section role round trips on regions and defaults on older documents",
          "[timeline][marker][persistence]") {
    const auto registry = builtins();
    auto track = take(Track::create({3}, "root track", {}));
    auto sequence = take(Sequence::create(
        {2}, "root", TickDuration{8 * kTicksPerQuarter}, std::nullopt, {track}, {},
        {SequenceRegion{{6}, "A", {0}, {4 * kTicksPerQuarter}, std::nullopt, SectionRole::Verse},
         SequenceRegion{{7}, "B", {4 * kTicksPerQuarter}, {2 * kTicksPerQuarter}, 0x00c08040u,
                        SectionRole::Chorus},
         SequenceRegion{{8}, "?", {6 * kTicksPerQuarter}, {kTicksPerQuarter}, std::nullopt}}));
    const auto project =
        take(Project::create(ProjectInput{{1}, "roles", 9, {2}, {}, {sequence}}));

    const auto encoded = take(serialize_project(project, registry)).json;
    const auto decoded = take(deserialize_project(encoded, registry));
    const auto regions = decoded.find_sequence({2})->regions();
    REQUIRE(regions.size() == 3);
    REQUIRE(regions[0].role == SectionRole::Verse);
    REQUIRE(regions[1].role == SectionRole::Chorus);
    // A region that claims no part states Unspecified rather than nothing, so
    // the absence is a value the round trip carries rather than a gap.
    REQUIRE(regions[2].role == SectionRole::Unspecified);
    REQUIRE(take(serialize_project(decoded, registry)).json == encoded);

    // A region whose role is outside the vocabulary is refused, not clamped.
    auto invalid = SequenceRegion{{6}, "A", {0}, {kTicksPerQuarter}, std::nullopt};
    invalid.role = static_cast<SectionRole>(99);
    const auto rejected = Sequence::create({2}, "root", TickDuration{8 * kTicksPerQuarter},
                                           std::nullopt, {track}, {}, {invalid});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ModelErrorCode::InvalidRegion);

    // The v4 fixture predates roles entirely; every region it carries upgrades
    // to Unspecified, which is what a reader without roles already saw.
    const auto legacy = take(deserialize_project(marker_fixture("v4/sequence-markers.json"),
                                                 registry));
    for (const auto& region : legacy.find_sequence({2})->regions())
        REQUIRE(region.role == SectionRole::Unspecified);
}

TEST_CASE("the section-role downgrade drops the role and keeps the region",
          "[timeline][marker][migration]") {
    const auto registry = builtins();
    DecodeLimits limits;
    auto track = take(Track::create({3}, "root track", {}));
    auto sequence = take(Sequence::create(
        {2}, "root", TickDuration{8 * kTicksPerQuarter}, std::nullopt, {track}, {},
        {SequenceRegion{{6}, "A", {0}, {4 * kTicksPerQuarter}, 0x00c08040u,
                        SectionRole::Bridge}}));
    const auto project = take(Project::create(ProjectInput{{1}, "roles", 7, {2}, {}, {sequence}}));
    const auto encoded = take(serialize_project(project, registry)).json;

    const auto envelope = sequence_envelope(encoded);

    // Unlike the chord detail, a role is an annotation beside a name the older
    // reader still sees, so the downgrade drops it instead of refusing.
    const auto lowered = take(
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 7, 6, envelope, limits));
    REQUIRE(lowered.find(R"("role")") == std::string::npos);
    // The region itself survives: same identity, name, colour, and span. A v6
    // reader sees the same music with one fewer label on it.
    REQUIRE(lowered.find(R"("name":"A")") != std::string::npos);
    REQUIRE(lowered.find(R"("color":12615744)") != std::string::npos);
    REQUIRE(lowered.find(R"("id":"6")") != std::string::npos);
    const auto span = encoded.find(R"("duration":")");
    REQUIRE(span != std::string::npos);
    REQUIRE(lowered.find(encoded.substr(span, encoded.find(',', span) - span)) !=
            std::string::npos);

    // The upgrade puts the default back, so a v6 document reads as a region
    // that claims no part rather than as a broken one.
    const auto raised = take(
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 6, 7, lowered, limits));
    REQUIRE(raised.find(R"("role":"unspecified")") != std::string::npos);
}

TEST_CASE("a sequence older than section roles may not carry one",
          "[timeline][marker][persistence]") {
    const auto registry = builtins();
    auto track = take(Track::create({3}, "root track", {}));
    auto sequence = take(Sequence::create(
        {2}, "root", TickDuration{8 * kTicksPerQuarter}, std::nullopt, {track}, {},
        {SequenceRegion{{6}, "A", {0}, {4 * kTicksPerQuarter}, std::nullopt,
                        SectionRole::Verse}}));
    auto mislabelled =
        take(serialize_project(take(Project::create(ProjectInput{{1}, "roles", 7, {2}, {},
                                                                {sequence}})),
                               registry))
            .json;
    constexpr std::string_view stamp = R"("type_name":"pulp.timeline.sequence","version":7)";
    const auto at = mislabelled.find(stamp);
    REQUIRE(at != std::string::npos);
    mislabelled.replace(at, stamp.size(),
                        R"("type_name":"pulp.timeline.sequence","version":6)");
    auto rejected = deserialize_project(mislabelled, registry);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == PersistenceErrorCode::InvalidSchema);
}
