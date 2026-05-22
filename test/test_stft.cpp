#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/stft.hpp>
#include <pulp/signal/spectrogram.hpp>
#include <pulp/signal/multi_channel_meter.hpp>
#include <cmath>
#include <vector>
#include <numeric>
#include <limits>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

static constexpr float kPi = 3.14159265358979323846f;

// ── STFT Tests ──────────────────────────────────────────────────────────────

TEST_CASE("STFT default construction", "[signal][stft]") {
    Stft stft;
    REQUIRE_FALSE(stft.frame_ready());
}

TEST_CASE("STFT configure and basic properties", "[signal][stft]") {
    StftConfig cfg;
    cfg.fft_size = 512;
    cfg.hop_size = 128;
    cfg.window = WindowFunction::Type::hann;

    Stft stft(cfg);
    REQUIRE(stft.fft_size() == 512);
    REQUIRE(stft.hop_size() == 128);
    REQUIRE(stft.num_bins() == 257); // 512/2 + 1
    REQUIRE_FALSE(stft.frame_ready());
}

TEST_CASE("STFT produces frame after enough samples", "[signal][stft]") {
    StftConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 64;

    Stft stft(cfg);

    // Need at least fft_size samples before first frame
    std::vector<float> silence(256, 0.0f);
    bool got_frame = stft.push_samples(silence.data(), 256);
    REQUIRE(got_frame);
    REQUIRE(stft.frame_ready());
    REQUIRE(stft.latest_frame().num_bins == 129);
}

TEST_CASE("STFT does not produce frame before fft_size samples", "[signal][stft]") {
    StftConfig cfg;
    cfg.fft_size = 1024;
    cfg.hop_size = 256;

    Stft stft(cfg);

    // Feed fewer than fft_size samples
    std::vector<float> buf(512, 0.0f);
    bool got_frame = stft.push_samples(buf.data(), 512);
    REQUIRE_FALSE(got_frame);
    REQUIRE_FALSE(stft.frame_ready());
}

TEST_CASE("STFT detects single sine frequency", "[signal][stft]") {
    StftConfig cfg;
    cfg.fft_size = 1024;
    cfg.hop_size = 512;
    cfg.window = WindowFunction::Type::hann;

    Stft stft(cfg);

    // Generate 1kHz sine at 44100 sample rate
    float sr = 44100.0f;
    float freq = 1000.0f;
    std::vector<float> signal(2048);
    for (int i = 0; i < 2048; ++i)
        signal[i] = std::sin(2.0f * kPi * freq * i / sr);

    stft.push_samples(signal.data(), 2048);
    REQUIRE(stft.frame_ready());

    const auto& frame = stft.latest_frame();

    // Find peak bin
    int peak_bin = 0;
    float peak_mag = 0;
    for (int i = 1; i < frame.num_bins; ++i) {
        if (frame.magnitude[i] > peak_mag) {
            peak_mag = frame.magnitude[i];
            peak_bin = i;
        }
    }

    // Expected bin: freq * fft_size / sr = 1000 * 1024 / 44100 ≈ 23.2
    float expected_bin = freq * cfg.fft_size / sr;
    REQUIRE(std::abs(peak_bin - expected_bin) < 2.0f);
    REQUIRE(peak_mag > 0.1f); // Should have significant energy
}

TEST_CASE("STFT impulse response has flat magnitude", "[signal][stft]") {
    StftConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 256;
    cfg.window = WindowFunction::Type::rectangular; // No windowing for flat response

    Stft stft(cfg);

    std::vector<float> impulse(256, 0.0f);
    impulse[0] = 1.0f;

    stft.push_samples(impulse.data(), 256);
    REQUIRE(stft.frame_ready());

    const auto& frame = stft.latest_frame();

    // All bins should have approximately equal magnitude for an impulse
    for (int i = 0; i < frame.num_bins; ++i) {
        REQUIRE(frame.magnitude[i] > 0.9f);
        REQUIRE(frame.magnitude[i] < 1.1f);
    }
}

TEST_CASE("STFT window functions produce different results", "[signal][stft]") {
    float sr = 44100.0f;
    float freq = 440.0f;
    std::vector<float> signal(1024);
    for (int i = 0; i < 1024; ++i)
        signal[i] = std::sin(2.0f * kPi * freq * i / sr);

    // Rectangular window
    StftConfig cfg_rect;
    cfg_rect.fft_size = 1024;
    cfg_rect.hop_size = 1024;
    cfg_rect.window = WindowFunction::Type::rectangular;
    Stft stft_rect(cfg_rect);
    stft_rect.push_samples(signal.data(), 1024);

    // Hann window
    StftConfig cfg_hann;
    cfg_hann.fft_size = 1024;
    cfg_hann.hop_size = 1024;
    cfg_hann.window = WindowFunction::Type::hann;
    Stft stft_hann(cfg_hann);
    stft_hann.push_samples(signal.data(), 1024);

    // Both should detect the frequency, but with different sidelobe patterns
    const auto& rect = stft_rect.latest_frame();
    const auto& hann = stft_hann.latest_frame();

    // Find peaks - should be at similar bins
    int rect_peak = 0, hann_peak = 0;
    float rect_max = 0, hann_max = 0;
    for (int i = 1; i < rect.num_bins; ++i) {
        if (rect.magnitude[i] > rect_max) { rect_max = rect.magnitude[i]; rect_peak = i; }
        if (hann.magnitude[i] > hann_max) { hann_max = hann.magnitude[i]; hann_peak = i; }
    }
    REQUIRE(std::abs(rect_peak - hann_peak) <= 1);

    // Hann should have lower sidelobes (sum of non-peak bins should be lower)
    float rect_sidelobes = 0, hann_sidelobes = 0;
    for (int i = 1; i < rect.num_bins; ++i) {
        if (std::abs(i - rect_peak) > 3) rect_sidelobes += rect.magnitude[i];
        if (std::abs(i - hann_peak) > 3) hann_sidelobes += hann.magnitude[i];
    }
    REQUIRE(hann_sidelobes < rect_sidelobes);
}

TEST_CASE("STFT hop size produces multiple frames", "[signal][stft]") {
    StftConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 64;

    Stft stft(cfg);

    // Feed 256 + 256 = 512 samples → after first 256 (= fft_size), frames
    // come every 64 samples. So: 256 samples → 1 frame at sample 256.
    // Then 256 more → frames at 320, 384, 448, 512 → 4 more frames.
    std::vector<float> buf(512, 0.1f);
    int frame_count = 0;

    // Feed in chunks to count frames
    for (int offset = 0; offset < 512; offset += 64) {
        if (stft.push_samples(buf.data() + offset, 64))
            ++frame_count;
    }

    REQUIRE(frame_count >= 4); // Multiple frames from overlapping hops
}

TEST_CASE("STFT to_db conversion", "[signal][stft]") {
    float mags[] = {1.0f, 0.1f, 0.01f, 0.001f};
    Stft::to_db(mags, 4);
    REQUIRE_THAT(mags[0], WithinAbs(0.0, 0.1));
    REQUIRE_THAT(mags[1], WithinAbs(-20.0, 0.1));
    REQUIRE_THAT(mags[2], WithinAbs(-40.0, 0.1));
    REQUIRE_THAT(mags[3], WithinAbs(-60.0, 0.1));
}

TEST_CASE("STFT latest_magnitude_db", "[signal][stft]") {
    StftConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 256;
    cfg.window = WindowFunction::Type::rectangular;

    Stft stft(cfg);

    std::vector<float> impulse(256, 0.0f);
    impulse[0] = 1.0f;
    stft.push_samples(impulse.data(), 256);

    auto db = stft.latest_magnitude_db();
    REQUIRE(db.size() == 129);
    // Impulse → all bins ~0 dB
    REQUIRE_THAT(db[0], WithinAbs(0.0, 1.0));
}

TEST_CASE("STFT reset clears state", "[signal][stft]") {
    StftConfig cfg;
    cfg.fft_size = 256;
    cfg.hop_size = 256;

    Stft stft(cfg);

    std::vector<float> buf(256, 1.0f);
    stft.push_samples(buf.data(), 256);
    REQUIRE(stft.frame_ready());

    stft.reset();
    REQUIRE_FALSE(stft.frame_ready());
}

// ── Spectrogram Tests ───────────────────────────────────────────────────────

TEST_CASE("ColorMapper inferno range", "[signal][spectrogram]") {
    ColorMapper mapper(ColorRamp::inferno);

    auto low = mapper.map(0.0f);
    auto mid = mapper.map(0.5f);
    auto high = mapper.map(1.0f);

    // Low should be near black
    REQUIRE(low.r < 10);
    REQUIRE(low.g < 10);

    // High should be bright
    REQUIRE(high.r > 200);
    REQUIRE(high.g > 200);

    // Mid should differ from both
    REQUIRE(mid.r != low.r);
    REQUIRE(mid.r != high.r);
}

TEST_CASE("ColorMapper clamps out-of-range", "[signal][spectrogram]") {
    ColorMapper mapper(ColorRamp::grayscale);

    auto below = mapper.map(-0.5f);
    auto above = mapper.map(1.5f);

    REQUIRE(below.r == 0);
    REQUIRE(above.r == 255);
}

TEST_CASE("ColorMapper all ramps produce distinct colors", "[signal][spectrogram]") {
    for (auto ramp : {ColorRamp::inferno, ColorRamp::viridis, ColorRamp::grayscale, ColorRamp::heat}) {
        ColorMapper mapper(ramp);
        auto c0 = mapper.map(0.0f);
        auto c1 = mapper.map(1.0f);
        // Each ramp should produce different colors at 0 vs 1
        REQUIRE((c0.r != c1.r || c0.g != c1.g || c0.b != c1.b));
    }
}

TEST_CASE("FrequencyAxis bin to Hz", "[signal][spectrogram]") {
    FrequencyAxis axis;
    axis.configure(1024, 44100.0f, FrequencyScale::linear);

    // Bin 0 = 0 Hz (DC)
    REQUIRE_THAT(axis.bin_to_hz(0), WithinAbs(0.0, 0.1));

    // Bin 512 = Nyquist = 22050 Hz
    REQUIRE_THAT(axis.bin_to_hz(512), WithinAbs(22050.0, 1.0));

    // Bin 1 = 44100/1024 ≈ 43.07 Hz
    REQUIRE_THAT(axis.bin_to_hz(1), WithinAbs(43.07, 0.1));
}

TEST_CASE("FrequencyAxis Hz to bin round-trip", "[signal][spectrogram]") {
    FrequencyAxis axis;
    axis.configure(1024, 44100.0f, FrequencyScale::linear);

    // Round-trip: hz → bin → hz should be close
    float hz = 1000.0f;
    int bin = axis.hz_to_bin(hz);
    float recovered = axis.bin_to_hz(bin);
    REQUIRE(std::abs(recovered - hz) < 50.0f); // Within one bin width
}

TEST_CASE("FrequencyAxis log scale maps correctly", "[signal][spectrogram]") {
    FrequencyAxis axis;
    axis.configure(1024, 44100.0f, FrequencyScale::logarithmic);

    // Low frequencies should occupy more display space in log scale
    float pos_100 = axis.hz_to_display(100.0f);
    float pos_1000 = axis.hz_to_display(1000.0f);
    float pos_10000 = axis.hz_to_display(10000.0f);

    REQUIRE(pos_100 < pos_1000);
    REQUIRE(pos_1000 < pos_10000);
    REQUIRE(pos_10000 <= 1.0f);
    REQUIRE(pos_100 >= 0.0f);
}

TEST_CASE("FrequencyAxis mel scale", "[signal][spectrogram]") {
    FrequencyAxis axis;
    axis.configure(1024, 44100.0f, FrequencyScale::mel);

    float pos_1k = axis.hz_to_display(1000.0f);
    float pos_10k = axis.hz_to_display(10000.0f);

    // Mel scale compresses high frequencies more than log
    REQUIRE(pos_1k > 0.0f);
    REQUIRE(pos_10k < 1.0f);
    REQUIRE(pos_1k < pos_10k);
}

TEST_CASE("SpectrogramBuffer push and read", "[signal][spectrogram]") {
    SpectrogramBuffer buf;
    buf.configure(64, 32);

    REQUIRE(buf.width() == 64);
    REQUIRE(buf.height() == 32);
    REQUIRE(buf.frames_written() == 0);

    // Push a column of known dB values
    std::vector<float> mags(128, -40.0f);
    ColorMapper mapper(ColorRamp::grayscale);
    buf.push_column(mags.data(), 128, mapper, -80.0f, 0.0f);

    REQUIRE(buf.frames_written() == 1);

    // Pixels should be non-zero (mapped from -40 dB in -80..0 range = 0.5 normalized)
    auto* px = buf.pixels();
    REQUIRE(px != nullptr);
    // At -40 dB in -80..0 range, normalized = 0.5, grayscale → ~128
    REQUIRE(px[0].r > 100);
    REQUIRE(px[0].r < 160);
}

TEST_CASE("ColorMapper ramp switching and control points", "[signal][spectrogram][issue-645]") {
    ColorMapper mapper(ColorRamp::heat);
    REQUIRE(mapper.ramp() == ColorRamp::heat);

    auto heat_mid = mapper.map(0.33f);
    REQUIRE(heat_mid.r == 180);
    REQUIRE(heat_mid.g == 0);
    REQUIRE(heat_mid.b == 0);
    REQUIRE(heat_mid.a == 255);

    mapper.set_ramp(ColorRamp::viridis);
    REQUIRE(mapper.ramp() == ColorRamp::viridis);
    auto viridis_quarter = mapper.map(0.25f);
    REQUIRE(viridis_quarter.r == 59);
    REQUIRE(viridis_quarter.g == 82);
    REQUIRE(viridis_quarter.b == 139);
    REQUIRE(viridis_quarter.a == 255);

    mapper.set_ramp(ColorRamp::grayscale);
    auto gray_mid = mapper.map(0.5f);
    REQUIRE(gray_mid.r == 127);
    REQUIRE(gray_mid.g == 127);
    REQUIRE(gray_mid.b == 127);
    REQUIRE(gray_mid.a == 255);
}

TEST_CASE("FrequencyAxis clamps display and bin conversions", "[signal][spectrogram][issue-645]") {
    FrequencyAxis axis;
    axis.configure(8, 8000.0f, FrequencyScale::linear);

    REQUIRE(axis.num_bins() == 5);
    REQUIRE(axis.nyquist() == 4000.0f);
    REQUIRE(axis.scale() == FrequencyScale::linear);
    REQUIRE(axis.hz_to_bin(-100.0f) == 0);
    REQUIRE(axis.hz_to_bin(100000.0f) == 4);
    REQUIRE(axis.display_to_bin(-1.0f) == 0);
    REQUIRE(axis.display_to_bin(2.0f) == 4);
    REQUIRE_THAT(axis.bin_to_display(4), WithinAbs(1.0, 1e-5));
    REQUIRE_THAT(axis.hz_to_display(0.0f), WithinAbs(1.0 / 4000.0, 1e-6));
    REQUIRE_THAT(axis.display_to_hz(-1.0f), WithinAbs(0.0, 1e-5));
    REQUIRE_THAT(axis.display_to_hz(2.0f), WithinAbs(4000.0, 1e-5));

    axis.configure(1024, 48000.0f, FrequencyScale::logarithmic);
    REQUIRE_THAT(axis.display_to_hz(0.0f), WithinAbs(1.0, 1e-5));
    REQUIRE_THAT(axis.display_to_hz(1.0f), WithinAbs(24000.0, 1.0));
    REQUIRE(axis.display_to_bin(1.0f) == axis.num_bins() - 1);

    axis.configure(1024, 48000.0f, FrequencyScale::mel);
    REQUIRE_THAT(axis.display_to_hz(0.0f), WithinAbs(0.0, 1e-4));
    REQUIRE_THAT(axis.display_to_hz(1.0f), WithinAbs(24000.0, 1.0));
    REQUIRE(axis.display_to_bin(1.0f) == axis.num_bins() - 1);
}

TEST_CASE("SpectrogramBuffer wraps columns and maps rows to bins", "[signal][spectrogram][issue-645]") {
    SpectrogramBuffer buf;
    ColorMapper mapper(ColorRamp::grayscale);
    buf.configure(3, 4);

    const std::vector<float> quiet = {-80.0f, -80.0f, -80.0f, -80.0f};
    const std::vector<float> loud = {0.0f, 0.0f, 0.0f, 0.0f};
    const std::vector<float> mid = {-40.0f, -40.0f, -40.0f, -40.0f};
    const std::vector<float> sloped = {-80.0f, -40.0f, -20.0f, 0.0f};

    buf.push_column(quiet.data(), static_cast<int>(quiet.size()), mapper, -80.0f, 0.0f);
    REQUIRE(buf.write_column() == 1);
    buf.push_column(loud.data(), static_cast<int>(loud.size()), mapper, -80.0f, 0.0f);
    REQUIRE(buf.write_column() == 2);
    buf.push_column(mid.data(), static_cast<int>(mid.size()), mapper, -80.0f, 0.0f);
    REQUIRE(buf.write_column() == 0);
    buf.push_column(sloped.data(), static_cast<int>(sloped.size()), mapper, -80.0f, 0.0f);

    REQUIRE(buf.frames_written() == 4);
    REQUIRE(buf.write_column() == 1);

    const auto* px = buf.pixels();
    REQUIRE(px[0 * buf.width() + 0].r == 0);
    REQUIRE(px[1 * buf.width() + 0].r == 127);
    REQUIRE(px[2 * buf.width() + 0].r == 191);
    REQUIRE(px[3 * buf.width() + 0].r == 255);
    REQUIRE(px[0 * buf.width() + 1].r == 255);
    REQUIRE(px[0 * buf.width() + 2].r == 127);
}

TEST_CASE("SpectrogramBuffer handles degenerate dB range and empty rows", "[signal][spectrogram][issue-645]") {
    SpectrogramBuffer buf;
    ColorMapper mapper(ColorRamp::grayscale);
    buf.configure(2, 2);

    const std::vector<float> mags = {-100.0f, 0.0f};
    buf.push_column(mags.data(), static_cast<int>(mags.size()), mapper, -80.0f, -80.0f);

    const auto* px = buf.pixels();
    REQUIRE(px[0].r == 0);
    REQUIRE(px[2].r == 255);
    REQUIRE(buf.frames_written() == 1);
    REQUIRE(buf.write_column() == 1);

    SpectrogramBuffer empty_rows;
    empty_rows.configure(4, 0);
    empty_rows.push_column(mags.data(), static_cast<int>(mags.size()), mapper, -80.0f, 0.0f);
    REQUIRE(empty_rows.frames_written() == 0);
    REQUIRE(empty_rows.write_column() == 0);
    REQUIRE(empty_rows.height() == 0);
}

// ── Multi-Channel Meter Tests ───────────────────────────────────────────────

TEST_CASE("MultiChannelMeter stereo peak and RMS", "[signal][meter]") {
    MultiChannelMeter meter;
    meter.prepare(44100.0f, 2);

    constexpr int N = 4410; // 100ms at 44100
    std::vector<float> ch0(N), ch1(N);

    // ch0: sine at 0.5 amplitude
    for (int i = 0; i < N; ++i) {
        ch0[i] = 0.5f * std::sin(2.0f * kPi * 440.0f * i / 44100.0f);
        ch1[i] = 0.0f; // silent
    }

    const float* channels[] = {ch0.data(), ch1.data()};
    meter.process(channels, 2, N);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 2);

    // ch0 peak ≈ 0.5
    REQUIRE(snap.channels[0].peak > 0.45f);
    REQUIRE(snap.channels[0].peak < 0.55f);

    // ch0 RMS ≈ 0.5/sqrt(2) ≈ 0.354
    REQUIRE(snap.channels[0].rms > 0.30f);
    REQUIRE(snap.channels[0].rms < 0.40f);

    // ch1 should be ~0
    REQUIRE(snap.channels[1].peak < 0.01f);
    REQUIRE(snap.channels[1].rms < 0.01f);
}

TEST_CASE("MultiChannelMeter clip detection", "[signal][meter]") {
    MultiChannelMeter meter;
    meter.prepare(44100.0f, 1);

    std::vector<float> loud(4410, 1.0f); // Constant at exactly 1.0
    const float* channels[] = {loud.data()};
    meter.process(channels, 1, 4410);

    REQUIRE(meter.snapshot().channels[0].clipped);
}

TEST_CASE("MultiChannelMeter stereo correlation", "[signal][meter]") {
    MultiChannelMeter meter;
    meter.prepare(44100.0f, 2);

    constexpr int N = 4410;
    std::vector<float> ch0(N), ch1(N);

    SECTION("Identical channels → correlation ≈ +1") {
        for (int i = 0; i < N; ++i) {
            ch0[i] = ch1[i] = std::sin(2.0f * kPi * 440.0f * i / 44100.0f);
        }
        const float* channels[] = {ch0.data(), ch1.data()};
        meter.process(channels, 2, N);
        REQUIRE(meter.snapshot().correlation > 0.95f);
    }

    SECTION("Inverted channels → correlation ≈ -1") {
        for (int i = 0; i < N; ++i) {
            ch0[i] = std::sin(2.0f * kPi * 440.0f * i / 44100.0f);
            ch1[i] = -ch0[i];
        }
        const float* channels[] = {ch0.data(), ch1.data()};
        meter.process(channels, 2, N);
        REQUIRE(meter.snapshot().correlation < -0.95f);
    }
}

TEST_CASE("MultiChannelMeter supports high channel counts", "[signal][meter]") {
    MultiChannelMeter meter;
    meter.prepare(44100.0f, 8);

    constexpr int N = 4410;
    std::array<std::vector<float>, 8> bufs;
    std::array<const float*, 8> ptrs;

    for (int ch = 0; ch < 8; ++ch) {
        bufs[ch].resize(N);
        float amp = 0.1f * (ch + 1);
        for (int i = 0; i < N; ++i)
            bufs[ch][i] = amp * std::sin(2.0f * kPi * (220.0f * (ch + 1)) * i / 44100.0f);
        ptrs[ch] = bufs[ch].data();
    }

    meter.process(ptrs.data(), 8, N);
    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 8);

    // Each channel should have increasing peak levels
    for (int ch = 0; ch < 7; ++ch) {
        REQUIRE(snap.channels[ch + 1].peak > snap.channels[ch].peak);
    }
}

TEST_CASE("MultiChannelBallistics smoothing", "[signal][meter]") {
    MultiChannelBallistics ballistics;
    ballistics.num_channels = 1;

    MultiChannelMeterData data;
    data.num_channels = 1;
    data.channels[0].peak = 0.8f;
    data.channels[0].rms = 0.5f;

    // Feed sudden peak
    ballistics.update(data, 1.0f / 60.0f);
    REQUIRE(ballistics.channels[0].display_peak > 0);

    // Feed silence — should decay
    data.channels[0].peak = 0.0f;
    data.channels[0].rms = 0.0f;
    float prev = ballistics.channels[0].display_peak;
    for (int i = 0; i < 30; ++i)
        ballistics.update(data, 1.0f / 60.0f);

    REQUIRE(ballistics.channels[0].display_peak < prev);
    REQUIRE(ballistics.channels[0].display_peak > 0); // Not yet zero
}

TEST_CASE("MultiChannelBallistics clip indicator hold", "[signal][meter]") {
    MultiChannelBallistics ballistics;
    ballistics.num_channels = 1;
    ballistics.clip_hold_time = 1.0f;

    MultiChannelMeterData data;
    data.num_channels = 1;
    data.channels[0].clipped = true;
    ballistics.update(data, 1.0f / 60.0f);
    REQUIRE(ballistics.channels[0].clip_indicator);

    // Stop clipping — indicator should hold
    data.channels[0].clipped = false;
    for (int i = 0; i < 30; ++i) // 0.5s at 60fps
        ballistics.update(data, 1.0f / 60.0f);
    REQUIRE(ballistics.channels[0].clip_indicator); // Still held

    // After hold time expires
    for (int i = 0; i < 60; ++i) // Another second
        ballistics.update(data, 1.0f / 60.0f);
    REQUIRE_FALSE(ballistics.channels[0].clip_indicator);
}

TEST_CASE("MultiChannelBallistics clear_clips", "[signal][meter]") {
    MultiChannelBallistics ballistics;
    ballistics.num_channels = 2;

    MultiChannelMeterData data;
    data.num_channels = 2;
    data.channels[0].clipped = true;
    data.channels[1].clipped = true;
    ballistics.update(data, 1.0f / 60.0f);

    REQUIRE(ballistics.channels[0].clip_indicator);
    REQUIRE(ballistics.channels[1].clip_indicator);

    ballistics.clear_clips();
    REQUIRE_FALSE(ballistics.channels[0].clip_indicator);
    REQUIRE_FALSE(ballistics.channels[1].clip_indicator);
}

TEST_CASE("ColorMapper sanitizes NaN input (no NaN->uint8 UB)",
          "[signal][spectrogram][issue-2695]") {
    const float nan_t = std::numeric_limits<float>::quiet_NaN();
    const float pinf  = std::numeric_limits<float>::infinity();
    for (auto ramp : {ColorRamp::grayscale, ColorRamp::inferno,
                      ColorRamp::viridis, ColorRamp::heat}) {
        ColorMapper mapper(ramp);
        // NaN must not reach the uint8_t cast (UB); map() sanitizes NaN to 0,
        // so map(NaN) == map(0). +/-Inf clamp safely to 1.0 / 0.0.
        auto at_zero = mapper.map(0.0f);
        auto at_one  = mapper.map(1.0f);
        auto at_nan  = mapper.map(nan_t);
        REQUIRE(at_nan.r == at_zero.r);
        REQUIRE(at_nan.g == at_zero.g);
        REQUIRE(at_nan.b == at_zero.b);
        auto at_pinf = mapper.map(pinf);   // clamps to 1.0
        REQUIRE(at_pinf.r == at_one.r);
        REQUIRE(at_pinf.g == at_one.g);
        REQUIRE(at_pinf.b == at_one.b);
    }
}
