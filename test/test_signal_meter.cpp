#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/signal.hpp>
#include <array>
#include <cmath>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

// ── MultiChannelMeter ────────────────────────────────────────────────────────

TEST_CASE("MultiChannelMeter emits peak RMS clipping and stereo correlation", "[signal][meter]") {
    MultiChannelMeter meter;
    meter.prepare(44100.0f, 2);

    std::vector<float> left(441, 0.5f);
    std::vector<float> right(441, 0.5f);
    left[10] = 1.2f;

    const float* channels[] = {left.data(), right.data()};
    meter.process(channels, 2, static_cast<int>(left.size()));

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 2);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(1.2f, 0.001f));
    REQUIRE(snap.channels[0].clipped);
    REQUIRE_THAT(snap.channels[1].peak, WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(snap.channels[1].rms, WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(snap.correlation, WithinAbs(1.0f, 0.005f));
    REQUIRE(std::isfinite(snap.channels[0].lufs_momentary));
}

TEST_CASE("MultiChannelMeter tracks negative correlation and integrated loudness", "[signal][meter]") {
    MultiChannelMeter meter;
    meter.prepare(1000.0f, 2);

    std::vector<float> left(10, 0.25f);
    std::vector<float> right(10, -0.25f);
    const float* channels[] = {left.data(), right.data()};

    for (int i = 0; i < 40; ++i) {
        meter.process(channels, 2, static_cast<int>(left.size()));
    }

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 2);
    REQUIRE_THAT(snap.correlation, WithinAbs(-1.0f, 0.001f));
    REQUIRE(std::isfinite(snap.lufs_integrated));
    REQUIRE_THAT(snap.lufs_integrated, WithinAbs(-12.73f, 0.1f));
}

TEST_CASE("MultiChannelMeter prepare clears stale clip flags", "[signal][meter]") {
    MultiChannelMeter meter;
    meter.prepare(1000.0f, 1);

    std::vector<float> clipped(10, 1.2f);
    const float* clipped_channels[] = {clipped.data()};
    meter.process(clipped_channels, 1, static_cast<int>(clipped.size()));
    REQUIRE(meter.snapshot().channels[0].clipped);

    meter.prepare(1000.0f, 1);

    std::vector<float> clean(10, 0.25f);
    const float* clean_channels[] = {clean.data()};
    meter.process(clean_channels, 1, static_cast<int>(clean.size()));
    REQUIRE_FALSE(meter.snapshot().channels[0].clipped);
}

TEST_CASE("MultiChannelMeter clamps channels and reset clears snapshot", "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, kMaxMeterChannels + 4);
    REQUIRE(meter.snapshot().num_channels == kMaxMeterChannels);

    std::array<std::array<float, 1>, kMaxMeterChannels> samples{};
    std::array<const float*, kMaxMeterChannels> channels{};
    for (int ch = 0; ch < kMaxMeterChannels; ++ch) {
        samples[ch][0] = static_cast<float>(ch + 1) / 20.0f;
        channels[ch] = samples[ch].data();
    }

    meter.process(channels.data(), kMaxMeterChannels, 1);
    REQUIRE(meter.snapshot().num_channels == kMaxMeterChannels);
    REQUIRE_THAT(meter.snapshot().channels[15].peak, WithinAbs(0.8f, 1e-6f));

    meter.reset();
    REQUIRE(meter.snapshot().num_channels == 0);
}

TEST_CASE("MultiChannelMeter silent stereo emits finite zero correlation", "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(10.0f, 2);

    std::vector<float> left(4, 0.0f);
    std::vector<float> right(4, 0.0f);
    const float* channels[] = {left.data(), right.data()};
    meter.process(channels, 2, static_cast<int>(left.size()));

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 2);
    REQUIRE_THAT(snap.correlation, WithinAbs(0.0f, 1e-6f));
    REQUIRE(std::isinf(snap.channels[0].lufs_momentary));
    REQUIRE(std::isinf(snap.lufs_integrated));
}

TEST_CASE("MultiChannelMeter waits for block threshold before emitting", "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(1000.0f, 1);

    std::vector<float> samples(5, 0.75f);
    const float* channels[] = {samples.data()};
    meter.process(channels, 1, static_cast<int>(samples.size()));

    REQUIRE(meter.snapshot().num_channels == 1);
    REQUIRE_THAT(meter.snapshot().channels[0].peak, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("MultiChannelMeter resets correlation accumulation window", "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(10.0f, 2);

    float left[] = {0.5f};
    float right[] = {0.5f};
    const float* channels[] = {left, right};
    meter.process(channels, 2, 1);

    REQUIRE_THAT(meter.snapshot().correlation, WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("MultiChannelMeter clamps negative prepared channel counts",
          "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, -3);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 0);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("MultiChannelMeter clamps process channel count to prepared channels",
          "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, 1);

    float left[] = {0.25f};
    float ignored_right[] = {1.0f};
    std::vector<const float*> channels;
    channels.push_back(left);
    channels.push_back(ignored_right);
    channels.push_back(nullptr);

    meter.process(channels.data(), static_cast<int>(channels.size()), 1);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 1);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(snap.channels[1].peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_FALSE(snap.channels[1].clipped);
}

TEST_CASE("MultiChannelMeter clamps negative process channel counts",
          "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, 1);

    float sample[] = {1.0f};
    const float* channels[] = {sample};
    meter.process(channels, -1, 1);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 0);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_FALSE(snap.channels[0].clipped);
}

TEST_CASE("MultiChannelBallistics holds peaks and clip indicators", "[signal][meter]") {
    MultiChannelBallistics ballistics;

    MultiChannelMeterData data;
    data.num_channels = 1;
    data.channels[0].peak = 1.0f;
    data.channels[0].rms = 0.5f;
    data.channels[0].clipped = true;

    ballistics.update(data, 0.016f);
    REQUIRE(ballistics.num_channels == 1);
    REQUIRE(ballistics.channels[0].display_peak > 0.9f);
    REQUIRE(ballistics.channels[0].display_rms > 0.45f);
    REQUIRE_THAT(ballistics.channels[0].held_peak, WithinAbs(1.0f, 0.001f));
    REQUIRE(ballistics.channels[0].clip_indicator);

    data.channels[0].peak = 0.0f;
    data.channels[0].rms = 0.0f;
    data.channels[0].clipped = false;

    ballistics.update(data, 0.5f);
    REQUIRE(ballistics.channels[0].clip_indicator);
    REQUIRE_THAT(ballistics.channels[0].held_peak, WithinAbs(1.0f, 0.001f));

    ballistics.update(data, 3.0f);
    REQUIRE_FALSE(ballistics.channels[0].clip_indicator);

    data.channels[0].clipped = true;
    ballistics.update(data, 0.016f);
    REQUIRE(ballistics.channels[0].clip_indicator);
    ballistics.clear_clips();
    REQUIRE_FALSE(ballistics.channels[0].clip_indicator);
}

TEST_CASE("MultiChannelBallistics clamps invalid snapshot channel counts",
          "[signal][meter][issue-645]") {
    MultiChannelBallistics ballistics;

    MultiChannelMeterData data;
    data.num_channels = -2;
    ballistics.update(data, 0.01f);
    REQUIRE(ballistics.num_channels == 0);

    data.num_channels = kMaxMeterChannels + 3;
    for (int ch = 0; ch < kMaxMeterChannels; ++ch) {
        data.channels[ch].peak = static_cast<float>(ch + 1) / 20.0f;
    }

    ballistics.update(data, 0.01f);
    REQUIRE(ballistics.num_channels == kMaxMeterChannels);
    REQUIRE(ballistics.channels[0].display_peak > 0.0f);
    REQUIRE(ballistics.channels[kMaxMeterChannels - 1].display_peak > 0.0f);
}
