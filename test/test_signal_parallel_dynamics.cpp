#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/parallel_dynamics.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <thread>

using Catch::Matchers::WithinAbs;
using pulp::signal::CrossfadeGainLaw;
using pulp::signal::ParallelDynamicsConfig64;
using pulp::signal::ParallelDynamicsMixer64;
using pulp::signal::prepare_parallel_dynamics_config;

namespace {

constexpr double kGainTolerance = 2.0e-14;

bool process_stereo(ParallelDynamicsMixer64& mixer,
                    const double* dry_left,
                    const double* dry_right,
                    const double* wet_left,
                    const double* wet_right,
                    double* output_left,
                    double* output_right,
                    std::size_t frames) {
    const std::array<const double*, 2> dry{dry_left, dry_right};
    const std::array<const double*, 2> wet{wet_left, wet_right};
    const std::array<double*, 2> output{output_left, output_right};
    return mixer.process(dry.data(), wet.data(), output.data(), frames);
}

double hand_mix(double dry, double wet, double mix,
                CrossfadeGainLaw law, double output_gain) {
    double dry_gain = 1.0 - mix;
    double wet_gain = mix;
    if (mix == 0.0) {
        dry_gain = 1.0;
        wet_gain = 0.0;
    } else if (mix == 1.0) {
        dry_gain = 0.0;
        wet_gain = 1.0;
    } else if (law == CrossfadeGainLaw::EqualPower) {
        const double theta = mix * 1.57079632679489661923;
        dry_gain = std::cos(theta);
        wet_gain = std::sin(theta);
    }
    return (dry * dry_gain + wet * wet_gain) * output_gain;
}

} // namespace

TEST_CASE("Parallel dynamics gains match independent linear and equal-power oracles",
          "[signal][parallel-dynamics][oracle]") {
    for (const auto law : {CrossfadeGainLaw::EqualGain,
                           CrossfadeGainLaw::EqualPower}) {
        for (int step = 0; step <= 20; ++step) {
            const double mix = static_cast<double>(step) / 20.0;
            ParallelDynamicsConfig64 config{};
            config.wet_mix = mix;
            config.mix_law = law;
            config.output_gain_db = 20.0 * std::log10(1.5);
            const auto prepared = prepare_parallel_dynamics_config(config, 0);
            REQUIRE(prepared.has_value());
            REQUIRE_THAT(prepared->dry_gain,
                         WithinAbs(hand_mix(1.0, 0.0, mix, law, 1.0),
                                   kGainTolerance));
            REQUIRE_THAT(prepared->wet_gain,
                         WithinAbs(hand_mix(0.0, 1.0, mix, law, 1.0),
                                   kGainTolerance));
            REQUIRE_THAT(prepared->output_gain, WithinAbs(1.5, kGainTolerance));
        }
    }
}

TEST_CASE("Parallel dynamics endpoints are exact and output gain is optional",
          "[signal][parallel-dynamics][endpoint]") {
    ParallelDynamicsMixer64 mixer;
    REQUIRE(mixer.prepare(0, 4));
    std::array<double, 4> dry_left{1.0, -2.0, 3.0, -4.0};
    std::array<double, 4> dry_right{-5.0, 6.0, -7.0, 8.0};
    std::array<double, 4> wet_left{20.0, 21.0, 22.0, 23.0};
    std::array<double, 4> wet_right{-30.0, -31.0, -32.0, -33.0};
    std::array<double, 4> output_left{};
    std::array<double, 4> output_right{};

    for (const auto law : {CrossfadeGainLaw::EqualGain,
                           CrossfadeGainLaw::EqualPower}) {
        ParallelDynamicsConfig64 dry_only{};
        dry_only.wet_mix = 0.0;
        dry_only.mix_law = law;
        REQUIRE(mixer.publish_config(dry_only));
        REQUIRE(process_stereo(mixer, dry_left.data(), dry_right.data(),
                               wet_left.data(), wet_right.data(), output_left.data(),
                               output_right.data(), dry_left.size()));
        REQUIRE(output_left == dry_left);
        REQUIRE(output_right == dry_right);

        ParallelDynamicsConfig64 wet_only{};
        wet_only.wet_mix = 1.0;
        wet_only.mix_law = law;
        wet_only.output_gain_db = 20.0 * std::log10(2.0);
        REQUIRE(mixer.publish_config(wet_only));
        REQUIRE(process_stereo(mixer, dry_left.data(), dry_right.data(),
                               wet_left.data(), wet_right.data(), output_left.data(),
                               output_right.data(), dry_left.size()));
        for (std::size_t i = 0; i < wet_left.size(); ++i) {
            REQUIRE_THAT(output_left[i], WithinAbs(2.0 * wet_left[i], kGainTolerance));
            REQUIRE_THAT(output_right[i], WithinAbs(2.0 * wet_right[i], kGainTolerance));
        }
    }
}

TEST_CASE("Parallel dynamics aligns dry and wet impulses to declared latency",
          "[signal][parallel-dynamics][latency][impulse]") {
    ParallelDynamicsMixer64 mixer;
    REQUIRE(mixer.prepare(8, 5));
    ParallelDynamicsConfig64 config{};
    config.wet_mix = 0.5;
    config.mix_law = CrossfadeGainLaw::EqualGain;
    config.dry_latency_samples = 0;
    config.wet_latency_samples = 3;
    config.dry_tail_samples = 0;
    config.wet_tail_samples = 17;
    REQUIRE(mixer.configure_latency_and_publish(config));
    REQUIRE(mixer.latency_samples() == 3);
    REQUIRE(mixer.tail_samples() == 17);

    std::array<double, 10> dry_left{};
    std::array<double, 10> dry_right{};
    std::array<double, 10> wet_left{};
    std::array<double, 10> wet_right{};
    std::array<double, 10> output_left{};
    std::array<double, 10> output_right{};
    dry_left[0] = 1.0;
    dry_right[0] = -1.0;
    wet_left[3] = 1.0;
    wet_right[3] = -1.0;

    std::array<double, 10> unaligned_left{};
    std::array<double, 10> unaligned_right{};
    for (std::size_t i = 0; i < dry_left.size(); ++i) {
        unaligned_left[i] = 0.5 * dry_left[i] + 0.5 * wet_left[i];
        unaligned_right[i] = 0.5 * dry_right[i] + 0.5 * wet_right[i];
    }

    for (std::size_t offset : {std::size_t{0}, std::size_t{5}})
        REQUIRE(process_stereo(mixer, dry_left.data() + offset, dry_right.data() + offset,
                               wet_left.data() + offset, wet_right.data() + offset,
                               output_left.data() + offset, output_right.data() + offset, 5));

    for (std::size_t i = 0; i < output_left.size(); ++i) {
        REQUIRE_THAT(output_left[i], WithinAbs(i == 3 ? 1.0 : 0.0, 0.0));
        REQUIRE_THAT(output_right[i], WithinAbs(i == 3 ? -1.0 : 0.0, 0.0));
    }
    REQUIRE(unaligned_left[0] == 0.5);
    REQUIRE(unaligned_left[3] == 0.5);
    REQUIRE(unaligned_left != output_left);
    REQUIRE(unaligned_right != output_right);
    REQUIRE(mixer.latency_samples() == 3);
    REQUIRE(mixer.tail_samples() == 17);
}

TEST_CASE("Parallel dynamics alignment preserves correlated phase across partitions",
          "[signal][parallel-dynamics][phase][partition]") {
    constexpr std::size_t frames = 257;
    constexpr std::size_t latency = 7;
    std::array<double, frames> dry_left{};
    std::array<double, frames> dry_right{};
    std::array<double, frames> wet_left{};
    std::array<double, frames> wet_right{};
    for (std::size_t i = 0; i < frames; ++i) {
        dry_left[i] = std::sin(0.17 * static_cast<double>(i));
        dry_right[i] = std::cos(0.11 * static_cast<double>(i));
        if (i >= latency) {
            wet_left[i] = dry_left[i - latency];
            wet_right[i] = dry_right[i - latency];
        }
    }

    ParallelDynamicsConfig64 config{};
    config.wet_mix = 0.37;
    config.mix_law = CrossfadeGainLaw::EqualGain;
    config.wet_latency_samples = latency;
    ParallelDynamicsMixer64 whole;
    ParallelDynamicsMixer64 partitioned;
    REQUIRE(whole.prepare(latency, frames));
    REQUIRE(partitioned.prepare(latency, frames));
    REQUIRE(whole.configure_latency_and_publish(config));
    REQUIRE(partitioned.configure_latency_and_publish(config));
    std::array<double, frames> whole_left{};
    std::array<double, frames> whole_right{};
    std::array<double, frames> partitioned_left{};
    std::array<double, frames> partitioned_right{};

    REQUIRE(process_stereo(whole, dry_left.data(), dry_right.data(), wet_left.data(),
                           wet_right.data(), whole_left.data(), whole_right.data(), frames));
    const std::array<std::size_t, 4> chunks{31, 64, 97, 65};
    std::size_t offset = 0;
    for (const auto chunk : chunks) {
        REQUIRE(process_stereo(partitioned, dry_left.data() + offset,
                               dry_right.data() + offset, wet_left.data() + offset,
                               wet_right.data() + offset, partitioned_left.data() + offset,
                               partitioned_right.data() + offset, chunk));
        offset += chunk;
    }

    REQUIRE(partitioned_left == whole_left);
    REQUIRE(partitioned_right == whole_right);
    for (std::size_t i = latency; i < frames; ++i) {
        REQUIRE_THAT(whole_left[i], WithinAbs(dry_left[i - latency], kGainTolerance));
        REQUIRE_THAT(whole_right[i], WithinAbs(dry_right[i - latency], kGainTolerance));
    }
}

TEST_CASE("Parallel dynamics configuration is transactional and propagates active tails",
          "[signal][parallel-dynamics][configuration]") {
    ParallelDynamicsMixer64 mixer;
    REQUIRE(mixer.prepare(4, 2));
    ParallelDynamicsConfig64 valid{};
    valid.wet_mix = 0.25;
    valid.dry_tail_samples = 3;
    valid.wet_tail_samples = 11;
    REQUIRE(mixer.publish_config(valid));

    std::array<double, 2> dry_left{2.0, 2.0};
    std::array<double, 2> dry_right{4.0, 4.0};
    std::array<double, 2> wet_left{10.0, 10.0};
    std::array<double, 2> wet_right{20.0, 20.0};
    std::array<double, 2> output_left{};
    std::array<double, 2> output_right{};
    REQUIRE(process_stereo(mixer, dry_left.data(), dry_right.data(), wet_left.data(),
                           wet_right.data(), output_left.data(), output_right.data(), 2));
    REQUIRE_THAT(output_left[0], WithinAbs(4.0, 0.0));
    REQUIRE(mixer.tail_samples() == 11);

    auto invalid = valid;
    invalid.wet_mix = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(mixer.publish_config(invalid));
    invalid = valid;
    invalid.output_gain_db = 25.0;
    REQUIRE_FALSE(mixer.publish_config(invalid));
    invalid = valid;
    invalid.wet_latency_samples = 5;
    REQUIRE_FALSE(mixer.publish_config(invalid));
    invalid = valid;
    invalid.wet_latency_samples = 4;
    invalid.dry_tail_samples = std::numeric_limits<std::size_t>::max();
    REQUIRE_FALSE(mixer.configure_latency_and_publish(invalid));
    REQUIRE(process_stereo(mixer, dry_left.data(), dry_right.data(), wet_left.data(),
                           wet_right.data(), output_left.data(), output_right.data(), 2));
    REQUIRE_THAT(output_left[0], WithinAbs(4.0, 0.0));
}

TEST_CASE("Parallel dynamics sanitizes faults and enforces stereo in-place rules",
          "[signal][parallel-dynamics][fault][in-place]") {
    ParallelDynamicsMixer64 mixer;
    REQUIRE_FALSE(mixer.prepare(0, 0));
    REQUIRE(mixer.prepare(0, 4));
    ParallelDynamicsConfig64 config{};
    config.wet_mix = 0.5;
    REQUIRE(mixer.publish_config(config));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::array<double, 4> dry_left{2.0, nan, 2.0, 2.0};
    std::array<double, 4> dry_right{4.0, 4.0, 4.0, 4.0};
    std::array<double, 4> wet_left{10.0, 10.0, nan, 10.0};
    std::array<double, 4> wet_right{20.0, 20.0, 20.0, 20.0};

    REQUIRE(process_stereo(mixer, dry_left.data(), dry_right.data(), wet_left.data(),
                           wet_right.data(), wet_left.data(), wet_right.data(), 4));
    REQUIRE_THAT(wet_left[0], WithinAbs(6.0, 0.0));
    REQUIRE_THAT(wet_left[1], WithinAbs(5.0, 0.0));
    REQUIRE_THAT(wet_left[2], WithinAbs(1.0, 0.0));
    REQUIRE_THAT(wet_right[0], WithinAbs(12.0, 0.0));
    REQUIRE_FALSE(process_stereo(mixer, dry_left.data(), dry_right.data(),
                                 wet_left.data(), wet_right.data(), wet_left.data(),
                                 wet_right.data(), 5));

    std::array<double, 8> overlap{};
    REQUIRE_FALSE(process_stereo(mixer, overlap.data(), dry_right.data(),
                                 wet_left.data(), wet_right.data(), overlap.data() + 1,
                                 dry_right.data(), 4));
    REQUIRE_FALSE(process_stereo(mixer, dry_left.data(), dry_left.data(),
                                 wet_left.data(), wet_right.data(), wet_left.data(),
                                 wet_right.data(), 4));
}

TEST_CASE("Parallel dynamics reset clears latency history and processing allocates no memory",
          "[signal][parallel-dynamics][reset][rt-safety]") {
    ParallelDynamicsMixer64 mixer;
    REQUIRE(mixer.prepare(4, 4));
    ParallelDynamicsConfig64 config{};
    config.wet_mix = 0.0;
    config.wet_latency_samples = 4;
    REQUIRE_FALSE(mixer.publish_config(config));
    REQUIRE(mixer.configure_latency_and_publish(config));
    std::array<double, 4> ones_left{1.0, 1.0, 1.0, 1.0};
    std::array<double, 4> ones_right{1.0, 1.0, 1.0, 1.0};
    std::array<double, 4> zeros_left{};
    std::array<double, 4> zeros_right{};
    std::array<double, 4> output_left{};
    std::array<double, 4> output_right{};
    REQUIRE(process_stereo(mixer, ones_left.data(), ones_right.data(),
                           zeros_left.data(), zeros_right.data(), output_left.data(),
                           output_right.data(), 4));
    mixer.reset();

    std::size_t allocations = 0;
    bool processed = false;
    {
        pulp::test::RtAllocationProbe probe;
        processed = process_stereo(mixer, zeros_left.data(), zeros_right.data(),
                                   zeros_left.data(), zeros_right.data(),
                                   output_left.data(), output_right.data(), 4);
        allocations = probe.allocation_count();
    }
    REQUIRE(processed);
    REQUIRE(allocations == 0);
    REQUIRE(output_left == zeros_left);
    REQUIRE(output_right == zeros_right);
    REQUIRE(mixer.tail_samples() == 4);
}

TEST_CASE("Parallel dynamics publication never exposes a mixed configuration",
          "[signal][parallel-dynamics][thread]") {
    ParallelDynamicsMixer64 mixer;
    REQUIRE(mixer.prepare(0, 1));
    ParallelDynamicsConfig64 first{};
    first.wet_mix = 0.25;
    first.mix_law = CrossfadeGainLaw::EqualGain;
    ParallelDynamicsConfig64 second{};
    second.wet_mix = 0.75;
    second.mix_law = CrossfadeGainLaw::EqualPower;
    second.output_gain_db = 20.0 * std::log10(0.5);
    REQUIRE(mixer.publish_config(first));

    double dry_left = 2.0;
    double dry_right = 4.0;
    double wet_left = 10.0;
    double wet_right = 20.0;
    double output_left = 0.0;
    double output_right = 0.0;
    REQUIRE(process_stereo(mixer, &dry_left, &dry_right, &wet_left, &wet_right,
                           &output_left, &output_right, 1));
    const double first_expected = hand_mix(2.0, 10.0, 0.25,
                                           CrossfadeGainLaw::EqualGain, 1.0);
    const double second_expected = hand_mix(2.0, 10.0, 0.75,
                                            CrossfadeGainLaw::EqualPower, 0.5);
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<bool> torn{false};

    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 20000; ++i)
            if (!mixer.publish_config((i & 1) == 0 ? first : second))
                torn.store(true, std::memory_order_relaxed);
        done.store(true, std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    while (!done.load(std::memory_order_acquire)) {
        if (!process_stereo(mixer, &dry_left, &dry_right, &wet_left, &wet_right,
                            &output_left, &output_right, 1) ||
            (std::abs(output_left - first_expected) > kGainTolerance &&
             std::abs(output_left - second_expected) > kGainTolerance))
            torn.store(true, std::memory_order_relaxed);
    }
    writer.join();
    REQUIRE_FALSE(torn.load(std::memory_order_relaxed));
}
