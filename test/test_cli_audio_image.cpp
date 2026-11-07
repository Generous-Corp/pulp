// test_cli_audio_image.cpp — shell-out tests for `pulp audio spectrogram` and
// `pulp audio waveform`.
//
// Per CLAUDE.md ("CLI behavior changes — shell out to the built binary, assert
// exit code + stderr content"): these write small WAV fixtures, launch the
// built `pulp-cpp`, and assert exit codes, stderr, the PNG, and the
// `pulp.audio.image.v1` sidecar. They cover argument validation, the sidecar
// key set and path redaction, float input above full scale, and a synthetic
// 70-minute WAV that `decode_wav` refuses but the ranged reader renders.

#include "test_cli_shellout_helpers.hpp"

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/wav_decoder.hpp>
#include <pulp/runtime/memory_mapped_file.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <regex>
#include <set>
#include <string>

using namespace pulp_test_cli;
namespace fs = std::filesystem;

namespace {

fs::path write_sine_wav(const fs::path& path, double hz, double sample_rate, int frames,
                        float amplitude, int channels, pulp::audio::WavBitDepth depth) {
    pulp::audio::AudioFileData data;
    data.sample_rate = static_cast<uint32_t>(sample_rate);
    data.channels.resize(static_cast<std::size_t>(channels));
    for (int c = 0; c < channels; ++c) {
        auto& ch = data.channels[static_cast<std::size_t>(c)];
        ch.resize(static_cast<std::size_t>(frames));
        for (int i = 0; i < frames; ++i)
            ch[static_cast<std::size_t>(i)] = static_cast<float>(
                amplitude * std::sin(2.0 * std::numbers::pi * hz * (c + 1) * i / sample_rate));
    }
    REQUIRE(pulp::audio::write_wav_file(path.string(), data, depth));
    return path;
}

// A canonical 44-byte PCM16 WAV header followed by `frames` frames of zeros,
// written sparsely: only the last byte is touched, so a 70-minute file costs
// no disk and reads as silence through the memory-mapped reader.
fs::path write_sparse_silent_wav(const fs::path& path, uint32_t sample_rate, uint64_t frames) {
    const uint64_t data_bytes = frames * 2;
    REQUIRE(data_bytes < 0xFFFFFFFFull);
    std::vector<uint8_t> header(44, 0);
    auto put32 = [&](std::size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            header[at + static_cast<std::size_t>(i)] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    };
    auto put16 = [&](std::size_t at, uint16_t v) {
        header[at] = static_cast<uint8_t>(v & 0xFF);
        header[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    std::memcpy(header.data(), "RIFF", 4);
    put32(4, static_cast<uint32_t>(36 + data_bytes));
    std::memcpy(header.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1); // PCM
    put16(22, 1); // mono
    put32(24, sample_rate);
    put32(28, sample_rate * 2);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(header.data() + 36, "data", 4);
    put32(40, static_cast<uint32_t>(data_bytes));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
    if (data_bytes > 0) {
        out.seekp(static_cast<std::streamoff>(44 + data_bytes - 1));
        const char zero = 0;
        out.write(&zero, 1);
    }
    REQUIRE(out.good());
    out.close();
    REQUIRE(fs::file_size(path) == 44 + data_bytes);
    return path;
}

struct Workspace {
    fs::path dir;
    Workspace() : dir(unique_temp_dir("pulp-cli-audio-image")) {
        fs::create_directories(dir);
    }
    ~Workspace() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path operator/(const std::string& name) const {
        return dir / name;
    }
};

bool has_png_signature(const fs::path& path) {
    const std::string bytes = read_file(path);
    static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return bytes.size() > 8 && std::memcmp(bytes.data(), kSig, 8) == 0;
}

// Top-level keys of a flat-ish JSON object: every `"key":` at indentation 2.
std::set<std::string> top_level_keys(const std::string& json) {
    std::set<std::string> keys;
    const std::regex key_re(R"re(\n  "([a-z_]+)": )re");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), key_re);
         it != std::sregex_iterator(); ++it)
        keys.insert((*it)[1].str());
    return keys;
}

double json_number_after(const std::string& json, const std::string& key) {
    const std::regex re("\"" + key + R"re(": (-?[0-9.eE+-]+|null))re");
    std::smatch match;
    REQUIRE(std::regex_search(json, match, re));
    if (match[1].str() == "null")
        return std::nan("");
    return std::stod(match[1].str());
}

std::string json_string_after(const std::string& json, const std::string& key) {
    const std::regex re("\"" + key + R"re(": "([^"]*)")re");
    std::smatch match;
    REQUIRE(std::regex_search(json, match, re));
    return match[1].str();
}

} // namespace

TEST_CASE("audio image verbs show help and refuse an empty invocation",
          "[cli][shellout][audio-image]") {
    if (!binary_exists())
        SKIP("pulp not built");
    for (const std::string verb : {"spectrogram", "waveform"}) {
        auto help = run_pulp({"audio", verb, "--help"});
        CHECK(help.exit_code == 0);
        CHECK(help.stdout_output.find("--png") != std::string::npos);
        auto bare = run_pulp({"audio", verb});
        CHECK(bare.exit_code == 1);
    }
}

TEST_CASE("audio image verbs validate arguments before touching audio",
          "[cli][shellout][audio-image]") {
    if (!binary_exists())
        SKIP("pulp not built");
    Workspace ws;
    const auto wav = write_sine_wav(ws / "stereo.wav", 440.0, 48000.0, 24000, 0.5f, 2,
                                    pulp::audio::WavBitDepth::Int16);
    const std::string png = (ws / "out.png").string();

    auto expect_error = [&](std::vector<std::string> args, const std::string& needle,
                            int code = 1) {
        auto r = run_pulp(args);
        INFO("args: " << args.back() << "\nstderr: " << r.stderr_output);
        CHECK(r.exit_code == code);
        CHECK(r.stderr_output.find(needle) != std::string::npos);
        CHECK(r.stderr_output.find("terminat") == std::string::npos);
    };

    expect_error({"audio", "spectrogram", wav.string()}, "--png");
    expect_error({"audio", "spectrogram", wav.string(), "--png", png, "--scale", "bark"},
                 "--scale");
    expect_error({"audio", "spectrogram", wav.string(), "--png", png, "--hop", "0"}, "--hop");
    expect_error({"audio", "spectrogram", wav.string(), "--png", png, "--fft", "1000"},
                 "power of two");
    expect_error({"audio", "spectrogram", wav.string(), "--png", png, "--fft", "16", "--hop", "32"},
                 "hop");
    expect_error({"audio", "spectrogram", wav.string(), "--png", png, "--window", "triangle"},
                 "--window");
    expect_error({"audio", "spectrogram", wav.string(), "--png", png, "--db-range", "0"},
                 "--db-range");
    expect_error(
        {"audio", "spectrogram", wav.string(), "--png", png, "--start", "1", "--end", "0.5"},
        "--end must be greater");
    expect_error({"audio", "spectrogram", wav.string(), "--png", png, "--start", "9"},
                 "past the end");
    expect_error({"audio", "spectrogram", wav.string(), "--png", png, "--channel", "2"},
                 "out of range");
    expect_error({"audio", "spectrogram", wav.string(), "--png", wav.string()},
                 "must not be the input");
    expect_error({"audio", "spectrogram", wav.string(), "--png", png, "--json", png},
                 "must not be the PNG");
    expect_error({"audio", "spectrogram", wav.string(), "--png", png, "--width", "0"}, "--width");
    expect_error({"audio", "spectrogram", wav.string(), "--png", png, "--bogus"}, "unknown option");
    expect_error({"audio", "spectrogram", (ws / "missing.wav").string(), "--png", png},
                 "not a readable file");
    expect_error({"audio", "waveform", wav.string(), "--png", png, "--select", "2,1"}, "--select");
    expect_error({"audio", "waveform", wav.string(), "--png", png, "--channel", "7"},
                 "out of range");

    // A PNG path whose parent is a regular file cannot be created.
    write_text(ws / "blocker", "x");
    expect_error(
        {"audio", "waveform", wav.string(), "--png", (ws / "blocker" / "out.png").string()},
        "failed to write PNG");

    // Input shorter than one FFT is a refusal (2), not a crash.
    expect_error({"audio", "spectrogram", wav.string(), "--png", png, "--fft", "65536"},
                 "shorter than --fft", 2);

    // Empty input (a valid header declaring zero frames) is a refusal too.
    write_sparse_silent_wav(ws / "empty.wav", 48000, 0);
    expect_error({"audio", "waveform", (ws / "empty.wav").string(), "--png", png}, "no frames", 2);
    CHECK_FALSE(fs::exists(png));
}

TEST_CASE("audio spectrogram writes a PNG and a pulp.audio.image.v1 sidecar",
          "[cli][shellout][audio-image]") {
    if (!binary_exists())
        SKIP("pulp not built");
    Workspace ws;
    const auto wav = write_sine_wav(ws / "tone.wav", 1000.0, 48000.0, 48000, 0.5f, 2,
                                    pulp::audio::WavBitDepth::Int16);
    const fs::path png = ws / "nested" / "spec.png";

    auto r = run_pulp({"audio", "spectrogram", wav.string(), "--png", png.string(), "--scale",
                       "mel", "--channel", "1", "--start", "0.25", "--end", "0.75", "--width",
                       "200", "--height", "100"});
    INFO(r.stderr_output);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(png));
    CHECK(has_png_signature(png));

    const fs::path sidecar = fs::path(png.string() + ".json");
    REQUIRE(fs::exists(sidecar));
    const std::string json = read_file(sidecar);

    const std::set<std::string> expected = {
        "schema",     "display_only", "verb",     "tool_version", "input",    "decoder",
        "time_range", "channel",      "settings", "image",        "true_peak"};
    CHECK(top_level_keys(json) == expected);
    CHECK(json_string_after(json, "schema") == "pulp.audio.image.v1");
    CHECK(json.find("\"display_only\": true") != std::string::npos);
    CHECK(json_string_after(json, "verb") == "spectrogram");
    CHECK(json_string_after(json, "name") == "tone.wav");
    CHECK(json_string_after(json, "png") == "spec.png");
    CHECK(json_string_after(json, "sha256").size() == 64);
    CHECK(json_string_after(json, "name").find('/') == std::string::npos);
    CHECK(json.find(ws.dir.string()) == std::string::npos); // no absolute paths by default
    CHECK(json_number_after(json, "channel") == 1.0);
    CHECK(json_number_after(json, "start_seconds") == Catch::Approx(0.25));
    CHECK(json_number_after(json, "end_seconds") == Catch::Approx(0.75));
    CHECK(json.find("\"scale\": \"mel\"") != std::string::npos);
    CHECK(json.find("\"mel_convention\": \"htk\"") != std::string::npos);
    CHECK(json_number_after(json, "fft") == 2048.0);
    CHECK(json_number_after(json, "plot_width") == 200.0);
    CHECK(json_number_after(json, "linear") == Catch::Approx(0.5).margin(0.002));
    CHECK(json.find("\"clipped\": false") != std::string::npos);
    CHECK(json_string_after(json, "tool_version").size() > 0);

    SECTION("--absolute-paths and --json opt into full paths and a custom sidecar") {
        const fs::path custom = ws / "custom.json";
        auto abs = run_pulp({"audio", "spectrogram", wav.string(), "--png", png.string(), "--json",
                             custom.string(), "--absolute-paths"});
        REQUIRE(abs.exit_code == 0);
        const std::string json2 = read_file(custom);
        CHECK(json_string_after(json2, "name").find('/') != std::string::npos);
        CHECK(json2.find("tone.wav") != std::string::npos);
    }
}

TEST_CASE("audio waveform reports float input above full scale without normalizing",
          "[cli][shellout][audio-image][clamp]") {
    if (!binary_exists())
        SKIP("pulp not built");
    Workspace ws;
    // +3 dBFS sine: amplitude 10^(3/20) = 1.4125, stored as float32.
    const float amplitude = static_cast<float>(std::pow(10.0, 3.0 / 20.0));
    const auto wav = write_sine_wav(ws / "hot.wav", 440.0, 48000.0, 24000, amplitude, 1,
                                    pulp::audio::WavBitDepth::Float32);
    const fs::path png = ws / "hot.png";
    auto r = run_pulp({"audio", "waveform", wav.string(), "--png", png.string()});
    INFO(r.stderr_output);
    REQUIRE(r.exit_code == 0);
    const std::string json = read_file(fs::path(png.string() + ".json"));
    CHECK(json_string_after(json, "verb") == "waveform");
    CHECK(json_number_after(json, "linear") == Catch::Approx(amplitude).margin(0.002));
    CHECK(json_number_after(json, "dbfs") == Catch::Approx(3.0).margin(0.02));
    CHECK(json.find("\"clipped\": true") != std::string::npos);
    CHECK(json.find("\"method\": \"sample_peak\"") != std::string::npos);

    // The spectrogram of the same file reports the same unclamped peak.
    auto s =
        run_pulp({"audio", "spectrogram", wav.string(), "--png", (ws / "hot-spec.png").string()});
    REQUIRE(s.exit_code == 0);
    const std::string json2 = read_file(ws / "hot-spec.png.json");
    CHECK(json_number_after(json2, "linear") == Catch::Approx(amplitude).margin(0.002));
    CHECK(json2.find("\"clipped\": true") != std::string::npos);
}

TEST_CASE("audio image verbs render a 70-minute WAV that decode_wav refuses",
          "[cli][shellout][audio-image][long]") {
    if (!binary_exists())
        SKIP("pulp not built");
    Workspace ws;
    constexpr uint32_t kRate = 48000;
    constexpr uint64_t kFrames = 70ull * 60 * kRate; // 201,600,000 > the 100,000,000 decode cap
    const auto wav = write_sparse_silent_wav(ws / "seventy-minutes.wav", kRate, kFrames);

    // Control: the whole-file decoder refuses this layout, so a render can
    // only have come through the ranged reader.
    {
        pulp::runtime::MemoryMappedFile mapped;
        REQUIRE(mapped.open(wav.string()));
        const std::span<const std::uint8_t> bytes(mapped.data(), mapped.size());
        const auto info = pulp::audio::inspect_wav(bytes);
        REQUIRE(info.has_value());
        CHECK(info->num_frames == kFrames);
        CHECK_FALSE(pulp::audio::decode_wav(bytes).has_value());
    }

    auto wave =
        run_pulp({"audio", "waveform", wav.string(), "--png", (ws / "long.png").string()}, 120000);
    INFO(wave.stderr_output);
    REQUIRE(wave.exit_code == 0);
    const std::string json = read_file(ws / "long.png.json");
    CHECK(json_number_after(json, "frames") == static_cast<double>(kFrames));
    CHECK(json_number_after(json, "duration_seconds") == Catch::Approx(4200.0));
    CHECK(json.find("\"clipped\": false") != std::string::npos);

    // A one-minute range past the decode cap renders a spectrogram.
    auto spec = run_pulp({"audio", "spectrogram", wav.string(), "--png",
                          (ws / "long-spec.png").string(), "--start", "3600", "--end", "3660"},
                         120000);
    INFO(spec.stderr_output);
    REQUIRE(spec.exit_code == 0);
    const std::string json2 = read_file(ws / "long-spec.png.json");
    CHECK(json_number_after(json2, "start_seconds") == Catch::Approx(3600.0));
    CHECK(json_number_after(json2, "end_seconds") == Catch::Approx(3660.0));
    CHECK(json_number_after(json2, "start_frame") == 3600.0 * kRate);
    CHECK(has_png_signature(ws / "long-spec.png"));
}
