// test_mcp_audio_tools.cpp — the audio image MCP tools (pulp_audio_spectrogram,
// pulp_audio_waveform): argument guards and an end-to-end call against the
// staged CLI. Kept beside test_mcp_server.cpp so the server suite stays
// readable; both compile into pulp-test-mcp-server.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mcp_server_test_support.hpp"

#include "mcp_json.hpp"
#include "mcp_server.hpp"
#include "mcp_tools.hpp"

using namespace mcp_test;
using namespace pulp_mcp;
using namespace pulp_mcp::server;

TEST_CASE("MCP audio image tools validate parameters before shelling out",
          "[mcp][tools][audio][image]") {
    CliProjectRoot project(built_cli());
    ScopedCurrentPath cwd(project.path());

    auto no_file = handle_request(
        tool_call("71", "pulp_audio_spectrogram", R"JSON({"png_path":"/tmp/x.png"})JSON"));
    require_contains(no_file, R"JSON("id":71)JSON");
    require_contains(no_file, "Error: file is required");

    auto no_png =
        handle_request(tool_call("72", "pulp_audio_waveform", R"JSON({"file":"a.wav"})JSON"));
    require_contains(no_png, "Error: png_path is required");

    auto option_file = handle_request(tool_call(
        "73", "pulp_audio_spectrogram", R"JSON({"file":"--help","png_path":"/tmp/x.png"})JSON"));
    require_contains(option_file, "Error: file is required and must be an audio file path");

    auto zero_fft =
        handle_request(tool_call("74", "pulp_audio_spectrogram",
                                 R"JSON({"file":"a.wav","png_path":"/tmp/x.png","fft":0})JSON"));
    require_contains(zero_fft, "Error: fft must be a positive integer");

    auto bad_channel = handle_request(
        tool_call("75", "pulp_audio_waveform",
                  R"JSON({"file":"a.wav","png_path":"/tmp/x.png","channel":-1})JSON"));
    require_contains(bad_channel, "Error: channel must be a non-negative integer");

    auto fft_on_waveform =
        handle_request(tool_call("76", "pulp_audio_waveform",
                                 R"JSON({"file":"a.wav","png_path":"/tmp/x.png","fft":1024})JSON"));
    require_contains(fft_on_waveform, "Error: fft applies only to pulp_audio_spectrogram");

    auto negative_start =
        handle_request(tool_call("77", "pulp_audio_spectrogram",
                                 R"JSON({"file":"a.wav","png_path":"/tmp/x.png","start":-1})JSON"));
    require_contains(negative_start, "Error: start must be a non-negative number of seconds");

    auto bad_range = handle_request(
        tool_call("78", "pulp_audio_spectrogram",
                  R"JSON({"file":"a.wav","png_path":"/tmp/x.png","db_range":0})JSON"));
    require_contains(bad_range, "Error: db_range must be a positive number of dB");

    auto option_scale = handle_request(
        tool_call("79", "pulp_audio_spectrogram",
                  R"JSON({"file":"a.wav","png_path":"/tmp/x.png","scale":"--mel"})JSON"));
    require_contains(option_scale, "Error: scale must be a name, not an option");
}

TEST_CASE("MCP audio image tools return the PNG path and sidecar from the built CLI",
          "[mcp][tools][audio][image]") {
    CliProjectRoot project(built_cli());
    ScopedCurrentPath cwd(project.path());

    // A 0.5 s, 8 kHz mono PCM16 sine, written by hand so this test needs no
    // audio library: the CLI under test is the decoder.
    const auto dir = project.path() / "image-tool";
    std::filesystem::create_directories(dir);
    const auto wav = dir / "tone.wav";
    {
        const uint32_t rate = 8000;
        const uint32_t frames = 4000;
        std::vector<unsigned char> bytes(44 + frames * 2, 0);
        auto put32 = [&](std::size_t at, uint32_t v) {
            for (int i = 0; i < 4; ++i)
                bytes[at + static_cast<std::size_t>(i)] =
                    static_cast<unsigned char>((v >> (8 * i)) & 0xFF);
        };
        auto put16 = [&](std::size_t at, uint16_t v) {
            bytes[at] = static_cast<unsigned char>(v & 0xFF);
            bytes[at + 1] = static_cast<unsigned char>((v >> 8) & 0xFF);
        };
        std::copy_n("RIFF", 4, bytes.begin());
        put32(4, 36 + frames * 2);
        std::copy_n("WAVEfmt ", 8, bytes.begin() + 8);
        put32(16, 16);
        put16(20, 1);
        put16(22, 1);
        put32(24, rate);
        put32(28, rate * 2);
        put16(32, 2);
        put16(34, 16);
        std::copy_n("data", 4, bytes.begin() + 36);
        put32(40, frames * 2);
        for (uint32_t i = 0; i < frames; ++i) {
            const double s = 0.5 * std::sin(2.0 * 3.14159265358979 * 1000.0 * i / rate);
            put16(44 + static_cast<std::size_t>(i) * 2,
                  static_cast<uint16_t>(static_cast<int16_t>(s * 32767.0)));
        }
        std::ofstream out(wav, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        REQUIRE(out.good());
    }
    const auto png = dir / "tone.png";
    const std::string args =
        std::string("{\"file\":") + json_string(wav.string()) +
        ",\"png_path\":" + json_string(png.string()) +
        ",\"fft\":256,\"hop\":64,\"scale\":\"mel\",\"width\":64,\"height\":32}";
    auto response = handle_request(tool_call("80", "pulp_audio_spectrogram", args));
    INFO(response);
    require_contains(response, R"JSON("id":80)JSON");
    require_contains(response, "pulp.audio.image.v1");
    require_contains(response, "display_only");
    require_contains(response, "sidecar_path");
    require_contains(response, "tone.png.json");
    REQUIRE(std::filesystem::exists(png));
    REQUIRE(std::filesystem::exists(dir / "tone.png.json"));

    auto wave = handle_request(tool_call(
        "81", "pulp_audio_waveform",
        std::string("{\"file\":") + json_string(wav.string()) +
            ",\"png_path\":" + json_string((dir / "wave.png").string()) + ",\"width\":64}"));
    require_contains(wave, "pulp.audio.image.v1");
    require_contains(wave, R"(verb\": \"waveform)");

    // A CLI failure is an MCP error carrying the CLI's message, never a payload.
    auto missing = handle_request(
        tool_call("82", "pulp_audio_spectrogram",
                  std::string("{\"file\":") + json_string((dir / "missing.wav").string()) +
                      ",\"png_path\":" + json_string((dir / "missing.png").string()) + "}"));
    require_contains(missing, "Error: pulp audio spectrogram exited 1");
    require_contains(missing, "not a readable file");
}
