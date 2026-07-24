#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>

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
#include <sys/stat.h>
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
                         std::string locator_hint = {}) {
    constexpr std::uint64_t frame_count = 24;
    auto clip = take(Clip::create_absolute({4}, {0}, frame_count, {48'000, 1},
                                           MediaRef{{5}, {0}, frame_count}, {.gain_linear = 1.0f}));
    auto track = take(Track::create({3}, "audio", {clip}));
    auto sequence = take(Sequence::create(
        {2}, "root", std::nullopt, AbsoluteTimelineDuration{frame_count, {48'000, 1}}, {track}));
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
    std::string value = path.string();
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
        REQUIRE(run_cli(cli + " seq apply " + quote(project_path) + " " +
                        quote(command_path) + " --out " + quote(acl_path) +
                        " > /dev/null") == 0);
        REQUIRE(linux_acl_test::read(acl_path) == expected_acl);
    }
#endif

    const auto new_path = temp.path() / "new.json";
    const auto previous_mask = ::umask(0027);
    const auto new_result =
        run_cli(cli + " seq apply " + quote(project_path) + " " + quote(command_path) + " --out " +
                quote(new_path) + " > /dev/null");
    ::umask(previous_mask);
    REQUIRE(new_result == 0);
    REQUIRE(std::filesystem::status(new_path).permissions() ==
            (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
             std::filesystem::perms::group_read));

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
