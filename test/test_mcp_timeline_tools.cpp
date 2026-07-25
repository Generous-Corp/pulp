#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/tools/timeline/agent.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "../tools/mcp/mcp_tools.hpp"
#include "mcp_server_test_support.hpp"

namespace {

using Catch::Matchers::WithinAbs;
using namespace mcp_test;
using namespace pulp_mcp;

template <typename T, typename E> T require_timeline_result(pulp::runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

std::string make_timeline_project_json(
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

std::string timeline_project_from_response(const std::string& response) {
    auto parsed = require_timeline_result(pulp::timeline::parse_json(response));
    const auto* structured = parsed->root().find("structuredContent");
    REQUIRE(structured != nullptr);
    const auto* project = structured->find("project");
    REQUIRE(project != nullptr);
    return std::string(parsed->raw(*project));
}

TEST_CASE("timeline MCP operations edit and render inline projects", "[mcp][tools][timeline]") {
    TempDir temp;
    pulp::audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.8f)};
    const auto source_path = temp.path / "source.wav";
    REQUIRE(pulp::audio::write_wav_file(source_path.string(), source,
                                        pulp::audio::WavBitDepth::Float32));

    const auto project = make_timeline_project_json(source_path);
    const auto project_argument = pulp::timeline::quote_json_string(project);
    const auto project_only = "{\"project\":" + project_argument + "}";

    const auto opened = handle_timeline_project_open(project_only);
    require_contains(opened, R"JSON("ok":true)JSON");
    REQUIRE(timeline_project_from_response(opened) == project);

    const auto leading_brace_path = temp.path / "{project.json";
    {
        std::ofstream stream(leading_brace_path, std::ios::binary);
        REQUIRE(stream);
        stream << project;
    }
    const auto file_opened = handle_timeline_project_open(
        "{\"project\":" +
        pulp::timeline::quote_json_string(
            pulp::tools::timeline::filesystem_path_to_utf8(leading_brace_path)) +
        "}");
    require_contains(file_opened, R"JSON("ok":true)JSON");
    REQUIRE(timeline_project_from_response(file_opened) == project);

    const auto validated = handle_timeline_validate(project_only);
    require_contains(validated, R"JSON("diagnostics":[])JSON");

    const auto explained =
        handle_timeline_explain("{\"project\":" + project_argument + ",\"sample_rate\":44100}");
    require_contains(explained, R"JSON("audio_regions":1)JSON");
    require_contains(explained, R"JSON("pdc_offset_samples":null)JSON");

    const std::string command =
        R"JSON([{"data":{"clip_id":"4","expected":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1065353216"},"replacement":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1056964608"},"sequence_id":"2","track_id":"3"},"type_name":"pulp.timeline.command.set_clip_playback_properties","version":1}])JSON";
    const auto applied = handle_timeline_command_apply("{\"commands\":" + command +
                                                       ",\"project\":" + project_argument + "}");
    require_contains(applied, R"JSON("revision":"1")JSON");
    const auto changed_project = timeline_project_from_response(applied);

    const auto original_path = temp.path / "original.wav";
    const auto changed_path =
        temp.path / pulp::tools::timeline::filesystem_path_from_utf8("changed-\xE9\x9F\xB3.wav");
    const auto render_arguments = [](const std::string& project_json,
                                     const std::filesystem::path& output) {
        return "{\"output\":" +
               pulp::timeline::quote_json_string(
                   pulp::tools::timeline::filesystem_path_to_utf8(output)) +
               ",\"project\":" + pulp::timeline::quote_json_string(project_json) +
               ",\"sample_rate\":48000}";
    };
    const auto original_render = handle_timeline_render(render_arguments(project, original_path));
    const auto changed_render =
        handle_timeline_render(render_arguments(changed_project, changed_path));
    require_contains(original_render, R"JSON("frames":"32")JSON");
    require_contains(changed_render, R"JSON("frames":"32")JSON");

    const auto original_audio = pulp::audio::read_audio_file(original_path.string());
    const auto changed_audio = pulp::audio::read_audio_file(changed_path.string());
    REQUIRE(original_audio);
    REQUIRE(changed_audio);
    REQUIRE_THAT(original_audio->channels[0][0], WithinAbs(0.8f, 1e-7f));
    REQUIRE_THAT(changed_audio->channels[0][0], WithinAbs(0.4f, 1e-7f));

    const auto invalid_rate = handle_timeline_render(
        "{\"output\":\"ignored.wav\",\"project\":" + project_argument + ",\"sample_rate\":0}");
    require_contains(invalid_rate, R"JSON("isError":true)JSON");
    require_contains(invalid_rate, "sample_rate must be an integer between 1 and 768000");

    const auto excessive_rate = handle_timeline_render(
        "{\"output\":\"ignored.wav\",\"project\":" + project_argument + ",\"sample_rate\":768001}");
    require_contains(excessive_rate, R"JSON("isError":true)JSON");
    require_contains(excessive_rate, "sample_rate must be an integer between 1 and 768000");
}

TEST_CASE("timeline MCP confines package-relative media to the project base",
          "[mcp][tools][timeline]") {
    TempDir temp;
    const auto package = temp.path / "package";
    const auto media = package / "media";
    REQUIRE(std::filesystem::create_directories(media));
    pulp::audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.8f)};
    const auto nested_source = media / "source.wav";
    const auto outside_source = temp.path / "outside.wav";
    REQUIRE(pulp::audio::write_wav_file(nested_source.string(), source,
                                        pulp::audio::WavBitDepth::Float32));
    REQUIRE(pulp::audio::write_wav_file(outside_source.string(), source,
                                        pulp::audio::WavBitDepth::Float32));
    ScopedCurrentPath cwd(package);

    const auto arguments = [](const std::string& project_json,
                              const std::filesystem::path& output) {
        return "{\"output\":" +
               pulp::timeline::quote_json_string(
                   pulp::tools::timeline::filesystem_path_to_utf8(output)) +
               ",\"project\":" + pulp::timeline::quote_json_string(project_json) + "}";
    };
    const auto nested_project = make_timeline_project_json(
        nested_source, pulp::timeline::AssetLocatorKind::PackageRelative, "media/source.wav");
    const auto nested_response =
        handle_timeline_render(arguments(nested_project, temp.path / "nested.wav"));
    require_contains(nested_response, R"JSON("frames":"32")JSON");

    const auto absolute_project = make_timeline_project_json(
        nested_source, pulp::timeline::AssetLocatorKind::PackageRelative, nested_source.string());
    const auto absolute_response =
        handle_timeline_render(arguments(absolute_project, temp.path / "absolute.wav"));
    require_contains(absolute_response, R"JSON("isError":true)JSON");
    require_contains(absolute_response, R"JSON("stage":"render")JSON");

    const auto traversal_project = make_timeline_project_json(
        outside_source, pulp::timeline::AssetLocatorKind::PackageRelative, "../outside.wav");
    const auto traversal_response =
        handle_timeline_render(arguments(traversal_project, temp.path / "traversal.wav"));
    require_contains(traversal_response, R"JSON("isError":true)JSON");
    require_contains(traversal_response, R"JSON("stage":"render")JSON");
}

// The agent loop end to end: apply a command to make a second journal variant,
// render both variants headless, and hand the two renders to
// pulp_audio_compare for an advisory verdict.
//
// The measurement itself belongs to the opt-in Audio Quality Lab, so what is
// asserted here is that the loop CLOSES over the agent's own renders and comes
// back typed — not that a particular judgment was returned. When the lab is not
// installed the typed response carries its actionable install hint, which is
// the documented opt-in path rather than a failure of the loop. The one thing
// that would mean the loop did not close is an argument refusal, so that is
// asserted against explicitly.
TEST_CASE("timeline agent renders two journal variants and receives a typed compare verdict",
          "[mcp][tools][timeline][audio]") {
    TempDir temp;
    pulp::audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.8f)};
    const auto source_path = temp.path / "source.wav";
    REQUIRE(pulp::audio::write_wav_file(source_path.string(), source,
                                        pulp::audio::WavBitDepth::Float32));

    const auto project = make_timeline_project_json(source_path);
    const auto project_argument = pulp::timeline::quote_json_string(project);

    // Variant B is a journal variant of A: the same document plus one typed
    // command, which is what an agent exploring an edit actually produces.
    const std::string command =
        R"JSON([{"data":{"clip_id":"4","expected":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1065353216"},"replacement":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1056964608"},"sequence_id":"2","track_id":"3"},"type_name":"pulp.timeline.command.set_clip_playback_properties","version":1}])JSON";
    const auto applied = handle_timeline_command_apply("{\"commands\":" + command +
                                                       ",\"project\":" + project_argument + "}");
    require_contains(applied, R"JSON("revision":"1")JSON");
    const auto variant_project = timeline_project_from_response(applied);

    const auto reference_path = temp.path / "variant-a.wav";
    const auto candidate_path = temp.path / "variant-b.wav";
    const auto render_arguments = [](const std::string& project_json,
                                     const std::filesystem::path& output) {
        return "{\"output\":" +
               pulp::timeline::quote_json_string(
                   pulp::tools::timeline::filesystem_path_to_utf8(output)) +
               ",\"project\":" + pulp::timeline::quote_json_string(project_json) +
               ",\"sample_rate\":48000}";
    };
    require_contains(handle_timeline_render(render_arguments(project, reference_path)),
                     R"JSON("frames":"32")JSON");
    require_contains(handle_timeline_render(render_arguments(variant_project, candidate_path)),
                     R"JSON("frames":"32")JSON");

    // The two renders must actually differ, otherwise the comparison below
    // would be asked to judge a document against itself.
    const auto reference_audio = pulp::audio::read_audio_file(reference_path.string());
    const auto candidate_audio = pulp::audio::read_audio_file(candidate_path.string());
    REQUIRE(reference_audio);
    REQUIRE(candidate_audio);
    REQUIRE_THAT(reference_audio->channels[0][0], WithinAbs(0.8f, 1e-7f));
    REQUIRE_THAT(candidate_audio->channels[0][0], WithinAbs(0.4f, 1e-7f));

    // pulp_audio_compare resolves its delegated CLI relative to a project root.
    ScopedCurrentPath cwd(std::filesystem::path(PULP_SOURCE_DIR));
    const auto verdict = handle_audio_compare(
        "{\"candidate\":" +
        pulp::timeline::quote_json_string(
            pulp::tools::timeline::filesystem_path_to_utf8(candidate_path)) +
        ",\"reference\":" +
        pulp::timeline::quote_json_string(
            pulp::tools::timeline::filesystem_path_to_utf8(reference_path)) +
        "}");

    require_contains(verdict, R"JSON("content")JSON");
    require_contains(verdict, R"JSON("type":"text")JSON");
    // Reaching the compare stage is the loop closing. These refusals would mean
    // it never got there with the agent's renders.
    REQUIRE(verdict.find("reference and candidate are required") == std::string::npos);
    REQUIRE(verdict.find("must be WAV paths, not options") == std::string::npos);
    REQUIRE(verdict.find("not in a Pulp project") == std::string::npos);
}

} // namespace
