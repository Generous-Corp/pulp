#include <pulp/signal/multi_channel_meter.hpp>

#include "harness/rt_allocation_probe.hpp"
#include "support/audio_test_signals.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <type_traits>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace pulp::signal;

namespace pulp::signal::detail {

struct MultiChannelMeterTestAccess {
    template <typename SampleType>
    static std::uint64_t gate_epoch(const MultiChannelMeterT<SampleType>& meter) {
        return meter.gate_epoch_;
    }

    template <typename SampleType>
    static std::size_t current_epoch_nodes(const MultiChannelMeterT<SampleType>& meter) {
        return static_cast<std::size_t>(std::count(
            meter.gate_node_epoch_.begin(), meter.gate_node_epoch_.end(),
            meter.gate_epoch_));
    }

    template <typename SampleType>
    static std::size_t gate_capacity(const MultiChannelMeterT<SampleType>& meter) {
        return meter.gate_node_epoch_.size();
    }
};

} // namespace pulp::signal::detail

namespace {

double biquad_response_db(const LoudnessBiquadCoefficients& coefficients,
                          double frequency, double sample_rate) {
    const auto z = std::polar(1.0, -2.0 * std::numbers::pi * frequency / sample_rate);
    const auto z2 = z * z;
    return 20.0 * std::log10(std::abs(
        (coefficients.b0 + coefficients.b1 * z + coefficients.b2 * z2) /
        (1.0 + coefficients.a1 * z + coefficients.a2 * z2)));
}

double k_weighting_response_db(double frequency, double sample_rate) {
    const auto coefficients = k_weighting_coefficients(sample_rate);
    REQUIRE(coefficients.valid);
    return biquad_response_db(coefficients.shelf, frequency, sample_rate)
         + biquad_response_db(coefficients.high_pass, frequency, sample_rate);
}

} // namespace

static_assert(std::is_aggregate_v<MultiChannelMeterData>);

TEST_CASE("MultiChannelMeter requires preparation before reporting loudness support",
          "[signal][meter][loudness]") {
    MultiChannelMeter meter;
    REQUIRE_FALSE(meter.loudness_supported());
}

TEST_CASE("MultiChannelMeterData preserves legacy positional aggregate initialization",
          "[signal][meter][compatibility]") {
    std::array<ChannelLevels, kMaxMeterChannels> channels{};
    MultiChannelMeterData legacy{channels, 2, 0.25f, -17.5f};

    REQUIRE(legacy.num_channels == 2);
    REQUIRE_THAT(legacy.correlation, WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(legacy.lufs_integrated, WithinAbs(-17.5f, 1e-6f));
    REQUIRE(std::isinf(legacy.lufs_momentary));
}

TEST_CASE("MultiChannelMeter default construction remains safe before prepare",
          "[signal][meter][compatibility]") {
    MultiChannelMeter meter;
    std::array<float, 17640> samples{};
    samples.fill(0.5f);
    const float* channels[] = {samples.data()};

    meter.process(channels, 1, static_cast<int>(samples.size()));

    REQUIRE(meter.snapshot().num_channels == 1);
    REQUIRE(std::isfinite(meter.snapshot().channels[0].rms));
    REQUIRE(std::isinf(meter.snapshot().lufs_integrated));
}

TEST_CASE("MultiChannelMeter process and ballistics are allocation-free after prepare",
          "[signal][meter][rt-safety]") {
    MultiChannelMeter meter;
    meter.prepare(48000.0f, 2);

    std::array<float, 4800> left{};
    std::array<float, 4800> right{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = (i == 3) ? 1.1f : 0.25f;
        right[i] = (i % 2 == 0) ? -0.25f : 0.25f;
    }

    const float* channels[] = {left.data(), right.data()};
    MultiChannelBallistics ballistics;

    {
        pulp::test::RtAllocationProbe probe;
        for (int hop = 0; hop < 5; ++hop)
            meter.process(channels, 2, static_cast<int>(left.size()));
        const auto& snapshot = meter.snapshot();
        (void) ballistics.update(snapshot, 0.016f);
        ballistics.clear_clips();
        meter.reset();
        REQUIRE_FALSE(probe.saw_allocation());
    }

    REQUIRE(meter.snapshot().num_channels == 0);
    REQUIRE_FALSE(ballistics.channels[0].clip_indicator);
}

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

TEST_CASE("MultiChannelMeter ignores null channel arrays without changing snapshot",
          "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, 2);

    meter.process(nullptr, 2, 1);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 2);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(snap.channels[1].peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_FALSE(snap.channels[0].clipped);
    REQUIRE_FALSE(snap.channels[1].clipped);
}

TEST_CASE("MultiChannelMeter truncates processing at the first null channel",
          "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, 2);

    float left[] = {0.75f};
    const float* channels[] = {left, nullptr};
    meter.process(channels, 2, 1);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 1);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(0.75f, 1e-6f));
    REQUIRE_THAT(snap.channels[0].rms, WithinAbs(0.75f, 1e-6f));
    REQUIRE_THAT(snap.channels[1].peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(snap.correlation, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("MultiChannelMeter clears dropped-channel accumulators before reuse",
          "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(1000.0f, 2);

    std::array<float, 5> warm_left{};
    std::array<float, 5> warm_right{};
    warm_left.fill(0.1f);
    warm_right.fill(1.0f);
    const float* warm_channels[] = {warm_left.data(), warm_right.data()};
    meter.process(warm_channels, 2, static_cast<int>(warm_left.size()));
    REQUIRE_THAT(meter.snapshot().channels[1].peak, WithinAbs(0.0f, 1e-6f));

    std::array<float, 5> left_only{};
    left_only.fill(0.1f);
    const float* dropped_channels[] = {left_only.data(), nullptr};
    meter.process(dropped_channels, 2, static_cast<int>(left_only.size()));
    REQUIRE(meter.snapshot().num_channels == 1);

    std::array<float, 10> fresh_left{};
    std::array<float, 10> fresh_right{};
    fresh_left.fill(0.1f);
    fresh_right.fill(0.25f);
    const float* fresh_channels[] = {fresh_left.data(), fresh_right.data()};
    meter.process(fresh_channels, 2, static_cast<int>(fresh_left.size()));

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 2);
    REQUIRE_THAT(snap.channels[1].peak, WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(snap.channels[1].rms, WithinAbs(0.25f, 1e-6f));
    REQUIRE_FALSE(snap.channels[1].clipped);
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

TEST_CASE("K-weighting design reproduces the BS.1770 48 kHz coefficient tables",
          "[signal][meter][loudness]") {
    const auto coefficients = k_weighting_coefficients(48000.0);
    REQUIRE(coefficients.valid);
    REQUIRE_THAT(coefficients.shelf.b0, WithinAbs(1.53512485958697, 2e-12));
    REQUIRE_THAT(coefficients.shelf.b1, WithinAbs(-2.69169618940638, 2e-12));
    REQUIRE_THAT(coefficients.shelf.b2, WithinAbs(1.19839281085285, 2e-12));
    REQUIRE_THAT(coefficients.shelf.a1, WithinAbs(-1.69065929318241, 2e-12));
    REQUIRE_THAT(coefficients.shelf.a2, WithinAbs(0.73248077421585, 2e-12));
    REQUIRE_THAT(coefficients.high_pass.b0, WithinAbs(1.0, 1e-15));
    REQUIRE_THAT(coefficients.high_pass.b1, WithinAbs(-2.0, 1e-15));
    REQUIRE_THAT(coefficients.high_pass.b2, WithinAbs(1.0, 1e-15));
    REQUIRE_THAT(coefficients.high_pass.a1, WithinAbs(-1.99004745483398, 2e-12));
    REQUIRE_THAT(coefficients.high_pass.a2, WithinAbs(0.99007225036621, 2e-12));
    REQUIRE_FALSE(k_weighting_coefficients(0.0).valid);
}

TEST_CASE("K-weighting arbitrary-rate design has the independently tabulated response",
          "[signal][meter][loudness]") {
    // Values were calculated independently from the BS.1770 analog prototype
    // and a bilinear transform, rather than from this implementation's output.
    REQUIRE_THAT(k_weighting_response_db(100.0, 44100.0),
                 WithinAbs(-1.129671272642, 1e-9));
    REQUIRE_THAT(k_weighting_response_db(1000.0, 44100.0),
                 WithinAbs(0.700461752790, 1e-9));
    REQUIRE_THAT(k_weighting_response_db(10000.0, 44100.0),
                 WithinAbs(4.045849905647, 1e-9));
    REQUIRE_THAT(k_weighting_response_db(100.0, 96000.0),
                 WithinAbs(-1.155142518290, 1e-9));
    REQUIRE_THAT(k_weighting_response_db(1000.0, 96000.0),
                 WithinAbs(0.680401211521, 1e-9));
    REQUIRE_THAT(k_weighting_response_db(10000.0, 96000.0),
                 WithinAbs(4.019537351811, 1e-9));
}

TEST_CASE("MultiChannelMeter matches EBU Tech 3341 minimum-requirements Test 1",
          "[signal][meter][loudness][ebu-vector]") {
    // Fetch-free numeric representation of EBU Tech 3341 v4.0, Table 1,
    // Test 1: stereo, in-phase 1000 Hz sine, -23.0 dBFS peak per channel,
    // 20 seconds, expected M/S/I -23.0 +/- 0.1 LUFS. Source:
    // https://tech.ebu.ch/files/live/sites/tech/files/shared/tech/tech3341v4_0.pdf
    constexpr int sample_rate = 48000;
    constexpr int duration_samples = 20 * sample_rate;
    constexpr float amplitude = 0.0707945784384138f; // 10^(-23/20)
    auto vector = pulp::test::audio::make_sine(
        2, duration_samples, 1000.0f, sample_rate, amplitude);
    const float* channels[] = {vector.channel(0).data(), vector.channel(1).data()};

    MultiChannelMeter meter;
    meter.prepare(sample_rate, 2);
    meter.process(channels, 2, duration_samples);

    REQUIRE_THAT(meter.snapshot().lufs_momentary, WithinAbs(-23.0f, 0.1f));
    REQUIRE_THAT(meter.snapshot().lufs_integrated, WithinAbs(-23.0f, 0.1f));
}

TEST_CASE("MultiChannelMeter matches the BS.1770 997 Hz reference level",
          "[signal][meter][loudness]") {
    constexpr int sample_rate = 48000;
    auto tone = pulp::test::audio::make_sine(1, sample_rate * 3, 997.0f, sample_rate);
    const float* channels[] = {tone.channel(0).data()};

    MultiChannelMeter meter;
    meter.prepare(sample_rate, 1);
    meter.process(channels, 1, static_cast<int>(tone.num_samples()));

    // BS.1770-5 Annex 1 states -3.01 LKFS for a full-scale 997 Hz sine.
    REQUIRE_THAT(meter.snapshot().lufs_momentary, WithinAbs(-3.01f, 0.01f));
    REQUIRE_THAT(meter.snapshot().lufs_integrated, WithinAbs(-3.01f, 0.01f));

    // Planted negative control: unweighted mean square plus -0.691 is wrong by
    // the K-weighting calibration gain and must not satisfy the oracle.
    const float unweighted = -0.691f + 10.0f * std::log10(0.5f);
    REQUIRE(std::abs(unweighted - meter.snapshot().lufs_momentary) > 0.65f);
}

TEST_CASE("MultiChannelMeter applies the K-weighting frequency response",
          "[signal][meter][loudness]") {
    constexpr int sample_rate = 48000;
    auto tone = pulp::test::audio::make_sine(1, sample_rate * 3, 100.0f, sample_rate);
    const float* channels[] = {tone.channel(0).data()};

    MultiChannelMeter meter;
    meter.prepare(sample_rate, 1);
    meter.process(channels, 1, static_cast<int>(tone.num_samples()));

    // Independent double-precision evaluation of the BS.1770-5 Tables 1/2
    // biquads gives -4.835 dB for this steady-state vector.
    REQUIRE_THAT(meter.snapshot().lufs_momentary, WithinAbs(-4.835f, 0.015f));
    const float unweighted = -0.691f + 10.0f * std::log10(0.5f);
    REQUIRE(std::abs(unweighted - meter.snapshot().lufs_momentary) > 1.0f);
}

TEST_CASE("MultiChannelMeter integrated loudness uses absolute and relative gates",
          "[signal][meter][loudness]") {
    constexpr int sample_rate = 48000;
    constexpr int segment_frames = sample_rate * 5;
    auto loud = pulp::test::audio::make_sine(1, segment_frames, 997.0f, sample_rate, 0.1f);
    auto quiet = pulp::test::audio::make_sine(1, segment_frames, 997.0f, sample_rate, 0.01f);

    MultiChannelMeter meter;
    meter.prepare(sample_rate, 1);
    const float* loud_channels[] = {loud.channel(0).data()};
    const float* quiet_channels[] = {quiet.channel(0).data()};
    meter.process(loud_channels, 1, segment_frames);
    meter.process(quiet_channels, 1, segment_frames);

    // The -43.01 LUFS half lies below the relative gate. Independent block
    // enumeration gives -23.141 LUFS because the three 75%-overlapped blocks
    // crossing the level transition remain above the gate. An unweighted,
    // ungated energy average would be about -25.98 LUFS.
    REQUIRE_THAT(meter.snapshot().lufs_integrated, WithinAbs(-23.141f, 0.02f));
    constexpr float ungated_negative_control = -25.977f;
    REQUIRE(std::abs(ungated_negative_control - meter.snapshot().lufs_integrated) > 2.8f);
}

TEST_CASE("MultiChannelMeter gates finite programmes above 40 LUFS",
          "[signal][meter][loudness]") {
    constexpr int sample_rate = 48000;
    constexpr int segment_frames = sample_rate * 2;
    auto hot = pulp::test::audio::make_sine(
        1, segment_frames, 997.0f, sample_rate, 200.0f);
    const float* hot_channels[] = {hot.channel(0).data()};

    MultiChannelMeter meter;
    meter.prepare(sample_rate, 1);
    meter.process(hot_channels, 1, segment_frames);

    // Hot blocks are about +43.01 LUFS, putting their relative gate above the
    // old +30 LUFS histogram ceiling. The integrated result must remain finite.
    REQUIRE(std::isfinite(meter.snapshot().lufs_integrated));
    REQUIRE_THAT(meter.snapshot().lufs_integrated, WithinAbs(43.01f, 0.03f));
}

TEST_CASE("MultiChannelMeter restarts every window across 2 to 1 to 2 topology changes",
          "[signal][meter][loudness][rt-safety]") {
    constexpr int sample_rate = 48000;
    MultiChannelMeter meter;
    meter.prepare(sample_rate, 2);

    std::array<float, sample_rate * 4 / 10> clipped_left{};
    std::array<float, sample_rate * 4 / 10> clipped_right{};
    clipped_left.fill(1.2f);
    clipped_right.fill(1.2f);
    const float* clipped[] = {clipped_left.data(), clipped_right.data()};
    meter.process(clipped, 2, static_cast<int>(clipped_left.size()));
    REQUIRE(meter.snapshot().channels[0].clipped);
    REQUIRE(std::isfinite(meter.snapshot().lufs_integrated));
    const auto populated_epoch = detail::MultiChannelMeterTestAccess::gate_epoch(meter);
    REQUIRE(detail::MultiChannelMeterTestAccess::current_epoch_nodes(meter) > 0);
    REQUIRE(detail::MultiChannelMeterTestAccess::current_epoch_nodes(meter)
            < detail::MultiChannelMeterTestAccess::gate_capacity(meter));

    std::array<float, sample_rate / 200> mono{};
    mono.fill(0.75f);
    const float* mono_channels[] = {mono.data()};

    std::array<float, sample_rate / 100> fresh_left{};
    std::array<float, sample_rate / 100> fresh_right{};
    fresh_left.fill(0.25f);
    fresh_right.fill(-0.25f);
    const float* fresh[] = {fresh_left.data(), fresh_right.data()};
    MultiChannelMeterData mono_snapshot;
    bool allocated = false;
    {
        pulp::test::RtAllocationProbe probe;
        meter.process(mono_channels, 1, static_cast<int>(mono.size()));
        mono_snapshot = meter.snapshot();
        meter.process(fresh, 2, static_cast<int>(fresh_left.size()));
        allocated = probe.saw_allocation();
    }

    REQUIRE_FALSE(allocated);
    REQUIRE(mono_snapshot.num_channels == 1);
    REQUIRE_THAT(mono_snapshot.channels[0].peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_FALSE(mono_snapshot.channels[0].clipped);
    REQUIRE(std::isinf(mono_snapshot.lufs_integrated));
    REQUIRE(detail::MultiChannelMeterTestAccess::gate_epoch(meter)
            == populated_epoch + 2);
    // Neither short post-change block reaches a loudness hop. Zero initialized
    // nodes proves the resets only advanced an epoch; they did not touch all
    // histogram bins.
    REQUIRE(detail::MultiChannelMeterTestAccess::current_epoch_nodes(meter) == 0);

    const auto& snapshot = meter.snapshot();
    REQUIRE(snapshot.num_channels == 2);
    REQUIRE_THAT(snapshot.channels[0].peak, WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(snapshot.channels[0].rms, WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(snapshot.channels[1].peak, WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(snapshot.channels[1].rms, WithinAbs(0.25f, 1e-6f));
    REQUIRE_FALSE(snapshot.channels[0].clipped);
    REQUIRE_FALSE(snapshot.channels[1].clipped);
    REQUIRE_THAT(snapshot.correlation, WithinAbs(-1.0f, 1e-6f));
    REQUIRE(std::isinf(snapshot.lufs_momentary));
    REQUIRE(std::isinf(snapshot.lufs_integrated));
}

TEST_CASE("MultiChannelMeter64 clamps finite samples outside its audio domain",
          "[signal][meter][loudness][f64]") {
    constexpr int sample_rate = 48000;
    constexpr int frames = sample_rate;
    std::vector<double> near_limit(frames);
    for (int i = 0; i < frames; ++i) {
        near_limit[static_cast<std::size_t>(i)] = kMaxMeterInputMagnitude * std::sin(
            2.0 * std::numbers::pi * 997.0 * i / sample_rate);
    }
    const double* near_channels[] = {near_limit.data()};

    MultiChannelMeter64 near_meter;
    near_meter.prepare(sample_rate, 1);
    near_meter.process(near_channels, 1, frames);
    REQUIRE(std::isfinite(near_meter.snapshot().lufs_momentary));
    REQUIRE(std::isfinite(near_meter.snapshot().lufs_integrated));
    REQUIRE(near_meter.snapshot().channels[0].peak <= kMaxMeterInputMagnitude);
    REQUIRE(near_meter.snapshot().channels[0].peak > 0.99 * kMaxMeterInputMagnitude);

    std::vector<double> over_limit(frames);
    for (int i = 0; i < frames; ++i)
        over_limit[static_cast<std::size_t>(i)] = i % 2 == 0
            ? std::numeric_limits<double>::max()
            : -std::numeric_limits<double>::max();
    const double* over_channels[] = {over_limit.data()};

    MultiChannelMeter64 over_meter;
    over_meter.prepare(sample_rate, 1);
    over_meter.process(over_channels, 1, frames);
    const auto first = over_meter.snapshot();
    REQUIRE_THAT(first.channels[0].peak,
                 WithinAbs(static_cast<float>(kMaxMeterInputMagnitude), 1.0f));
    REQUIRE_THAT(first.channels[0].rms,
                 WithinAbs(static_cast<float>(kMaxMeterInputMagnitude), 1.0f));
    REQUIRE(std::isfinite(first.lufs_momentary));
    REQUIRE(std::isfinite(first.lufs_integrated));

    over_meter.reset();
    over_meter.process(over_channels, 1, frames);
    REQUIRE_THAT(over_meter.snapshot().lufs_integrated,
                 WithinAbs(first.lufs_integrated, 1e-5f));
}

TEST_CASE("MultiChannelMeter applies surround weighting and excludes LFE",
          "[signal][meter][loudness]") {
    constexpr int sample_rate = 48000;
    auto tone = pulp::test::audio::make_sine(2, sample_rate * 2, 997.0f, sample_rate);
    const float* channels[] = {tone.channel(0).data(), tone.channel(1).data()};
    const LoudnessChannelRole roles[] = {
        LoudnessChannelRole::left_surround, LoudnessChannelRole::lfe};

    MultiChannelMeter meter;
    meter.prepare(sample_rate, 2, roles);
    REQUIRE(meter.loudness_supported());
    meter.process(channels, 2, static_cast<int>(tone.num_samples()));

    REQUIRE_THAT(meter.snapshot().lufs_momentary, WithinAbs(-1.518f, 0.015f));
    REQUIRE(std::isinf(meter.snapshot().channels[1].lufs_momentary));

    const LoudnessChannelRole rear_role[] = {LoudnessChannelRole::left_rear_surround};
    const float* rear_channel[] = {tone.channel(0).data()};
    MultiChannelMeter rear_meter;
    rear_meter.prepare(sample_rate, 1, rear_role);
    rear_meter.process(rear_channel, 1, static_cast<int>(tone.num_samples()));
    REQUIRE_THAT(rear_meter.snapshot().lufs_momentary, WithinAbs(-3.01f, 0.015f));
}

TEST_CASE("MultiChannelMeter default quad layout uses left and right surrounds",
          "[signal][meter][loudness]") {
    constexpr int sample_rate = 48000;
    auto tone = pulp::test::audio::make_sine(1, sample_rate * 2, 997.0f, sample_rate);
    const float* channels[] = {tone.channel(0).data(), tone.channel(0).data(),
                               tone.channel(0).data(), tone.channel(0).data()};

    MultiChannelMeter meter;
    meter.prepare(sample_rate, 4);
    meter.process(channels, 4, static_cast<int>(tone.num_samples()));

    const float expected = -3.01f + 10.0f * std::log10(2.0f + 2.0f * 1.41f);
    REQUIRE_THAT(meter.snapshot().lufs_momentary, WithinAbs(expected, 0.02f));
    const float center_lfe_layout = -3.01f + 10.0f * std::log10(3.0f);
    REQUIRE(std::abs(meter.snapshot().lufs_momentary - center_lfe_layout) > 1.5f);
}

TEST_CASE("MultiChannelMeter rejects layouts without an honest BS.1770 mapping",
          "[signal][meter][loudness]") {
    constexpr int sample_rate = 48000;
    constexpr int frames = sample_rate / 2;
    std::array<std::vector<float>, 9> audio;
    std::array<const float*, 9> channels{};
    for (std::size_t channel = 0; channel < audio.size(); ++channel) {
        audio[channel].resize(frames);
        for (int frame = 0; frame < frames; ++frame)
            audio[channel][static_cast<std::size_t>(frame)] =
                0.1f * std::sin(2.0f * std::numbers::pi_v<float> * 997.0f
                                * static_cast<float>(frame) / sample_rate);
        channels[channel] = audio[channel].data();
    }

    for (const int unsupported_count : {7, 9}) {
        MultiChannelMeter meter;
        meter.prepare(sample_rate, unsupported_count);
        REQUIRE_FALSE(meter.loudness_supported());
        meter.process(channels.data(), unsupported_count, frames);
        REQUIRE(std::isinf(meter.snapshot().lufs_momentary));
        REQUIRE(std::isinf(meter.snapshot().lufs_integrated));
        REQUIRE(meter.snapshot().channels[0].peak > 0.09f);
        REQUIRE(meter.snapshot().channels[0].rms > 0.06f);
    }
}

TEST_CASE("MultiChannelMeter validates every explicit loudness channel role",
          "[signal][meter][loudness]") {
    const LoudnessChannelRole unknown[] = {LoudnessChannelRole::unknown};
    const LoudnessChannelRole invalid[] = {static_cast<LoudnessChannelRole>(255)};
    const LoudnessChannelRole lfe_only[] = {LoudnessChannelRole::lfe};
    for (const auto* roles : {unknown, invalid, lfe_only}) {
        MultiChannelMeter meter;
        meter.prepare(48000.0, 1, roles);
        REQUIRE_FALSE(meter.loudness_supported());
    }
    const LoudnessChannelRole explicit_seven[] = {
        LoudnessChannelRole::left, LoudnessChannelRole::right,
        LoudnessChannelRole::center, LoudnessChannelRole::lfe,
        LoudnessChannelRole::left_surround, LoudnessChannelRole::right_surround,
        LoudnessChannelRole::left_rear_surround};
    MultiChannelMeter explicit_meter;
    explicit_meter.prepare(48000.0, 7, explicit_seven);
    REQUIRE(explicit_meter.loudness_supported());

    std::array<LoudnessChannelRole, 9> explicit_nine{};
    explicit_nine.fill(LoudnessChannelRole::left);
    MultiChannelMeter repeated_role_meter;
    repeated_role_meter.prepare(48000.0, static_cast<int>(explicit_nine.size()),
                                explicit_nine.data());
    REQUIRE(repeated_role_meter.loudness_supported());
    auto tone = pulp::test::audio::make_sine(1, 48000, 997.0f, 48000);
    std::array<const float*, 9> repeated_channels{};
    repeated_channels.fill(tone.channel(0).data());
    repeated_role_meter.process(repeated_channels.data(),
                                static_cast<int>(repeated_channels.size()), 48000);
    const float expected = -3.01f + 10.0f * std::log10(9.0f);
    REQUIRE_THAT(repeated_role_meter.snapshot().lufs_momentary,
                 WithinAbs(expected, 0.02f));

    std::array<LoudnessChannelRole, kMaxMeterChannels + 1> over_limit_roles{};
    over_limit_roles.fill(LoudnessChannelRole::left);
    MultiChannelMeter over_limit_meter;
    over_limit_meter.prepare(48000.0, static_cast<int>(over_limit_roles.size()),
                             over_limit_roles.data());
    REQUIRE_FALSE(over_limit_meter.loudness_supported());
}

TEST_CASE("MultiChannelMeter keeps level metering safe for unsupported sample rates",
          "[signal][meter][loudness]") {
    const float left[] = {0.25f};
    const float right[] = {-0.5f};
    const float* channels[] = {left, right};

    for (const double sample_rate : {
             std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::max()}) {
        MultiChannelMeter meter;
        meter.prepare(sample_rate, 2);
        REQUIRE_FALSE(meter.loudness_supported());
        meter.process(channels, 2, 1);
        REQUIRE_THAT(meter.snapshot().channels[0].peak, WithinAbs(0.25f, 1e-6f));
        REQUIRE_THAT(meter.snapshot().channels[1].rms, WithinAbs(0.5f, 1e-6f));
        REQUIRE(std::isinf(meter.snapshot().lufs_momentary));
        REQUIRE(std::isinf(meter.snapshot().lufs_integrated));
    }
}

TEST_CASE("MultiChannelMeter loudness requires the complete prepared layout",
          "[signal][meter][loudness]") {
    constexpr int sample_rate = 48000;
    auto tone = pulp::test::audio::make_sine(2, sample_rate, 997.0f, sample_rate, 0.1f);
    const float* stereo[] = {tone.channel(0).data(), tone.channel(1).data()};
    const float* mono[] = {tone.channel(0).data()};
    const float* oversized[] = {
        tone.channel(0).data(), tone.channel(1).data(), tone.channel(0).data()};

    MultiChannelMeter meter;
    meter.prepare(sample_rate, 2);
    REQUIRE(meter.loudness_supported());
    meter.process(mono, 1, sample_rate);
    REQUIRE(std::isinf(meter.snapshot().lufs_integrated));
    REQUIRE(meter.snapshot().channels[0].peak > 0.09f);
    meter.process(stereo, 2, sample_rate);
    REQUIRE(std::isfinite(meter.snapshot().lufs_integrated));
    meter.process(oversized, 3, sample_rate);
    REQUIRE(std::isinf(meter.snapshot().lufs_momentary));
    REQUIRE(std::isinf(meter.snapshot().lufs_integrated));
    REQUIRE(meter.snapshot().channels[0].peak > 0.09f);
}

TEST_CASE("MultiChannelMeter loudness is invariant to process block partitioning",
          "[signal][meter][loudness]") {
    constexpr int sample_rate = 48000;
    auto tone = pulp::test::audio::make_sine(2, sample_rate * 3, 997.0f, sample_rate, 0.2f);

    MultiChannelMeter contiguous;
    contiguous.prepare(sample_rate, 2);
    const float* all[] = {tone.channel(0).data(), tone.channel(1).data()};
    contiguous.process(all, 2, static_cast<int>(tone.num_samples()));

    MultiChannelMeter partitioned;
    partitioned.prepare(sample_rate, 2);
    constexpr std::array<int, 7> block_sizes{1, 17, 64, 127, 255, 511, 1024};
    int offset = 0;
    std::size_t block_index = 0;
    while (offset < static_cast<int>(tone.num_samples())) {
        const int frames = std::min(block_sizes[block_index++ % block_sizes.size()],
                                    static_cast<int>(tone.num_samples()) - offset);
        const float* block[] = {tone.channel(0).data() + offset,
                                tone.channel(1).data() + offset};
        partitioned.process(block, 2, frames);
        offset += frames;
    }

    REQUIRE_THAT(partitioned.snapshot().lufs_momentary,
                 WithinAbs(contiguous.snapshot().lufs_momentary, 1e-6f));
    REQUIRE_THAT(partitioned.snapshot().lufs_integrated,
                 WithinAbs(contiguous.snapshot().lufs_integrated, 1e-6f));
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
    (void) ballistics.update(data, 0.01f);

    REQUIRE(ballistics.channels[0].display_peak > 0.0f);
    REQUIRE(ballistics.channels[0].display_rms > 0.0f);
    REQUIRE_THAT(ballistics.channels[0].held_peak, WithinAbs(0.75f, 1e-6f));

    data.channels[0].peak = 0.0f;
    data.channels[0].rms = 0.0f;
    (void) ballistics.update(data, 1.0f);

    REQUIRE_THAT(ballistics.channels[0].display_peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(ballistics.channels[0].display_rms, WithinAbs(0.0f, 1e-6f));
    REQUIRE(ballistics.channels[0].held_peak < 1e-3f);
}

TEST_CASE("MultiChannelBallistics higher peak refreshes held peak", "[signal][meter][issue-645]") {
    MultiChannelBallistics ballistics;

    MultiChannelMeterData data;
    data.num_channels = 1;
    data.channels[0].peak = 0.5f;
    (void) ballistics.update(data, 0.01f);
    REQUIRE_THAT(ballistics.channels[0].held_peak, WithinAbs(0.5f, 1e-6f));

    data.channels[0].peak = 0.25f;
    (void) ballistics.update(data, 0.01f);
    REQUIRE_THAT(ballistics.channels[0].held_peak, WithinAbs(0.5f, 1e-6f));

    data.channels[0].peak = 0.8f;
    (void) ballistics.update(data, 0.01f);
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
    (void) ballistics.update(data, 0.01f);

    REQUIRE(ballistics.num_channels == kMaxMeterChannels);
    REQUIRE(ballistics.channels[0].clip_indicator);
    REQUIRE_THAT(ballistics.channels[0].clip_hold_counter, WithinAbs(0.5f, 1e-6f));

    data.channels[0].clipped = false;
    (void) ballistics.update(data, 0.25f);
    REQUIRE(ballistics.channels[0].clip_indicator);

    (void) ballistics.update(data, 0.30f);
    REQUIRE_FALSE(ballistics.channels[0].clip_indicator);

    data.channels[0].clipped = true;
    (void) ballistics.update(data, 0.01f);
    REQUIRE(ballistics.channels[0].clip_indicator);

    ballistics.clear_clips();
    REQUIRE_FALSE(ballistics.channels[0].clip_indicator);
    REQUIRE_THAT(ballistics.channels[0].clip_hold_counter, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("MultiChannelMeter treats non-finite samples as silence (no NaN poisoning)",
          "[signal][meter][issue-2695]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, 1);  // ~1-sample blocks so snapshots emit quickly

    const float nan_s = std::numeric_limits<float>::quiet_NaN();
    const float inf_s = std::numeric_limits<float>::infinity();
    float nan_buf[] = {nan_s};
    float inf_buf[] = {inf_s};
    float good[]    = {0.5f};
    const float* ch_nan[]  = {nan_buf};
    const float* ch_inf[]  = {inf_buf};
    const float* ch_good[] = {good};

    // A NaN/Inf sample must not irrecoverably poison the RMS/LUFS/integrated
    // accumulators (#2695): subsequent valid audio must still read finite.
    meter.process(ch_nan, 1, 1);
    meter.process(ch_inf, 1, 1);
    for (int i = 0; i < 64; ++i) meter.process(ch_good, 1, 1);

    const auto& snap = meter.snapshot();
    REQUIRE(std::isfinite(snap.channels[0].peak));
    REQUIRE(std::isfinite(snap.channels[0].rms));
    REQUIRE_FALSE(std::isnan(snap.lufs_integrated));
    REQUIRE_FALSE(std::isnan(snap.channels[0].lufs_momentary));
    // The valid 0.5 samples should register a real RMS, not 0/NaN.
    REQUIRE(snap.channels[0].rms > 0.0f);
}

TEST_CASE("MultiChannelMeter accumulates short blocks until the emit threshold",
          "[signal][meter]") {
    MultiChannelMeter meter;
    meter.prepare(1000.0f, 1);  // 10-sample snapshot block

    float first_half[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
    const float* first_channels[] = {first_half};
    meter.process(first_channels, 1, 5);

    REQUIRE_THAT(meter.snapshot().channels[0].peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(meter.snapshot().channels[0].rms, WithinAbs(0.0f, 1e-6f));
    REQUIRE_FALSE(meter.snapshot().channels[0].clipped);

    float second_half[] = {-0.25f, -0.5f, 0.75f, -1.25f, 0.0f};
    const float* second_channels[] = {second_half};
    meter.process(second_channels, 1, 5);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 1);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(1.25f, 1e-6f));
    REQUIRE(snap.channels[0].clipped);
    REQUIRE(snap.channels[0].rms > 0.5f);
    REQUIRE(std::isinf(snap.channels[0].lufs_momentary));
}

TEST_CASE("MultiChannelMeter first null channel clears stale stereo state",
          "[signal][meter]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, 2);

    float left[] = {0.5f};
    float right[] = {0.5f};
    const float* stereo[] = {left, right};
    meter.process(stereo, 2, 1);

    REQUIRE(meter.snapshot().num_channels == 2);
    REQUIRE_THAT(meter.snapshot().channels[0].peak, WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(meter.snapshot().channels[1].peak, WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(meter.snapshot().correlation, WithinAbs(1.0f, 1e-6f));

    const float* first_null[] = {nullptr, right};
    meter.process(first_null, 2, 1);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 0);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(snap.channels[1].peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_FALSE(snap.channels[0].clipped);
    REQUIRE_FALSE(snap.channels[1].clipped);
    REQUIRE_THAT(snap.correlation, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("MultiChannelMeter sanitizes non-finite stereo correlation samples",
          "[signal][meter]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, 2);

    float left[] = {
        std::numeric_limits<float>::quiet_NaN(),
        0.25f
    };
    float right[] = {
        std::numeric_limits<float>::infinity(),
        -0.25f
    };
    const float* channels[] = {left, right};
    meter.process(channels, 2, 2);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 2);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(snap.channels[1].peak, WithinAbs(0.25f, 1e-6f));
    REQUIRE_FALSE(snap.channels[0].clipped);
    REQUIRE_FALSE(snap.channels[1].clipped);
    REQUIRE(std::isfinite(snap.correlation));
    REQUIRE_THAT(snap.correlation, WithinAbs(-1.0f, 1e-6f));
    REQUIRE(std::isfinite(snap.channels[0].rms));
    REQUIRE(std::isfinite(snap.channels[1].rms));
}

TEST_CASE("MultiChannelMeter zero sample rate still emits finite one-sample snapshots",
          "[signal][meter]") {
    MultiChannelMeter meter;
    meter.prepare(0.0f, 1);

    float sample[] = {0.5f};
    const float* channels[] = {sample};
    meter.process(channels, 1, 1);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 1);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(snap.channels[0].rms, WithinAbs(0.5f, 1e-6f));
    REQUIRE(std::isinf(snap.channels[0].lufs_momentary));
    REQUIRE(std::isinf(snap.lufs_integrated));
}

TEST_CASE("MultiChannelBallistics releases peaks RMS and clip holds independently",
          "[signal][meter]") {
    MultiChannelBallistics ballistics;
    ballistics.attack_time = 0.01f;
    ballistics.release_time = 0.01f;
    ballistics.peak_hold_time = 0.02f;
    ballistics.clip_hold_time = 0.03f;

    MultiChannelMeterData data;
    data.num_channels = 2;
    data.channels[0].peak = 0.8f;
    data.channels[0].rms = 0.4f;
    data.channels[0].clipped = true;
    data.channels[1].peak = 0.2f;
    data.channels[1].rms = 0.1f;
    (void) ballistics.update(data, 0.01f);

    REQUIRE(ballistics.num_channels == 2);
    REQUIRE(ballistics.channels[0].display_peak > ballistics.channels[1].display_peak);
    REQUIRE(ballistics.channels[0].display_rms > ballistics.channels[1].display_rms);
    REQUIRE_THAT(ballistics.channels[0].held_peak, WithinAbs(0.8f, 1e-6f));
    REQUIRE_THAT(ballistics.channels[1].held_peak, WithinAbs(0.2f, 1e-6f));
    REQUIRE(ballistics.channels[0].clip_indicator);
    REQUIRE_FALSE(ballistics.channels[1].clip_indicator);

    data.channels[0].peak = 0.0f;
    data.channels[0].rms = 0.0f;
    data.channels[0].clipped = false;
    data.channels[1].peak = 0.0f;
    data.channels[1].rms = 0.0f;
    (void) ballistics.update(data, 0.2f);

    REQUIRE_FALSE(ballistics.channels[0].clip_indicator);
    REQUIRE(ballistics.channels[0].held_peak < 0.02f);
    REQUIRE(ballistics.channels[1].held_peak < 0.02f);
    REQUIRE_THAT(ballistics.channels[0].display_peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(ballistics.channels[1].display_rms, WithinAbs(0.0f, 1e-6f));

    data.num_channels = -4;
    (void) ballistics.update(data, 0.01f);
    REQUIRE(ballistics.num_channels == 0);
}

// A non-finite sample must not disable the multi-channel idle gate forever, for
// the same reason as its single-channel sibling: NaN survives the noise-floor
// snaps and compares unequal to itself, so one bad sample would make the
// ballistics report movement on every frame for the rest of the process.
TEST_CASE("MultiChannelBallistics survives a non-finite sample",
          "[signal][meter][idle-gate]") {
    MultiChannelBallistics b;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    MultiChannelMeterData bad;
    bad.num_channels = 2;
    for (int ch = 0; ch < 2; ++ch) { bad.channels[ch].peak = nan; bad.channels[ch].rms = nan; }
    (void) b.update(bad, 0.016f);
    CHECK(std::isfinite(b.channels[0].display_peak));
    CHECK(std::isfinite(b.channels[0].display_rms));

    MultiChannelMeterData silent;
    silent.num_channels = 2;
    for (int i = 0; i < 600; ++i) (void) b.update(silent, 0.016f);
    CHECK(b.channels[0].display_peak == 0.0f);
    CHECK(b.channels[0].held_peak == 0.0f);
    for (int i = 0; i < 120; ++i)
        CHECK_FALSE(b.update(silent, 0.016f));
}
