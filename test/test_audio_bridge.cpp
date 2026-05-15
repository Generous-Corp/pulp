#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/audio_bridge.hpp>
#include <cmath>
#include <vector>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

TEST_CASE("AudioBridge push and pop meter", "[view][bridge]") {
    AudioBridge bridge;

    MeterData data;
    data.num_channels = 2;
    data.peak[0] = 0.8f;
    data.peak[1] = 0.5f;
    data.rms[0] = 0.3f;
    data.rms[1] = 0.2f;

    bridge.push_meter(data);

    MeterData out;
    REQUIRE(bridge.pop_latest_meter(out));
    REQUIRE(out.num_channels == 2);
    REQUIRE_THAT(out.peak[0], WithinAbs(0.8, 0.001));
    REQUIRE_THAT(out.rms[1], WithinAbs(0.2, 0.001));
}

TEST_CASE("AudioBridge pop_latest drains queue", "[view][bridge]") {
    AudioBridge bridge;

    // Push 3 meter snapshots
    for (int i = 1; i <= 3; ++i) {
        MeterData data;
        data.num_channels = 1;
        data.peak[0] = 0.1f * i; // 0.1, 0.2, 0.3
        bridge.push_meter(data);
    }

    MeterData out;
    REQUIRE(bridge.pop_latest_meter(out));
    // Should get the LAST pushed value
    REQUIRE_THAT(out.peak[0], WithinAbs(0.3, 0.001));

    // TripleBuffer always has a latest value after first push — verify it's still 0.3
    REQUIRE(bridge.pop_latest_meter(out));
    REQUIRE_THAT(out.peak[0], WithinAbs(0.3, 0.001));
}

TEST_CASE("AudioBridge pop_latest reports empty before first meter",
          "[view][bridge][issue-493]") {
    AudioBridge bridge;

    MeterData out;
    REQUIRE_FALSE(bridge.pop_latest_meter(out));
    REQUIRE(out.num_channels == 0);
}

TEST_CASE("AudioBridge analyze_and_push", "[view][bridge]") {
    AudioBridge bridge;

    // Generate a buffer with known values
    constexpr int num_samples = 256;
    std::vector<float> ch0(num_samples);
    std::vector<float> ch1(num_samples);

    // ch0: sine wave with peak at 0.5
    for (int i = 0; i < num_samples; ++i) {
        ch0[i] = 0.5f * std::sin(2.0f * 3.14159f * i / num_samples);
        ch1[i] = 0.0f; // silent
    }

    const float* channels[] = {ch0.data(), ch1.data()};
    bridge.analyze_and_push(channels, 2, num_samples);

    MeterData out;
    REQUIRE(bridge.pop_latest_meter(out));
    REQUIRE(out.num_channels == 2);

    // ch0 peak should be ~0.5
    REQUIRE(out.peak[0] > 0.49f);
    REQUIRE(out.peak[0] < 0.51f);

    // ch0 RMS should be ~0.5/sqrt(2) ≈ 0.354
    REQUIRE(out.rms[0] > 0.3f);
    REQUIRE(out.rms[0] < 0.4f);

    // ch1 should be silent
    REQUIRE_THAT(out.peak[1], WithinAbs(0.0, 0.001));
    REQUIRE_THAT(out.rms[1], WithinAbs(0.0, 0.001));
}

TEST_CASE("AudioBridge analyze clamps channels and handles zero sample blocks",
          "[view][bridge][issue-493]") {
    AudioBridge bridge;

    std::vector<std::vector<float>> samples(10, std::vector<float>(4));
    std::vector<const float*> channels(samples.size());
    for (std::size_t ch = 0; ch < samples.size(); ++ch) {
        float base = 0.05f * static_cast<float>(ch + 1);
        samples[ch] = {base, -2.0f * base, 0.5f * base, 0.0f};
        channels[ch] = samples[ch].data();
    }

    bridge.analyze_and_push(channels.data(), static_cast<int>(channels.size()), 4);

    MeterData out;
    REQUIRE(bridge.pop_latest_meter(out));
    REQUIRE(out.num_channels == MeterData::max_channels);
    for (int ch = 0; ch < out.num_channels; ++ch) {
        float base = 0.05f * static_cast<float>(ch + 1);
        REQUIRE_THAT(out.peak[ch], WithinAbs(2.0f * base, 0.0001));
        REQUIRE_THAT(out.rms[ch],
                     WithinAbs(std::sqrt((base * base +
                                          4.0f * base * base +
                                          0.25f * base * base) /
                                         4.0f),
                               0.0001));
    }

    bridge.analyze_and_push(channels.data(), static_cast<int>(channels.size()), 0);
    REQUIRE(bridge.pop_latest_meter(out));
    REQUIRE(out.num_channels == MeterData::max_channels);
    for (int ch = 0; ch < out.num_channels; ++ch) {
        REQUIRE_THAT(out.peak[ch], WithinAbs(0.0, 0.0001));
        REQUIRE_THAT(out.rms[ch], WithinAbs(0.0, 0.0001));
    }
}

TEST_CASE("MeterBallistics smooth response", "[view][bridge]") {
    MeterBallistics ballistics;

    // Initial state should be zero
    REQUIRE_THAT(ballistics.display_peak, WithinAbs(0.0, 0.001));

    // Feed a sudden peak
    ballistics.update(0.8f, 0.3f, 1.0f / 60.0f); // 60fps frame time
    REQUIRE(ballistics.display_peak > 0); // Should have moved toward 0.8

    // Feed silence — should decay slowly
    float prev = ballistics.display_peak;
    for (int i = 0; i < 10; ++i) {
        ballistics.update(0.0f, 0.0f, 1.0f / 60.0f);
    }
    REQUIRE(ballistics.display_peak < prev);
    REQUIRE(ballistics.display_peak > 0); // Should not have reached zero yet
}

TEST_CASE("MeterBallistics peak hold", "[view][bridge]") {
    MeterBallistics ballistics;

    // Hit a peak
    ballistics.update(0.9f, 0.5f, 1.0f / 60.0f);
    REQUIRE_THAT(ballistics.held_peak, WithinAbs(0.9, 0.001));

    // Feed silence for less than hold time — peak should stay held
    for (int i = 0; i < 30; ++i) { // 0.5 seconds at 60fps
        ballistics.update(0.0f, 0.0f, 1.0f / 60.0f);
    }
    REQUIRE_THAT(ballistics.held_peak, WithinAbs(0.9, 0.001)); // Still held

    // Feed silence past hold time — peak should start decaying
    for (int i = 0; i < 120; ++i) { // 2 more seconds
        ballistics.update(0.0f, 0.0f, 1.0f / 60.0f);
    }
    REQUIRE(ballistics.held_peak < 0.9f); // Should have decayed
}

TEST_CASE("MeterBallistics releases tiny values to exact zero",
          "[view][bridge][issue-493]") {
    MeterBallistics ballistics;

    ballistics.display_peak = 5e-7f;
    ballistics.display_rms = 4e-7f;
    ballistics.update(0.0f, 0.0f, 1.0f / 60.0f);

    REQUIRE(ballistics.display_peak == 0.0f);
    REQUIRE(ballistics.display_rms == 0.0f);
}
