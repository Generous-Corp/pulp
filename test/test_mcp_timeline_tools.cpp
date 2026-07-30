#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/interchange/export_plan.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/tools/timeline/agent.hpp>

#include <miniz.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "../tools/mcp/mcp_tools.hpp"
#include "../tools/mcp/timeline_session_store.hpp"
#include "../tools/timeline/src/timeline_agent_internal.hpp"
#include "mcp_server_test_support.hpp"
#include "mcp_timeline_test_support.hpp"

namespace {

using Catch::Matchers::WithinAbs;
using namespace mcp_test;
using namespace mcp_timeline_test;
using namespace pulp_mcp;

void write_zip(const std::filesystem::path& path,
               const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_file(&zip, path.string().c_str(), 0));
    for (const auto& [name, bytes] : entries)
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), bytes.data(), bytes.size(),
                                      MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
}

std::vector<std::uint8_t> read_zip_entry(const std::filesystem::path& path,
                                         const char* name) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_reader_init_file(&zip, path.string().c_str(), 0));
    std::size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&zip, name, &size, 0);
    REQUIRE(data != nullptr);
    std::vector<std::uint8_t> bytes(static_cast<std::uint8_t*>(data),
                                    static_cast<std::uint8_t*>(data) + size);
    mz_free(data);
    mz_zip_reader_end(&zip);
    return bytes;
}

/// Builds a project whose sole asset carries the given locator, and returns the
/// Result rather than unwrapping it. Confinement is asserted at two layers, and
/// the model layer's assertion is that construction *refuses* a hint — so this
/// path must be able to hand back a refusal without the helper turning it into
/// a test failure the way `require_timeline_result` would.
pulp::runtime::Result<pulp::timeline::Project, pulp::timeline::ModelError>
try_timeline_project_with_locator(const std::filesystem::path& source,
                                  pulp::timeline::AssetLocatorKind locator_kind,
                                  const std::string& hint) {
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
    MediaAsset asset{{5},   "source.wav", frame_count, {48'000, 1}, *hash,
                     AssetStoragePolicy::External, {{locator_kind, hint}}, {}, {}};
    return Project::create(ProjectInput{{1}, "mcp", 6, {2}, {asset}, {sequence}});
}

std::string
make_nested_timeline_project_json(const std::filesystem::path& source) {
    using namespace pulp::timeline;
    using pulp::timebase::kTicksPerQuarter;
    constexpr std::uint64_t frame_count = 32;
    auto child_clip = require_timeline_result(Clip::create(
        {12}, {0}, {kTicksPerQuarter},
        MediaRef{{5}, {0}, frame_count}));
    auto unreachable_clip = require_timeline_result(Clip::create(
        {13}, {2 * kTicksPerQuarter}, {kTicksPerQuarter},
        MediaRef{{6}, {0}, frame_count}));
    auto child_track =
        require_timeline_result(Track::create({11}, "child audio",
                                              {child_clip,
                                               unreachable_clip}));
    auto child = require_timeline_result(Sequence::create(
        {10}, "child",
        pulp::timebase::TickDuration{3 * kTicksPerQuarter},
        {child_track}));
    auto root_clip = require_timeline_result(Clip::create(
        {4}, {0}, {kTicksPerQuarter},
        SequenceRef{{10}, {0}}));
    auto root_track =
        require_timeline_result(Track::create({3}, "root", {root_clip}));
    auto root = require_timeline_result(Sequence::create(
        {2}, "root", pulp::timebase::TickDuration{kTicksPerQuarter},
        {root_track}));

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
                     {{AssetLocatorKind::ExternalUri, source.string()}},
                     {},
                     {}};
    MediaAsset unreachable_asset{
        {6},
        "unreachable.wav",
        frame_count,
        {48'000, 1},
        *ContentHash::from_hex(std::string(64, 'b')),
        AssetStoragePolicy::External,
        {{AssetLocatorKind::ExternalUri,
          source.string() + ".missing"}},
        {},
        {}};
    auto project = require_timeline_result(Project::create(
        ProjectInput{{1},
                     "nested mcp",
                     14,
                     {2},
                     {asset, unreachable_asset},
                     {root, child}}));
    auto registry =
        require_timeline_result(make_builtin_timeline_registry());
    return require_timeline_result(serialize_project(project, registry)).json;
}

TEST_CASE("timeline asset discovery budgets non-intersecting nested probes",
          "[mcp][tools][timeline]") {
    using namespace pulp::timeline;

    auto child_a = require_timeline_result(
        Clip::create({12}, {20}, {10}, EmptyContent{}));
    auto child_b = require_timeline_result(
        Clip::create({13}, {40}, {10}, EmptyContent{}));
    auto child_track = require_timeline_result(
        Track::create({11}, "child", {child_a, child_b}));
    auto child = require_timeline_result(Sequence::create(
        {10}, "child", pulp::timebase::TickDuration{200}, {child_track}));
    auto first_reference = require_timeline_result(Clip::create(
        {4}, {0}, {10}, SequenceRef{{10}, {0}}));
    auto second_reference = require_timeline_result(Clip::create(
        {5}, {10}, {10}, SequenceRef{{10}, {100}}));
    auto root_track = require_timeline_result(
        Track::create({3}, "root", {first_reference, second_reference}));
    auto root = require_timeline_result(Sequence::create(
        {2}, "root", pulp::timebase::TickDuration{20}, {root_track}));
    auto project = require_timeline_result(Project::create(
        ProjectInput{{1}, "bounded probes", 14, {2}, {},
                     {root, child}}));
    auto tempo = require_timeline_result(
        pulp::timebase::CompiledTempoMap::compile(
            project.tempo_map().points(), {48'000, 1}));

    auto reachable =
        pulp::tools::timeline::detail::reachable_assets(
            project, *project.find_sequence({2}), tempo, 100, 4);
    REQUIRE_FALSE(reachable);
    REQUIRE(reachable.error().code ==
            pulp::playback::CompileErrorCode::ExpansionBudgetExceeded);
    REQUIRE(reachable.error().item == ItemId{12});
}

TEST_CASE("timeline asset discovery attributes active-take budget overflow to its track",
          "[mcp][tools][timeline]") {
    using namespace pulp::timeline;

    const auto hash =
        *ContentHash::from_hex(std::string(64, 'a'));
    auto recorded = require_timeline_result(
        Take::create({5}, MediaRef{{20}, {0}, 4}, {0}, {48'000, 1}));
    auto lane = require_timeline_result(TakeLane::create(
        {4}, "comp", {recorded},
        {{.take_id = {5}, .range = {{0}, 2, {48'000, 1}}},
         {.take_id = {5}, .range = {{2}, 2, {48'000, 1}}}}));
    auto track = require_timeline_result(Track::create(
        TrackInput{.id = {3},
                   .name = "active comp",
                   .take_lanes = {lane},
                   .active_take_lane_id = {4}}));
    auto root = require_timeline_result(Sequence::create(
        {2}, "root", std::nullopt, std::nullopt, {track}));
    MediaAsset asset{{20}, "take.wav", 4, {48'000, 1}, hash};
    auto project = require_timeline_result(Project::create(
        ProjectInput{{1}, "bounded comp", 21, {2}, {asset}, {root}}));
    auto tempo = require_timeline_result(
        pulp::timebase::CompiledTempoMap::compile(
            project.tempo_map().points(), {48'000, 1}));

    auto reachable =
        pulp::tools::timeline::detail::reachable_assets(
            project, *project.find_sequence({2}), tempo, 100, 1);
    REQUIRE_FALSE(reachable);
    REQUIRE(reachable.error().code ==
            pulp::playback::CompileErrorCode::ExpansionBudgetExceeded);
    REQUIRE(reachable.error().item == ItemId{3});
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

    const auto nested_project =
        make_nested_timeline_project_json(source_path);
    const auto nested_explained = handle_timeline_explain(
        "{\"project\":" +
        pulp::timeline::quote_json_string(nested_project) + "}");
    require_contains(nested_explained, R"JSON("audio_regions":1)JSON");
    require_contains(nested_explained, R"JSON("ok":true)JSON");

    auto overflowing_project = nested_project;
    const std::string valid_window = "\"sequence_id\":\"10\",\"source_start\":\"0\"";
    const std::string overflowing_window =
        "\"sequence_id\":\"10\",\"source_start\":\"9223372036854775807\"";
    const auto window_offset = overflowing_project.find(valid_window);
    REQUIRE(window_offset != std::string::npos);
    overflowing_project.replace(window_offset, valid_window.size(),
                                overflowing_window);
    const auto overflowing_explained = handle_timeline_explain(
        "{\"project\":" +
        pulp::timeline::quote_json_string(overflowing_project) + "}");
    require_contains(overflowing_explained, R"JSON("isError":true)JSON");
    require_contains(overflowing_explained,
                     R"JSON("stage":"open")JSON");

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

    // Layer 1 -- the model. A package-relative locator that leaves the package
    // is refused at construction, so an escaping document cannot be built in
    // memory, let alone serialized or handed to a renderer.
    const auto absolute_project = try_timeline_project_with_locator(
        nested_source, pulp::timeline::AssetLocatorKind::PackageRelative, nested_source.string());
    REQUIRE_FALSE(absolute_project);
    REQUIRE(absolute_project.error().code == pulp::timeline::ModelErrorCode::InvalidAssetLocator);

    const auto traversal_project = try_timeline_project_with_locator(
        outside_source, pulp::timeline::AssetLocatorKind::PackageRelative, "../outside.wav");
    REQUIRE_FALSE(traversal_project);
    REQUIRE(traversal_project.error().code == pulp::timeline::ModelErrorCode::InvalidAssetLocator);

    // Layer 2 -- the load path, which must not trust a document just because it
    // arrived as JSON. The model constructor cannot run on bytes someone else
    // wrote, so the hostile hint is spliced into serialized output rather than
    // built through the API.
    const auto with_hint = [&](const std::string& hint) {
        auto hostile = nested_project;
        const auto offset = hostile.find("media/source.wav");
        REQUIRE(offset != std::string::npos);
        hostile.replace(offset, std::string("media/source.wav").size(), hint);
        return hostile;
    };

    const auto spliced_traversal =
        handle_timeline_render(arguments(with_hint("../outside.wav"), temp.path / "spliced.wav"));
    require_contains(spliced_traversal, R"JSON("isError":true)JSON");
    require_contains(spliced_traversal, R"JSON("stage":"open")JSON");

    // The one escape only layer 2 can catch: a hint that is lexically clean and
    // therefore passes the model, but whose resolved target is a symlink out of
    // the package. Nothing short of canonicalizing the path sees this, which is
    // why the loader's `fs::canonical` + beneath-base check is not redundant
    // with the model's lexical check.
    std::error_code link_error;
    std::filesystem::create_symlink(outside_source, media / "link.wav", link_error);
    if (!link_error) {
        const auto symlinked = try_timeline_project_with_locator(
            outside_source, pulp::timeline::AssetLocatorKind::PackageRelative, "media/link.wav");
        REQUIRE(symlinked); // lexically safe: the model has no basis to refuse it
        auto registry = require_timeline_result(pulp::timeline::make_builtin_timeline_registry());
        const auto symlink_json =
            require_timeline_result(pulp::timeline::serialize_project(*symlinked, registry)).json;
        const auto symlink_response =
            handle_timeline_render(arguments(symlink_json, temp.path / "symlink.wav"));
        require_contains(symlink_response, R"JSON("isError":true)JSON");
        require_contains(symlink_response, R"JSON("stage":"render")JSON");
    }
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

TEST_CASE("timeline MCP export and import publish new directories atomically",
          "[mcp][tools][timeline][interchange]") {
    TempDir temp;
    const std::string project =
        R"json({"data":{"assets":[],"id":"1","name":"fixture","next_item_id":"3","root_sequence_id":"2","sequences":[{"data":{"absolute_duration":null,"id":"2","musical_duration":"0","name":"root","tracks":[]},"type_name":"pulp.timeline.sequence","version":1}]},"type_name":"pulp.timeline.project","version":1})json";
    const auto exported = temp.path / "smf-export";
    const auto export_args =
        "{\"format\":\"smf\",\"output\":" +
        pulp::timeline::quote_json_string(
            pulp::tools::timeline::filesystem_path_to_utf8(exported)) +
        ",\"project\":" + pulp::timeline::quote_json_string(project) + "}";

    const auto plan_args =
        "{\"format\":\"smf\",\"plan_only\":true,\"project\":" +
        pulp::timeline::quote_json_string(project) + "}";
    const auto plan_result = handle_timeline_export(plan_args);
    REQUIRE(pulp::timeline::parse_json(plan_result));
    require_contains(plan_result, R"JSON("plan_only":true)JSON");
    require_contains(plan_result, R"JSON("manifest":{"schema_version":1)JSON");
    require_contains(plan_result, R"JSON("required_consent":[])JSON");
    REQUIRE(plan_result.find(R"JSON("output")JSON") == std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(exported));

    const auto direct_plan = pulp::tools::timeline::plan_export_project(
        pulp::tools::timeline::ProjectSource::inline_json(project), "smf");
    REQUIRE(direct_plan.exit_code == 0);
    REQUIRE(pulp::timeline::parse_json(direct_plan.json));
    REQUIRE(direct_plan.json.find(R"JSON("output")JSON") == std::string::npos);

    auto plan_with_output = plan_args;
    plan_with_output.pop_back();
    plan_with_output += ",\"output\":\"unused\"}";
    require_contains(handle_timeline_export(plan_with_output),
                     "output must be absent when plan_only is true");
    auto plan_with_consent = plan_args;
    plan_with_consent.pop_back();
    plan_with_consent += ",\"accept_losses\":[\"clip.absolute\"]}";
    require_contains(handle_timeline_export(plan_with_consent),
                     "accept_losses must be absent when plan_only is true");
    require_contains(handle_timeline_export(
                         "{\"format\":\"smf\",\"project\":" +
                         pulp::timeline::quote_json_string(project) + "}"),
                     "output is required when publishing an export");

    const auto export_result = handle_timeline_export(export_args);
    REQUIRE(pulp::timeline::parse_json(export_result));
    require_contains(export_result, R"JSON("ok":true)JSON");
    require_contains(export_result, R"JSON("manifest":{"schema_version":1)JSON");
    REQUIRE(std::filesystem::is_regular_file(exported / "project.mid"));
    REQUIRE(std::filesystem::is_regular_file(exported / "pulp-loss-manifest.json"));

    {
        std::ofstream sentinel(exported / "sentinel.txt", std::ios::binary);
        REQUIRE(sentinel);
        sentinel << "preserve";
    }
    const auto publish_refusal = handle_timeline_export(export_args);
    REQUIRE(pulp::timeline::parse_json(publish_refusal));
    require_contains(publish_refusal, R"JSON("isError":true)JSON");
    require_contains(publish_refusal, R"JSON("manifest":{"schema_version":1)JSON");
    require_contains(publish_refusal, R"JSON("stage":"publish")JSON");
    REQUIRE(std::filesystem::is_regular_file(exported / "sentinel.txt"));

    auto unknown_args = export_args;
    unknown_args.pop_back();
    unknown_args += ",\"accept_losses\":[\"clip.telepathy\"]}";
    require_contains(handle_timeline_export(unknown_args), "unknown loss concept");

    const auto imported = temp.path / "smf-import";
    const auto import_args =
        "{\"format\":\"smf\",\"input\":" +
        pulp::timeline::quote_json_string(
            pulp::tools::timeline::filesystem_path_to_utf8(exported / "project.mid")) +
        ",\"output\":" +
        pulp::timeline::quote_json_string(
            pulp::tools::timeline::filesystem_path_to_utf8(imported)) +
        "}";
    require_contains(handle_timeline_import(import_args), R"JSON("ok":true)JSON");
    REQUIRE(std::filesystem::is_regular_file(imported / "project.json"));

    const auto malformed = temp.path / "malformed.mid";
    {
        std::ofstream stream(malformed, std::ios::binary);
        REQUIRE(stream);
        stream << "not midi";
    }
    const auto failed_output = temp.path / "failed-import";
    const auto malformed_args =
        "{\"format\":\"smf\",\"input\":" +
        pulp::timeline::quote_json_string(
            pulp::tools::timeline::filesystem_path_to_utf8(malformed)) +
        ",\"output\":" +
        pulp::timeline::quote_json_string(
            pulp::tools::timeline::filesystem_path_to_utf8(failed_output)) +
        "}";
    require_contains(handle_timeline_import(malformed_args), R"JSON("isError":true)JSON");
    REQUIRE_FALSE(std::filesystem::exists(failed_output));

    // Exercise a standard media-bearing .dawproject ZIP through the same MCP
    // boundary, including media sealing and atomic file publication.
    pulp::audio::AudioFileData daw_media;
    daw_media.sample_rate = 44'100;
    daw_media.channels = {std::vector<float>(44'100, 0.25f)};
    const auto daw_media_path = temp.path / "source.wav";
    REQUIRE(pulp::audio::write_wav_file(daw_media_path.string(), daw_media,
                                        pulp::audio::WavBitDepth::Float32));
    const std::string daw_xml = R"xml(<Project version="1.0"><Structure><Track contentType="audio" id="audio" name="Audio"/></Structure><Arrangement><Lanes timeUnit="beats"><Lanes track="audio"><Clips><Clip time="0" duration="4"><Audio channels="1" duration="1" sampleRate="44100"><File path="audio/source.wav"/></Audio></Clip></Clips></Lanes></Lanes></Arrangement></Project>)xml";
    std::ifstream daw_media_stream(daw_media_path, std::ios::binary);
    REQUIRE(daw_media_stream);
    const std::vector<std::uint8_t> daw_media_bytes{
        std::istreambuf_iterator<char>(daw_media_stream), std::istreambuf_iterator<char>()};
    const std::vector<std::uint8_t> daw_xml_bytes(daw_xml.begin(), daw_xml.end());
    const auto daw_archive = temp.path / "source.dawproject";
    write_zip(daw_archive, {{"project.xml", daw_xml_bytes},
                            {"audio/source.wav", daw_media_bytes}});
    const auto daw_imported = temp.path / "daw-imported";
    const auto daw_import_args =
        "{\"format\":\"dawproject\",\"input\":" +
        pulp::timeline::quote_json_string(pulp::tools::timeline::filesystem_path_to_utf8(
            daw_archive)) +
        ",\"output\":" + pulp::timeline::quote_json_string(
                                   pulp::tools::timeline::filesystem_path_to_utf8(daw_imported)) +
        "}";
    require_contains(handle_timeline_import(daw_import_args), R"JSON("ok":true)JSON");
    REQUIRE(std::filesystem::is_regular_file(daw_imported / "project.json"));
    REQUIRE(std::filesystem::is_regular_file(daw_imported / "audio/source.wav"));

    const auto daw_exported = temp.path / "daw-exported.dawproject";
    const auto daw_export_args =
        "{\"accept_losses\":[\"media.provenance\",\"media.provenance\"],"
        "\"format\":\"dawproject\",\"output\":" +
        pulp::timeline::quote_json_string(
            pulp::tools::timeline::filesystem_path_to_utf8(daw_exported)) +
        ",\"project\":" + pulp::timeline::quote_json_string(
                                   pulp::tools::timeline::filesystem_path_to_utf8(
                                       daw_imported / "project.json")) +
        "}";
    require_contains(handle_timeline_export(daw_export_args), R"JSON("ok":true)JSON");
    REQUIRE(std::filesystem::is_regular_file(daw_exported));
    const auto exported_xml_bytes = read_zip_entry(daw_exported, "project.xml");
    REQUIRE_FALSE(read_zip_entry(daw_exported, "pulp-loss-manifest.json").empty());
    REQUIRE_FALSE(read_zip_entry(daw_exported, "audio/audio/source.wav").empty());
    const std::string exported_xml(exported_xml_bytes.begin(), exported_xml_bytes.end());
    REQUIRE(exported_xml.find("duration=\"1\"") != std::string::npos);
    require_contains(handle_timeline_export(daw_export_args), R"JSON("isError":true)JSON");
    REQUIRE(std::filesystem::is_regular_file(daw_exported));
    REQUIRE(read_zip_entry(daw_exported, "project.xml") == exported_xml_bytes);
}

TEST_CASE("timeline MCP interchange rejects malformed boundaries without publishing",
          "[mcp][tools][timeline][interchange]") {
    TempDir temp;

    require_contains(handle_timeline_export("{"), "arguments must be valid JSON");
    require_contains(handle_timeline_import("{"), "arguments must be valid JSON");
    require_contains(handle_timeline_export(
                         R"json({"accept_losses":{},"format":"smf","output":"unused","project":"{}"})json"),
                     "accept_losses must be an array");
    require_contains(handle_timeline_export(
                         R"json({"accept_losses":[7],"format":"smf","output":"unused","project":"{}"})json"),
                     "every accept_losses entry must be a concept id");
    require_contains(handle_timeline_export(
                         R"json({"accept_losses":[""],"format":"smf","output":"unused","project":"{}"})json"),
                     "every accept_losses entry must be a concept id");
    require_contains(handle_timeline_export(
                         R"json({"format":"smf","output":"unused","plan_only":"yes","project":"{}"})json"),
                     "plan_only must be a boolean");

    const auto invalid_export = pulp::tools::timeline::export_project(
        pulp::tools::timeline::ProjectSource::inline_json("{}"), "unknown",
        temp.path / "invalid-export", {});
    REQUIRE(invalid_export.exit_code == 2);
    REQUIRE_FALSE(std::filesystem::exists(temp.path / "invalid-export"));

    const auto malformed_project = pulp::tools::timeline::export_project(
        pulp::tools::timeline::ProjectSource::inline_json("{}"), "smf",
        temp.path / "malformed-project", {});
    REQUIRE(malformed_project.exit_code == 1);
    REQUIRE_FALSE(std::filesystem::exists(temp.path / "malformed-project"));

    const auto invalid_plan = pulp::tools::timeline::plan_export_project(
        pulp::tools::timeline::ProjectSource::inline_json("{}"), "unknown");
    REQUIRE(invalid_plan.exit_code == 2);
    REQUIRE(invalid_plan.json.find(R"json("output")json") == std::string::npos);

    const auto renamed_xml = temp.path / "renamed.xml";
    {
        std::ofstream stream(renamed_xml, std::ios::binary);
        REQUIRE(stream);
        stream << R"xml(<Project version="1.0"/>)xml";
    }
    const auto dishonest = pulp::tools::timeline::import_project(
        renamed_xml, "dawproject", temp.path / "dishonest");
    REQUIRE(dishonest.exit_code == 2);
    REQUIRE_FALSE(std::filesystem::exists(temp.path / "dishonest"));

    const auto missing_input = temp.path / "missing" / "project.dawproject";
    const auto missing = pulp::tools::timeline::import_project(
        missing_input, "dawproject", temp.path / "missing-output");
    REQUIRE(missing.exit_code == 1);
    REQUIRE_FALSE(std::filesystem::exists(temp.path / "missing-output"));

    const auto not_zip = temp.path / "not-zip.dawproject";
    {
        std::ofstream stream(not_zip, std::ios::binary);
        REQUIRE(stream);
        stream << "not a zip";
    }
    const auto unreadable = pulp::tools::timeline::import_project(
        not_zip, "dawproject", temp.path / "not-zip-output");
    REQUIRE(unreadable.exit_code == 1);
    REQUIRE(unreadable.json.find("ZIP container") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(temp.path / "not-zip-output"));

    const auto missing_project = temp.path / "missing-project.dawproject";
    write_zip(missing_project, {{"metadata.xml", std::vector<std::uint8_t>{'x'}}});
    const auto no_project = pulp::tools::timeline::import_project(
        missing_project, "dawproject", temp.path / "missing-project-output");
    REQUIRE(no_project.exit_code == 1);
    REQUIRE(no_project.json.find("root project.xml") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(temp.path / "missing-project-output"));

    const auto malformed_archive = temp.path / "malformed.dawproject";
    const std::string malformed_xml = "not xml";
    const std::vector<std::uint8_t> malformed_xml_bytes(malformed_xml.begin(),
                                                        malformed_xml.end());
    write_zip(malformed_archive, {{"project.xml", malformed_xml_bytes}});
    const auto malformed = pulp::tools::timeline::import_project(
        malformed_archive, "dawproject", temp.path / "malformed-output");
    REQUIRE(malformed.exit_code == 1);
    REQUIRE_FALSE(std::filesystem::exists(temp.path / "malformed-output"));

    const auto existing_output = temp.path / "existing-output";
    REQUIRE(std::filesystem::create_directory(existing_output));
    const auto refused = pulp::tools::timeline::import_project(
        temp.path / "does-not-matter.mid", "unknown", existing_output);
    REQUIRE(refused.exit_code == 2);
}

TEST_CASE("DAWproject media export enforces one cumulative retained-byte budget",
          "[mcp][tools][timeline][interchange]") {
    using namespace pulp::timeline;
    TempDir temp;

    pulp::audio::AudioFileData first_audio;
    first_audio.sample_rate = 48'000;
    first_audio.channels = {std::vector<float>(16'384, 0.25f)};
    pulp::audio::AudioFileData second_audio = first_audio;
    second_audio.channels[0][0] = -0.25f;
    const auto first_path = temp.path / "first.wav";
    const auto second_path = temp.path / "second.wav";
    REQUIRE(pulp::audio::write_wav_file(first_path.string(), first_audio,
                                        pulp::audio::WavBitDepth::Float32));
    REQUIRE(pulp::audio::write_wav_file(second_path.string(), second_audio,
                                        pulp::audio::WavBitDepth::Float32));

    const auto hash_file = [](const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream);
        const std::string bytes{std::istreambuf_iterator<char>(stream),
                                std::istreambuf_iterator<char>()};
        auto hash = ContentHash::from_hex(pulp::runtime::sha256_hex(bytes));
        REQUIRE(hash);
        return *hash;
    };
    constexpr auto duration = pulp::timebase::TickDuration{pulp::timebase::kTicksPerQuarter};
    auto first_clip = require_timeline_result(
        Clip::create({4}, {0}, duration, MediaRef{{5}, {0}, 32}));
    auto second_clip = require_timeline_result(
        Clip::create({6}, {duration.value}, duration, MediaRef{{7}, {0}, 32}));
    auto track = require_timeline_result(
        Track::create({3}, "two assets", {first_clip, second_clip}));
    auto sequence = require_timeline_result(Sequence::create(
        {2}, "root", pulp::timebase::TickDuration{2 * duration.value}, {track}));
    MediaAsset first{{5}, "first.wav", 32, {48'000, 1}, hash_file(first_path),
                     AssetStoragePolicy::External,
                     {{AssetLocatorKind::ExternalUri, first_path.string()}}, {}, {}};
    MediaAsset second{{7}, "second.wav", 32, {48'000, 1}, hash_file(second_path),
                      AssetStoragePolicy::External,
                      {{AssetLocatorKind::ExternalUri, second_path.string()}}, {}, {}};
    auto project = require_timeline_result(
        Project::create(ProjectInput{{1}, "budget", 8, {2}, {first, second}, {sequence}}));
    pulp::tools::timeline::detail::LoadedProject loaded{std::move(project), temp.path};

    const auto first_bytes = std::filesystem::file_size(first_path);
    const auto second_bytes = std::filesystem::file_size(second_path);
    pulp::interchange::ExportArtifacts refused;
    const auto refusal = pulp::tools::timeline::detail::add_dawproject_media(
        loaded, refused, 3u * std::max(first_bytes, second_bytes));
    REQUIRE_FALSE(refusal);
    REQUIRE(refusal.error().code ==
            pulp::tools::timeline::detail::DawProjectMediaErrorCode::AssetReadFailed);
    REQUIRE(refusal.error().asset_id == 7);
    REQUIRE(refusal.error().asset_name == "second.wav");
    REQUIRE_FALSE(refusal.error().reason.empty());
    REQUIRE(refused.artifacts.empty());

    pulp::interchange::ExportArtifacts accepted;
    const auto accumulated = pulp::tools::timeline::detail::add_dawproject_media(
        loaded, accepted, 1024u * 1024u);
    REQUIRE(accumulated);
    REQUIRE(accumulated.value() == 2);
    REQUIRE(accepted.artifacts.size() == 2);

    pulp::interchange::ExportArtifacts near_limit;
    const auto near_limit_result = pulp::tools::timeline::detail::add_dawproject_media(
        loaded, near_limit, 320u * 1024u);
    REQUIRE(near_limit_result);
    REQUIRE(near_limit.artifacts.size() == 2);

    std::error_code removed;
    REQUIRE(std::filesystem::remove(first_path, removed));
    REQUIRE_FALSE(removed);
    pulp::interchange::ExportArtifacts metadata_heavy;
    for (std::size_t index = 0; index < 64; ++index)
        metadata_heavy.artifacts.push_back(
            {"metadata/" + std::to_string(index) + "/" + std::string(1'024, 'x'), {}});
    const auto metadata_refusal = pulp::tools::timeline::detail::add_dawproject_media(
        loaded, metadata_heavy, 32u * 1024u);
    REQUIRE_FALSE(metadata_refusal);
    REQUIRE(metadata_refusal.error().code ==
            pulp::tools::timeline::detail::DawProjectMediaErrorCode::ByteLimitExceeded);
}

TEST_CASE("DAWproject ZIP export and import enforce one exact aggregate working set",
          "[mcp][tools][timeline][interchange]") {
    namespace detail = pulp::tools::timeline::detail;
    TempDir temp;

    auto incompressible = [](std::size_t size, std::uint32_t seed) {
        std::vector<std::uint8_t> bytes(size);
        for (auto& byte : bytes) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            byte = static_cast<std::uint8_t>(seed);
        }
        return bytes;
    };
    auto long_name = [](std::size_t index) {
        std::string name = "audio/" + std::to_string(index) + "/";
        while (name.size() + 65 < 4'096)
            name += std::string(64, 'p') + '/';
        name.append(4'096 - name.size(), 'x');
        return name;
    };

    pulp::interchange::ExportArtifacts artifacts;
    artifacts.artifacts.push_back(
        {"project.xml", std::vector<std::uint8_t>{'<', 'p', 'r', 'o', 'j', 'e', 'c', 't', '/', '>'}});
    constexpr std::size_t kEntryCount = 128;
    for (std::size_t index = 0; index < kEntryCount; ++index)
        artifacts.artifacts.push_back(
            {long_name(index), incompressible(8u * 1024u, static_cast<std::uint32_t>(index + 1))});

    constexpr std::uint64_t kTestLimit = 16u * 1024u * 1024u;
    const auto archive = temp.path / std::filesystem::path(u8"música-工作集.dawproject");
    const auto written = detail::write_dawproject_archive_no_replace(
        artifacts, archive, kTestLimit);
    REQUIRE(written);
    REQUIRE(written.value() > 0);
    REQUIRE(written.value() < kTestLimit);
    REQUIRE(std::filesystem::exists(archive));

    const auto export_refused = detail::write_dawproject_archive_no_replace(
        artifacts, temp.path / "export-over-limit.dawproject", written.value() - 1);
    REQUIRE_FALSE(export_refused);
    REQUIRE(export_refused.error().code == detail::DawProjectArchiveErrorCode::Export);
    REQUIRE_FALSE(std::filesystem::exists(temp.path / "export-over-limit.dawproject"));

    const auto inspected = detail::inspect_dawproject_archive(
        archive, kTestLimit, artifacts.artifacts.size(), artifacts.artifacts.size(), 4'096);
    REQUIRE(inspected);
    REQUIRE(inspected.value().peak_bytes > 0);
    REQUIRE(inspected.value().final_balance_bytes == 0);
    const auto import_refused = detail::inspect_dawproject_archive(
        archive, inspected.value().peak_bytes - 1, artifacts.artifacts.size(),
        artifacts.artifacts.size(), 4'096);
    REQUIRE_FALSE(import_refused);
    REQUIRE(import_refused.error().find("working-set") != std::string::npos);

    REQUIRE_FALSE(detail::inspect_dawproject_archive(
        archive, kTestLimit, artifacts.artifacts.size() - 1,
        artifacts.artifacts.size(), 4'096));
    REQUIRE_FALSE(detail::inspect_dawproject_archive(
        archive, kTestLimit, artifacts.artifacts.size(),
        artifacts.artifacts.size() - 1, 4'096));
    REQUIRE_FALSE(detail::inspect_dawproject_archive(
        archive, kTestLimit, artifacts.artifacts.size(),
        artifacts.artifacts.size(), 4'095));

    const auto occupied = temp.path / "occupied.dawproject";
    const std::vector<std::uint8_t> sentinel{'k', 'e', 'e', 'p'};
    {
        std::ofstream stream(occupied, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(sentinel.data()),
                     static_cast<std::streamsize>(sentinel.size()));
    }
    const auto publish_refused = detail::write_dawproject_archive_no_replace(
        artifacts, occupied, kTestLimit);
    REQUIRE_FALSE(publish_refused);
    REQUIRE(publish_refused.error().code == detail::DawProjectArchiveErrorCode::Publish);
    std::ifstream preserved_stream(occupied, std::ios::binary);
    const std::vector<std::uint8_t> preserved{std::istreambuf_iterator<char>(preserved_stream),
                                              std::istreambuf_iterator<char>()};
    REQUIRE(preserved == sentinel);

    pulp::interchange::ExportArtifacts nul_collision;
    nul_collision.artifacts.push_back(
        {std::string("audio/x.wav\0a", 13), std::vector<std::uint8_t>{'a'}});
    nul_collision.artifacts.push_back(
        {std::string("audio/x.wav\0b", 13), std::vector<std::uint8_t>{'b'}});
    const auto nul_destination = temp.path / "nul-collision.dawproject";
    const auto nul_refused = detail::write_dawproject_archive_no_replace(
        nul_collision, nul_destination, kTestLimit);
    REQUIRE_FALSE(nul_refused);
    REQUIRE(nul_refused.error().code == detail::DawProjectArchiveErrorCode::Export);
    REQUIRE_FALSE(std::filesystem::exists(nul_destination));

    pulp::interchange::ExportArtifacts tiny;
    tiny.artifacts.push_back({"project.xml", std::vector<std::uint8_t>{'x'}});
    const auto tiny_archive = temp.path / "tiny.dawproject";
    REQUIRE(detail::write_dawproject_archive_no_replace(tiny, tiny_archive,
                                                         1024u * 1024u));
    REQUIRE_FALSE(detail::write_dawproject_archive_no_replace(
        artifacts, temp.path / "payload-dimension.dawproject", 1024u * 1024u));
    REQUIRE(detail::inspect_dawproject_archive(tiny_archive, 1024u * 1024u,
                                                1, 1, 4'096));
    REQUIRE_FALSE(detail::inspect_dawproject_archive(archive, 1024u * 1024u,
                                                      artifacts.artifacts.size(),
                                                      artifacts.artifacts.size(), 4'096));
}

} // namespace
