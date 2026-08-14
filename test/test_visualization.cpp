#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/visualization_bridge.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include "harness/rt_allocation_probe.hpp"
#include "support/thread_progress.hpp"
#include <cmath>
#include <vector>
#include <thread>
#include <atomic>
#include <limits>

using namespace pulp::view;
using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

static constexpr float kPi = 3.14159265358979323846f;

namespace {

int peak_bin(const SpectrumData& spectrum) {
    int result = 0;
    for (int bin = 1; bin < spectrum.num_bins; ++bin) {
        if (spectrum.magnitude_db[bin] > spectrum.magnitude_db[result]) result = bin;
    }
    return result;
}

std::vector<float> bin_sine(int bin, int fft_size, float amplitude = 1.0f) {
    std::vector<float> samples(static_cast<std::size_t>(fft_size));
    for (int i = 0; i < fft_size; ++i) {
        samples[static_cast<std::size_t>(i)] = amplitude * std::sin(
            2.0f * kPi * static_cast<float>(bin * i) / static_cast<float>(fft_size));
    }
    return samples;
}

SpectrumData analyze_stereo(const std::vector<float>& left,
                            const std::vector<float>& right) {
    VisualizationBridge bridge;
    VisualizationConfig cfg;
    cfg.fft_size = static_cast<int>(left.size());
    cfg.hop_size = cfg.fft_size;
    cfg.num_channels = 2;
    cfg.sample_rate = 48000.0f;
    cfg.capture_waveform = false;
    bridge.configure(cfg);
    const float* channels[] = {left.data(), right.data()};
    bridge.process(channels, 2, cfg.fft_size);
    REQUIRE(bridge.poll());
    return bridge.read_spectrum();
}

}  // namespace

// ── VisualizationBridge Tests ───────────────────────────────────────────────

TEST_CASE("VisualizationConfig preserves positional aggregate initialization",
          "[view][vizbridge][config]") {
    const VisualizationConfig cfg{
        2048, 512, WindowFunction::Type::blackman, 0.25f,
        6, 96000.0f, false, 4096, 16384, 1024, -96.0f};

    REQUIRE(cfg.fft_size == 2048);
    REQUIRE(cfg.hop_size == 512);
    REQUIRE(cfg.window == WindowFunction::Type::blackman);
    REQUIRE(cfg.window_param == 0.25f);
    REQUIRE(cfg.num_channels == 6);
    REQUIRE(cfg.sample_rate == 96000.0f);
    REQUIRE_FALSE(cfg.capture_waveform);
    REQUIRE(cfg.waveform_length == 4096);
    REQUIRE(cfg.capture_buffer_frames == 16384);
    REQUIRE(cfg.max_frames_per_poll == 1024);
    REQUIRE(cfg.spectrum_floor_db == -96.0f);
    REQUIRE(cfg.max_callback_frames == 4096);
}

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
    REQUIRE(bridge.poll());

    // Read spectrum from UI thread
    const auto& spec = bridge.read_spectrum();
    REQUIRE(spec.num_bins > 0);
    REQUIRE(spec.num_bins == 129); // 256/2 + 1
    REQUIRE(spec.epoch == 1);
    REQUIRE(spec.sequence_number == 1);
    REQUIRE(spec.source_channels == 1);
    REQUIRE(spec.fft_size == 256);
    REQUIRE(spec.sample_rate == 44100.0f);
    REQUIRE(spec.floor_db == -120.0f);

    // Should have non-trivial spectrum values (not all -120 dB)
    float max_db = -200.0f;
    for (int i = 0; i < spec.num_bins; ++i) {
        if (spec.magnitude_db[i] > max_db) max_db = spec.magnitude_db[i];
    }
    REQUIRE(max_db > -60.0f); // The sine should produce significant energy
}

TEST_CASE("VisualizationBridge audio producer stays allocation-free when tap fills",
          "[view][vizbridge][rt-safety]") {
    VisualizationBridge bridge;
    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 128;
    cfg.num_channels = 2;
    cfg.sample_rate = 48000.0f;
    cfg.capture_waveform = true;
    cfg.waveform_length = 128;
    cfg.max_callback_frames = 256;
    cfg.capture_buffer_frames = 256;
    bridge.configure(cfg);

    std::array<float, 64> left{};
    std::array<float, 64> right{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = std::sin(2.0f * kPi * static_cast<float>(i) / 16.0f);
        right[i] = -left[i];
    }
    const float* channels[] = {left.data(), right.data()};

    std::size_t allocations = 0;
    std::size_t bytes = 0;
    {
        pulp::test::RtAllocationProbe allocation_probe;
        pulp::runtime::ScopedNoAlloc no_alloc;
        // This deliberately overruns the fixed tap. A mutation that restores
        // FFT/vector work to process(), or grows storage on overflow, is caught.
        for (int block = 0; block < 1000; ++block)
            bridge.process(channels, 2, static_cast<int>(left.size()));
        allocations = allocation_probe.allocation_count();
        bytes = allocation_probe.allocated_bytes();
    }

    REQUIRE(allocations == 0);
    REQUIRE(bytes == 0);
    REQUIRE_FALSE(bridge.poll());
    std::array<float, 256> fresh{};
    const float* fresh_channels[] = {fresh.data(), fresh.data()};
    bridge.process(fresh_channels, 2, static_cast<int>(fresh.size()));
    REQUIRE(bridge.poll());
    const auto spectrum = bridge.peek_spectrum();
    REQUIRE(spectrum.dropped_frames > 0);
    REQUIRE(spectrum.epoch == 2);
}

TEST_CASE("VisualizationBridge spectrum follows the input tone and silence",
          "[view][vizbridge][spectrum]") {
    VisualizationBridge bridge;
    VisualizationConfig cfg;
    cfg.fft_size = 1024;
    cfg.hop_size = 1024;
    cfg.num_channels = 2;
    cfg.sample_rate = 48000.0f;
    cfg.capture_waveform = false;
    bridge.configure(cfg);

    auto low = bin_sine(10, cfg.fft_size);
    const float* low_channels[] = {low.data(), low.data()};
    bridge.process(low_channels, 2, cfg.fft_size);
    REQUIRE(bridge.poll());
    const auto low_spectrum = bridge.read_spectrum();
    REQUIRE(std::abs(peak_bin(low_spectrum) - 10) <= 1);

    auto high = bin_sine(40, cfg.fft_size);
    const float* high_channels[] = {high.data(), high.data()};
    bridge.process(high_channels, 2, cfg.fft_size);
    REQUIRE(bridge.poll());
    const auto high_spectrum = bridge.read_spectrum();
    REQUIRE(std::abs(peak_bin(high_spectrum) - 40) <= 1);
    REQUIRE(peak_bin(high_spectrum) > peak_bin(low_spectrum));
    REQUIRE(high_spectrum.sequence_number > low_spectrum.sequence_number);

    std::vector<float> silence(static_cast<std::size_t>(cfg.fft_size), 0.0f);
    const float* silent_channels[] = {silence.data(), silence.data()};
    bridge.process(silent_channels, 2, cfg.fft_size);
    REQUIRE(bridge.poll());
    const auto silent_spectrum = bridge.read_spectrum();
    for (int bin = 0; bin < silent_spectrum.num_bins; ++bin) {
        REQUIRE(std::isfinite(silent_spectrum.magnitude_db[bin]));
        REQUIRE(silent_spectrum.magnitude_db[bin] == silent_spectrum.floor_db);
    }
}

TEST_CASE("VisualizationBridge stereo spectrum is power-averaged",
          "[view][vizbridge][spectrum][stereo]") {
    constexpr int kFftSize = 1024;
    auto tone = bin_sine(21, kFftSize, 0.75f);
    std::vector<float> silence(kFftSize, 0.0f);
    std::vector<float> inverted = tone;
    for (float& sample : inverted) sample = -sample;

    const auto left_only = analyze_stereo(tone, silence);
    const auto right_only = analyze_stereo(silence, tone);
    const auto in_phase = analyze_stereo(tone, tone);
    const auto anti_phase = analyze_stereo(tone, inverted);

    REQUIRE(peak_bin(left_only) == peak_bin(right_only));
    REQUIRE(peak_bin(in_phase) == peak_bin(anti_phase));
    REQUIRE_THAT(left_only.magnitude_db[21],
                 WithinAbs(right_only.magnitude_db[21], 1.0e-5f));
    REQUIRE_THAT(in_phase.magnitude_db[21],
                 WithinAbs(anti_phase.magnitude_db[21], 1.0e-5f));
    // One active side contributes half the stereo power: -3.0103 dB.
    REQUIRE_THAT(in_phase.magnitude_db[21] - left_only.magnitude_db[21],
                 WithinAbs(3.0103f, 1.0e-3f));
}

TEST_CASE("VisualizationBridge rejects capture topology mismatches",
          "[view][vizbridge][spectrum][channels]") {
    constexpr int kFftSize = 1024;
    auto tone = bin_sine(17, kFftSize, 0.5f);

    VisualizationBridge stereo_capacity;
    VisualizationConfig stereo_cfg;
    stereo_cfg.fft_size = kFftSize;
    stereo_cfg.hop_size = kFftSize;
    stereo_cfg.num_channels = 2;
    stereo_cfg.sample_rate = 48000.0f;
    stereo_cfg.capture_waveform = false;
    stereo_capacity.configure(stereo_cfg);
    const float* mono_input[] = {tone.data()};
    stereo_capacity.process(mono_input, 1, kFftSize);
    REQUIRE_FALSE(stereo_capacity.poll());
    REQUIRE(stereo_capacity.peek_spectrum().num_bins == 0);
    REQUIRE(stereo_capacity.read_meter().num_channels == 1);

    const float* matching_stereo[] = {tone.data(), tone.data()};
    stereo_capacity.process(matching_stereo, 2, kFftSize);
    REQUIRE(stereo_capacity.poll());
    REQUIRE(stereo_capacity.peek_spectrum().source_channels == 2);

    VisualizationBridge mono_capacity;
    auto mono_cfg = stereo_cfg;
    mono_cfg.num_channels = 1;
    mono_capacity.configure(mono_cfg);
    const float* stereo_input[] = {tone.data(), tone.data()};
    mono_capacity.process(stereo_input, 2, kFftSize);
    REQUIRE_FALSE(mono_capacity.poll());
    REQUIRE(mono_capacity.peek_spectrum().num_bins == 0);
    REQUIRE(mono_capacity.read_meter().num_channels == 1);
}

TEST_CASE("VisualizationBridge resets continuity across a topology mismatch",
          "[view][vizbridge][spectrum][channels][discontinuity]") {
    constexpr int kFftSize = 256;
    auto tone_a = bin_sine(11, kFftSize, 0.75f);
    auto tone_b = bin_sine(73, kFftSize, 0.75f);
    const std::vector<float> silence(kFftSize, 0.0f);

    VisualizationBridge bridge;
    VisualizationConfig cfg;
    cfg.fft_size = kFftSize;
    cfg.hop_size = kFftSize;
    cfg.num_channels = 2;
    cfg.sample_rate = 48000.0f;
    cfg.capture_waveform = false;
    bridge.configure(cfg);

    const float* valid_a[] = {tone_a.data(), tone_a.data()};
    bridge.process(valid_a, 2, kFftSize / 2); // incomplete pre-gap frame

    const float* mismatched[] = {silence.data()};
    bridge.process(mismatched, 1, kFftSize);

    // A valid block arriving before the UI observes the gap is intentionally
    // rejected. It must not complete a hybrid A/B STFT frame.
    const float* valid_b[] = {tone_b.data(), tone_b.data()};
    bridge.process(valid_b, 2, kFftSize);
    REQUIRE_FALSE(bridge.poll());
    REQUIRE(bridge.peek_spectrum().num_bins == 0);

    // Once poll acknowledges the discontinuity, B starts a fresh epoch.
    bridge.process(valid_b, 2, kFftSize);
    REQUIRE(bridge.poll());
    const auto spectrum = bridge.peek_spectrum();
    REQUIRE(spectrum.epoch == 2);
    REQUIRE(peak_bin(spectrum) == 73);
}

TEST_CASE("VisualizationBridge resets continuity across a missing-channel block",
          "[view][vizbridge][spectrum][channels][discontinuity]") {
    constexpr int kFftSize = 256;
    auto tone_a = bin_sine(9, kFftSize, 0.75f);
    auto tone_b = bin_sine(61, kFftSize, 0.75f);

    VisualizationBridge bridge;
    VisualizationConfig cfg;
    cfg.fft_size = kFftSize;
    cfg.hop_size = kFftSize;
    cfg.num_channels = 1;
    cfg.capture_waveform = false;
    bridge.configure(cfg);

    const float* valid_a[] = {tone_a.data()};
    bridge.process(valid_a, 1, kFftSize / 2);
    bridge.process(nullptr, 0, kFftSize); // positive-length host-time gap

    const float* valid_b[] = {tone_b.data()};
    bridge.process(valid_b, 1, kFftSize); // suspended pending poll acknowledgement
    REQUIRE_FALSE(bridge.poll());
    REQUIRE(bridge.peek_spectrum().num_bins == 0);

    bridge.process(valid_b, 1, kFftSize);
    REQUIRE(bridge.poll());
    const auto spectrum = bridge.peek_spectrum();
    REQUIRE(spectrum.epoch == 2);
    REQUIRE(peak_bin(spectrum) == 61);
}

TEST_CASE("VisualizationBridge publishes only finite spectrum values",
          "[view][vizbridge][spectrum]") {
    VisualizationBridge bridge;
    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 256;
    cfg.num_channels = 2;
    cfg.sample_rate = 48000.0f;
    cfg.capture_waveform = false;
    cfg.spectrum_floor_db = std::numeric_limits<float>::quiet_NaN();
    bridge.configure(cfg);

    std::vector<float> left(256, std::numeric_limits<float>::quiet_NaN());
    std::vector<float> right(256, std::numeric_limits<float>::infinity());
    const float* channels[] = {left.data(), right.data()};
    bridge.process(channels, 2, 256);
    REQUIRE(bridge.poll());
    const auto spectrum = bridge.read_spectrum();

    REQUIRE(spectrum.floor_db == -120.0f);
    for (int bin = 0; bin < spectrum.num_bins; ++bin) {
        REQUIRE(std::isfinite(spectrum.magnitude_db[bin]));
        REQUIRE(spectrum.magnitude_db[bin] == spectrum.floor_db);
    }
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
    REQUIRE(bridge.poll());

    const auto& wf = bridge.read_waveform();
    REQUIRE(wf.num_samples == 128);
    REQUIRE(wf.epoch == 1);
    REQUIRE(wf.sequence_number == 1);
    REQUIRE(wf.dropped_frames == 0);

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
    REQUIRE_FALSE(bridge.poll());

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
    REQUIRE(bridge.poll());

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
    REQUIRE(bridge.poll());
    const auto before_reset = bridge.read_spectrum();
    REQUIRE(before_reset.epoch == 1);
    REQUIRE(before_reset.sequence_number == 1);

    bridge.reset();
    REQUIRE(bridge.read_spectrum().num_bins == 0);

    // After reset, process silence
    std::vector<float> silence(256, 0.0f);
    const float* sil_channels[] = {silence.data()};
    bridge.process(sil_channels, 1, 256);
    REQUIRE(bridge.poll());

    const auto& meter = bridge.read_meter();
    const auto after_reset = bridge.read_spectrum();
    REQUIRE(meter.channels[0].peak < 0.01f);
    REQUIRE(after_reset.epoch == 2);
    REQUIRE(after_reset.sequence_number == 1);
}

TEST_CASE("VisualizationBridge snapshot reads never poll",
          "[view][vizbridge][poll]") {
    VisualizationBridge bridge;
    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 256;
    cfg.num_channels = 1;
    cfg.capture_waveform = true;
    cfg.waveform_length = 64;
    bridge.configure(cfg);

    auto tone = bin_sine(12, cfg.fft_size);
    const float* channels[] = {tone.data()};
    bridge.process(channels, 1, cfg.fft_size);

    REQUIRE(bridge.peek_spectrum().num_bins == 0);
    REQUIRE(bridge.read_spectrum().num_bins == 0);
    REQUIRE(bridge.peek_waveform().num_samples == 0);
    REQUIRE(bridge.read_waveform().num_samples == 0);

    REQUIRE(bridge.poll());
    const auto sequence = bridge.peek_spectrum().sequence_number;
    REQUIRE(sequence == 1);
    REQUIRE(bridge.peek_waveform().sequence_number == 1);
    REQUIRE_FALSE(bridge.poll());
    REQUIRE(bridge.read_spectrum().sequence_number == sequence);
}

TEST_CASE("VisualizationBridge poll handles a multiframe producer push",
          "[view][vizbridge][poll][multiframe]") {
    VisualizationBridge bridge;
    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 256;
    cfg.num_channels = 1;
    cfg.capture_waveform = false;
    cfg.capture_buffer_frames = 1024;
    cfg.max_frames_per_poll = 256;
    bridge.configure(cfg);

    auto tone = bin_sine(19, 1024);
    // Keep the tone exactly on bin 19 of each 256-sample FFT as well.
    for (int i = 0; i < 1024; ++i)
        tone[static_cast<std::size_t>(i)] = std::sin(2.0f * kPi * 19.0f * i / 256.0f);
    const float* channels[] = {tone.data()};
    bridge.process(channels, 1, static_cast<int>(tone.size()));

    for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
        REQUIRE(bridge.poll());
        const auto spectrum = bridge.peek_spectrum();
        REQUIRE(spectrum.sequence_number == sequence);
        REQUIRE(std::abs(peak_bin(spectrum) - 19) <= 1);
    }
    REQUIRE_FALSE(bridge.poll());
}

TEST_CASE("VisualizationBridge one poll publishes the latest completed FFT frame",
          "[view][vizbridge][poll][multiframe][latest]") {
    constexpr int kFftSize = 256;
    auto tone_a = bin_sine(13, kFftSize, 0.75f);
    auto tone_b = bin_sine(71, kFftSize, 0.75f);
    std::vector<float> captured;
    captured.reserve(2 * kFftSize);
    captured.insert(captured.end(), tone_a.begin(), tone_a.end());
    captured.insert(captured.end(), tone_b.begin(), tone_b.end());

    VisualizationBridge bridge;
    VisualizationConfig cfg;
    cfg.fft_size = kFftSize;
    cfg.hop_size = kFftSize;
    cfg.num_channels = 1;
    cfg.capture_waveform = false;
    cfg.capture_buffer_frames = static_cast<int>(captured.size());
    bridge.configure(cfg);

    // One producer push and one consumer chunk complete two FFT frames. The
    // single publication must expose B, the latest completed frame, not A.
    const float* channels[] = {captured.data()};
    bridge.process(channels, 1, static_cast<int>(captured.size()));
    REQUIRE(bridge.poll());
    const auto spectrum = bridge.peek_spectrum();
    REQUIRE(spectrum.sequence_number == 1);
    REQUIRE(peak_bin(spectrum) == 71);
    REQUIRE_FALSE(bridge.poll());
}

TEST_CASE("VisualizationBridge overflow starts a fresh continuity epoch",
          "[view][vizbridge][poll][overflow]") {
    VisualizationBridge bridge;
    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 256;
    cfg.num_channels = 1;
    cfg.capture_waveform = true;
    cfg.waveform_length = 128;
    cfg.max_callback_frames = 256;
    cfg.capture_buffer_frames = 256;
    bridge.configure(cfg);

    std::array<float, 128> pre_gap{};
    const float* pre_gap_channels[] = {pre_gap.data()};
    bridge.process(pre_gap_channels, 1, static_cast<int>(pre_gap.size()));

    auto overflow = bin_sine(7, 256);
    const float* overflow_channels[] = {overflow.data()};
    bridge.process(overflow_channels, 1, static_cast<int>(overflow.size()));

    std::array<float, 64> suspended{};
    const float* suspended_channels[] = {suspended.data()};
    bridge.process(suspended_channels, 1, static_cast<int>(suspended.size()));

    // The first poll observes the drop, flushes the entry snapshot, and resets
    // continuity without publishing a frame assembled across the gap.
    REQUIRE_FALSE(bridge.poll());
    REQUIRE(bridge.peek_spectrum().num_bins == 0);

    auto post_gap = bin_sine(31, 256);
    const float* post_gap_channels[] = {post_gap.data()};
    bridge.process(post_gap_channels, 1, static_cast<int>(post_gap.size()));
    REQUIRE(bridge.poll());

    const auto spectrum = bridge.peek_spectrum();
    REQUIRE(spectrum.epoch == 2);
    REQUIRE(spectrum.sequence_number == 1);
    REQUIRE(spectrum.dropped_frames == 320);
    REQUIRE(std::abs(peak_bin(spectrum) - 31) <= 1);

    const auto waveform = bridge.peek_waveform();
    REQUIRE(waveform.epoch == 2);
    REQUIRE(waveform.sequence_number == 1);
    REQUIRE(waveform.dropped_frames == 320);
}

TEST_CASE("VisualizationBridge automatic storage admits the declared callback maximum",
          "[view][vizbridge][poll][capacity]") {
    VisualizationBridge bridge;
    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 256;
    cfg.num_channels = 1;
    cfg.capture_waveform = false;
    cfg.max_callback_frames = 8192;
    bridge.configure(cfg);

    std::vector<float> block(8192);
    for (std::size_t i = 0; i < block.size(); ++i) {
        block[i] = std::sin(2.0f * kPi * 17.0f
                            * static_cast<float>(i) / 256.0f);
    }
    const float* channels[] = {block.data()};
    bridge.process(channels, 1, static_cast<int>(block.size()));
    REQUIRE(bridge.poll());
    const auto spectrum = bridge.peek_spectrum();
    REQUIRE(spectrum.dropped_frames == 0);
    REQUIRE(std::abs(peak_bin(spectrum) - 17) <= 1);
}

TEST_CASE("VisualizationBridge poll returns while producer continuously refills",
          "[view][vizbridge][poll][concurrency]") {
    VisualizationBridge bridge;
    VisualizationConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 256;
    cfg.num_channels = 1;
    cfg.capture_waveform = false;
    cfg.max_callback_frames = 256;
    cfg.capture_buffer_frames = 256;
    bridge.configure(cfg);

    std::array<float, 256> samples{};
    const float* channels[] = {samples.data()};
    bridge.process(channels, 1, static_cast<int>(samples.size()));

    std::atomic<bool> producer_started{false};
    std::atomic<bool> producer_running{true};
    std::atomic<int> attempts{0};
    std::thread producer([&] {
        producer_started.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline) {
            bridge.process(channels, 1, static_cast<int>(samples.size()));
            attempts.fetch_add(1, std::memory_order_relaxed);
        }
        producer_running.store(false, std::memory_order_release);
    });

    REQUIRE(pulp::test::wait_for_condition([&] {
        return producer_started.load(std::memory_order_acquire)
            && attempts.load(std::memory_order_relaxed) > 10;
    }));

    const auto start = std::chrono::steady_clock::now();
    (void)bridge.poll();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const bool returned_while_refilling =
        producer_running.load(std::memory_order_acquire);

    producer.join();
    REQUIRE(returned_while_refilling);
    REQUIRE(elapsed < std::chrono::milliseconds(750));
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
            (void)bridge.poll();
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
