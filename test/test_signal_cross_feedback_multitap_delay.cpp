#include <pulp/signal/cross_feedback_multitap_delay.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

using Catch::Approx;
using pulp::signal::CrossFeedbackMultitapDelay;
using pulp::signal::CrossFeedbackMultitapDelay64;

namespace {

constexpr double kSampleRate = 48000.0;
using Delay = CrossFeedbackMultitapDelay64;
using Tap = Delay::Tap;

Tap make_tap(double delay_samples, double level = 1.0, double feedback_weight = 1.0,
             double pan = 0.0, double width = 1.0) {
    return {.delay_ms = delay_samples * 1000.0 / kSampleRate,
            .level = level,
            .pan = pan,
            .stereo_width = width,
            .feedback_weight = feedback_weight};
}

} // namespace

TEST_CASE("cross-feedback multitap delay validates preparation and tap bounds",
          "[signal][multitap-delay][contract]") {
    Delay delay;
    REQUIRE_FALSE(delay.prepared());
    REQUIRE(delay.tail_samples() == 0);
    REQUIRE(delay.set_tap(0, {.delay_ms = 150.0}));
    REQUIRE_FALSE(delay.prepare(999.0, 100.0));
    REQUIRE_FALSE(delay.prepare(kSampleRate, 0.0));
    REQUIRE_FALSE(delay.prepare(kSampleRate, 0.01));
    REQUIRE(delay.prepare(kSampleRate, 200.0));
    REQUIRE(delay.prepared());
    REQUIRE(delay.sample_rate() == kSampleRate);
    REQUIRE(delay.maximum_delay_ms() == 200.0);
    REQUIRE(delay.tap(0).delay_ms == 150.0);
    REQUIRE(delay.retained_bytes() > 0);
    REQUIRE(delay.active_tap_count() == 1);
    REQUIRE(delay.latency_samples() == 0);
    REQUIRE(delay.tail_samples() == 7200);

    REQUIRE(delay.set_tap(0, {.delay_ms = 50.0, .level = 0.0, .feedback_weight = 1.0}));
    delay.set_feedback_gain(0.8);
    REQUIRE(delay.tail_samples() == 0);

    delay.set_active_tap_count(100);
    REQUIRE(delay.active_tap_count() == Delay::kMaxTaps);
    Tap clamped{.delay_ms = 200.0,
                .level = 2.0,
                .pan = -2.0,
                .stereo_width = 2.0,
                .feedback_weight = -2.0};
    REQUIRE(delay.set_tap(0, clamped));
    const Tap stored = delay.tap(0);
    REQUIRE(stored.delay_ms == 200.0);
    REQUIRE(stored.level == 1.0);
    REQUIRE(stored.pan == -1.0);
    REQUIRE(stored.stereo_width == 1.0);
    REQUIRE(stored.feedback_weight == -1.0);
    REQUIRE_FALSE(delay.set_tap(Delay::kMaxTaps, Tap{}));
    clamped.delay_ms = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(delay.set_tap(0, clamped));

    delay.set_feedback_gain(2.0);
    delay.set_cross_feedback(-1.0);
    REQUIRE(delay.feedback_gain() == Delay::kMaximumFeedbackGain);
    REQUIRE(delay.cross_feedback() == 0.0);
    REQUIRE(delay.worst_case_feedback_gain() == Delay::kMaximumFeedbackGain);
    REQUIRE(delay.tail_samples() == -1);
}

TEST_CASE("multitap impulse positions integer and fractional taps independently",
          "[signal][multitap-delay][impulse][fractional]") {
    Delay delay;
    REQUIRE(delay.prepare(kSampleRate, 100.0));
    delay.set_active_tap_count(3);
    REQUIRE(delay.set_tap(0, make_tap(4.0, 1.0, 0.0)));
    REQUIRE(delay.set_tap(1, make_tap(8.5, 0.5, 0.0)));
    REQUIRE(delay.set_tap(2, make_tap(12.0, -0.25, 0.0)));

    for (int sample = 0; sample < 16; ++sample) {
        double output_left = 0.0;
        double output_right = 0.0;
        delay.process_sample(sample == 0 ? 1.0 : 0.0, 0.0, output_left, output_right);
        double expected = 0.0;
        if (sample == 4)
            expected += 1.0;
        if (sample == 8 || sample == 9)
            expected += 0.25;
        if (sample == 12)
            expected -= 0.25;
        REQUIRE(output_left == Approx(expected).margin(1.0e-14));
        REQUIRE(std::abs(output_right) < 1.0e-14);
    }

    SECTION("exact minimum capacity remains a one-sample delay after rounding") {
        constexpr double awkward_rate = 1062.0;
        Delay minimum;
        REQUIRE(minimum.prepare(awkward_rate, 1000.0 / awkward_rate));
        REQUIRE(minimum.set_tap(0, {.delay_ms = 1000.0 / awkward_rate}));
        double left = 0.0;
        double right = 0.0;
        minimum.process_sample(1.0, 0.0, left, right);
        REQUIRE(left == 0.0);
        minimum.process_sample(0.0, 0.0, left, right);
        REQUIRE(left == Approx(1.0));
        REQUIRE(right == 0.0);
    }

    SECTION("sample-exact milliseconds do not over-report tail after rounding") {
        constexpr double standard_rate = 44100.0;
        Delay exact;
        REQUIRE(exact.prepare(standard_rate, 100.0));
        REQUIRE(exact.set_tap(0, {.delay_ms = 3000.0 / standard_rate}));
        REQUIRE(exact.tail_samples() == 3);
        for (int sample = 0; sample < 5; ++sample) {
            double left = 0.0;
            double right = 0.0;
            exact.process_sample(sample == 0 ? 1.0 : 0.0, 0.0, left, right);
            REQUIRE(left == Approx(sample == 3 ? 1.0 : 0.0).margin(1.0e-14));
            REQUIRE(right == 0.0);
        }
    }
}

TEST_CASE("tap pan and width place each delayed source with equal-power gains",
          "[signal][multitap-delay][pan]") {
    Delay delay;
    REQUIRE(delay.prepare(kSampleRate, 100.0));
    delay.set_active_tap_count(1);
    REQUIRE(delay.set_tap(0, make_tap(2.0, 1.0, 0.0, 1.0, 0.0)));

    for (int sample = 0; sample < 4; ++sample) {
        double output_left = 0.0;
        double output_right = 0.0;
        delay.process_sample(sample == 0 ? 1.0 : 0.0, 0.0, output_left, output_right);
        REQUIRE(std::abs(output_left) < 1.0e-14);
        REQUIRE(output_right == Approx(sample == 2 ? 1.0 : 0.0).margin(1.0e-14));
    }
}

TEST_CASE("pure cross feedback alternates echoes and rejects a self-feedback control",
          "[signal][multitap-delay][cross-feedback][negative-control]") {
    auto render = [](double cross) {
        Delay delay;
        REQUIRE(delay.prepare(kSampleRate, 100.0));
        delay.set_active_tap_count(1);
        REQUIRE(delay.set_tap(0, make_tap(4.0)));
        delay.set_feedback_gain(0.5);
        delay.set_cross_feedback(cross);
        std::array<double, 17> left{};
        std::array<double, 17> right{};
        for (std::size_t sample = 0; sample < left.size(); ++sample)
            delay.process_sample(sample == 0 ? 1.0 : 0.0, 0.0, left[sample], right[sample]);
        return std::array{left, right};
    };

    const auto cross = render(1.0);
    const auto self = render(0.0);
    const auto alternates = [](const auto& channels) {
        const auto& left = channels[0];
        const auto& right = channels[1];
        return left[4] == Approx(1.0) && right[4] == 0.0 && right[8] == Approx(0.5) &&
               left[8] == 0.0 && left[12] == Approx(0.25) && right[12] == 0.0 &&
               right[16] == Approx(0.125) && left[16] == 0.0;
    };
    REQUIRE(alternates(cross));
    REQUIRE_FALSE(alternates(self));
    REQUIRE(self[0][8] == Approx(0.5));
    REQUIRE(self[1][8] == 0.0);
}

TEST_CASE("normalized tap feedback remains geometric while an unnormalized mutation grows",
          "[signal][multitap-delay][stability][negative-control]") {
    Delay delay;
    REQUIRE(delay.prepare(kSampleRate, 100.0));
    delay.set_active_tap_count(2);
    REQUIRE(delay.set_tap(0, make_tap(4.0, 0.5, 1.0)));
    REQUIRE(delay.set_tap(1, make_tap(4.0, 0.5, 1.0)));
    delay.set_feedback_gain(0.9);
    delay.set_cross_feedback(0.0);

    std::array<double, 65> output{};
    for (std::size_t sample = 0; sample < output.size(); ++sample) {
        double right = 0.0;
        delay.process_sample(sample == 0 ? 1.0 : 0.0, 0.0, output[sample], right);
        REQUIRE(std::isfinite(output[sample]));
        REQUIRE(right == 0.0);
    }
    for (int echo = 1; echo <= 15; ++echo)
        REQUIRE(output[static_cast<std::size_t>(echo * 4)] ==
                Approx(std::pow(0.9, echo - 1)).epsilon(1.0e-12));

    const auto passes_decay_gate = [](double ratio) { return std::abs(ratio) < 1.0; };
    const double normalized_ratio = output[8] / output[4];
    const double unnormalized_two_tap_mutation = 2.0 * 0.9;
    REQUIRE(passes_decay_gate(normalized_ratio));
    REQUIRE_FALSE(passes_decay_gate(unnormalized_two_tap_mutation));
}

TEST_CASE("multitap block processing is partition and same-channel in-place invariant",
          "[signal][multitap-delay][partition]") {
    constexpr std::size_t sample_count = 1001;
    std::array<double, sample_count> input_left{};
    std::array<double, sample_count> input_right{};
    for (std::size_t i = 0; i < sample_count; ++i) {
        input_left[i] = 0.6 * std::sin(0.037 * static_cast<double>(i));
        input_right[i] = 0.4 * std::cos(0.023 * static_cast<double>(i));
    }

    Delay whole;
    Delay partitioned;
    REQUIRE(whole.prepare(kSampleRate, 100.0));
    REQUIRE(partitioned.prepare(kSampleRate, 100.0));
    for (Delay* delay : {&whole, &partitioned}) {
        delay->set_active_tap_count(3);
        REQUIRE(delay->set_tap(0, make_tap(7.25, 0.7, 1.0)));
        REQUIRE(delay->set_tap(1, make_tap(19.5, -0.2, -0.3, -0.4, 0.5)));
        REQUIRE(delay->set_tap(2, make_tap(41.0, 0.4, 0.2, 0.6, 0.2)));
        delay->set_feedback_gain(0.7);
        delay->set_cross_feedback(0.8);
    }

    std::array<double, sample_count> whole_left{};
    std::array<double, sample_count> whole_right{};
    std::array<double, sample_count> split_left = input_left;
    std::array<double, sample_count> split_right = input_right;
    whole.process_block(input_left.data(), input_right.data(), whole_left.data(),
                        whole_right.data(), sample_count);

    std::size_t offset = 0;
    for (std::size_t block : {1u, 64u, 7u, 255u, 3u, 511u, 160u}) {
        const std::size_t count = std::min(block, sample_count - offset);
        partitioned.process_block(split_left.data() + offset, split_right.data() + offset,
                                  split_left.data() + offset, split_right.data() + offset, count);
        offset += count;
        if (offset == sample_count)
            break;
    }
    if (offset < sample_count)
        partitioned.process_block(split_left.data() + offset, split_right.data() + offset,
                                  split_left.data() + offset, split_right.data() + offset,
                                  sample_count - offset);

    REQUIRE(split_left == whole_left);
    REQUIRE(split_right == whole_right);
}

TEST_CASE("reset, non-finite recovery, and null blocks fail silent",
          "[signal][multitap-delay][reset][fault]") {
    Delay delay;
    double left = 1.0;
    double right = 1.0;
    delay.process_sample(1.0, 1.0, left, right);
    REQUIRE(left == 0.0);
    REQUIRE(right == 0.0);

    REQUIRE(delay.prepare(kSampleRate, 100.0));
    delay.set_active_tap_count(1);
    REQUIRE(delay.set_tap(0, make_tap(2.0)));
    delay.process_sample(1.0, 0.0, left, right);
    delay.process_sample(0.0, 0.0, left, right);
    delay.reset();
    for (int sample = 0; sample < 8; ++sample) {
        delay.process_sample(0.0, 0.0, left, right);
        REQUIRE(left == 0.0);
        REQUIRE(right == 0.0);
    }

    delay.process_sample(std::numeric_limits<double>::infinity(), 0.0, left, right);
    REQUIRE(left == 0.0);
    REQUIRE(right == 0.0);
    delay.process_sample(0.0, 0.0, left, right);
    REQUIRE(left == 0.0);
    REQUIRE(right == 0.0);

    std::array<double, 2> buffer{};
    delay.process_block(nullptr, buffer.data(), buffer.data(), buffer.data(), 1);
    delay.process_block(buffer.data(), nullptr, buffer.data(), buffer.data(), 1);
    delay.process_block(buffer.data(), buffer.data(), nullptr, buffer.data(), 1);
    delay.process_block(buffer.data(), buffer.data(), buffer.data(), nullptr, 1);
}

TEST_CASE("cross-feedback multitap realtime paths allocate no memory",
          "[signal][multitap-delay][rt-safety]") {
    CrossFeedbackMultitapDelay delay;
    REQUIRE(delay.prepare(kSampleRate, 100.0));
    delay.set_active_tap_count(3);
    REQUIRE(delay.set_tap(0, {.delay_ms = 2.5, .level = 0.7, .feedback_weight = 1.0}));
    REQUIRE(delay.set_tap(1, {.delay_ms = 7.25,
                              .level = -0.2,
                              .pan = -0.5,
                              .stereo_width = 0.5,
                              .feedback_weight = 0.4}));
    REQUIRE(delay.set_tap(2, {.delay_ms = 19.0,
                              .level = 0.3,
                              .pan = 0.6,
                              .stereo_width = 0.2,
                              .feedback_weight = -0.2}));
    delay.set_feedback_gain(0.8);
    std::array<float, 257> left{};
    std::array<float, 257> right{};

    pulp::test::RtAllocationProbe probe;
    delay.process_block(left.data(), right.data(), left.data(), right.data(), left.size());
    delay.reset();
    REQUIRE_FALSE(probe.saw_allocation());
}
