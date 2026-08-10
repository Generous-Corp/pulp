#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/visualization_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include "support/thread_progress.hpp"
#include <cmath>
#include <vector>
#include <thread>
#include <atomic>

using namespace pulp::view;
using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

static constexpr float kPi = 3.14159265358979323846f;

// ── VisualizationBridge Tests ───────────────────────────────────────────────

TEST_CASE("VisualizationBridge configure and process", "[view][vizbridge]") {
    VisualizationBridge bridge;

    VisualizationConfig cfg;
    cfg.fft_size = 512;
    cfg.hop_size = 128;
    cfg.num_channels = 2;
    cfg.sample_rate = 44100.0f;
    cfg.capture_waveform = true;
    cfg.waveform_length = 256;

    bridge.configure(cfg);

    REQUIRE(bridge.fft_size() == 512);
    REQUIRE(bridge.num_channels() == 2);
    REQUIRE(bridge.sample_rate() == 44100.0f);
}

TEST_CASE("VisualizationBridge publishes spectrum", "[view][vizbridge]") {
    VisualizationBridge bridge;

    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 256;
    cfg.num_channels = 1;
    cfg.sample_rate = 44100.0f;
    cfg.capture_waveform = false;

    bridge.configure(cfg);

    // Generate a sine wave
    std::vector<float> buf(512);
    for (int i = 0; i < 512; ++i)
        buf[i] = std::sin(2.0f * kPi * 1000.0f * i / 44100.0f);

    const float* channels[] = {buf.data()};
    bridge.process(channels, 1, 512);

    // Read spectrum from UI thread
    const auto& spec = bridge.read_spectrum();
    REQUIRE(spec.num_bins > 0);
    REQUIRE(spec.num_bins == 129); // 256/2 + 1

    // Should have non-trivial spectrum values (not all -120 dB)
    float max_db = -200.0f;
    for (int i = 0; i < spec.num_bins; ++i) {
        if (spec.magnitude_db[i] > max_db) max_db = spec.magnitude_db[i];
    }
    REQUIRE(max_db > -60.0f); // The sine should produce significant energy
}

TEST_CASE("VisualizationBridge publishes meter data", "[view][vizbridge]") {
    VisualizationBridge bridge;

    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 256;
    cfg.num_channels = 2;
    cfg.sample_rate = 44100.0f;

    bridge.configure(cfg);

    constexpr int N = 4410;
    std::vector<float> ch0(N), ch1(N);
    for (int i = 0; i < N; ++i) {
        ch0[i] = 0.7f * std::sin(2.0f * kPi * 440.0f * i / 44100.0f);
        ch1[i] = 0.3f * std::sin(2.0f * kPi * 880.0f * i / 44100.0f);
    }

    const float* channels[] = {ch0.data(), ch1.data()};
    bridge.process(channels, 2, N);

    const auto& meter = bridge.read_meter();
    REQUIRE(meter.num_channels == 2);
    REQUIRE(meter.channels[0].peak > 0.5f);
    REQUIRE(meter.channels[1].peak > 0.2f);
    REQUIRE(meter.channels[0].peak > meter.channels[1].peak);
}

TEST_CASE("VisualizationBridge publishes waveform", "[view][vizbridge]") {
    VisualizationBridge bridge;

    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 256;
    cfg.num_channels = 1;
    cfg.sample_rate = 44100.0f;
    cfg.capture_waveform = true;
    cfg.waveform_length = 128;

    bridge.configure(cfg);

    // Feed a ramp signal
    std::vector<float> ramp(256);
    for (int i = 0; i < 256; ++i)
        ramp[i] = static_cast<float>(i) / 256.0f;

    const float* channels[] = {ramp.data()};
    bridge.process(channels, 1, 256);

    const auto& wf = bridge.read_waveform();
    REQUIRE(wf.num_samples == 128);

    // Waveform should contain the last 128 samples of the ramp
    // (values from 128/256=0.5 to 255/256≈1.0)
    REQUIRE(wf.samples[wf.num_samples - 1] > 0.9f);
}

TEST_CASE("VisualizationBridge skips waveform when capture is disabled", "[view][vizbridge]") {
    VisualizationBridge bridge;

    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 128;
    cfg.num_channels = 1;
    cfg.sample_rate = 44100.0f;
    cfg.capture_waveform = false;
    cfg.waveform_length = 64;

    bridge.configure(cfg);

    std::vector<float> buf(128, 0.25f);
    const float* channels[] = {buf.data()};
    bridge.process(channels, 1, 128);

    const auto& wf = bridge.read_waveform();
    REQUIRE(wf.num_samples == 0);
    REQUIRE(wf.num_channels == 0);
}

TEST_CASE("VisualizationBridge zero-channel block only publishes empty meter", "[view][vizbridge]") {
    VisualizationBridge bridge;

    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 128;
    cfg.num_channels = 2;
    cfg.sample_rate = 1000.0f;
    cfg.capture_waveform = true;
    cfg.waveform_length = 32;

    bridge.configure(cfg);

    bridge.process(nullptr, 0, 16);

    const auto& meter = bridge.read_meter();
    const auto& spec = bridge.read_spectrum();
    const auto& wf = bridge.read_waveform();

    REQUIRE(meter.num_channels == 0);
    REQUIRE(spec.num_bins == 0);
    REQUIRE(wf.num_samples == 0);
    REQUIRE(wf.num_channels == 0);
}

TEST_CASE("VisualizationBridge clamps waveform capture length to storage", "[view][vizbridge]") {
    VisualizationBridge bridge;

    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 128;
    cfg.num_channels = 1;
    cfg.sample_rate = 44100.0f;
    cfg.capture_waveform = true;
    cfg.waveform_length = WaveformData::kMaxSamples + 256;

    bridge.configure(cfg);

    std::vector<float> ramp(16);
    for (int i = 0; i < static_cast<int>(ramp.size()); ++i)
        ramp[i] = static_cast<float>(i) / static_cast<float>(ramp.size());

    const float* channels[] = {ramp.data()};
    bridge.process(channels, 1, static_cast<int>(ramp.size()));

    const auto& wf = bridge.read_waveform();
    REQUIRE(wf.num_samples == WaveformData::kMaxSamples);
    REQUIRE(wf.num_channels == 1);
    REQUIRE_THAT(wf.samples[wf.num_samples - 1], WithinAbs(15.0f / 16.0f, 1e-6f));
}

TEST_CASE("VisualizationBridge reset clears state", "[view][vizbridge]") {
    VisualizationBridge bridge;

    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 256;
    cfg.num_channels = 1;
    cfg.sample_rate = 44100.0f;

    bridge.configure(cfg);

    std::vector<float> buf(256, 0.5f);
    const float* channels[] = {buf.data()};
    bridge.process(channels, 1, 256);

    bridge.reset();

    // After reset, process silence
    std::vector<float> silence(256, 0.0f);
    const float* sil_channels[] = {silence.data()};
    bridge.process(sil_channels, 1, 256);

    const auto& meter = bridge.read_meter();
    REQUIRE(meter.channels[0].peak < 0.01f);
}

TEST_CASE("VisualizationBridge lock-free stress test", "[view][vizbridge]") {
    VisualizationBridge bridge;

    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 128;
    cfg.num_channels = 2;
    cfg.sample_rate = 48000.0f;

    bridge.configure(cfg);

    std::atomic<bool> running{true};
    std::atomic<int> write_count{0};
    std::atomic<int> read_count{0};

    // Simulate audio thread at 48kHz with 128-sample blocks
    std::thread audio_thread([&]() {
        std::vector<float> ch0(128), ch1(128);
        const float* channels[] = {ch0.data(), ch1.data()};

        while (running.load(std::memory_order_relaxed)) {
            for (int i = 0; i < 128; ++i) {
                float t = static_cast<float>(write_count.load()) * 128 + i;
                ch0[i] = 0.5f * std::sin(2.0f * kPi * 440.0f * t / 48000.0f);
                ch1[i] = 0.3f * std::sin(2.0f * kPi * 880.0f * t / 48000.0f);
            }
            bridge.process(channels, 2, 128);
            write_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Simulate UI thread at 60fps
    std::thread ui_thread([&]() {
        while (running.load(std::memory_order_relaxed)) {
            auto& spec = bridge.read_spectrum();
            auto& meter = bridge.read_meter();
            auto& wave = bridge.read_waveform();
            (void)spec;
            (void)meter;
            (void)wave;
            read_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Run for 500ms — enough for even slow VMs (ARM64 x64 emulation)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // That window is a duration budget, not an ordering guarantee: nothing puts
    // either thread's first iteration inside it, so a saturated host can spend
    // the whole window before they are scheduled and leave both counts at zero.
    // Wait for the floor the assertions below need; the deadline turns a bridge
    // that genuinely never advances into a failed REQUIRE rather than a hang.
    (void)pulp::test::wait_for_condition([&] {
        return write_count.load(std::memory_order_relaxed) > 5
            && read_count.load(std::memory_order_relaxed) > 5;
    });
    running.store(false, std::memory_order_relaxed);

    audio_thread.join();
    ui_thread.join();

    // Both threads should have executed meaningfully — at 48kHz with
    // 128-sample blocks, 500ms should yield ~187 audio blocks. Even on
    // an ARM64 VM with x64 emulation overhead, 5 is a safe floor.
    REQUIRE(write_count.load() > 5);
    REQUIRE(read_count.load() > 5);
}

// ── Widget Tests ────────────────────────────────────────────────────────────

TEST_CASE("SpectrogramView configure and push", "[view][widgets]") {
    SpectrogramView spectrogram;
    spectrogram.configure(128, 64, ColorRamp::inferno, -80.0f, 0.0f);

    REQUIRE(spectrogram.history_columns() == 128);
    REQUIRE(spectrogram.freq_rows() == 64);

    // Push some data
    std::vector<float> mags(256, -40.0f);
    spectrogram.push_spectrum(mags.data(), 256);
    // Should not crash
}

TEST_CASE("SpectrogramView auto-configure on first push", "[view][widgets]") {
    SpectrogramView spectrogram;

    std::vector<float> mags(128, -20.0f);
    spectrogram.push_spectrum(mags.data(), 128);

    // Should have auto-configured
    REQUIRE(spectrogram.freq_rows() == 128);
    REQUIRE(spectrogram.history_columns() == 256); // default
}

TEST_CASE("MultiMeter update and channel count", "[view][widgets]") {
    MultiMeter meter;
    meter.set_channel_count(4);
    REQUIRE(meter.channel_count() == 4);

    MultiChannelMeterData data;
    data.num_channels = 4;
    for (int ch = 0; ch < 4; ++ch) {
        data.channels[ch].peak = 0.2f * (ch + 1);
        data.channels[ch].rms = 0.1f * (ch + 1);
    }

    (void) meter.update(data, 1.0f / 60.0f);

    // Ballistics should have moved toward target
    REQUIRE(meter.ballistics().channels[0].display_peak > 0);
    REQUIRE(meter.ballistics().channels[3].display_peak > meter.ballistics().channels[0].display_peak);
}

TEST_CASE("MultiMeter layout options", "[view][widgets]") {
    MultiMeter meter;

    meter.set_layout(MultiMeter::Layout::vertical);
    REQUIRE(meter.layout() == MultiMeter::Layout::vertical);

    meter.set_layout(MultiMeter::Layout::horizontal);
    REQUIRE(meter.layout() == MultiMeter::Layout::horizontal);
}

TEST_CASE("CorrelationMeter smoothing", "[view][widgets]") {
    CorrelationMeter meter;

    // Initial state
    REQUIRE_THAT(meter.display_correlation(), WithinAbs(0.0, 0.001));

    // Feed positive correlation
    for (int i = 0; i < 60; ++i)
        (void) meter.update(1.0f, 1.0f / 60.0f);

    // Should have moved toward +1
    REQUIRE(meter.display_correlation() > 0.9f);

    // Feed negative correlation
    for (int i = 0; i < 120; ++i)
        (void) meter.update(-1.0f, 1.0f / 60.0f);

    // Should have moved toward -1
    REQUIRE(meter.display_correlation() < -0.9f);
}

TEST_CASE("CorrelationMeter clamps input range", "[view][widgets]") {
    CorrelationMeter meter;

    // Even with extreme inputs, should stay in [-1, +1]
    for (int i = 0; i < 120; ++i)
        (void) meter.update(5.0f, 1.0f / 60.0f);

    REQUIRE(meter.display_correlation() <= 1.0f);
    REQUIRE(meter.display_correlation() >= -1.0f);
}

// ── The family rule, stated by a case that breaks the naive version ──────────
// CorrelationMeter smooths exponentially like the level meters, so it needs the
// same snap or it never reports a still frame. But it settles wherever the
// signal is, NOT at zero: a steady mono source rests at +1. The meters' floor
// test (`if (v < 1e-6f) v = 0`) would never fire here, so copying it would
// silently do nothing — which is exactly the mistake the next person will make.
// The general form snaps toward the TARGET.
TEST_CASE("CorrelationMeter settles at a non-zero target and then goes quiet",
          "[view][widgets][idle-gate]") {
    CorrelationMeter meter;
    constexpr float dt = 1.0f / 60.0f;

    // Control: the report can say yes.
    REQUIRE(meter.update(1.0f, dt));

    // A steady mono source. Two seconds is far past the 50 ms smoothing.
    for (int i = 0; i < 120; ++i) (void) meter.update(1.0f, dt);

    // It arrived exactly, at a value a floor-to-zero snap could never produce.
    CHECK(meter.display_correlation() == 1.0f);
    for (int i = 0; i < 120; ++i)
        CHECK_FALSE(meter.update(1.0f, dt));

    // The same holds for the other pole, and real motion still gets through.
    CHECK(meter.update(-1.0f, dt));
    for (int i = 0; i < 240; ++i) (void) meter.update(-1.0f, dt);
    CHECK(meter.display_correlation() == -1.0f);
    for (int i = 0; i < 120; ++i)
        CHECK_FALSE(meter.update(-1.0f, dt));
}
