// Coverage for the standalone live capture-to-WAV writer: drain the output
// probe's capture ring into a WAV the offline `pulp audio validate` verbs can
// read. Exercises the real AudioProbe capture path (prepare → analyze_output →
// read_capture) and the int16 WAV round-trip, plus the headless predicate that
// keeps a capture-wav-only run from forcing a screenshot.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/audio_probe.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/format/detail/standalone_audio_capture_wav.hpp>
#include <pulp/format/detail/standalone_environment.hpp>
#include <pulp/format/standalone.hpp>

#include <cmath>
#include <filesystem>
#include <vector>

namespace {

namespace fs = std::filesystem;

TEST_CASE("write_audio_capture_wav_file dumps the probe ring to a readable WAV",
          "[standalone][audio-capture-wav]") {
    constexpr int kChannels = 2;
    constexpr int kBlock = 128;
    constexpr int kWindow = 256;  // two blocks fit exactly

    pulp::audio::AudioProbe probe;
    pulp::audio::AudioProbe::CaptureConfig cc;
    cc.capture_frames = kWindow;
    probe.prepare(kChannels, kBlock, 48000.0,
                  pulp::audio::AudioProbeStage::kStandaloneOutputBoundary, cc);

    // Feed two blocks of a per-channel ramp distinct across channels, so a
    // channel swap or a wrong window would diverge. The FIFO is drop-on-full, so
    // these two blocks (256 frames) are exactly the captured window.
    auto sample = [](int ch, int frame) {
        return static_cast<float>(ch + 1) * 0.1f + static_cast<float>(frame) * 0.0001f;
    };
    int global = 0;
    for (int b = 0; b < 2; ++b) {
        std::vector<std::vector<float>> blk(kChannels, std::vector<float>(kBlock, 0.0f));
        std::vector<const float*> ptrs;
        for (int ch = 0; ch < kChannels; ++ch) {
            for (int i = 0; i < kBlock; ++i) blk[ch][i] = sample(ch, global + i);
            ptrs.push_back(blk[ch].data());
        }
        global += kBlock;
        pulp::audio::BufferView<const float> view(ptrs.data(), kChannels, kBlock);
        probe.analyze_output(view);
    }

    pulp::format::StandaloneConfig cfg;
    cfg.audio_capture_wav_frames = kWindow;
    const auto path = (fs::temp_directory_path() / "pulp_capture_wav_test.wav").string();
    fs::remove(path);

    REQUIRE(pulp::format::detail::write_audio_capture_wav_file(path, probe, cfg));

    const auto data = pulp::audio::read_audio_file(path);
    REQUIRE(data.has_value());
    CHECK(data->num_channels() == static_cast<std::uint32_t>(kChannels));
    CHECK(data->num_frames() == static_cast<std::uint64_t>(kWindow));
    CHECK(data->sample_rate == 48000u);

    // int16 round-trip tolerance (~1/32768). The earliest-window contract means
    // frame f holds sample(ch, f).
    for (int ch = 0; ch < kChannels; ++ch) {
        CHECK(std::abs(data->channels[ch][0] - sample(ch, 0)) < 1e-3f);
        CHECK(std::abs(data->channels[ch][kWindow - 1] - sample(ch, kWindow - 1)) < 1e-3f);
    }
    // The two channels are genuinely distinct (not a degenerate all-equal dump).
    CHECK(std::abs(data->channels[0][10] - data->channels[1][10]) > 1e-2f);

    fs::remove(path);
}

TEST_CASE("write_audio_capture_wav_file with an empty path or empty ring is a no-op",
          "[standalone][audio-capture-wav]") {
    pulp::audio::AudioProbe probe;  // not prepared with capture → nothing to drain
    pulp::format::StandaloneConfig cfg;
    CHECK_FALSE(pulp::format::detail::write_audio_capture_wav_file("", probe, cfg));
    CHECK_FALSE(pulp::format::detail::write_audio_capture_wav_file(
        (fs::temp_directory_path() / "pulp_capture_empty.wav").string(), probe, cfg));
}

#if PULP_ENABLE_AUDIO_PROBES
TEST_CASE("a capture-wav-only headless run does not require a screenshot",
          "[standalone][audio-capture-wav]") {
    pulp::format::StandaloneConfig cfg;
    cfg.headless = true;
    cfg.audio_capture_wav_path = "out.wav";
    CHECK_FALSE(pulp::format::detail::standalone_headless_requires_screenshot(cfg));
}
#endif

}  // namespace
