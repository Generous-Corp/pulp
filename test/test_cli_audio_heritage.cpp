#include "test_cli_shellout_helpers.hpp"

#include <fstream>
#include <string_view>
#include <utility>

using namespace pulp_test_cli;
namespace fs = std::filesystem;

namespace {

constexpr const char* profile_json =
    "{\"schema_version\":3,\"profile_id\":\"neutral.cli-fixture\","
    "\"host_sample_rate\":48000,\"voice\":[{\"domain\":\"voice\","
    "\"type\":\"converter\",\"bypass\":false,\"family\":\"linear_pcm\","
    "\"bit_depth\":12,\"dac_nonlinearity\":0,\"dither_lsb\":0.25,"
    "\"seed\":\"17\",\"seed_policy\":\"restart_from_profile_seed\"}],"
    "\"bus\":[],\"record_commit\":[]}";

constexpr const char* stretched_profile_json =
    "{\"schema_version\":3,\"profile_id\":\"neutral.cli-stretched-fixture\","
    "\"host_sample_rate\":48000,\"voice\":[{\"domain\":\"voice\","
    "\"type\":\"live_cyclic_stretch\",\"bypass\":false,\"factor\":2,"
    "\"cycle_ms\":10,\"splice_ms\":1,\"stereo_link\":true,"
    "\"shuffle_divisions\":0,\"seed\":\"0\","
    "\"seed_policy\":\"restart_from_profile_seed\"}],"
    "\"bus\":[],\"record_commit\":[]}";

fs::path make_profile(const fs::path& directory, std::string_view contents = profile_json) {
    fs::create_directories(directory);
    const auto path = directory / "profile.json";
    std::ofstream output(path, std::ios::binary);
    output << contents;
    return path;
}

std::uint32_t little_u32(std::string_view bytes, std::size_t offset) {
    REQUIRE(offset + 4 <= bytes.size());
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
}

std::size_t mono_float32_wav_frames(std::string_view bytes) {
    REQUIRE(bytes.size() >= 12);
    REQUIRE(bytes.substr(0, 4) == "RIFF");
    REQUIRE(bytes.substr(8, 4) == "WAVE");
    std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const auto size = static_cast<std::size_t>(little_u32(bytes, offset + 4));
        if (bytes.substr(offset, 4) == "data") {
            REQUIRE(size % sizeof(float) == 0);
            return size / sizeof(float);
        }
        offset += 8 + size + (size & 1u);
    }
    FAIL("WAV has no data chunk");
    return 0;
}

} // namespace

TEST_CASE("audio heritage help, arity, and unknown verbs have stable exits",
          "[cli][shellout][audio-heritage]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }
    const auto help = run_pulp({"audio", "heritage", "--help"});
    REQUIRE(help.exit_code == 0);
    REQUIRE(help.stdout_output.find("canonicalize PROFILE --out FILE") != std::string::npos);

    const auto missing = run_pulp({"audio", "heritage", "validate"});
    REQUIRE(missing.exit_code == 2);
    REQUIRE(missing.stderr_output.find("PROFILE is required") != std::string::npos);

    const auto unknown = run_pulp({"audio", "heritage", "mystery"});
    REQUIRE(unknown.exit_code == 2);
    REQUIRE(unknown.stderr_output.find("unknown verb") != std::string::npos);
}

TEST_CASE("audio heritage validate reports version and strict parse failures",
          "[cli][shellout][audio-heritage]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }
    const auto directory = unique_temp_dir("pulp-cli-heritage-validate");
    auto profile = make_profile(directory);
    auto valid = run_pulp({"audio", "heritage", "validate", profile.string(), "--json"});
    REQUIRE(valid.exit_code == 0);
    REQUIRE(valid.stdout_output.find("\"status\":\"ok\"") != std::string::npos);
    REQUIRE(valid.stdout_output.find("\"field_path\":\"\"") != std::string::npos);

    auto version_text = std::string(profile_json);
    version_text.replace(version_text.find("\"schema_version\":3"), 18, "\"schema_version\":4");
    profile = make_profile(directory, version_text);
    auto version = run_pulp({"audio", "heritage", "validate", profile.string(), "--json"});
    REQUIRE(version.exit_code == 1);
    REQUIRE(version.stdout_output.find("unsupported_schema_version") != std::string::npos);

    profile = make_profile(directory, "{\"schema_version\":3,\"tampered\":true}");
    auto tampered = run_pulp({"audio", "heritage", "validate", profile.string(), "--json"});
    REQUIRE(tampered.exit_code == 1);
    REQUIRE(tampered.stdout_output.find("unknown_field") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE("audio heritage canonicalization is deterministic and bare-output safe",
          "[cli][shellout][audio-heritage]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }
    const auto directory = unique_temp_dir("pulp-cli-heritage-canonical");
    const auto profile = make_profile(directory, std::string(" \n") + profile_json + "\n");
    fs::create_directories(directory / "elsewhere");
    auto first = run_pulp_in_directory(
        directory / "elsewhere",
        {"audio", "heritage", "canonicalize", profile.string(), "--out", "canonical.json"});
    REQUIRE(first.exit_code == 0);
    const auto first_bytes = read_file(directory / "elsewhere" / "canonical.json");
    REQUIRE(first_bytes == profile_json);
    auto second =
        run_pulp_in_directory(directory / "elsewhere", {"audio", "heritage", "canonicalize",
                                                        "canonical.json", "--out", "again.json"});
    REQUIRE(second.exit_code == 0);
    REQUIRE(read_file(directory / "elsewhere" / "again.json") == first_bytes);
    fs::remove_all(directory);
}

TEST_CASE("audio heritage inspect is neutral and stable JSON", "[cli][shellout][audio-heritage]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }
    const auto directory = unique_temp_dir("pulp-cli-heritage-inspect");
    const auto profile = make_profile(directory);
    const auto result = run_pulp({"audio", "heritage", "inspect", profile.string(), "--json"});
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_output.find("\"profile_id\":\"neutral.cli-fixture\"") !=
            std::string::npos);
    REQUIRE(result.stdout_output.find("\"active_mechanisms\":[\"voice.converter\"]") !=
            std::string::npos);
    REQUIRE(result.stdout_output.find("\"seed\":\"17\"") != std::string::npos);
    REQUIRE(result.stdout_output.find("\"latency_frames\":") != std::string::npos);
    REQUIRE(result.stdout_output.find("\"profile_source_frames_per_output\":1") !=
            std::string::npos);
    REQUIRE(result.stdout_output.find("\"resident_source\":\"exempt\"") != std::string::npos);
    REQUIRE(result.stdout_output.find("\"maximum_note_pitch_factor_at_cap\":4") !=
            std::string::npos);
    REQUIRE(result.stdout_output.find("\"sinc_promotion\":\"unchanged\"") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE("audio heritage fixture render and report are deterministic",
          "[cli][shellout][audio-heritage]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }
    const auto directory = unique_temp_dir("pulp-cli-heritage-render");
    const auto profile = make_profile(directory);
    const auto invoke = [&](std::string wav, std::string report) {
        return run_pulp_in_directory(directory, {"audio", "heritage", "render", profile.string(),
                                                 "--fixture", "two-tone", "--out", std::move(wav),
                                                 "--report", std::move(report), "--frames", "1024",
                                                 "--block-size", "127"});
    };
    const auto first = invoke("first.wav", "first.json");
    REQUIRE(first.exit_code == 0);
    const auto second = invoke("second.wav", "second.json");
    REQUIRE(second.exit_code == 0);
    REQUIRE(read_file(directory / "first.wav") == read_file(directory / "second.wav"));
    auto first_report = read_file(directory / "first.json");
    auto second_report = read_file(directory / "second.json");
    REQUIRE(first_report == second_report);
    REQUIRE(first_report.find("\"status\":\"ok\"") != std::string::npos);
    REQUIRE(first_report.find("\"wav_format\":\"float32\"") != std::string::npos);
    REQUIRE(first_report.find("\"audio_sha256\":") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE("audio heritage live cyclic render ends at exact rounded source duration",
          "[cli][shellout][audio-heritage]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }
    const auto directory = unique_temp_dir("pulp-cli-heritage-live-duration");
    const auto profile = make_profile(directory, stretched_profile_json);
    const auto result = run_pulp_in_directory(
        directory,
        {"audio", "heritage", "render", profile.string(), "--fixture", "sine", "--out",
         "stretched.wav", "--report", "stretched.json", "--frames", "257", "--block-size", "127"});
    REQUIRE(result.exit_code == 0);
    REQUIRE(mono_float32_wav_frames(read_file(directory / "stretched.wav")) == 514);
    const auto report = read_file(directory / "stretched.json");
    REQUIRE(report.find("\"requested_frames\":257") != std::string::npos);
    REQUIRE(report.find("\"frames\":514") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE("audio heritage output staging cannot collide with destination-like paths",
          "[cli][shellout][audio-heritage]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }
    const auto directory = unique_temp_dir("pulp-cli-heritage-temp-collision");
    const auto profile = make_profile(directory);
    write_text(directory / "result.tmp.tmp", "unrelated sentinel");
    const auto result = run_pulp_in_directory(
        directory, {"audio", "heritage", "render", profile.string(), "--fixture", "impulse",
                    "--out", "result.tmp", "--report", "result", "--frames", "64"});
    REQUIRE(result.exit_code == 0);
    REQUIRE(mono_float32_wav_frames(read_file(directory / "result.tmp")) >= 64);
    REQUIRE(read_file(directory / "result").find("\"status\":\"ok\"") != std::string::npos);
    REQUIRE(read_file(directory / "result.tmp.tmp") == "unrelated sentinel");
    fs::remove_all(directory);
}

TEST_CASE("audio heritage failed atomic publish preserves the old destination",
          "[cli][shellout][audio-heritage]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }
    const auto directory = unique_temp_dir("pulp-cli-heritage-publish-rollback");
    const auto profile = make_profile(directory);
    const auto destination = directory / "preserved.json";
    fs::create_directories(destination);
    write_text(destination / "sentinel.txt", "old destination remains");

    const auto result = run_pulp(
        {"audio", "heritage", "canonicalize", profile.string(), "--out", destination.string()});
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_output.find("could not publish output atomically") != std::string::npos);
    REQUIRE(fs::is_directory(destination));
    REQUIRE(read_file(destination / "sentinel.txt") == "old destination remains");
    for (const auto& entry : fs::directory_iterator(directory))
        REQUIRE(entry.path().filename().string().find("preserved.json.tmp.") != 0);
    fs::remove_all(directory);
}
