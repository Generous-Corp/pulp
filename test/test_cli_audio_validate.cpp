// test_cli_audio_validate.cpp — shell-out tests for `pulp audio validate`.
//
// Per CLAUDE.md ("CLI behavior changes — shell out to the built binary, assert
// exit code + stderr content"): these tests write tiny known WAV fixtures with
// pulp::audio::write_wav_file, then launch the built `pulp-cpp` binary and
// assert exit codes + stdout for each validate verb. They prove the verbs
// discriminate (clean vs distorted THD, identical vs differing compare) and
// fail loudly on bad input — not just that the binary launches.
//
// They also guard the hard constraint that adding `pulp audio validate` did
// not disturb the existing `pulp audio model/excerpt-find/read-bundle` surface.

#include "test_cli_shellout_helpers.hpp"

#include <pulp/audio/audio_file.hpp>

#include <cmath>
#include <numbers>

using namespace pulp_test_cli;
namespace fs = std::filesystem;

namespace {

// Write a mono sine WAV. When `clip` is set, hard-clip it so the output is rich
// in harmonics (a deterministic distortion the THD analyzer must flag).
fs::path write_sine_wav(const fs::path& path, double hz, double sample_rate,
                        int frames, float amplitude, bool clip) {
    pulp::audio::AudioFileData data;
    data.sample_rate = static_cast<uint32_t>(sample_rate);
    data.channels.resize(1);
    auto& ch = data.channels[0];
    ch.resize(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        double s = amplitude *
                   std::sin(2.0 * std::numbers::pi * hz * i / sample_rate);
        if (clip) s = std::clamp(s * 4.0, -0.3, 0.3); // hard clip → harmonics
        ch[static_cast<std::size_t>(i)] = static_cast<float>(s);
    }
    REQUIRE(pulp::audio::write_wav_file(path.string(), data));
    return path;
}

fs::path temp_wav(const std::string& name) {
    return fs::temp_directory_path() / ("pulp-cli-audio-validate-" + name);
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 4);
    constexpr char kHex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xf];
                    out += kHex[c & 0xf];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

} // namespace

TEST_CASE("audio validate summarize reports pitch and level on a sine",
          "[cli][shellout][audio-validate]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    const auto wav = write_sine_wav(temp_wav("sine.wav"), 440.0, 48000.0, 24000,
                                    0.5f, /*clip=*/false);

    auto r = run_pulp({"audio", "validate", "summarize", wav.string()});
    REQUIRE(r.exit_code == 0);
    // 440 Hz fundamental and -6 dBFS peak (amplitude 0.5) must be reported.
    REQUIRE(r.stdout_output.find("440") != std::string::npos);
    REQUIRE(r.stdout_output.find("dominant pitch") != std::string::npos);
    REQUIRE(r.stdout_output.find("-6.0 dBFS") != std::string::npos);
}

TEST_CASE("audio validate doctor --thd discriminates clean from distorted",
          "[cli][shellout][audio-validate]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    const auto clean = write_sine_wav(temp_wav("clean.wav"), 440.0, 48000.0,
                                      24000, 0.5f, /*clip=*/false);
    const auto dirty = write_sine_wav(temp_wav("dirty.wav"), 440.0, 48000.0,
                                      24000, 0.5f, /*clip=*/true);

    auto a = run_pulp(
        {"audio", "validate", "doctor", clean.string(), "--thd", "--fundamental", "440"});
    auto b = run_pulp(
        {"audio", "validate", "doctor", dirty.string(), "--thd", "--fundamental", "440"});
    REQUIRE(a.exit_code == 0);
    REQUIRE(b.exit_code == 0);
    REQUIRE(a.stdout_output.find("THD") != std::string::npos);
    REQUIRE(b.stdout_output.find("THD") != std::string::npos);

    // Parse the leading THD percentage off each line ("THD @ 440 Hz: X%...").
    auto thd_percent = [](const std::string& out) -> double {
        auto pos = out.find(": ");
        REQUIRE(pos != std::string::npos);
        return std::strtod(out.c_str() + pos + 2, nullptr);
    };
    const double clean_thd = thd_percent(a.stdout_output);
    const double dirty_thd = thd_percent(b.stdout_output);
    // The hard-clipped tone must read dramatically higher distortion.
    REQUIRE(clean_thd < 0.1);
    REQUIRE(dirty_thd > 1.0);
    REQUIRE(dirty_thd > clean_thd * 10.0);
}

TEST_CASE("audio validate compare passes identical, fails differing files",
          "[cli][shellout][audio-validate]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    const auto a = write_sine_wav(temp_wav("a.wav"), 440.0, 48000.0, 24000,
                                  0.5f, false);
    const auto b = write_sine_wav(temp_wav("b.wav"), 440.0, 48000.0, 24000,
                                  0.5f, false); // identical to a
    const auto c = write_sine_wav(temp_wav("c.wav"), 660.0, 48000.0, 24000,
                                  0.5f, false); // different pitch

    auto same = run_pulp({"audio", "validate", "compare", a.string(), b.string()});
    auto diff = run_pulp({"audio", "validate", "compare", a.string(), c.string()});
    REQUIRE(same.exit_code == 0);
    REQUIRE(same.stdout_output.find("PASS") != std::string::npos);
    REQUIRE(diff.exit_code == 1);
    REQUIRE(diff.stdout_output.find("FAIL") != std::string::npos);
}

TEST_CASE("audio validate assert exits nonzero on a failing assertions.json",
          "[cli][shellout][audio-validate]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    const auto wav = write_sine_wav(temp_wav("assert.wav"), 440.0, 48000.0,
                                    24000, 0.5f, false);
    REQUIRE(json_escape("C:\\tmp\\pulp\\assert.wav") ==
            "C:\\\\tmp\\\\pulp\\\\assert.wav");

    const auto pass_json = temp_wav("pass.json");
    write_text(pass_json,
               "{\"schema_version\":1,\"assertions\":[{\"name\":\"signal\","
               "\"file\":\"" + json_escape(wav.string()) +
                   "\",\"check\":\"not_silent\",\"min_rms_dbfs\":-60.0}]}");
    const auto fail_json = temp_wav("fail.json");
    write_text(fail_json,
               "{\"schema_version\":1,\"assertions\":[{\"name\":\"pitch\","
               "\"file\":\"" + json_escape(wav.string()) +
                   "\",\"check\":\"frequency_near\",\"expected_hz\":660.0,"
                   "\"tolerance_cents\":5.0}]}");

    auto pass = run_pulp({"audio", "validate", "assert", pass_json.string()});
    auto fail = run_pulp({"audio", "validate", "assert", fail_json.string()});
    REQUIRE(pass.exit_code == 0);
    REQUIRE(pass.stdout_output.find("1/1 passed") != std::string::npos);
    REQUIRE(fail.exit_code == 1);
    REQUIRE(fail.stdout_output.find("0/1 passed") != std::string::npos);
}

TEST_CASE("audio validate doctor --response is a peak-normalized self-spectrum",
          "[cli][shellout][audio-validate]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    // A 1 kHz tone: its own magnitude spectrum, normalized to the peak bin,
    // must read ~0 dB at 1000 Hz (the loudest frequency) and well below at an
    // off-tone frequency. This guards the prior bug where dividing by a flat
    // unit "reference" (a DC delta in frequency) produced absurd ~+240 dB,
    // backend-dependent magnitudes instead of a real spectrum.
    const auto wav = write_sine_wav(temp_wav("resp.wav"), 1000.0, 48000.0,
                                    48000, 0.5f, /*clip=*/false);
    auto r = run_pulp({"audio", "validate", "doctor", wav.string(),
                       "--response", "1000,5000"});
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("Magnitude spectrum") != std::string::npos);

    // Parse "  <hz> Hz: <db> dB" lines into hz -> db.
    auto db_at = [&](const std::string& hz) -> double {
        auto p = r.stdout_output.find("  " + hz + " Hz: ");
        REQUIRE(p != std::string::npos);
        return std::strtod(r.stdout_output.c_str() + p + 2 + hz.size() + 5, nullptr);
    };
    const double db1000 = db_at("1000");
    const double db5000 = db_at("5000");
    // Peak frequency ~0 dB; never a large positive value (the old bug).
    REQUIRE(db1000 > -1.0);
    REQUIRE(db1000 < 1.0);
    // Off-tone frequency is far below the peak.
    REQUIRE(db5000 < -40.0);
}

TEST_CASE("audio validate reports errors and exits nonzero on bad input",
          "[cli][shellout][audio-validate]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    // Missing file: clear error, nonzero exit, no crash.
    auto missing = run_pulp(
        {"audio", "validate", "summarize", "/nonexistent/none.wav"});
    REQUIRE(missing.exit_code == 1);
    REQUIRE(missing.stderr_output.find("not found") != std::string::npos);
    // Unknown verb: usage + nonzero.
    auto bad = run_pulp({"audio", "validate", "bogus"});
    REQUIRE(bad.exit_code == 1);
}

TEST_CASE("adding validate did not disturb the existing pulp audio surface",
          "[cli][shellout][audio-validate]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    // The existing model/read-bundle verbs still work; bare `pulp audio`
    // prints usage and exits 0.
    auto bare = run_pulp({"audio"});
    REQUIRE(bare.exit_code == 0);
    REQUIRE(bare.stdout_output.find("model list") != std::string::npos);
    auto model = run_pulp({"audio", "model", "list"});
    REQUIRE(model.exit_code == 0);
}
