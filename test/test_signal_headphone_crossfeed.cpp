#include <pulp/signal/headphone_crossfeed.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

using Catch::Approx;
using pulp::signal::HeadphoneCrossfeed;
using pulp::signal::HeadphoneCrossfeed64;

namespace {

constexpr double kSampleRate = 48000.0;

std::complex<double> response_at(const std::vector<double>& impulse, double frequency_hz) {
    std::complex<double> response{};
    const double omega = 2.0 * std::numbers::pi * frequency_hz / kSampleRate;
    for (std::size_t i = 0; i < impulse.size(); ++i)
        response += impulse[i] * std::polar(1.0, -omega * static_cast<double>(i));
    return response;
}

} // namespace

TEST_CASE("headphone crossfeed parameters are finite and bounded", "[signal][crossfeed]") {
    HeadphoneCrossfeed64 crossfeed;
    REQUIRE(crossfeed.prepare(kSampleRate));
    REQUIRE(crossfeed.sample_rate() == kSampleRate);
    REQUIRE(crossfeed.amount() == 0.5);
    REQUIRE(crossfeed.delay_ms() == 0.25);
    REQUIRE(crossfeed.delay_samples() == 12.0);
    REQUIRE(crossfeed.cutoff_hz() == 700.0);
    REQUIRE(crossfeed.latency_samples() == 0);
    REQUIRE(crossfeed.tail_samples() == -1);

    crossfeed.set_amount(-1.0);
    crossfeed.set_delay_ms(2.0);
    crossfeed.set_cutoff_hz(50000.0);
    REQUIRE(crossfeed.amount() == 0.0);
    REQUIRE(crossfeed.delay_ms() == 1.0);
    REQUIRE(crossfeed.delay_samples() == 48.0);
    REQUIRE(crossfeed.cutoff_hz() == 20000.0);
    REQUIRE(crossfeed.tail_samples() == 0);

    crossfeed.set_amount(std::numeric_limits<double>::quiet_NaN());
    crossfeed.set_delay_ms(std::numeric_limits<double>::infinity());
    crossfeed.set_cutoff_hz(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(crossfeed.amount() == 0.0);
    REQUIRE(crossfeed.delay_ms() == 1.0);
    REQUIRE(crossfeed.cutoff_hz() == 20000.0);
    REQUIRE_FALSE(crossfeed.prepare(999.0));
    REQUIRE(crossfeed.sample_rate() == kSampleRate);
}

TEST_CASE("headphone crossfeed impulse follows the stated feed-forward topology",
          "[signal][crossfeed][impulse]") {
    HeadphoneCrossfeed64 crossfeed;
    REQUIRE(crossfeed.prepare(kSampleRate));
    crossfeed.set_amount(1.0);
    crossfeed.set_delay_ms(0.25);
    crossfeed.set_cutoff_hz(700.0);

    const double gain = HeadphoneCrossfeed64::kMaximumCrossfeedGain;
    const double direct = 1.0 / (1.0 + gain);
    const double cross = gain / (1.0 + gain);
    const double pole = std::exp(-2.0 * std::numbers::pi * 700.0 / kSampleRate);

    for (int sample = 0; sample < 16; ++sample) {
        double left = sample == 0 ? 1.0 : 0.0;
        double right = 0.0;
        double output_left = 0.0;
        double output_right = 0.0;
        crossfeed.process_sample(left, right, output_left, output_right);
        REQUIRE(output_left == Approx(sample == 0 ? direct : 0.0).margin(1.0e-15));
        if (sample < 12)
            REQUIRE(output_right == 0.0);
        else
            REQUIRE(output_right == Approx(cross * (1.0 - pole) * std::pow(pole, sample - 12))
                                        .margin(1.0e-15));
    }
}

TEST_CASE("headphone crossfeed fractional delay has the stated two-tap orientation",
          "[signal][crossfeed][impulse][fractional-delay]") {
    HeadphoneCrossfeed64 crossfeed;
    REQUIRE(crossfeed.prepare(kSampleRate));
    crossfeed.set_amount(1.0);
    crossfeed.set_delay_ms(12.25 / 48.0);
    crossfeed.set_cutoff_hz(700.0);
    REQUIRE(crossfeed.delay_samples() == Approx(12.25).margin(1.0e-14));

    const double gain = HeadphoneCrossfeed64::kMaximumCrossfeedGain;
    const double cross = gain / (1.0 + gain);
    const double pole = std::exp(-2.0 * std::numbers::pi * 700.0 / kSampleRate);
    double previous_filter_output = 0.0;

    for (int sample = 0; sample < 16; ++sample) {
        double output_left = 0.0;
        double output_right = 0.0;
        crossfeed.process_sample(sample == 0 ? 1.0 : 0.0, 0.0, output_left, output_right);
        const double delayed_input = sample == 12 ? 0.75 : sample == 13 ? 0.25 : 0.0;
        const double expected_filter_output =
            pole * previous_filter_output + (1.0 - pole) * delayed_input;
        REQUIRE(output_right == Approx(cross * expected_filter_output).margin(1.0e-15));
        previous_filter_output = expected_filter_output;
    }
}

TEST_CASE("headphone crossfeed rejects bright crosstalk unlike the flat negative control",
          "[signal][crossfeed][frequency][negative-control]") {
    HeadphoneCrossfeed64 crossfeed;
    REQUIRE(crossfeed.prepare(kSampleRate));
    crossfeed.set_amount(1.0);
    crossfeed.set_delay_ms(0.25);
    crossfeed.set_cutoff_hz(700.0);

    constexpr std::size_t sample_count = 8192;
    std::vector<double> left(sample_count, 0.0);
    std::vector<double> right(sample_count, 0.0);
    for (std::size_t i = 0; i < sample_count; ++i) {
        const double input_left = i == 0 ? 1.0 : 0.0;
        crossfeed.process_sample(input_left, 0.0, left[i], right[i]);
    }

    const double low_ratio =
        std::abs(response_at(right, 100.0)) / std::abs(response_at(left, 100.0));
    const double high_ratio =
        std::abs(response_at(right, 8000.0)) / std::abs(response_at(left, 8000.0));
    const double flat_negative_control = HeadphoneCrossfeed64::kMaximumCrossfeedGain;

    const auto expected_ratio = [](double frequency_hz) {
        const double pole = std::exp(-2.0 * std::numbers::pi * 700.0 / kSampleRate);
        const double omega = 2.0 * std::numbers::pi * frequency_hz / kSampleRate;
        const double lowpass =
            (1.0 - pole) / std::abs(1.0 - pole * std::polar(1.0, -omega));
        return HeadphoneCrossfeed64::kMaximumCrossfeedGain * lowpass;
    };

    const auto passes_crosstalk_gate = [flat_negative_control](double low, double high) {
        return low > flat_negative_control * 0.95 && high < flat_negative_control * 0.1 &&
               high < low * 0.1;
    };

    REQUIRE(low_ratio == Approx(expected_ratio(100.0)).epsilon(1.0e-10));
    REQUIRE(high_ratio == Approx(expected_ratio(8000.0)).epsilon(1.0e-10));
    REQUIRE(passes_crosstalk_gate(low_ratio, high_ratio));

    std::vector<double> flat_left(sample_count, 0.0);
    std::vector<double> flat_right(sample_count, 0.0);
    const double direct_weight = 1.0 / (1.0 + flat_negative_control);
    const double cross_weight = flat_negative_control / (1.0 + flat_negative_control);
    flat_left[0] = direct_weight;
    flat_right[12] = cross_weight;
    const double flat_low_ratio =
        std::abs(response_at(flat_right, 100.0)) / std::abs(response_at(flat_left, 100.0));
    const double flat_high_ratio =
        std::abs(response_at(flat_right, 8000.0)) / std::abs(response_at(flat_left, 8000.0));
    REQUIRE(flat_low_ratio == Approx(flat_negative_control).epsilon(1.0e-12));
    REQUIRE(flat_high_ratio == Approx(flat_negative_control).epsilon(1.0e-12));
    REQUIRE_FALSE(passes_crosstalk_gate(flat_low_ratio, flat_high_ratio));
}

TEST_CASE("headphone crossfeed is channel symmetric and preserves mono DC gain",
          "[signal][crossfeed][stereo]") {
    HeadphoneCrossfeed64 normal;
    HeadphoneCrossfeed64 swapped;
    REQUIRE(normal.prepare(kSampleRate));
    REQUIRE(swapped.prepare(kSampleRate));

    for (int sample = 0; sample < 1024; ++sample) {
        const double left = 0.7 * std::sin(0.031 * sample);
        const double right = 0.4 * std::cos(0.017 * sample);
        double output_left = 0.0;
        double output_right = 0.0;
        double swapped_left = 0.0;
        double swapped_right = 0.0;
        normal.process_sample(left, right, output_left, output_right);
        swapped.process_sample(right, left, swapped_left, swapped_right);
        REQUIRE(output_left == swapped_right);
        REQUIRE(output_right == swapped_left);
    }

    normal.reset();
    double output_left = 0.0;
    double output_right = 0.0;
    for (int sample = 0; sample < 4096; ++sample)
        normal.process_sample(0.25, 0.25, output_left, output_right);
    REQUIRE(output_left == Approx(0.25).margin(1.0e-12));
    REQUIRE(output_right == Approx(0.25).margin(1.0e-12));
}

TEST_CASE("headphone crossfeed bypass is exact while reset clears its warm history",
          "[signal][crossfeed][bypass][reset]") {
    HeadphoneCrossfeed64 crossfeed;
    REQUIRE(crossfeed.prepare(kSampleRate));
    crossfeed.set_enabled(false);
    for (int sample = 0; sample < 64; ++sample) {
        const double left = 0.01 * sample;
        const double right = -0.02 * sample;
        double output_left = 0.0;
        double output_right = 0.0;
        crossfeed.process_sample(left, right, output_left, output_right);
        REQUIRE(output_left == left);
        REQUIRE(output_right == right);
    }

    crossfeed.set_enabled(true);
    double warm_left = 0.0;
    double warm_right = 0.0;
    crossfeed.process_sample(0.0, 0.0, warm_left, warm_right);
    REQUIRE(warm_left != 0.0);
    REQUIRE(warm_right != 0.0);

    crossfeed.reset();
    crossfeed.process_sample(0.0, 0.0, warm_left, warm_right);
    REQUIRE(warm_left == 0.0);
    REQUIRE(warm_right == 0.0);
}

TEST_CASE("headphone crossfeed recovers from non-finite audio and bounds finite extremes",
          "[signal][crossfeed][fault]") {
    HeadphoneCrossfeed64 crossfeed;
    REQUIRE(crossfeed.prepare(kSampleRate));
    double output_left = 1.0;
    double output_right = 1.0;
    crossfeed.process_sample(std::numeric_limits<double>::quiet_NaN(), 0.5, output_left,
                             output_right);
    REQUIRE(output_left == 0.0);
    REQUIRE(output_right == 0.0);
    crossfeed.process_sample(0.0, 0.0, output_left, output_right);
    REQUIRE(output_left == 0.0);
    REQUIRE(output_right == 0.0);

    const double maximum = std::numeric_limits<double>::max();
    for (int sample = 0; sample < 128; ++sample) {
        const double left = (sample & 1) == 0 ? maximum : -maximum;
        const double right = -left;
        crossfeed.process_sample(left, right, output_left, output_right);
        REQUIRE(std::isfinite(output_left));
        REQUIRE(std::isfinite(output_right));
        REQUIRE(std::abs(output_left) <= maximum);
        REQUIRE(std::abs(output_right) <= maximum);
    }
}

TEST_CASE("headphone crossfeed recursive tails snap exactly to zero",
          "[signal][crossfeed][denormal]") {
    HeadphoneCrossfeed64 crossfeed;
    REQUIRE(crossfeed.prepare(kSampleRate));
    double output_left = 0.0;
    double output_right = 0.0;
    crossfeed.process_sample(1.0, 0.0, output_left, output_right);

    bool reached_zero = false;
    for (int sample = 0; sample < 4096; ++sample) {
        crossfeed.process_sample(0.0, 0.0, output_left, output_right);
        if (sample > 12 && output_right == 0.0)
            reached_zero = true;
        if (reached_zero)
            REQUIRE(output_right == 0.0);
    }
    REQUIRE(reached_zero);
}

TEST_CASE("headphone crossfeed block partitioning and in-place processing are invariant",
          "[signal][crossfeed][partition]") {
    constexpr std::size_t sample_count = 997;
    std::array<double, sample_count> input_left{};
    std::array<double, sample_count> input_right{};
    for (std::size_t i = 0; i < sample_count; ++i) {
        input_left[i] = 0.6 * std::sin(0.037 * static_cast<double>(i));
        input_right[i] = 0.5 * std::cos(0.021 * static_cast<double>(i));
    }

    HeadphoneCrossfeed64 whole;
    HeadphoneCrossfeed64 partitioned;
    REQUIRE(whole.prepare(kSampleRate));
    REQUIRE(partitioned.prepare(kSampleRate));
    std::array<double, sample_count> whole_left{};
    std::array<double, sample_count> whole_right{};
    std::array<double, sample_count> partitioned_left = input_left;
    std::array<double, sample_count> partitioned_right = input_right;
    whole.process_block(input_left.data(), input_right.data(), whole_left.data(),
                        whole_right.data(), sample_count);

    std::size_t offset = 0;
    for (std::size_t block : {1u, 17u, 3u, 255u, 64u, 19u, 511u, 127u}) {
        const std::size_t count = std::min(block, sample_count - offset);
        partitioned.process_block(partitioned_left.data() + offset,
                                  partitioned_right.data() + offset,
                                  partitioned_left.data() + offset,
                                  partitioned_right.data() + offset, count);
        offset += count;
        if (offset == sample_count)
            break;
    }
    if (offset < sample_count)
        partitioned.process_block(partitioned_left.data() + offset,
                                  partitioned_right.data() + offset,
                                  partitioned_left.data() + offset,
                                  partitioned_right.data() + offset, sample_count - offset);

    REQUIRE(partitioned_left == whole_left);
    REQUIRE(partitioned_right == whole_right);

    whole.process_block(nullptr, input_right.data(), whole_left.data(), whole_right.data(), 1);
    whole.process_block(input_left.data(), nullptr, whole_left.data(), whole_right.data(), 1);
    whole.process_block(input_left.data(), input_right.data(), nullptr, whole_right.data(), 1);
    whole.process_block(input_left.data(), input_right.data(), whole_left.data(), nullptr, 1);
}

TEST_CASE("headphone crossfeed realtime paths allocate no memory",
          "[signal][crossfeed][rt-safety]") {
    HeadphoneCrossfeed crossfeed;
    REQUIRE(crossfeed.prepare(kSampleRate));
    std::array<float, 257> left{};
    std::array<float, 257> right{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = 0.5f * std::sin(0.1f * static_cast<float>(i));
        right[i] = 0.4f * std::cos(0.07f * static_cast<float>(i));
    }

    pulp::test::RtAllocationProbe probe;
    crossfeed.process_block(left.data(), right.data(), left.data(), right.data(), left.size());
    crossfeed.reset();
    REQUIRE_FALSE(probe.saw_allocation());
}
