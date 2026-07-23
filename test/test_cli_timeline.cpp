#include <pulp/audio/audio_file.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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

ContentHash test_hash() {
    auto hash = ContentHash::from_hex(std::string(64, 'b'));
    REQUIRE(hash);
    return *hash;
}

std::string project_json(const std::filesystem::path& source) {
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
                     test_hash(),
                     AssetStoragePolicy::External,
                     {{AssetLocatorKind::ExternalUri, source.string()}},
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
}
