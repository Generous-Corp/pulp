#include <pulp/signal/early_reflections.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>

using Catch::Approx;
using pulp::signal::EarlyReflections;
using pulp::signal::EarlyReflections64;
using pulp::signal::MatrixHeadroomPolicy;

namespace {

constexpr double kSampleRate = 48000.0;
using Reflections = EarlyReflections64;
using Tap = Reflections::Tap;

Tap samples(double delay, double gain = 1.0, double pan = 0.0, double width = 1.0) {
    return {
        .delay_ms = delay * 1000.0 / kSampleRate, .gain = gain, .pan = pan, .stereo_width = width};
}

struct StereoRender {
    std::array<double, 40> left{};
    std::array<double, 40> right{};
};

/// Closed-form sparse-convolution oracle. It deliberately does not call the
/// production panner, delay line, or renderer.
StereoRender oracle(const std::array<Tap, 3>& taps, bool swap_routes = false,
                    int timing_offset = 0) {
    StereoRender result;
    const auto add = [&](std::array<double, 40>& channel, double time, double value) {
        const auto younger = static_cast<int>(std::floor(time)) + timing_offset;
        const double fraction = time - std::floor(time);
        if (younger >= 0 && younger < static_cast<int>(channel.size()))
            channel[static_cast<std::size_t>(younger)] += value * (1.0 - fraction);
        if (fraction != 0.0 && younger + 1 >= 0 && younger + 1 < static_cast<int>(channel.size()))
            channel[static_cast<std::size_t>(younger + 1)] += value * fraction;
    };

    for (const auto& tap : taps) {
        const double delay = tap.delay_ms * kSampleRate / 1000.0;
        const auto gains = [](double position) {
            const double angle = (std::clamp(position, -1.0, 1.0) + 1.0) * std::numbers::pi / 4.0;
            return std::array{std::cos(angle), std::sin(angle)};
        };
        const auto from_left = gains(tap.pan - tap.stereo_width);
        const auto from_right = gains(tap.pan + tap.stereo_width);

        // The independent stimulus is L=+1 at frame zero, R=-0.5 at frame two.
        auto& left_target = swap_routes ? result.right : result.left;
        auto& right_target = swap_routes ? result.left : result.right;
        add(left_target, delay, tap.gain * from_left[0]);
        add(right_target, delay, tap.gain * from_left[1]);
        add(left_target, delay + 2.0, -0.5 * tap.gain * from_right[0]);
        add(right_target, delay + 2.0, -0.5 * tap.gain * from_right[1]);
    }
    return result;
}

bool matches(const StereoRender& actual, const StereoRender& expected, double tolerance = 1.0e-12) {
    for (std::size_t i = 0; i < actual.left.size(); ++i)
        if (std::abs(actual.left[i] - expected.left[i]) > tolerance ||
            std::abs(actual.right[i] - expected.right[i]) > tolerance)
            return false;
    return true;
}

} // namespace

TEST_CASE("early reflections prepare and whole-pattern configuration are transactional",
          "[signal][early-reflections][contract]") {
    Reflections reflections;
    const std::array initial{samples(4.0, 0.75), samples(12.5, -0.25, 0.3, 0.4)};
    REQUIRE(reflections.configure(initial));
    REQUIRE(reflections.tap_count() == initial.size());
    REQUIRE_FALSE(reflections.prepare(999.0, 100.0));
    REQUIRE_FALSE(reflections.prepare(kSampleRate, samples(3.0).delay_ms));
    REQUIRE(reflections.prepare(kSampleRate, 100.0));
    REQUIRE(reflections.prepared());
    REQUIRE(reflections.sample_rate() == kSampleRate);
    REQUIRE(reflections.maximum_delay_ms() == 100.0);
    REQUIRE(reflections.latency_samples() == 0);
    REQUIRE(reflections.tail_samples() == 13);
    REQUIRE(reflections.retained_bytes() > 0);

    double left = 0.0;
    double right = 0.0;
    reflections.process_sample(1.0, 0.0, left, right);

    auto rejected = initial;
    rejected[0].gain = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(reflections.configure(rejected));
    REQUIRE(reflections.tap_count() == initial.size());
    REQUIRE(reflections.tap(0).gain == initial[0].gain);

    rejected = initial;
    rejected[0].delay_ms = 101.0;
    REQUIRE_FALSE(reflections.configure(rejected));
    REQUIRE(reflections.tap(0).delay_ms == initial[0].delay_ms);
    REQUIRE_FALSE(reflections.prepare(44100.0, 0.05));
    REQUIRE(reflections.sample_rate() == kSampleRate);
    REQUIRE(reflections.maximum_delay_ms() == 100.0);
    for (int frame = 1; frame <= 4; ++frame) {
        reflections.process_sample(0.0, 0.0, left, right);
        REQUIRE(left == Approx(frame == 4 ? 0.75 : 0.0).margin(1.0e-14));
        REQUIRE(right == 0.0);
    }

    std::array<Tap, Reflections::kMaxTaps + 1> too_many{};
    REQUIRE_FALSE(reflections.configure(too_many));
    REQUIRE(reflections.tap_count() == initial.size());

    REQUIRE(reflections.configure(std::span<const Tap>{}));
    REQUIRE(reflections.tap_count() == 0);
    REQUIRE(reflections.tail_samples() == 0);
}

TEST_CASE("early-reflection impulse timing pan and true-stereo routing match an independent oracle",
          "[signal][early-reflections][impulse][fractional][stereo][negative-control]") {
    const std::array taps{samples(5.0, 0.6, -0.4, 0.6), samples(11.5, -0.3, 0.65, 0.25),
                          samples(17.25, 0.2, 0.0, 1.0)};
    Reflections reflections;
    REQUIRE(reflections.configure(taps, MatrixHeadroomPolicy::Raw));
    REQUIRE(reflections.prepare(kSampleRate, 100.0));

    StereoRender actual;
    for (std::size_t frame = 0; frame < actual.left.size(); ++frame) {
        const double left = frame == 0 ? 1.0 : 0.0;
        const double right = frame == 2 ? -0.5 : 0.0;
        reflections.process_sample(left, right, actual.left[frame], actual.right[frame]);
    }

    const auto expected = oracle(taps);
    for (std::size_t frame = 0; frame < actual.left.size(); ++frame) {
        CAPTURE(frame, actual.left[frame], expected.left[frame], actual.right[frame],
                expected.right[frame]);
        REQUIRE(actual.left[frame] == Approx(expected.left[frame]).margin(1.0e-12));
        REQUIRE(actual.right[frame] == Approx(expected.right[frame]).margin(1.0e-12));
    }
    // Planted controls prove the oracle gate detects both classic failure
    // classes instead of passing any sparse delayed output.
    REQUIRE_FALSE(matches(actual, oracle(taps, true, 0)));
    REQUIRE_FALSE(matches(actual, oracle(taps, false, 1)));
}

TEST_CASE("peak-normalized reflection routing contains correlated energy while raw is explicit",
          "[signal][early-reflections][headroom][energy]") {
    const std::array taps{samples(4.0, 1.0, 0.0, 0.0), samples(4.0, 1.0, 0.0, 0.0),
                          samples(4.0, 1.0, 0.0, 0.0)};
    auto render = [&](MatrixHeadroomPolicy policy) {
        Reflections reflections;
        REQUIRE(reflections.configure(taps, policy));
        REQUIRE(reflections.prepare(kSampleRate, 20.0));
        std::array<double, 7> left{};
        std::array<double, 7> right{};
        for (std::size_t frame = 0; frame < left.size(); ++frame)
            reflections.process_sample(frame == 0 ? 1.0 : 0.0, frame == 0 ? 1.0 : 0.0, left[frame],
                                       right[frame]);
        return std::array{left[4], right[4]};
    };

    const auto normalized = render(MatrixHeadroomPolicy::NormalizePeak);
    const auto raw = render(MatrixHeadroomPolicy::Raw);
    REQUIRE(normalized[0] == Approx(1.0).margin(1.0e-14));
    REQUIRE(normalized[1] == Approx(1.0).margin(1.0e-14));
    REQUIRE(normalized[0] * normalized[0] + normalized[1] * normalized[1] ==
            Approx(2.0).margin(1.0e-13));
    REQUIRE(raw[0] > 4.0);
    REQUIRE(raw[1] > 4.0);
}

TEST_CASE("early reflections are block-partition and same-channel in-place invariant",
          "[signal][early-reflections][partition][in-place]") {
    constexpr std::size_t kFrames = 1001;
    std::array<double, kFrames> input_left{};
    std::array<double, kFrames> input_right{};
    for (std::size_t i = 0; i < kFrames; ++i) {
        input_left[i] = 0.6 * std::sin(0.037 * static_cast<double>(i));
        input_right[i] = 0.4 * std::cos(0.023 * static_cast<double>(i));
    }
    const std::array taps{samples(7.25, 0.7), samples(19.5, -0.2, -0.4, 0.5),
                          samples(41.0, 0.4, 0.6, 0.2)};

    Reflections whole;
    Reflections partitioned;
    for (auto* renderer : {&whole, &partitioned}) {
        REQUIRE(renderer->configure(taps));
        REQUIRE(renderer->prepare(kSampleRate, 100.0));
    }

    std::array<double, kFrames> expected_left{};
    std::array<double, kFrames> expected_right{};
    auto in_place_left = input_left;
    auto in_place_right = input_right;
    whole.process_block(input_left.data(), input_right.data(), expected_left.data(),
                        expected_right.data(), kFrames);

    std::size_t offset = 0;
    for (const std::size_t requested : {1u, 64u, 7u, 255u, 3u, 511u, 160u}) {
        const auto count = std::min(requested, kFrames - offset);
        partitioned.process_block(in_place_left.data() + offset, in_place_right.data() + offset,
                                  in_place_left.data() + offset, in_place_right.data() + offset,
                                  count);
        offset += count;
        if (offset == kFrames)
            break;
    }
    if (offset < kFrames)
        partitioned.process_block(in_place_left.data() + offset, in_place_right.data() + offset,
                                  in_place_left.data() + offset, in_place_right.data() + offset,
                                  kFrames - offset);

    REQUIRE(in_place_left == expected_left);
    REQUIRE(in_place_right == expected_right);
}

TEST_CASE("early reflections reset and recover from non-finite input",
          "[signal][early-reflections][reset][fault]") {
    Reflections reflections;
    const std::array taps{samples(3.0)};
    REQUIRE(reflections.configure(taps));

    double left = 1.0;
    double right = 1.0;
    reflections.process_sample(1.0, 1.0, left, right);
    REQUIRE(left == 0.0);
    REQUIRE(right == 0.0);

    REQUIRE(reflections.prepare(kSampleRate, 20.0));
    reflections.process_sample(1.0, 0.0, left, right);
    reflections.process_sample(0.0, 0.0, left, right);
    reflections.process_sample(std::numeric_limits<double>::infinity(), 0.0, left, right);
    REQUIRE(left == 0.0);
    REQUIRE(right == 0.0);
    for (int i = 0; i < 5; ++i) {
        reflections.process_sample(0.0, 0.0, left, right);
        REQUIRE(left == 0.0);
        REQUIRE(right == 0.0);
    }

    reflections.process_sample(1.0, 0.0, left, right);
    reflections.reset();
    for (int i = 0; i < 5; ++i) {
        reflections.process_sample(0.0, 0.0, left, right);
        REQUIRE(left == 0.0);
        REQUIRE(right == 0.0);
    }

    std::array<double, 2> buffer{};
    reflections.process_block(nullptr, buffer.data(), buffer.data(), buffer.data(), 1);
    reflections.process_block(buffer.data(), nullptr, buffer.data(), buffer.data(), 1);
    reflections.process_block(buffer.data(), buffer.data(), nullptr, buffer.data(), 1);
    reflections.process_block(buffer.data(), buffer.data(), buffer.data(), nullptr, 1);
}

TEST_CASE("early-reflection realtime paths allocate no memory",
          "[signal][early-reflections][rt-safety]") {
    EarlyReflections reflections;
    const std::array taps{
        EarlyReflections::Tap{.delay_ms = 2.5, .gain = 0.7},
        EarlyReflections::Tap{.delay_ms = 7.25, .gain = -0.2, .pan = -0.5, .stereo_width = 0.5},
        EarlyReflections::Tap{.delay_ms = 19.0, .gain = 0.3, .pan = 0.6, .stereo_width = 0.2}};
    REQUIRE(reflections.configure(taps));
    REQUIRE(reflections.prepare(kSampleRate, 100.0));
    std::array<float, 257> left{};
    std::array<float, 257> right{};

    pulp::test::RtAllocationProbe probe;
    reflections.process_block(left.data(), right.data(), left.data(), right.data(), left.size());
    reflections.reset();
    REQUIRE_FALSE(probe.saw_allocation());
}
