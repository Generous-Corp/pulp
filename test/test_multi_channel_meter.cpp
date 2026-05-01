#include <pulp/signal/multi_channel_meter.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <limits>

using Catch::Matchers::WithinAbs;
using namespace pulp::signal;

TEST_CASE("MultiChannelMeter process clamps to prepared channel count", "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, 1);

    float first[] = {0.25f};
    float ignored[] = {1.0f};
    const float* channels[] = {first, ignored};
    meter.process(channels, 2, 1);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 1);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(snap.channels[1].peak, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("MultiChannelMeter process clamps negative channel count", "[signal][meter][issue-645]") {
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

TEST_CASE("MultiChannelMeter empty process leaves current snapshot untouched", "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, 1);

    float sample[] = {0.5f};
    const float* channels[] = {sample};
    meter.process(channels, 1, 0);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 1);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE(std::isinf(snap.channels[0].lufs_momentary));
}

TEST_CASE("MultiChannelMeter correlation window can replace previous sign", "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(10.0f, 2);

    float left[] = {0.5f};
    float right[] = {0.5f};
    const float* channels[] = {left, right};
    meter.process(channels, 2, 1);
    REQUIRE_THAT(meter.snapshot().correlation, WithinAbs(1.0f, 1e-6f));

    right[0] = -0.5f;
    meter.process(channels, 2, 1);
    REQUIRE_THAT(meter.snapshot().correlation, WithinAbs(-1.0f, 1e-6f));
}

TEST_CASE("MultiChannelMeter integrated LUFS averages multiple windows", "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(10.0f, 1);

    float half[] = {0.5f};
    const float* half_channels[] = {half};
    for (int i = 0; i < 4; ++i)
        meter.process(half_channels, 1, 1);

    REQUIRE(std::isfinite(meter.snapshot().lufs_integrated));

    float quarter[] = {0.25f};
    const float* quarter_channels[] = {quarter};
    for (int i = 0; i < 4; ++i)
        meter.process(quarter_channels, 1, 1);

    constexpr double first_mean_sq = 0.25;
    constexpr double second_mean_sq = 0.0625;
    auto expected = -0.691f + 10.0f * static_cast<float>(
        std::log10((first_mean_sq + second_mean_sq) / 2.0));
    REQUIRE_THAT(meter.snapshot().lufs_integrated, WithinAbs(expected, 1e-4f));
}

TEST_CASE("MultiChannelMeter prepare clamps channel count and reset clears snapshot", "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, kMaxMeterChannels + 4);

    std::array<float, kMaxMeterChannels> samples{};
    std::array<const float*, kMaxMeterChannels> channels{};
    for (int ch = 0; ch < kMaxMeterChannels; ++ch) {
        samples[ch] = 0.05f * static_cast<float>(ch + 1);
        channels[ch] = &samples[ch];
    }

    meter.process(channels.data(), kMaxMeterChannels, 1);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == kMaxMeterChannels);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(0.05f, 1e-6f));
    REQUIRE_THAT(snap.channels[kMaxMeterChannels - 1].peak, WithinAbs(0.8f, 1e-6f));

    meter.reset();
    REQUIRE(meter.snapshot().num_channels == 0);
    REQUIRE_THAT(meter.snapshot().channels[0].peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(meter.snapshot().correlation, WithinAbs(0.0f, 1e-6f));
    REQUIRE(std::isinf(meter.snapshot().lufs_integrated));
}

TEST_CASE("MultiChannelMeter detects clips and handles silent stereo correlation", "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, 2);

    float left[] = {0.0f};
    float right[] = {0.0f};
    const float* silent[] = {left, right};
    meter.process(silent, 2, 1);
    REQUIRE_THAT(meter.snapshot().correlation, WithinAbs(0.0f, 1e-6f));
    REQUIRE_FALSE(meter.snapshot().channels[0].clipped);

    left[0] = -1.0f;
    right[0] = 0.25f;
    meter.process(silent, 2, 1);

    REQUIRE(meter.snapshot().channels[0].clipped);
    REQUIRE_FALSE(meter.snapshot().channels[1].clipped);
    REQUIRE_THAT(meter.snapshot().channels[0].peak, WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("MultiChannelBallistics releases held values and clamps display floor", "[signal][meter][issue-645]") {
    MultiChannelBallistics ballistics;
    ballistics.peak_hold_time = 0.05f;
    ballistics.release_time = 0.01f;

    MultiChannelMeterData data;
    data.num_channels = 1;
    data.channels[0].peak = 0.75f;
    data.channels[0].rms = 0.5f;
    ballistics.update(data, 0.01f);

    REQUIRE(ballistics.channels[0].display_peak > 0.0f);
    REQUIRE(ballistics.channels[0].display_rms > 0.0f);
    REQUIRE_THAT(ballistics.channels[0].held_peak, WithinAbs(0.75f, 1e-6f));

    data.channels[0].peak = 0.0f;
    data.channels[0].rms = 0.0f;
    ballistics.update(data, 1.0f);

    REQUIRE_THAT(ballistics.channels[0].display_peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(ballistics.channels[0].display_rms, WithinAbs(0.0f, 1e-6f));
    REQUIRE(ballistics.channels[0].held_peak < 1e-3f);
}

TEST_CASE("MultiChannelBallistics higher peak refreshes held peak", "[signal][meter][issue-645]") {
    MultiChannelBallistics ballistics;

    MultiChannelMeterData data;
    data.num_channels = 1;
    data.channels[0].peak = 0.5f;
    ballistics.update(data, 0.01f);
    REQUIRE_THAT(ballistics.channels[0].held_peak, WithinAbs(0.5f, 1e-6f));

    data.channels[0].peak = 0.25f;
    ballistics.update(data, 0.01f);
    REQUIRE_THAT(ballistics.channels[0].held_peak, WithinAbs(0.5f, 1e-6f));

    data.channels[0].peak = 0.8f;
    ballistics.update(data, 0.01f);
    REQUIRE_THAT(ballistics.channels[0].held_peak, WithinAbs(0.8f, 1e-6f));
    REQUIRE_THAT(ballistics.channels[0].hold_counter, WithinAbs(ballistics.peak_hold_time, 1e-6f));
}

TEST_CASE("MultiChannelBallistics clamps channel count and clears clip holds", "[signal][meter][issue-645]") {
    MultiChannelBallistics ballistics;
    ballistics.clip_hold_time = 0.5f;

    MultiChannelMeterData data;
    data.num_channels = kMaxMeterChannels + 8;
    data.channels[0].clipped = true;
    data.channels[0].peak = 1.0f;
    ballistics.update(data, 0.01f);

    REQUIRE(ballistics.num_channels == kMaxMeterChannels);
    REQUIRE(ballistics.channels[0].clip_indicator);
    REQUIRE_THAT(ballistics.channels[0].clip_hold_counter, WithinAbs(0.5f, 1e-6f));

    data.channels[0].clipped = false;
    ballistics.update(data, 0.25f);
    REQUIRE(ballistics.channels[0].clip_indicator);

    ballistics.update(data, 0.30f);
    REQUIRE_FALSE(ballistics.channels[0].clip_indicator);

    data.channels[0].clipped = true;
    ballistics.update(data, 0.01f);
    REQUIRE(ballistics.channels[0].clip_indicator);

    ballistics.clear_clips();
    REQUIRE_FALSE(ballistics.channels[0].clip_indicator);
    REQUIRE_THAT(ballistics.channels[0].clip_hold_counter, WithinAbs(0.0f, 1e-6f));
}
