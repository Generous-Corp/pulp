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
#include <pulp/format/detail/standalone_output_tap.hpp>
#include <pulp/format/detail/standalone_rolling_capture.hpp>
#include <pulp/format/standalone.hpp>

#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <atomic>
#include <cstdlib>

namespace {

namespace fs = std::filesystem;

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* value = std::getenv(name_.c_str()))
            previous_ = std::string(value);
    }

    ~ScopedEnv() {
#if !defined(_WIN32)
        if (previous_)
            ::setenv(name_.c_str(), previous_->c_str(), /*overwrite=*/1);
        else
            ::unsetenv(name_.c_str());
#else
        if (previous_)
            _putenv_s(name_.c_str(), previous_->c_str());
        else
            _putenv_s(name_.c_str(), "");
#endif
    }

    void set(const char* value) {
#if !defined(_WIN32)
        REQUIRE(::setenv(name_.c_str(), value, /*overwrite=*/1) == 0);
#else
        REQUIRE(_putenv_s(name_.c_str(), value) == 0);
#endif
    }

    void unset() {
#if !defined(_WIN32)
        REQUIRE(::unsetenv(name_.c_str()) == 0);
#else
        REQUIRE(_putenv_s(name_.c_str(), "") == 0);
#endif
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

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
TEST_CASE("standalone rolling capture prepares the configured last-window ring",
          "[standalone][audio-capture-rolling]") {
    pulp::audio::RollingAudioCaptureBuffer rolling;
    pulp::format::StandaloneConfig inactive;
    inactive.output_channels = 2;
    CHECK_FALSE(pulp::format::detail::prepare_standalone_rolling_capture(
        rolling, inactive));
    CHECK(rolling.num_channels() == 0u);
    CHECK(rolling.capacity_frames() == 0u);

    pulp::format::StandaloneConfig cfg;
    cfg.audio_capture_rolling_path = "out.wav";
    cfg.output_channels = 2;
    cfg.audio_capture_rolling_frames =
        pulp::format::detail::kMaxCaptureWindowSamples + 1024;
    REQUIRE(pulp::format::detail::prepare_standalone_rolling_capture(
        rolling, cfg));
    CHECK(rolling.num_channels() == 2u);
    CHECK(rolling.capacity_frames() ==
          static_cast<std::uint64_t>(
              pulp::format::detail::kMaxCaptureWindowSamples));

    pulp::audio::RollingAudioCaptureBuffer no_channels;
    cfg.output_channels = 0;
    CHECK_FALSE(pulp::format::detail::prepare_standalone_rolling_capture(
        no_channels, cfg));
    CHECK(no_channels.capacity_frames() == 0u);
}

TEST_CASE("standalone rolling capture append tap publishes delivered channels",
          "[standalone][audio-capture-rolling]") {
    constexpr int kPrepared = 2;
    constexpr int kDelivered = 1;
    constexpr int kFrames = 4;

    pulp::audio::RollingAudioCaptureBuffer rolling;
    pulp::audio::RollingAudioCaptureBufferConfig rc;
    rc.num_channels = kPrepared;
    rc.max_frames = 8;
    REQUIRE(rolling.prepare(rc));

    std::vector<float> mono{0.10f, 0.11f, 0.12f, 0.13f};
    const float* ptrs[] = {mono.data()};
    pulp::audio::BufferView<const float> output(ptrs, kDelivered, kFrames);
    std::atomic<int> delivered{99};

    pulp::format::detail::append_standalone_rolling_capture_output(
        false, rolling, delivered, output);
    CHECK(delivered.load(std::memory_order_relaxed) == 99);
    CHECK_FALSE(rolling.snapshot_last(1).valid);

    pulp::format::detail::append_standalone_rolling_capture_output(
        true, rolling, delivered, output);
    CHECK(delivered.load(std::memory_order_relaxed) == kDelivered);

    pulp::audio::Buffer<float> materialized(kPrepared, kFrames);
    const auto hold = rolling.hold_last(kFrames);
    REQUIRE(hold.valid());
    const auto result = rolling.materialize_held(hold, materialized.view());
    REQUIRE(result.status ==
            pulp::audio::RollingAudioCaptureMaterializeStatus::Ok);
    REQUIRE(result.frames_copied == kFrames);
    const auto materialized_view = materialized.view();
    CHECK(materialized_view.channel_ptr(0)[0] == mono[0]);
    CHECK(materialized_view.channel_ptr(0)[3] == mono[3]);
    CHECK(materialized_view.channel_ptr(1)[0] == 0.0f);
    CHECK(materialized_view.channel_ptr(1)[3] == 0.0f);
}

TEST_CASE("standalone output tap analyzes the delivered output view",
          "[standalone][audio-capture-rolling]") {
    constexpr int kPrepared = 2;
    constexpr int kFrames = 4;

    pulp::audio::Buffer<float> output(1, kFrames);
    auto output_view = output.view();
    for (int i = 0; i < kFrames; ++i)
        output_view.channel_ptr(0)[i] = 0.5f + static_cast<float>(i);

    pulp::audio::RollingAudioCaptureBuffer rolling;
    pulp::audio::RollingAudioCaptureBufferConfig rc;
    rc.num_channels = kPrepared;
    rc.max_frames = 8;
    REQUIRE(rolling.prepare(rc));

    float stale = 0.0f;
    std::vector<const float*> probe_ptrs(2, &stale);
    std::atomic<int> delivered{99};
    bool analyzed = false;

    pulp::format::detail::analyze_standalone_output_tap(
        output_view, probe_ptrs,
        [&](const pulp::audio::BufferView<const float>& view) {
            analyzed = true;
            CHECK(view.num_channels() == 1u);
            CHECK(view.num_samples() == static_cast<size_t>(kFrames));
            CHECK(view.channel_ptr(0) == output_view.channel_ptr(0));
            CHECK(view.channel_ptr(0)[3] == output_view.channel_ptr(0)[3]);
        },
        true, rolling, delivered);

    REQUIRE(analyzed);
    CHECK(probe_ptrs[0] == output_view.channel_ptr(0));
    CHECK(probe_ptrs[1] == &stale);
    CHECK(delivered.load(std::memory_order_relaxed) == 1);

    pulp::audio::Buffer<float> materialized(kPrepared, kFrames);
    const auto hold = rolling.hold_last(kFrames);
    REQUIRE(hold.valid());
    const auto result = rolling.materialize_held(hold, materialized.view());
    REQUIRE(result.status ==
            pulp::audio::RollingAudioCaptureMaterializeStatus::Ok);
    const auto materialized_view = materialized.view();
    CHECK(materialized_view.channel_ptr(0)[0] == output_view.channel_ptr(0)[0]);
    CHECK(materialized_view.channel_ptr(0)[3] == output_view.channel_ptr(0)[3]);
    CHECK(materialized_view.channel_ptr(1)[0] == 0.0f);
    CHECK(materialized_view.channel_ptr(1)[3] == 0.0f);
}

TEST_CASE("standalone output tap still analyzes when rolling capture is inactive",
          "[standalone][audio-capture-rolling]") {
    constexpr int kFrames = 2;

    pulp::audio::Buffer<float> output(1, kFrames);
    auto output_view = output.view();
    output_view.channel_ptr(0)[0] = 0.25f;
    output_view.channel_ptr(0)[1] = 0.5f;

    pulp::audio::RollingAudioCaptureBuffer rolling;
    pulp::audio::RollingAudioCaptureBufferConfig rc;
    rc.num_channels = 1;
    rc.max_frames = 4;
    REQUIRE(rolling.prepare(rc));

    std::vector<const float*> probe_ptrs(1, nullptr);
    std::atomic<int> delivered{7};
    bool analyzed = false;

    pulp::format::detail::analyze_standalone_output_tap(
        output_view, probe_ptrs,
        [&](const pulp::audio::BufferView<const float>& view) {
            analyzed = true;
            CHECK(view.num_channels() == 1u);
            CHECK(view.channel_ptr(0)[1] == 0.5f);
        },
        false, rolling, delivered);

    REQUIRE(analyzed);
    CHECK(delivered.load(std::memory_order_relaxed) == 7);
    CHECK_FALSE(rolling.snapshot_last(1).valid);
}

TEST_CASE("a rolling-capture-only headless run does not require a screenshot",
          "[standalone][audio-capture-rolling]") {
    pulp::format::StandaloneConfig cfg;
    cfg.headless = true;
    cfg.audio_capture_rolling_path = "out.wav";
    CHECK_FALSE(pulp::format::detail::standalone_headless_requires_screenshot(cfg));
}
#endif

#if PULP_ENABLE_AUDIO_PROBES
TEST_CASE("standalone environment arms rolling audio capture without a screenshot",
          "[standalone][audio-capture-rolling]") {
    ScopedEnv headless("PULP_HEADLESS");
    ScopedEnv screenshot("PULP_SCREENSHOT");
    ScopedEnv capture("PULP_AUDIO_CAPTURE_ROLLING");
    ScopedEnv frames("PULP_AUDIO_CAPTURE_ROLLING_FRAMES");
    headless.unset();
    screenshot.unset();
    capture.set("/tmp/pulp-rolling-env.wav");
    frames.set("512");

    auto config =
        pulp::format::detail::standalone_config_from_environment(
            pulp::format::StandaloneConfig{});

    REQUIRE(config.audio_capture_rolling_path == "/tmp/pulp-rolling-env.wav");
    REQUIRE(config.audio_capture_rolling_frames == 512);
    REQUIRE(config.headless);
    REQUIRE(config.screenshot_path.empty());
    REQUIRE_FALSE(pulp::format::detail::standalone_headless_requires_screenshot(config));
    REQUIRE_FALSE(pulp::format::detail::standalone_probe_json_requested_but_disabled(config));
}
#endif

}  // namespace
