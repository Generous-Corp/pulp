#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/tools/timeline/agent.hpp>

#include "../tools/mcp/mcp_tools.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#if defined(__linux__)
#include "linux_posix_acl_test_helpers.hpp"
#endif

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

using Catch::Matchers::WithinAbs;
using namespace pulp;
using namespace pulp::timeline;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

class TempDirectory {
  public:
    TempDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ =
            std::filesystem::temp_directory_path() / ("pulp-cli-timeline-" + std::to_string(nonce));
        REQUIRE(std::filesystem::create_directories(path_));
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

ContentHash file_hash(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream);
    const std::string bytes{std::istreambuf_iterator<char>(stream),
                            std::istreambuf_iterator<char>()};
    auto hash = ContentHash::from_hex(runtime::sha256_hex(bytes));
    REQUIRE(hash);
    return *hash;
}

std::string project_json(const std::filesystem::path& source,
                         AssetLocatorKind locator_kind = AssetLocatorKind::ExternalUri,
                         std::string locator_hint = {}, bool musical_clip = false) {
    constexpr std::uint64_t frame_count = 24;
    auto clip = musical_clip
                    ? take(Clip::create({4}, {0}, {timebase::kTicksPerQuarter},
                                        MediaRef{{5}, {0}, frame_count}, {.gain_linear = 1.0f}))
                    : take(Clip::create_absolute({4}, {0}, frame_count, {48'000, 1},
                                                 MediaRef{{5}, {0}, frame_count},
                                                 {.gain_linear = 1.0f}));
    auto track = take(Track::create({3}, "audio", {clip}));
    auto sequence = musical_clip
                        ? take(Sequence::create({2}, "root",
                                                timebase::TickDuration{timebase::kTicksPerQuarter},
                                                {track}))
                        : take(Sequence::create({2}, "root", std::nullopt,
                                                AbsoluteTimelineDuration{frame_count, {48'000, 1}},
                                                {track}));
    MediaAsset asset{{5},
                     "source.wav",
                     frame_count,
                     {48'000, 1},
                     file_hash(source),
                     AssetStoragePolicy::External,
                     {{locator_kind, locator_hint.empty() ? source.string() : locator_hint}},
                     {},
                     {}};
    auto project = take(Project::create(ProjectInput{{1}, "cli", 6, {2}, {asset}, {sequence}}));
    auto registry = take(make_builtin_timeline_registry());
    return take(serialize_project(project, registry)).json;
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(stream);
}

std::string quote(const std::filesystem::path& path) {
    std::string value = tools::timeline::filesystem_path_to_utf8(path);
#ifdef _WIN32
    return "\"" + value + "\"";
#else
    std::string result = "'";
    std::size_t offset = 0;
    while (true) {
        const auto quote_offset = value.find('\'', offset);
        if (quote_offset == std::string::npos)
            break;
        result.append(value, offset, quote_offset - offset);
        result += "'\\''";
        offset = quote_offset + 1;
    }
    result.append(value, offset, std::string::npos);
    result += "'";
    return result;
#endif
}

int run_cli(std::string command) {
#ifdef _WIN32
    command = "set PULP_UPDATE_CHECK_DISABLED=1 && " + command;
#else
    command = "PULP_UPDATE_CHECK_DISABLED=1 " + command;
#endif
    const auto status = std::system(command.c_str());
    if (status == -1)
        return 127;
#ifdef _WIN32
    return status;
#else
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128;
#endif
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool structurally_equal(const JsonValue& left, const JsonValue& right) {
    if (left.kind != right.kind)
        return false;
    switch (left.kind) {
    case JsonValue::Kind::Null: return true;
    case JsonValue::Kind::Boolean: return left.boolean == right.boolean;
    case JsonValue::Kind::Number:
    case JsonValue::Kind::String: return left.scalar == right.scalar;
    case JsonValue::Kind::Array:
        if (left.array.size() != right.array.size())
            return false;
        for (std::size_t index = 0; index < left.array.size(); ++index)
            if (!structurally_equal(left.array[index], right.array[index]))
                return false;
        return true;
    case JsonValue::Kind::Object:
        if (left.object.size() != right.object.size())
            return false;
        for (const auto& [key, value] : left.object) {
            const auto* candidate = right.find(key);
            if (candidate == nullptr || !structurally_equal(value, *candidate))
                return false;
        }
        return true;
    }
    return false;
}

Project decode_project_file(const std::filesystem::path& path) {
    auto registry = take(make_builtin_timeline_registry());
    return take(deserialize_project(read_text(path), registry));
}

void require_equivalent_media_arrangement(const Project& left, const Project& right) {
    const auto* left_sequence = left.find_sequence(left.root_sequence_id());
    const auto* right_sequence = right.find_sequence(right.root_sequence_id());
    REQUIRE(left_sequence != nullptr);
    REQUIRE(right_sequence != nullptr);
    REQUIRE(left_sequence->tracks().size() == right_sequence->tracks().size());
    REQUIRE(left_sequence->tracks().size() == 1);
    const auto left_clips = left_sequence->tracks().front().clips();
    const auto right_clips = right_sequence->tracks().front().clips();
    REQUIRE(left_clips.size() == right_clips.size());
    REQUIRE(left_clips.size() == 1);
    const auto& left_clip = left_clips[0];
    const auto& right_clip = right_clips[0];
    REQUIRE(left_clip.time_anchor() == ClipTimeAnchor::Musical);
    REQUIRE(right_clip.time_anchor() == ClipTimeAnchor::Musical);
    REQUIRE(left_clip.start() == right_clip.start());
    REQUIRE(left_clip.duration() == right_clip.duration());
    const auto* left_media = std::get_if<MediaRef>(&left_clip.content());
    const auto* right_media = std::get_if<MediaRef>(&right_clip.content());
    REQUIRE(left_media != nullptr);
    REQUIRE(right_media != nullptr);
    REQUIRE(left_media->source_start == right_media->source_start);
    REQUIRE(left_media->frame_count == right_media->frame_count);
    const auto* left_asset = left.find_asset(left_media->asset_id);
    const auto* right_asset = right.find_asset(right_media->asset_id);
    REQUIRE(left_asset != nullptr);
    REQUIRE(right_asset != nullptr);
    REQUIRE(left_asset->frame_count == right_asset->frame_count);
    REQUIRE(left_asset->sample_rate == right_asset->sample_rate);
    REQUIRE(left_asset->content_hash == right_asset->content_hash);
}

} // namespace

TEST_CASE("timeline CLI validates edits and renders through the installed command surface") {
    TempDirectory temp;
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(24, 0.8f)};
    const auto source_path = temp.path() / "source.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));

    const auto project_path = temp.path() / "project.json";
    const auto command_path = temp.path() / "commands.json";
    const auto changed_path = temp.path() / "changed.json";
    const auto command_result_path = temp.path() / "command-result.json";
    const auto explain_path = temp.path() / "explain.json";
    const auto validate_path = temp.path() / "validate.json";
    const auto original_wav = temp.path() / "original.wav";
    const auto changed_wav = temp.path() / "changed.wav";
    write_text(project_path, project_json(source_path));
    write_text(changed_path, "sentinel");
#ifndef _WIN32
    constexpr auto changed_permissions = std::filesystem::perms::set_uid |
                                         std::filesystem::perms::owner_all |
                                         std::filesystem::perms::group_read;
    std::filesystem::permissions(changed_path, changed_permissions,
                                 std::filesystem::perm_options::replace);
#endif
    write_text(
        command_path,
        R"([{"data":{"clip_id":"4","expected":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1065353216"},"replacement":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1056964608"},"sequence_id":"2","track_id":"3"},"type_name":"pulp.timeline.command.set_clip_playback_properties","version":1}])");

    const auto cli = quote(PULP_CLI_BIN);
    const auto braced_project_path = temp.path() / "{project.json";
    const auto braced_validate_path = temp.path() / "braced-validate.json";
    write_text(braced_project_path, project_json(source_path));
    REQUIRE(run_cli(cli + " seq validate " + quote(braced_project_path) + " > " +
                    quote(braced_validate_path)) == 0);
    REQUIRE(read_text(braced_validate_path).find(R"("ok":true)") != std::string::npos);

    const auto unicode_project_path =
        temp.path() / tools::timeline::filesystem_path_from_utf8("proyecto-\xE9\x9F\xB3.json");
    const auto unicode_command_path =
        temp.path() / tools::timeline::filesystem_path_from_utf8("comandos-\xCE\xA9.json");
    const auto unicode_output_path =
        temp.path() / tools::timeline::filesystem_path_from_utf8("cambiado-\xE9\x9F\xB3.json");
    const auto unicode_result_path =
        temp.path() / tools::timeline::filesystem_path_from_utf8("resultado-\xCE\xA9.json");
    write_text(unicode_project_path, project_json(source_path));
    write_text(unicode_command_path, read_text(command_path));
    REQUIRE(run_cli(cli + " seq validate " + quote(unicode_project_path) + " > " +
                    quote(unicode_result_path)) == 0);
    REQUIRE(run_cli(cli + " seq explain " + quote(unicode_project_path) + " > " +
                    quote(unicode_result_path)) == 0);
    REQUIRE(run_cli(cli + " seq apply " + quote(unicode_project_path) + " " +
                    quote(unicode_command_path) + " --out " + quote(unicode_output_path) +
                    " > " + quote(unicode_result_path)) == 0);
    REQUIRE(std::filesystem::is_regular_file(unicode_output_path));

    REQUIRE(run_cli(cli + " seq validate " + quote(project_path) + " > " + quote(validate_path)) ==
            0);
    REQUIRE(run_cli(cli + " seq explain " + quote(project_path) + " > " + quote(explain_path)) ==
            0);
    REQUIRE(read_text(explain_path).find(R"("audio_regions":1)") != std::string::npos);
    REQUIRE(run_cli(cli + " seq apply " + quote(project_path) + " " + quote(command_path) +
                    " --out " + quote(changed_path) + " > " + quote(command_result_path)) == 0);
    REQUIRE(std::filesystem::is_regular_file(changed_path));
    REQUIRE(read_text(changed_path) != "sentinel");
#ifndef _WIN32
    REQUIRE(std::filesystem::status(changed_path).permissions() == changed_permissions);
#if defined(__linux__)
    const auto acl_path = temp.path() / "acl.json";
    write_text(acl_path, "sentinel");
    const auto acl_result = linux_acl_test::install(acl_path);
    REQUIRE(acl_result != linux_acl_test::InstallResult::Failed);
    if (acl_result == linux_acl_test::InstallResult::Installed) {
        const auto expected_acl = linux_acl_test::read(acl_path);
        REQUIRE(expected_acl);
        REQUIRE(run_cli(cli + " seq apply " + quote(project_path) + " " + quote(command_path) +
                        " --out " + quote(acl_path) + " > /dev/null") == 0);
        REQUIRE(linux_acl_test::read(acl_path) == expected_acl);
    }
#endif

    const auto new_path = temp.path() / "new.json";
    const auto new_result =
        run_cli(cli + " seq apply " + quote(project_path) + " " + quote(command_path) + " --out " +
                quote(new_path) + " > /dev/null");
    REQUIRE(new_result == 0);
    REQUIRE(std::filesystem::status(new_path).permissions() ==
            (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));

    const auto write_only_path = temp.path() / "write-only.json";
    write_text(write_only_path, "sentinel");
    std::filesystem::permissions(write_only_path, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
    REQUIRE(run_cli(cli + " seq apply " + quote(project_path) + " " + quote(command_path) +
                    " --out " + quote(write_only_path) + " > /dev/null") == 0);
    REQUIRE(std::filesystem::status(write_only_path).permissions() ==
            std::filesystem::perms::owner_write);
#endif
    const auto changed_project = read_text(changed_path);
    for (const auto& entry : std::filesystem::directory_iterator(temp.path()))
        REQUIRE_FALSE(entry.path().filename().string().starts_with("changed.json.tmp."));

    const auto oversized_commands = temp.path() / "oversized-commands.json";
    const auto oversized_error = temp.path() / "oversized-error.txt";
    write_text(oversized_commands, "[]");
    std::error_code resize_error;
    std::filesystem::resize_file(oversized_commands,
                                 static_cast<std::uintmax_t>(DecodeLimits{}.max_input_bytes) +
                                     std::uintmax_t{1},
                                 resize_error);
    REQUIRE_FALSE(resize_error);
    write_text(changed_path, "sentinel");
    REQUIRE(run_cli(cli + " seq apply " + quote(project_path) + " " + quote(oversized_commands) +
                    " --out " + quote(changed_path) + " 2> " + quote(oversized_error)) == 1);
    REQUIRE(read_text(changed_path) == "sentinel");
    REQUIRE(read_text(oversized_error).find("command file exceeds") != std::string::npos);
    write_text(changed_path, changed_project);

    const auto blocked_output = temp.path() / "blocked-output.json";
    REQUIRE(std::filesystem::create_directory(blocked_output));
    REQUIRE(run_cli(cli + " seq apply " + quote(project_path) + " " + quote(command_path) +
                    " --out " + quote(blocked_output) + " > " + quote(command_result_path)) == 1);
    REQUIRE(std::filesystem::is_directory(blocked_output));

    const auto linked_target = temp.path() / "linked-target.json";
    const auto linked_output = temp.path() / "linked-output.json";
    write_text(linked_target, "preserve target");
    std::error_code symlink_error;
    std::filesystem::create_symlink(linked_target.filename(), linked_output, symlink_error);
    if (!symlink_error) {
        REQUIRE(run_cli(cli + " seq apply " + quote(project_path) + " " + quote(command_path) +
                        " --out " + quote(linked_output) + " > " + quote(command_result_path)) ==
                1);
        REQUIRE(std::filesystem::is_symlink(std::filesystem::symlink_status(linked_output)));
        REQUIRE(read_text(linked_target) == "preserve target");
    }

    REQUIRE(run_cli(cli + " render " + quote(project_path) + " --out " + quote(original_wav)) == 0);
    REQUIRE(run_cli(cli + " render " + quote(changed_path) + " --out " + quote(changed_wav)) == 0);
    const auto original = audio::read_audio_file(original_wav.string());
    const auto changed = audio::read_audio_file(changed_wav.string());
    REQUIRE(original);
    REQUIRE(changed);
    REQUIRE_THAT(original->channels[0][0], WithinAbs(0.8f, 1e-7f));
    REQUIRE_THAT(changed->channels[0][0], WithinAbs(0.4f, 1e-7f));

    REQUIRE(run_cli(cli + " render " + quote(project_path) + " --out " + quote(original_wav) +
                    " --sample-rate 0") == 2);
    REQUIRE(run_cli(cli + " render " + quote(project_path) + " --out " + quote(original_wav) +
                    " --sample-rate 768001") == 2);

    const auto package_project = temp.path() / "package-project.json";
    write_text(package_project,
               project_json(source_path, AssetLocatorKind::PackageRelative, "source.wav"));
    const auto moved_directory = temp.path() / "moved";
    REQUIRE(std::filesystem::create_directory(moved_directory));
    const auto moved_project = moved_directory / "changed.json";
    REQUIRE(run_cli(cli + " seq apply " + quote(package_project) + " " + quote(command_path) +
                    " --out " + quote(moved_project) + " > " + quote(command_result_path)) == 2);
    REQUIRE_FALSE(std::filesystem::exists(moved_project));
}

TEST_CASE("timeline CLI interchange requires exact consent and publishes new directories") {
    TempDirectory temp;
    const auto cli = quote(PULP_CLI_BIN);
    const auto project = temp.path() / "minimal.json";
    write_text(
        project,
        R"json({"data":{"assets":[],"id":"1","name":"fixture","next_item_id":"3","root_sequence_id":"2","sequences":[{"data":{"absolute_duration":null,"id":"2","musical_duration":"0","name":"root","tracks":[]},"type_name":"pulp.timeline.sequence","version":1}]},"type_name":"pulp.timeline.project","version":1})json");
    const auto exported = temp.path() / "exported";
    const auto export_command =
        cli + " seq export " + quote(project) + " --format smf --out " + quote(exported);
    const auto planned = temp.path() / "planned-but-unwritten";
    const auto plan_result = temp.path() / "plan-result.json";
    REQUIRE(run_cli(cli + " seq export " + quote(project) + " --format smf --plan > " +
                    quote(plan_result)) == 0);
    REQUIRE_FALSE(std::filesystem::exists(planned));
    const auto plan_json = read_text(plan_result);
    REQUIRE(parse_json(plan_json));
    REQUIRE(plan_json.find(R"json("plan_only":true)json") != std::string::npos);
    REQUIRE(plan_json.find(R"json("manifest":{"schema_version":1)json") !=
            std::string::npos);
    REQUIRE(plan_json.find(R"json("required_consent":[])json") != std::string::npos);
    REQUIRE(plan_json.find(R"json("output")json") == std::string::npos);
    REQUIRE(run_cli(cli + " seq export " + quote(project) + " --format smf --plan --out " +
                    quote(planned) + " > /dev/null 2>&1") == 2);
    REQUIRE(run_cli(cli + " seq export " + quote(project) +
                    " --format smf --plan --out '' > /dev/null 2>&1") == 2);
    REQUIRE(run_cli(cli + " seq export " + quote(project) +
                    " --format smf --plan --accept-loss clip.absolute > /dev/null 2>&1") == 2);
    REQUIRE(run_cli(cli + " seq export " + quote(project) +
                    " --format smf > /dev/null 2>&1") == 2);
    REQUIRE(run_cli(cli + " seq export " + quote(project) +
                    " --format smf --out '' > /dev/null 2>&1") == 2);
    const auto export_result = temp.path() / "export-result.json";
    REQUIRE(run_cli(export_command + " > " + quote(export_result)) == 0);
    const auto export_json = read_text(export_result);
    REQUIRE(parse_json(export_json));
    REQUIRE(export_json.find(R"json("manifest":{"schema_version":1)json") !=
            std::string::npos);
    REQUIRE(export_json.find(R"json("plan_only":false)json") != std::string::npos);
    REQUIRE(std::filesystem::is_regular_file(exported / "project.mid"));
    REQUIRE(std::filesystem::is_regular_file(exported / "pulp-loss-manifest.json"));

    write_text(exported / "sentinel.txt", "preserve");
    const auto publish_refusal = temp.path() / "publish-refusal.json";
    REQUIRE(run_cli(export_command + " > /dev/null 2> " + quote(publish_refusal)) == 1);
    const auto publish_refusal_json = read_text(publish_refusal);
    REQUIRE(parse_json(publish_refusal_json));
    REQUIRE(publish_refusal_json.find(R"json("manifest":{"schema_version":1)json") !=
            std::string::npos);
    REQUIRE(publish_refusal_json.find(R"json("stage":"publish")json") != std::string::npos);
    REQUIRE(read_text(exported / "sentinel.txt") == "preserve");

    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(24, 0.5f)};
    const auto source_path = temp.path() / "lossy-source.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));
    const auto lossy_project = temp.path() / "lossy-project.json";
    write_text(lossy_project, project_json(source_path));
    struct CapturedCli {
        int exit_code = 127;
        std::string stdout_text;
        std::string stderr_text;
    };
    const auto capture_cli = [&](std::string command, std::string_view name) {
        const auto stdout_path = temp.path() / (std::string(name) + ".stdout");
        const auto stderr_path = temp.path() / (std::string(name) + ".stderr");
        const auto exit_code = run_cli(std::move(command) + " > " + quote(stdout_path) + " 2> " +
                                       quote(stderr_path));
        return CapturedCli{exit_code, read_text(stdout_path), read_text(stderr_path)};
    };

    const auto lossy_plan = capture_cli(
        cli + " seq export " + quote(lossy_project) + " --format smf --plan", "lossy-plan");
    REQUIRE(lossy_plan.exit_code == 0);
    REQUIRE(lossy_plan.stderr_text.empty());
    const auto lossy_plan_document = take(parse_json(lossy_plan.stdout_text));
    const auto* plan_only = lossy_plan_document->root().find("plan_only");
    const auto* required = lossy_plan_document->root().find("required_consent");
    REQUIRE(plan_only != nullptr);
    REQUIRE(plan_only->kind == JsonValue::Kind::Boolean);
    REQUIRE(plan_only->boolean);
    REQUIRE(required != nullptr);
    REQUIRE(required->kind == JsonValue::Kind::Array);
    REQUIRE(required->array.size() > 1);
    std::vector<std::string> required_consent;
    for (const auto& concept_value : required->array) {
        REQUIRE(concept_value.kind == JsonValue::Kind::String);
        required_consent.push_back(concept_value.scalar);
    }

    const auto mcp_plan_document = take(parse_json(pulp_mcp::handle_timeline_export(
        "{\"format\":\"smf\",\"plan_only\":true,\"project\":" +
        quote_json_string(read_text(lossy_project)) + "}")));
    const auto* mcp_plan = mcp_plan_document->root().find("structuredContent");
    REQUIRE(mcp_plan != nullptr);
    REQUIRE(mcp_plan->kind == JsonValue::Kind::Object);
    const auto* mcp_required = mcp_plan->find("required_consent");
    const auto* cli_plan_manifest = lossy_plan_document->root().find("manifest");
    const auto* mcp_plan_manifest = mcp_plan->find("manifest");
    REQUIRE(mcp_required != nullptr);
    REQUIRE(structurally_equal(*required, *mcp_required));
    REQUIRE(cli_plan_manifest != nullptr);
    REQUIRE(mcp_plan_manifest != nullptr);
    const auto* cli_plan_losses = cli_plan_manifest->find("losses");
    const auto* mcp_plan_losses = mcp_plan_manifest->find("losses");
    REQUIRE(cli_plan_losses != nullptr);
    REQUIRE(mcp_plan_losses != nullptr);
    REQUIRE(structurally_equal(*cli_plan_losses, *mcp_plan_losses));

    const auto refused_output = temp.path() / "missing-one-consent";
    std::string refused_command = cli + " seq export " + quote(lossy_project) +
                                  " --format smf --out " + quote(refused_output);
    for (std::size_t index = 0; index + 1 < required_consent.size(); ++index)
        refused_command +=
            " --accept-loss " + quote(std::filesystem::path(required_consent[index]));
    const auto refused = capture_cli(std::move(refused_command), "missing-one-consent");
    REQUIRE(refused.exit_code == 1);
    REQUIRE(refused.stdout_text.empty());
    REQUIRE_FALSE(std::filesystem::exists(refused_output));
    const auto refused_document = take(parse_json(refused.stderr_text));
    const auto* refusal_ok = refused_document->root().find("ok");
    const auto* remaining = refused_document->root().find("required_consent");
    REQUIRE(refusal_ok != nullptr);
    REQUIRE(refusal_ok->kind == JsonValue::Kind::Boolean);
    REQUIRE_FALSE(refusal_ok->boolean);
    REQUIRE(remaining != nullptr);
    REQUIRE(remaining->kind == JsonValue::Kind::Array);
    REQUIRE(remaining->array.size() == 1);
    REQUIRE(remaining->array.front().kind == JsonValue::Kind::String);
    REQUIRE(remaining->array.front().scalar == required_consent.back());
    const auto* refusal_error = refused_document->root().find("error");
    REQUIRE(refusal_error != nullptr);
    REQUIRE(refusal_error->kind == JsonValue::Kind::Object);
    const auto* refusal_stage = refusal_error->find("stage");
    const auto* refusal_message = refusal_error->find("message");
    const auto* refusal_path = refusal_error->find("path");
    REQUIRE(refusal_stage != nullptr);
    REQUIRE(refusal_stage->kind == JsonValue::Kind::String);
    REQUIRE(refusal_stage->scalar == "export");
    REQUIRE(refusal_message != nullptr);
    REQUIRE(refusal_message->kind == JsonValue::Kind::String);
    REQUIRE(refusal_message->scalar ==
            "export to Standard MIDI File loses concepts that were not accepted: " +
                required_consent.back());
    REQUIRE(refusal_path != nullptr);
    REQUIRE(refusal_path->kind == JsonValue::Kind::String);
    REQUIRE(refusal_path->scalar == required_consent.back());

    const auto consented_output = temp.path() / "fully-consented";
    std::string consented_command = cli + " seq export " + quote(lossy_project) +
                                    " --format smf --out " + quote(consented_output);
    for (const auto& concept_id : required_consent)
        consented_command += " --accept-loss " + quote(std::filesystem::path(concept_id));
    const auto consented = capture_cli(std::move(consented_command), "fully-consented");
    REQUIRE(consented.exit_code == 0);
    REQUIRE(consented.stderr_text.empty());
    REQUIRE(std::filesystem::is_regular_file(consented_output / "project.mid"));
    REQUIRE(std::filesystem::is_regular_file(consented_output / "pulp-loss-manifest.json"));
    const auto consented_document = take(parse_json(consented.stdout_text));
    const auto artifact_manifest =
        take(parse_json(read_text(consented_output / "pulp-loss-manifest.json")));
    const auto* success_ok = consented_document->root().find("ok");
    const auto* success_manifest = consented_document->root().find("manifest");
    const auto* success_remaining = consented_document->root().find("required_consent");
    REQUIRE(success_ok != nullptr);
    REQUIRE(success_ok->kind == JsonValue::Kind::Boolean);
    REQUIRE(success_ok->boolean);
    REQUIRE(success_manifest != nullptr);
    REQUIRE(structurally_equal(*success_manifest, artifact_manifest->root()));
    REQUIRE(success_remaining != nullptr);
    REQUIRE(success_remaining->kind == JsonValue::Kind::Array);
    REQUIRE(success_remaining->array.empty());

    std::string accepted_json = "[";
    for (std::size_t index = 0; index < required_consent.size(); ++index) {
        if (index != 0)
            accepted_json += ',';
        accepted_json += quote_json_string(required_consent[index]);
    }
    accepted_json += ']';
    const auto mcp_output = temp.path() / "mcp-fully-consented";
    const auto mcp_success_document = take(parse_json(pulp_mcp::handle_timeline_export(
        "{\"accept_losses\":" + accepted_json + ",\"format\":\"smf\",\"output\":" +
        quote_json_string(tools::timeline::filesystem_path_to_utf8(mcp_output)) +
        ",\"project\":" + quote_json_string(read_text(lossy_project)) + "}")));
    const auto* mcp_success = mcp_success_document->root().find("structuredContent");
    REQUIRE(mcp_success != nullptr);
    REQUIRE(mcp_success->kind == JsonValue::Kind::Object);
    const auto* mcp_success_manifest = mcp_success->find("manifest");
    REQUIRE(mcp_success_manifest != nullptr);
    const auto* cli_success_losses = success_manifest->find("losses");
    const auto* mcp_success_losses = mcp_success_manifest->find("losses");
    REQUIRE(cli_success_losses != nullptr);
    REQUIRE(mcp_success_losses != nullptr);
    REQUIRE(structurally_equal(*cli_success_losses, *mcp_success_losses));
    REQUIRE(structurally_equal(*cli_success_losses, *cli_plan_losses));
    REQUIRE(std::filesystem::is_regular_file(mcp_output / "pulp-loss-manifest.json"));

    const auto unknown = temp.path() / "unknown";
    const auto unknown_loss = capture_cli(
        cli + " seq export " + quote(lossy_project) + " --format smf --out " + quote(unknown) +
            " --accept-loss clip.telepathy",
        "unknown-loss");
    REQUIRE(unknown_loss.exit_code == 2);
    REQUIRE(unknown_loss.stdout_text.empty());
    REQUIRE_FALSE(std::filesystem::exists(unknown));
    const auto unknown_document = take(parse_json(unknown_loss.stderr_text));
    const auto* unknown_ok = unknown_document->root().find("ok");
    const auto* unknown_error = unknown_document->root().find("error");
    REQUIRE(unknown_ok != nullptr);
    REQUIRE(unknown_ok->kind == JsonValue::Kind::Boolean);
    REQUIRE_FALSE(unknown_ok->boolean);
    REQUIRE(unknown_error != nullptr);
    REQUIRE(unknown_error->kind == JsonValue::Kind::Object);
    const auto* unknown_stage = unknown_error->find("stage");
    const auto* unknown_message = unknown_error->find("message");
    REQUIRE(unknown_stage != nullptr);
    REQUIRE(unknown_stage->kind == JsonValue::Kind::String);
    REQUIRE(unknown_stage->scalar == "arguments");
    REQUIRE(unknown_message != nullptr);
    REQUIRE(unknown_message->kind == JsonValue::Kind::String);
    REQUIRE(unknown_message->scalar == "unknown loss concept: clip.telepathy");

    const auto imported = temp.path() / "imported";
    REQUIRE(run_cli(cli + " seq import " + quote(exported / "project.mid") +
                    " --format smf --out '' > /dev/null 2>&1") == 2);
    REQUIRE(run_cli(cli + " seq import " + quote(exported / "project.mid") +
                    " --format smf --out " + quote(imported) + " > /dev/null") == 0);
    REQUIRE(std::filesystem::is_regular_file(imported / "project.json"));

    const auto daw_source_project = temp.path() / "daw-source-project.json";
    write_text(daw_source_project,
               project_json(source_path, AssetLocatorKind::ExternalUri, {}, true));
    const auto daw_archive = temp.path() / "source.dawproject";
    const auto daw_plan = capture_cli(
        cli + " seq export " + quote(daw_source_project) + " --format dawproject --plan",
        "daw-plan");
    REQUIRE(daw_plan.exit_code == 0);
    REQUIRE(daw_plan.stderr_text.empty());
    const auto daw_plan_document = take(parse_json(daw_plan.stdout_text));
    const auto* daw_required = daw_plan_document->root().find("required_consent");
    REQUIRE(daw_required != nullptr);
    REQUIRE(daw_required->kind == JsonValue::Kind::Array);
    std::string daw_export_command = cli + " seq export " + quote(daw_source_project) +
                                     " --format dawproject --out " + quote(daw_archive);
    for (const auto& concept_value : daw_required->array) {
        REQUIRE(concept_value.kind == JsonValue::Kind::String);
        daw_export_command +=
            " --accept-loss " + quote(std::filesystem::path(concept_value.scalar));
    }
    const auto daw_exported = capture_cli(std::move(daw_export_command), "daw-export-source");
    REQUIRE(daw_exported.exit_code == 0);
    REQUIRE(daw_exported.stderr_text.empty());
    REQUIRE(std::filesystem::is_regular_file(daw_archive));

    const auto daw_import = temp.path() / "daw-import";
    REQUIRE(run_cli(cli + " seq import " + quote(daw_archive) +
                    " --format dawproject --out " + quote(daw_import) + " > /dev/null") == 0);
    REQUIRE(std::filesystem::is_regular_file(daw_import / "project.json"));
    const auto only_media_beneath = [](const std::filesystem::path& root) {
        std::vector<std::filesystem::path> media_files;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
            if (entry.is_regular_file() && entry.path().filename() != "project.json")
                media_files.push_back(entry.path());
        REQUIRE(media_files.size() == 1);
        return media_files.front();
    };
    REQUIRE(file_hash(only_media_beneath(daw_import)) == file_hash(source_path));

    const auto daw_export = temp.path() / "roundtrip.dawproject";
    const auto imported_daw_plan = capture_cli(
        cli + " seq export " + quote(daw_import / "project.json") +
            " --format dawproject --plan",
        "daw-roundtrip-plan");
    REQUIRE(imported_daw_plan.exit_code == 0);
    REQUIRE(imported_daw_plan.stderr_text.empty());
    const auto imported_daw_plan_document = take(parse_json(imported_daw_plan.stdout_text));
    const auto* imported_daw_required =
        imported_daw_plan_document->root().find("required_consent");
    REQUIRE(imported_daw_required != nullptr);
    REQUIRE(imported_daw_required->kind == JsonValue::Kind::Array);
    std::string imported_daw_export_command =
        cli + " seq export " + quote(daw_import / "project.json") +
        " --format dawproject --out " + quote(daw_export);
    for (const auto& concept_value : imported_daw_required->array) {
        REQUIRE(concept_value.kind == JsonValue::Kind::String);
        imported_daw_export_command +=
            " --accept-loss " + quote(std::filesystem::path(concept_value.scalar));
    }
    const auto imported_daw_exported =
        capture_cli(std::move(imported_daw_export_command), "daw-roundtrip-export");
    REQUIRE(imported_daw_exported.exit_code == 0);
    REQUIRE(imported_daw_exported.stderr_text.empty());
    REQUIRE(std::filesystem::is_regular_file(daw_export));

    const auto daw_roundtrip = temp.path() / "daw-roundtrip";
    REQUIRE(run_cli(cli + " seq import " + quote(daw_export) +
                    " --format dawproject --out " + quote(daw_roundtrip) + " > /dev/null") == 0);
    REQUIRE(std::filesystem::is_regular_file(daw_roundtrip / "project.json"));
    REQUIRE(file_hash(only_media_beneath(daw_roundtrip)) == file_hash(source_path));
    require_equivalent_media_arrangement(decode_project_file(daw_import / "project.json"),
                                         decode_project_file(daw_roundtrip / "project.json"));

    const auto missing_source = temp.path() / "missing-source.wav";
    REQUIRE(audio::write_wav_file(missing_source.string(), source, audio::WavBitDepth::Float32));
    const auto missing_project = temp.path() / "missing-source-project.json";
    write_text(missing_project,
               project_json(missing_source, AssetLocatorKind::ExternalUri, {}, true));
    REQUIRE(std::filesystem::remove(missing_source));
    const auto missing_plan = capture_cli(
        cli + " seq export " + quote(missing_project) + " --format dawproject --plan",
        "missing-source-plan");
    REQUIRE(missing_plan.exit_code == 0);
    const auto missing_plan_document = take(parse_json(missing_plan.stdout_text));
    const auto* missing_required = missing_plan_document->root().find("required_consent");
    REQUIRE(missing_required != nullptr);
    REQUIRE(missing_required->kind == JsonValue::Kind::Array);
    const auto missing_archive = temp.path() / "missing-source.dawproject";
    std::string missing_command = cli + " seq export " + quote(missing_project) +
                                  " --format dawproject --out " + quote(missing_archive);
    for (const auto& concept_value : missing_required->array) {
        REQUIRE(concept_value.kind == JsonValue::Kind::String);
        missing_command +=
            " --accept-loss " + quote(std::filesystem::path(concept_value.scalar));
    }
    const auto missing_result =
        capture_cli(std::move(missing_command), "missing-source-export");
    REQUIRE(missing_result.exit_code == 1);
    REQUIRE(missing_result.stdout_text.empty());
    REQUIRE_FALSE(std::filesystem::exists(missing_archive));
    const auto missing_document = take(parse_json(missing_result.stderr_text));
    const auto* missing_error = missing_document->root().find("error");
    REQUIRE(missing_error != nullptr);
    REQUIRE(missing_error->kind == JsonValue::Kind::Object);
    const auto* missing_stage = missing_error->find("stage");
    const auto* missing_message = missing_error->find("message");
    const auto* missing_path = missing_error->find("path");
    REQUIRE(missing_stage != nullptr);
    REQUIRE(missing_stage->kind == JsonValue::Kind::String);
    REQUIRE(missing_stage->scalar == "export");
    REQUIRE(missing_message != nullptr);
    REQUIRE(missing_message->kind == JsonValue::Kind::String);
    REQUIRE(missing_message->scalar.find("asset 'source.wav'") != std::string::npos);
    REQUIRE(missing_path != nullptr);
    REQUIRE(missing_path->kind == JsonValue::Kind::String);
    REQUIRE(missing_path->scalar == "source.wav");

    const auto xml = temp.path() / "renamed.xml";
    write_text(xml, "<Project version=\"1.0\"/>");
    const auto dishonest = temp.path() / "dishonest";
    REQUIRE(run_cli(cli + " seq import " + quote(xml) + " --format dawproject --out " +
                    quote(dishonest) + " > /dev/null 2>&1") == 2);
    REQUIRE_FALSE(std::filesystem::exists(dishonest));
}
