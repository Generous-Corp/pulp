#pragma once

#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace mcp_timeline_test {

template <typename T, typename E> T require_timeline_result(pulp::runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

inline std::string make_timeline_project_json(
    const std::filesystem::path& source,
    pulp::timeline::AssetLocatorKind locator_kind = pulp::timeline::AssetLocatorKind::ExternalUri,
    std::string locator_hint = {}) {
    using namespace pulp::timeline;
    constexpr std::uint64_t frame_count = 32;
    auto clip = require_timeline_result(Clip::create_absolute({4}, {0}, frame_count, {48'000, 1},
                                                              MediaRef{{5}, {0}, frame_count},
                                                              {.gain_linear = 1.0f}));
    auto track = require_timeline_result(Track::create({3}, "audio", {clip}));
    auto sequence = require_timeline_result(Sequence::create(
        {2}, "root", std::nullopt, AbsoluteTimelineDuration{frame_count, {48'000, 1}}, {track}));
    std::ifstream stream(source, std::ios::binary);
    REQUIRE(stream);
    const std::string bytes{std::istreambuf_iterator<char>(stream),
                            std::istreambuf_iterator<char>()};
    auto hash = ContentHash::from_hex(pulp::runtime::sha256_hex(bytes));
    REQUIRE(hash);
    MediaAsset asset{{5},
                     "source.wav",
                     frame_count,
                     {48'000, 1},
                     *hash,
                     AssetStoragePolicy::External,
                     {{locator_kind, locator_hint.empty() ? source.string() : locator_hint}},
                     {},
                     {}};
    auto project = require_timeline_result(
        Project::create(ProjectInput{{1}, "mcp", 6, {2}, {asset}, {sequence}}));
    auto registry = require_timeline_result(make_builtin_timeline_registry());
    return require_timeline_result(serialize_project(project, registry)).json;
}

inline std::string timeline_project_from_response(const std::string& response) {
    auto parsed = require_timeline_result(pulp::timeline::parse_json(response));
    const auto* structured = parsed->root().find("structuredContent");
    REQUIRE(structured != nullptr);
    const auto* project = structured->find("project");
    REQUIRE(project != nullptr);
    return std::string(parsed->raw(*project));
}

inline std::string timeline_string_from_response(const std::string& response,
                                                 std::string_view key) {
    auto parsed = require_timeline_result(pulp::timeline::parse_json(response));
    const auto* structured = parsed->root().find("structuredContent");
    REQUIRE(structured != nullptr);
    const auto* value = structured->find(key);
    REQUIRE(value != nullptr);
    REQUIRE(value->kind == pulp::timeline::JsonValue::Kind::String);
    return value->scalar;
}

} // namespace mcp_timeline_test
