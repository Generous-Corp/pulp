#include "support/timeline_persistence_test_support.hpp"

#include <pulp/timeline/document_session.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

SequenceMarker musical_marker(std::uint64_t id, std::int64_t tick, std::string name) {
    return {{id}, MarkerTypeId::cue(), std::move(name), MusicalSequencePoint{{tick}}};
}

Project annotation_project() {
    auto type = take(MarkerTypeId::create("vendor.marker.chord"));
    auto sequence = take(Sequence::create(SequenceInput{
        .id = {2},
        .name = "root",
        .musical_duration = TickDuration{1000},
        .markers =
            {
                musical_marker(3, 100, "cue"),
                {{4}, std::move(type), "chord", MusicalSequencePoint{{200}}},
            },
        .regions = {{{5}, "verse", MusicalSequenceRange{{100}, {400}}}},
    }));
    return take(Project::create({{1}, "project", 6, {2}, {}, {std::move(sequence)}}));
}

constexpr std::string_view kSequenceV1 =
    R"({"data":{"absolute_duration":null,"id":"2","musical_duration":"100","name":"root","tracks":[]},"type_name":"pulp.timeline.sequence","version":1})";

constexpr std::string_view kSequenceV2Empty =
    R"({"data":{"absolute_duration":null,"id":"2","markers":[],"musical_duration":"100","name":"root","regions":[],"tracks":[]},"type_name":"pulp.timeline.sequence","version":2})";

} // namespace

TEST_CASE("Sequence annotations persist canonically with identity ownership",
          "[timeline][persistence][annotation]") {
    const auto registry = builtins();
    auto serialized = serialize_project(annotation_project(), registry);
    REQUIRE(serialized);
    CHECK(serialized->json.find("\"type\":\"vendor.marker.chord\"") != std::string::npos);
    CHECK(serialized->json.find("\"version\":2") != std::string::npos);

    auto decoded = deserialize_project(serialized->json, registry);
    REQUIRE(decoded);
    const auto* sequence = decoded->find_sequence({2});
    REQUIRE(sequence);
    REQUIRE(sequence->markers().size() == 2);
    CHECK(sequence->markers()[1].type.value() == "vendor.marker.chord");
    REQUIRE(sequence->regions().size() == 1);
    REQUIRE(decoded->locate({3}));
    CHECK(decoded->locate({3})->kind == ItemKind::SequenceMarker);
    REQUIRE(decoded->locate({5}));
    CHECK(decoded->locate({5})->kind == ItemKind::SequenceRegion);

    auto second = serialize_project(decoded.value(), registry);
    REQUIRE(second);
    CHECK(second->json == serialized->json);
}

TEST_CASE("Sequence schema migration adds annotations and only drops empty arrays",
          "[timeline][persistence][annotation]") {
    const auto registry = builtins();
    auto upgraded =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 1, 2, kSequenceV1);
    REQUIRE(upgraded);
    CHECK(upgraded.value() == kSequenceV2Empty);
    auto downgraded =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 2, 1, upgraded.value());
    REQUIRE(downgraded);
    CHECK(downgraded.value() == kSequenceV1);

    auto nonempty = std::string(kSequenceV2Empty);
    const auto marker =
        R"({"data":{"id":"3","name":"cue","point":{"kind":"musical","position_ticks":"0"},"type":"pulp.marker.cue"},"type_name":"pulp.timeline.sequence_marker","version":1})";
    nonempty.replace(nonempty.find("\"markers\":[]"), std::string_view("\"markers\":[]").size(),
                     "\"markers\":[" + std::string(marker) + "]");
    auto rejected =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 2, 1, nonempty);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == PersistenceErrorCode::MigrationFailed);
}

TEST_CASE("Sequence annotation decode limits reject before document construction",
          "[timeline][persistence][annotation]") {
    const auto registry = builtins();
    auto serialized = serialize_project(annotation_project(), registry);
    REQUIRE(serialized);
    auto limits = DecodeLimits{};
    limits.max_sequence_markers = 1;
    auto rejected = deserialize_project(serialized->json, registry, limits);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == PersistenceErrorCode::LimitExceeded);
    CHECK(rejected.error().actual == 2);
    CHECK(rejected.error().limit == 1);
    CHECK(rejected.error().path == "/data/sequences/0/data/markers");

    auto web = DecodeLimits::web_defaults();
    CHECK(web.max_sequence_markers < DecodeLimits{}.max_sequence_markers);
    CHECK(web.max_sequence_regions < DecodeLimits{}.max_sequence_regions);
}

TEST_CASE("Sequence annotation decode preserves the failing field path",
          "[timeline][persistence][annotation]") {
    const auto registry = builtins();
    auto serialized = serialize_project(annotation_project(), registry);
    REQUIRE(serialized);
    const auto offset = serialized->json.find("vendor.marker.chord");
    REQUIRE(offset != std::string::npos);
    serialized->json.replace(offset, std::string_view("vendor.marker.chord").size(), "Invalid");

    auto rejected = deserialize_project(serialized->json, registry);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == PersistenceErrorCode::InvalidSchema);
    CHECK(rejected.error().path == "/data/sequences/0/data/markers/1/data/type");
}

TEST_CASE("Sequence annotation structural errors identify the exact field",
          "[timeline][persistence][annotation]") {
    const auto registry = builtins();
    auto serialized = serialize_project(annotation_project(), registry);
    REQUIRE(serialized);
    const auto erase_through = [](std::string value, std::string_view first,
                                  std::string_view next) {
        const auto begin = value.find(first);
        REQUIRE(begin != std::string::npos);
        const auto end = value.find(next, begin);
        REQUIRE(end != std::string::npos);
        value.erase(begin, end - begin);
        return value;
    };
    const auto sequence_v1 = [](std::string value) {
        constexpr std::string_view from =
            R"("type_name":"pulp.timeline.sequence","version":2)";
        constexpr std::string_view to =
            R"("type_name":"pulp.timeline.sequence","version":1)";
        const auto offset = value.find(from);
        REQUIRE(offset != std::string::npos);
        value.replace(offset, from.size(), to);
        return value;
    };
    const auto reject_at = [&](const std::string& json, std::string_view path) {
        auto rejected = preflight_timeline_structure(json, DecodeLimits{});
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == PersistenceErrorCode::InvalidSchema);
        CHECK(rejected.error().path == path);
    };

    const auto without_markers =
        erase_through(serialized->json, R"("markers":)", R"("musical_duration":)");
    const auto without_regions =
        erase_through(serialized->json, R"("regions":)", R"("tracks":)");
    auto without_tracks = serialized->json;
    const auto tracks = without_tracks.find(R"(,"tracks":[])");
    REQUIRE(tracks != std::string::npos);
    without_tracks.erase(tracks, std::string_view(R"(,"tracks":[])").size());

    reject_at(without_markers, "/data/sequences/0/data/markers");
    reject_at(without_regions, "/data/sequences/0/data/regions");
    reject_at(without_tracks, "/data/sequences/0/data/tracks");
    reject_at(sequence_v1(without_regions), "/data/sequences/0/data/markers");
    reject_at(sequence_v1(without_markers), "/data/sequences/0/data/regions");
}

TEST_CASE("Sequence annotation tombstones survive checkpoint serialization",
          "[timeline][persistence][annotation]") {
    auto session = std::move(DocumentSession::create(annotation_project())).value();
    auto writer = std::move(session->register_writer()).value();
    Transaction remove;
    remove.id = writer.allocate_transaction_id();
    remove.commands.push_back({writer.allocate_command_id(), RemoveSequenceMarker{{2}, {3}}});
    REQUIRE(session->submit(writer, std::move(remove)));
    const auto registry = builtins();
    auto encoded = serialize_project(*session->snapshot(), registry);
    REQUIRE(encoded);
    auto decoded = deserialize_project(encoded->json, registry);
    REQUIRE(decoded);
    REQUIRE(decoded->locate({3}));
    CHECK_FALSE(decoded->locate({3})->active);
    CHECK(decoded->locate({3})->kind == ItemKind::SequenceMarker);
    CHECK(decoded->locate({3})->sequence_id == ItemId{2});
}
