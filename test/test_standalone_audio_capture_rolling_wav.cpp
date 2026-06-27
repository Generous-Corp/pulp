// Coverage for the standalone live ROLLING capture-to-WAV writer: materialize
// the RollingAudioCaptureBuffer's LAST window into a float WAV the offline `pulp
// audio validate` verbs can read. Exercises the real rolling buffer (prepare →
// append → hold_last → materialize_held) and the float WAV round-trip, proving
// the two properties that distinguish it from the earliest-window int16 dump:
// the LAST window survives an overflow, and sub-int16 values are preserved.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/rolling_audio_capture_buffer.hpp>
#include <pulp/format/detail/standalone_audio_capture_rolling_wav.hpp>
#include <pulp/format/detail/standalone_environment.hpp>
#include <pulp/format/standalone.hpp>

#include <cmath>
#include <filesystem>
#include <functional>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Append one block of a per-channel, absolute-frame ramp to the rolling buffer.
void append_block(pulp::audio::RollingAudioCaptureBuffer& rolling, int channels,
                  int block, std::uint64_t start_frame,
                  const std::function<float(int, std::uint64_t)>& sample) {
    std::vector<std::vector<float>> blk(channels, std::vector<float>(block, 0.0f));
    std::vector<const float*> ptrs;
    for (int ch = 0; ch < channels; ++ch) {
        for (int i = 0; i < block; ++i)
            blk[ch][i] = sample(ch, start_frame + static_cast<std::uint64_t>(i));
        ptrs.push_back(blk[ch].data());
    }
    pulp::audio::BufferView<const float> view(ptrs.data(), channels, block);
    rolling.append(view, static_cast<std::uint64_t>(block));
}

TEST_CASE("rolling capture WAV keeps the LAST window and preserves float precision",
          "[standalone][audio-capture-rolling]") {
    constexpr int kChannels = 2;
    constexpr int kBlock = 128;
    constexpr std::uint64_t kWindow = 256;  // ring holds the last 256 frames

    // A per-channel ramp PLUS a sub-int16 component so a float-vs-int16 writer
    // diverges. Distinct across channels so a swap or wrong window shows up.
    auto sample = [](int ch, std::uint64_t frame) {
        return static_cast<float>(ch + 1) * 0.1f
             + static_cast<float>(frame) * 0.0001f
             + 1.0e-6f;  // below the int16 step (~3e-5)
    };

    pulp::audio::RollingAudioCaptureBuffer rolling;
    pulp::audio::RollingAudioCaptureBufferConfig rc;
    rc.num_channels = kChannels;
    rc.max_frames = kWindow;
    REQUIRE(rolling.prepare(rc));

    // Append SIX blocks (768 frames) so the ring overflows several times — the
    // earliest-window FIFO would hold frames [0,256); the rolling ring must hold
    // the LAST 256 frames, i.e. [512,768).
    for (int b = 0; b < 6; ++b)
        append_block(rolling, kChannels, kBlock,
                     static_cast<std::uint64_t>(b) * kBlock, sample);

    pulp::format::StandaloneConfig cfg;
    cfg.sample_rate = 48000.0;
    cfg.audio_capture_rolling_frames = static_cast<int>(kWindow);
    const auto path =
        (fs::temp_directory_path() / "pulp_capture_rolling_test.wav").string();
    fs::remove(path);

    REQUIRE(pulp::format::detail::write_audio_capture_rolling_wav_file(path, rolling, cfg));

    const auto data = pulp::audio::read_audio_file(path);
    REQUIRE(data.has_value());
    CHECK(data->num_channels() == static_cast<std::uint32_t>(kChannels));
    CHECK(data->num_frames() == kWindow);
    CHECK(data->sample_rate == 48000u);

    // LAST window: the first captured frame is absolute frame 512 (768 - 256).
    // Float round-trip preserves the sub-int16 component, so the tolerance is
    // far tighter than int16's ~3e-5.
    const std::uint64_t first_abs = 6 * kBlock - kWindow;  // 512
    for (int ch = 0; ch < kChannels; ++ch) {
        CHECK(std::abs(data->channels[ch][0] - sample(ch, first_abs)) < 1.0e-5f);
        CHECK(std::abs(data->channels[ch][kWindow - 1]
                       - sample(ch, first_abs + kWindow - 1)) < 1.0e-5f);
    }
    CHECK(std::abs(data->channels[0][10] - data->channels[1][10]) > 1.0e-2f);

    fs::remove(path);
}

TEST_CASE("rolling capture writes int24 when requested (default float)",
          "[standalone][audio-capture-rolling]") {
    constexpr int kChannels = 1;
    constexpr int kBlock = 64;
    constexpr std::uint64_t kWindow = 128;

    pulp::audio::RollingAudioCaptureBuffer rolling;
    pulp::audio::RollingAudioCaptureBufferConfig rc;
    rc.num_channels = kChannels;
    rc.max_frames = kWindow;
    REQUIRE(rolling.prepare(rc));
    auto sig = [](int, std::uint64_t f) { return 0.25f + static_cast<float>(f) * 1.0e-4f; };
    for (int b = 0; b < 2; ++b)
        append_block(rolling, kChannels, kBlock, static_cast<std::uint64_t>(b) * kBlock, sig);

    pulp::format::StandaloneConfig cfg;
    cfg.sample_rate = 48000.0;
    cfg.audio_capture_rolling_frames = static_cast<int>(kWindow);
    cfg.audio_capture_rolling_int24 = true;
    const auto path =
        (fs::temp_directory_path() / "pulp_capture_rolling_i24.wav").string();
    fs::remove(path);

    REQUIRE(pulp::format::detail::write_audio_capture_rolling_wav_file(path, rolling, cfg));
    const auto info = pulp::audio::read_audio_file_info(path);
    REQUIRE(info.has_value());
    CHECK(info->bits_per_sample == 24);  // int24, not the float default
    fs::remove(path);
}

TEST_CASE("rolling capture WAV trims to the delivered channel count",
          "[standalone][audio-capture-rolling]") {
    // The ring is prepared for 2 channels (the configured output bus), but the
    // device only delivers 1 (a mono callback). The writer must emit a 1-channel
    // WAV — not a stereo file whose R channel is phantom silence (which would
    // read as a spurious channel imbalance in validate doctor/compare).
    constexpr int kPrepared = 2;
    constexpr int kDelivered = 1;
    constexpr int kBlock = 64;
    constexpr std::uint64_t kWindow = 128;

    pulp::audio::RollingAudioCaptureBuffer rolling;
    pulp::audio::RollingAudioCaptureBufferConfig rc;
    rc.num_channels = kPrepared;
    rc.max_frames = kWindow;
    REQUIRE(rolling.prepare(rc));

    // Append mono blocks (source has fewer channels than the ring).
    auto mono = [](int, std::uint64_t frame) {
        return 0.2f + static_cast<float>(frame) * 0.0001f;
    };
    for (int b = 0; b < 3; ++b)
        append_block(rolling, kDelivered, kBlock,
                     static_cast<std::uint64_t>(b) * kBlock, mono);

    pulp::format::StandaloneConfig cfg;
    cfg.sample_rate = 48000.0;
    cfg.audio_capture_rolling_frames = static_cast<int>(kWindow);
    const auto path =
        (fs::temp_directory_path() / "pulp_capture_rolling_mono.wav").string();
    fs::remove(path);

    // max_channels = the delivered count (1) → WAV has exactly one channel.
    REQUIRE(pulp::format::detail::write_audio_capture_rolling_wav_file(
        path, rolling, cfg, kDelivered));
    const auto data = pulp::audio::read_audio_file(path);
    REQUIRE(data.has_value());
    CHECK(data->num_channels() == 1u);

    fs::remove(path);
}

TEST_CASE("rolling capture WAV with an empty path or unprepared buffer is a no-op",
          "[standalone][audio-capture-rolling]") {
    pulp::audio::RollingAudioCaptureBuffer rolling;  // unprepared → 0 channels
    pulp::format::StandaloneConfig cfg;
    CHECK_FALSE(pulp::format::detail::write_audio_capture_rolling_wav_file("", rolling, cfg));
    CHECK_FALSE(pulp::format::detail::write_audio_capture_rolling_wav_file(
        (fs::temp_directory_path() / "pulp_rolling_empty.wav").string(), rolling, cfg));
}

#if PULP_ENABLE_AUDIO_PROBES
TEST_CASE("a rolling-capture-only headless run does not require a screenshot",
          "[standalone][audio-capture-rolling]") {
    pulp::format::StandaloneConfig cfg;
    cfg.headless = true;
    cfg.audio_capture_rolling_path = "out.wav";
    CHECK_FALSE(pulp::format::detail::standalone_headless_requires_screenshot(cfg));
}
#endif

}  // namespace
