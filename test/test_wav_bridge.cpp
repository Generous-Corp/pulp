// The WAV bridge writes an in-tree render to disk so the offline Python lane
// can analyze it. These tests prove the bytes that land on disk are the render:
// a float32 file round-trips the exact samples, an int24 file round-trips them
// within the quantization step, the header carries the render's rate / channel
// count / frame count, and the deterministic harness produces a byte-identical
// file on a repeat render.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "support/osc_wav_scenario.hpp"
#include "support/wav_bridge.hpp"

#include <pulp/audio/audio_file.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::osc::VaShape;
using pulp::test::audio::make_oscillator_scenario;
using pulp::test::audio::OscRenderSpec;
using pulp::test::audio::ScenarioResult;
using pulp::test::audio::write_scenario_wav;

namespace {

// A unique temp path in a bridge-owned subdirectory, removed by TempWav's dtor.
struct TempWav {
    std::filesystem::path path;
    explicit TempWav(const std::string& stem) {
        auto dir = std::filesystem::temp_directory_path() / "pulp-wav-bridge";
        std::filesystem::create_directories(dir);
        path = dir / (stem + ".wav");
        std::filesystem::remove(path);
    }
    ~TempWav() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::string str() const { return path.string(); }
};

OscRenderSpec sine_spec() {
    OscRenderSpec spec;
    spec.shape = VaShape::sine;
    spec.frequency_hz = 440.0;
    spec.sample_rate = 48000.0;
    spec.block_size = 128;
    spec.channels = 1;
    spec.duration_ms = 100.0;  // 4800 frames.
    spec.name = "bridge.sine";
    return spec;
}

std::vector<std::uint8_t> read_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

double max_abs_diff(const std::vector<float>& a, const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(a[i]) - b[i]));
    return worst;
}

} // namespace

TEST_CASE("float32 WAV round-trips the rendered samples exactly", "[wav-bridge]") {
    const ScenarioResult result = make_oscillator_scenario(sine_spec()).render();
    REQUIRE(result.output.num_channels() == 1);
    REQUIRE(result.output.num_samples() == 4800);
    // The render must actually contain signal — a silent buffer would pass a
    // round-trip trivially.
    REQUIRE(result.metrics.max_peak() > 0.1);

    TempWav wav("float-roundtrip");
    REQUIRE(write_scenario_wav(result, wav.str(),
                               pulp::audio::WavBitDepth::Float32));

    auto read = pulp::audio::read_audio_file(wav.str());
    REQUIRE(read.has_value());
    REQUIRE(read->sample_rate == 48000);
    REQUIRE(read->num_channels() == 1);
    REQUIRE(read->num_frames() == 4800);

    // float32 storage is lossless: the samples off disk equal the rendered
    // samples bit-for-bit.
    const std::vector<float> rendered(result.output.channel(0).begin(),
                                      result.output.channel(0).end());
    REQUIRE(max_abs_diff(rendered, read->channels[0]) == 0.0);
}

TEST_CASE("int24 WAV round-trips the samples within the quantization step",
          "[wav-bridge]") {
    const ScenarioResult result = make_oscillator_scenario(sine_spec()).render();

    TempWav wav("int24-roundtrip");
    REQUIRE(write_scenario_wav(result, wav.str(),
                               pulp::audio::WavBitDepth::Int24));

    auto read = pulp::audio::read_audio_file(wav.str());
    REQUIRE(read.has_value());
    REQUIRE(read->num_frames() == 4800);

    const std::vector<float> rendered(result.output.channel(0).begin(),
                                      result.output.channel(0).end());
    // One int24 step is 2^-23 ≈ 1.19e-7; the round-trip must land within one
    // step of the rendered value (rounding, not truncation, so allow one LSB).
    REQUIRE(max_abs_diff(rendered, read->channels[0]) < 2.0 / 8388608.0);
}

TEST_CASE("WAV header carries the render's rate, channels, and depth",
          "[wav-bridge]") {
    OscRenderSpec spec = sine_spec();
    spec.channels = 2;
    spec.sample_rate = 44100.0;
    spec.duration_ms = 50.0;  // round(50 * 44100 / 1000) = 2205 frames.
    const ScenarioResult result = make_oscillator_scenario(spec).render();

    TempWav wav("metadata");
    REQUIRE(write_scenario_wav(result, wav.str(),
                               pulp::audio::WavBitDepth::Float32));

    auto info = pulp::audio::read_audio_file_info(wav.str());
    REQUIRE(info.has_value());
    CHECK(info->sample_rate == 44100);
    CHECK(info->num_channels == 2);
    CHECK(info->num_frames == 2205);
    CHECK(info->bits_per_sample == 32);
    CHECK_FALSE(info->format.empty());
}

TEST_CASE("a repeated render writes a byte-identical WAV", "[wav-bridge]") {
    const OscRenderSpec spec = sine_spec();

    TempWav wav_a("determinism-a");
    TempWav wav_b("determinism-b");
    REQUIRE(write_scenario_wav(make_oscillator_scenario(spec).render(),
                               wav_a.str(), pulp::audio::WavBitDepth::Float32));
    REQUIRE(write_scenario_wav(make_oscillator_scenario(spec).render(),
                               wav_b.str(), pulp::audio::WavBitDepth::Float32));

    const auto bytes_a = read_bytes(wav_a.str());
    const auto bytes_b = read_bytes(wav_b.str());
    REQUIRE_FALSE(bytes_a.empty());
    REQUIRE(bytes_a == bytes_b);
}

TEST_CASE("drift/jitter renders stay deterministic through the bridge",
          "[wav-bridge]") {
    // With the seeded pitch noise active the render is still a pure function of
    // the spec (fixed seed), so the WAV bytes must still be reproducible — this
    // is the property the offline Allan-deviation analysis depends on.
    OscRenderSpec spec = sine_spec();
    spec.drift_cents = 5.0;
    spec.jitter_cents = 2.0;
    spec.name = "bridge.noisy";

    TempWav wav_a("noisy-a");
    TempWav wav_b("noisy-b");
    REQUIRE(write_scenario_wav(make_oscillator_scenario(spec).render(),
                               wav_a.str(), pulp::audio::WavBitDepth::Float32));
    REQUIRE(write_scenario_wav(make_oscillator_scenario(spec).render(),
                               wav_b.str(), pulp::audio::WavBitDepth::Float32));
    REQUIRE(read_bytes(wav_a.str()) == read_bytes(wav_b.str()));
}
