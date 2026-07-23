#include "support/timeline_persistence_test_support.hpp"

namespace {

AudioLoopInfo valid_loop_info() {
    return AudioLoopInfo{
        .musical_length = TickDuration{7'680},
        .meter = {4, 4},
        .one_shot = false,
        .root_note = 60,
        .active_range = AudioFrameRange{100, 95'900},
        .points =
            {
                {72'000, AudioLoopPointKind::Automatic},
                {24'000, AudioLoopPointKind::Manual},
            },
        .tags = {"warm", "drums"},
    };
}

runtime::Result<Project, ModelError> project_with_loop_info(AudioLoopInfo loop_info) {
    MediaAsset asset{{2}, "loop.wav", 96'000, {48'000, 1}, hash('a'),
                     AssetStoragePolicy::External, {}, {}, std::move(loop_info)};
    auto sequence = take(Sequence::create({3}, "root", TickDuration{7'680}, {}));
    return Project::create(ProjectInput{{1}, "loops", 4, {3}, {std::move(asset)}, {sequence}});
}

const SchemaReleaseMap& release(std::string_view label) {
    const auto* found = find_builtin_timeline_schema_release(label);
    REQUIRE(found != nullptr);
    return *found;
}

} // namespace

TEST_CASE("Audio loop metadata is canonical and round trips") {
    const auto registry = builtins();
    const auto project = take(project_with_loop_info(valid_loop_info()));
    const auto* asset = project.find_asset({2});
    REQUIRE(asset != nullptr);
    REQUIRE(asset->loop_info.has_value());
    const auto& loop = *asset->loop_info;
    REQUIRE(loop.points[0].frame == 24'000);
    REQUIRE(loop.points[1].frame == 72'000);
    REQUIRE(loop.tags == std::vector<std::string>{"drums", "warm"});

    const auto encoded = take(serialize_project(project, registry));
    REQUIRE(encoded.json.find("\"type_name\":\"pulp.timeline.asset\",\"version\":2") !=
            std::string::npos);
    REQUIRE(encoded.json.find("\"musical_length_ticks\":\"7680\"") != std::string::npos);
    REQUIRE(encoded.json.find("\"in_marker_frame\":\"100\"") != std::string::npos);
    REQUIRE(encoded.json.find("\"root_note\":60") != std::string::npos);

    const auto decoded_result = deserialize_project(encoded.json, registry);
    INFO((decoded_result.has_value() ? std::string{} : decoded_result.error().path));
    const auto decoded = take(std::move(decoded_result));
    REQUIRE(decoded.find_asset({2})->loop_info == project.find_asset({2})->loop_info);
    REQUIRE(take(serialize_project(decoded, registry)).json == encoded.json);
}

TEST_CASE("Audio loop metadata rejects invalid musical and frame state") {
    auto loop = valid_loop_info();

    SECTION("non-positive musical length") {
        loop.musical_length = TickDuration{0};
    }
    SECTION("invalid meter") {
        loop.meter = {4, 3};
    }
    SECTION("non-MIDI root note") {
        loop.root_note = 128;
    }
    SECTION("empty active range") {
        loop.active_range = AudioFrameRange{500, 500};
    }
    SECTION("active range beyond the asset") {
        loop.active_range = AudioFrameRange{500, 96'001};
    }
    SECTION("loop point beyond the asset") {
        loop.points.push_back({96'001, AudioLoopPointKind::Manual});
    }
    SECTION("duplicate loop point") {
        loop.points.push_back({24'000, AudioLoopPointKind::Automatic});
    }
    SECTION("invalid loop point kind") {
        loop.points[0].kind = static_cast<AudioLoopPointKind>(0xff);
    }
    SECTION("empty tag") {
        loop.tags.push_back("");
    }
    SECTION("duplicate tag") {
        loop.tags.push_back("warm");
    }
    SECTION("invalid UTF-8 tag") {
        loop.tags.emplace_back("\xC3\x28", 2);
    }

    const auto result = project_with_loop_info(std::move(loop));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == ModelErrorCode::InvalidAudioLoopInfo);
    REQUIRE(result.error().item == ItemId{2});
}

TEST_CASE("Audio asset v1 and v2 migrate only when loop metadata is lossless") {
    const auto registry = builtins();
    const auto without_loop = take(serialize_project(project_with(), registry));
    const auto asset_start =
        without_loop.json.find(R"({"data":{"content_hash":)");
    REQUIRE(asset_start != std::string::npos);
    const auto asset_end =
        without_loop.json.find(R"(,"type_name":"pulp.timeline.asset","version":2})", asset_start);
    REQUIRE(asset_end != std::string::npos);
    const auto v2 =
        without_loop.json.substr(asset_start, asset_end - asset_start) +
        R"(,"type_name":"pulp.timeline.asset","version":2})";

    const auto v1 =
        take(registry.migrate(SchemaDomain::Document, "pulp.timeline.asset", 2, 1, v2));
    REQUIRE(v1.find("\"version\":1") != std::string::npos);
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.asset", 1, 2, v1)) == v2);

    const auto with_loop =
        take(serialize_project(take(project_with_loop_info(valid_loop_info())), registry));
    const auto populated_start =
        with_loop.json.find(R"({"data":{"content_hash":)");
    const auto populated_end =
        with_loop.json.find(R"(,"type_name":"pulp.timeline.asset","version":2})",
                            populated_start);
    REQUIRE(populated_start != std::string::npos);
    REQUIRE(populated_end != std::string::npos);
    const auto populated =
        with_loop.json.substr(populated_start, populated_end - populated_start) +
        R"(,"type_name":"pulp.timeline.asset","version":2})";
    const auto rejected =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.asset", 2, 1, populated);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == PersistenceErrorCode::MigrationFailed);

    auto illegal_v1 = populated;
    const auto version = illegal_v1.rfind("\"version\":2");
    REQUIRE(version != std::string::npos);
    illegal_v1[version + std::string_view{"\"version\":"}.size()] = '1';
    const auto rejected_upgrade =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.asset", 1, 2, illegal_v1);
    REQUIRE_FALSE(rejected_upgrade.has_value());
    REQUIRE(rejected_upgrade.error().code == PersistenceErrorCode::MigrationFailed);
}

TEST_CASE("Audio loop metadata fails closed when exporting to shipped releases") {
    const auto registry = builtins();
    const auto project = take(project_with_loop_info(valid_loop_info()));
    for (const auto label : {"v0.736.0", "v0.744.0", "v0.748.0"}) {
        INFO(label);
        const auto result = serialize_project_for_release(project, registry, release(label));
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == PersistenceErrorCode::MigrationFailed);
    }
}

TEST_CASE("Audio loop metadata decode obeys aggregate limits") {
    const auto registry = builtins();
    const auto encoded =
        take(serialize_project(take(project_with_loop_info(valid_loop_info())), registry));
    DecodeLimits limits;

    SECTION("loop points") {
        limits.max_audio_loop_points = 1;
    }
    SECTION("tags") {
        limits.max_audio_loop_tags = 1;
    }

    const auto result = deserialize_project(encoded.json, registry, limits);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == PersistenceErrorCode::LimitExceeded);
    REQUIRE(result.error().actual == 2);
    REQUIRE(result.error().limit == 1);
}
