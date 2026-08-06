#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/compressor.hpp>
#include <pulp/signal/diode_bridge_compressor.hpp>
#include <pulp/signal/dynamics_contract.hpp>
#include <pulp/signal/feedforward_compressor.hpp>
#include <pulp/signal/fet_compressor.hpp>
#include <pulp/signal/noise_gate.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/vca_compressor.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kFollowerTolerance = 1e-8;
constexpr double kTimeToleranceSamples = 2.0;

double reference_coefficient(double milliseconds, double sample_rate) {
    return 1.0 - std::exp(-2.2 / (milliseconds * 0.001 * sample_rate));
}

int crossing_sample(EnvelopeFollower64& follower, double input, double threshold,
                    int maximum_samples, bool rising) {
    for (int i = 0; i < maximum_samples; ++i) {
        const double value = follower.process(input);
        if ((rising && value >= threshold) || (!rising && value <= threshold))
            return i;
    }
    return -1;
}

template <typename Fn> void require_no_allocation(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

} // namespace

TEST_CASE("Envelope follower publishes its 10-to-90-percent coefficient", "[signal][dynamics]") {
    for (double sample_rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (double milliseconds : {0.1, 1.0, 10.0, 250.0}) {
            EnvelopeFollower64 follower;
            follower.prepare(sample_rate);
            follower.set_attack_ms(milliseconds);
            follower.set_release_ms(milliseconds);

            const double expected = reference_coefficient(milliseconds, sample_rate);
            REQUIRE_THAT(follower.attack_coefficient(), WithinAbs(expected, kFollowerTolerance));
            REQUIRE_THAT(follower.release_coefficient(), WithinAbs(expected, kFollowerTolerance));
            REQUIRE_THAT(EnvelopeFollower64::coefficient_for_time_ms(milliseconds, sample_rate),
                         WithinAbs(expected, kFollowerTolerance));
        }
    }
}

TEST_CASE("Peak envelope attack and release use stated time-to-target semantics",
          "[signal][dynamics]") {
    for (double sample_rate : {44100.0, 48000.0, 96000.0}) {
        constexpr double milliseconds = 12.5;
        EnvelopeFollower64 follower;
        follower.prepare(sample_rate);
        follower.set_attack_ms(milliseconds);
        follower.set_release_ms(milliseconds);

        const int at_10 = crossing_sample(follower, 1.0, 0.1, 100000, true);
        const int at_90 = crossing_sample(follower, 1.0, 0.9, 100000, true);
        REQUIRE(at_10 >= 0);
        REQUIRE(at_90 >= 0);
        const double attack_samples = static_cast<double>(at_90 + 1);
        const double expected_samples = milliseconds * 0.001 * sample_rate;
        REQUIRE(std::abs(attack_samples - expected_samples) <= kTimeToleranceSamples);

        for (int i = 0; i < static_cast<int>(sample_rate); ++i)
            follower.process(1.0);
        const int release_at_90 = crossing_sample(follower, 0.0, 0.9, 100000, false);
        const int release_at_10 = crossing_sample(follower, 0.0, 0.1, 100000, false);
        REQUIRE(release_at_90 >= 0);
        REQUIRE(release_at_10 >= 0);
        const double release_samples = static_cast<double>(release_at_10 + 1);
        REQUIRE(std::abs(release_samples - expected_samples) <= kTimeToleranceSamples);
    }
}

TEST_CASE("RMS envelope timing applies to its mean-square power state", "[signal][dynamics]") {
    for (double sample_rate : {44100.0, 48000.0, 96000.0}) {
        constexpr double milliseconds = 12.5;
        EnvelopeFollower64 follower;
        follower.prepare(sample_rate);
        follower.set_attack_ms(milliseconds);
        follower.set_release_ms(milliseconds);
        follower.set_mode(EnvelopeFollower64::Mode::rms);

        const int at_10_power = crossing_sample(follower, 1.0, std::sqrt(0.1), 100000, true);
        const int at_90_power = crossing_sample(follower, 1.0, std::sqrt(0.9), 100000, true);
        REQUIRE(at_10_power >= 0);
        REQUIRE(at_90_power >= 0);
        const double expected_samples = milliseconds * 0.001 * sample_rate;
        REQUIRE(std::abs(static_cast<double>(at_90_power + 1) - expected_samples) <=
                kTimeToleranceSamples);

        for (int i = 0; i < static_cast<int>(sample_rate); ++i)
            follower.process(1.0);
        const int release_at_90_power =
            crossing_sample(follower, 0.0, std::sqrt(0.9), 100000, false);
        const int release_at_10_power =
            crossing_sample(follower, 0.0, std::sqrt(0.1), 100000, false);
        REQUIRE(release_at_90_power >= 0);
        REQUIRE(release_at_10_power >= 0);
        REQUIRE(std::abs(static_cast<double>(release_at_10_power + 1) - expected_samples) <=
                kTimeToleranceSamples);
    }
}

TEST_CASE("Envelope follower modes report linear and dB domains", "[signal][dynamics]") {
    constexpr double sample_rate = 48000.0;
    constexpr int sample_count = 480000;
    EnvelopeFollower64 peak;
    EnvelopeFollower64 rms;
    peak.prepare(sample_rate);
    rms.prepare(sample_rate);
    peak.set_attack_ms(20.0);
    peak.set_release_ms(20.0);
    rms.set_attack_ms(20.0);
    rms.set_release_ms(20.0);
    rms.set_mode(EnvelopeFollower64::Mode::rms);

    for (int i = 0; i < sample_count; ++i) {
        const double sine = std::sin(2.0 * std::numbers::pi * 997.0 * i / sample_rate);
        peak.process(sine);
        rms.process(sine);
    }

    REQUIRE_THAT(peak.current(), WithinAbs(2.0 / std::numbers::pi, 0.002));
    REQUIRE_THAT(rms.current(), WithinAbs(std::sqrt(0.5), 0.002));
    REQUIRE_THAT(rms.current_db(), WithinAbs(20.0 * std::log10(rms.current()), 1e-12));

    rms.reset();
    REQUIRE(rms.current() == 0.0);
    REQUIRE(rms.current_db() == -160.0);
}

TEST_CASE("Envelope follower handles silence DC bursts and invalid control values",
          "[signal][dynamics]") {
    EnvelopeFollower64 follower;
    follower.prepare(48000.0);
    follower.set_attack_ms(5.0);
    follower.set_release_ms(50.0);

    for (int i = 0; i < 1024; ++i)
        follower.process(0.0);
    REQUIRE(follower.current() == 0.0);
    for (int i = 0; i < 48000; ++i)
        follower.process(0.25);
    REQUIRE_THAT(follower.current(), WithinAbs(0.25, 1e-12));

    const double before_burst = follower.current();
    const double burst = follower.process(1.0);
    REQUIRE(burst > before_burst);
    const double after_silence = follower.process(0.0);
    REQUIRE(after_silence < burst);

    const double attack = follower.attack_ms();
    const double release = follower.release_ms();
    follower.set_attack_ms(std::numeric_limits<double>::quiet_NaN());
    follower.set_release_ms(std::numeric_limits<double>::infinity());
    REQUIRE(follower.attack_ms() == attack);
    REQUIRE(follower.release_ms() == release);
}

TEST_CASE("Envelope follower is deterministic across caller block partitions",
          "[signal][dynamics]") {
    constexpr std::size_t count = 8192;
    std::vector<double> input(count);
    Xorshift32 rng(0x4d595df4u);
    for (auto& sample : input)
        sample = rng.next_bipolar<double>();

    auto render = [&](int block_size) {
        EnvelopeFollower64 follower;
        follower.prepare(48000.0);
        follower.set_attack_ms(4.0);
        follower.set_release_ms(80.0);
        follower.set_mode(EnvelopeFollower64::Mode::rms);
        std::vector<double> output(count);
        for (std::size_t offset = 0; offset < count;) {
            const auto n =
                std::min<std::size_t>(static_cast<std::size_t>(block_size), count - offset);
            follower.process(input.data() + offset, output.data() + offset, static_cast<int>(n));
            offset += n;
        }
        return output;
    };

    const auto one = render(1);
    for (int block_size : {7, 64, 257, 1024})
        REQUIRE(render(block_size) == one);
}

TEST_CASE("Stereo envelope linking preserves detector energy and channel independence",
          "[signal][dynamics]") {
    StereoEnvelopeFollower64 linked;
    linked.prepare(48000.0);
    linked.set_attack_ms(0.0);
    linked.set_release_ms(100.0);
    linked.set_link(1.0);
    const auto both = linked.process(1.0, -0.25);
    REQUIRE_THAT(both[0], WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(both[1], WithinAbs(1.0, 1e-12));

    linked.reset();
    linked.set_link(0.0);
    const auto independent = linked.process(1.0, -0.25);
    REQUIRE_THAT(independent[0], WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(independent[1], WithinAbs(0.25, 1e-12));

    const auto isolated_fault = linked.process(std::numeric_limits<double>::quiet_NaN(), -0.5);
    REQUIRE(isolated_fault[0] == 0.0);
    REQUIRE(isolated_fault[1] > independent[1]);

    linked.set_link(1.0);
    const auto shared_fault = linked.process(0.5, std::numeric_limits<double>::infinity());
    constexpr std::array<double, 2> silent{0.0, 0.0};
    REQUIRE(shared_fault == silent);
    REQUIRE(linked.current() == silent);
}

TEST_CASE("Envelope follower recovers deterministically from non-finite detector input",
          "[signal][dynamics]") {
    EnvelopeFollower64 follower;
    follower.prepare(48000.0);
    follower.set_attack_ms(10.0);
    follower.set_release_ms(100.0);
    for (int i = 0; i < 1000; ++i)
        follower.process(1.0);
    REQUIRE(follower.current() > 0.0);

    REQUIRE(follower.process(std::numeric_limits<double>::quiet_NaN()) == 0.0);
    REQUIRE(follower.current() == 0.0);

    EnvelopeFollower64 fresh;
    fresh.prepare(48000.0);
    fresh.set_attack_ms(10.0);
    fresh.set_release_ms(100.0);
    REQUIRE(follower.process(0.5) == fresh.process(0.5));
}

TEST_CASE("Gain-reduction telemetry has one sign and unit across dynamics lineages",
          "[signal][dynamics]") {
    REQUIRE(GainReduction{}.db() == 0.0);
    const auto signed_meter = GainReduction::from_signed_db(-6.0);
    const auto magnitude_meter = GainReduction::from_magnitude_db(6.0);
    REQUIRE(signed_meter.db() == 6.0);
    REQUIRE(magnitude_meter.db() == 6.0);
    REQUIRE(signed_meter.signed_db() == -6.0);
    REQUIRE_THAT(signed_meter.linear_gain(), WithinAbs(std::pow(10.0, -6.0 / 20.0), 1e-15));
    REQUIRE(GainReduction::from_signed_db(std::numeric_limits<double>::infinity()).db() == 0.0);

    FeedforwardCompressor64 feedforward;
    feedforward.prepare(48000.0);
    feedforward.set_auto_makeup(false);
    feedforward.set_attack_ms(0.05);
    for (int i = 0; i < 48000; ++i)
        feedforward.process(1.0);
    REQUIRE(feedforward.gain_reduction().db() == feedforward.gain_reduction_db());

    VcaCompressor64 vca;
    vca.prepare(48000.0);
    for (int i = 0; i < 48000; ++i)
        vca.process(1.0);
    REQUIRE(vca.gain_reduction().db() == -vca.gain_reduction_db());

    FetCompressor64 fet;
    fet.prepare(48000.0);
    for (int i = 0; i < 48000; ++i)
        fet.process(1.0);
    REQUIRE(fet.gain_reduction().db() == fet.gain_reduction_db());

    DiodeBridgeCompressor64 diode;
    diode.prepare(48000.0);
    for (int i = 0; i < 48000; ++i)
        diode.process(1.0);
    REQUIRE(diode.gain_reduction().db() == -diode.gain_reduction_db());

    Compressor64 legacy;
    Compressor64::Params params;
    params.threshold_db = -30.0;
    legacy.set_params(params);
    legacy.set_sample_rate(48000.0);
    for (int i = 0; i < 48000; ++i)
        legacy.process(1.0);
    REQUIRE(legacy.gain_reduction().db() == -legacy.gain_reduction_db());

    NoiseGate64 gate;
    NoiseGate64::Params gate_params;
    gate_params.threshold_db = -20.0;
    gate.set_params(gate_params);
    for (int i = 0; i < 48000; ++i)
        gate.process(0.001);
    REQUIRE(gate.gain_reduction().db() >= 0.0);
}

TEST_CASE("External sidechain remains detector-domain input", "[signal][dynamics]") {
    Compressor64 internal;
    Compressor64 external;
    Compressor64::Params params;
    params.threshold_db = -20.0;
    params.ratio = 10.0;
    params.attack_ms = 1.0;
    params.release_ms = 100.0;
    internal.set_params(params);
    external.set_params(params);
    internal.set_sample_rate(48000.0);
    external.set_sample_rate(48000.0);

    double internal_output = 0.0;
    double external_output = 0.0;
    for (int i = 0; i < 48000; ++i) {
        internal_output = internal.process(0.01);
        external_output = external.process_with_sidechain(0.01, 1.0);
    }
    REQUIRE(std::abs(external_output) < std::abs(internal_output));
    REQUIRE(external.gain_reduction().db() > internal.gain_reduction().db() + 10.0);
}

TEST_CASE("Dynamics contract inspection is allocation-free", "[signal][dynamics][rt-safety]") {
    EnvelopeFollower follower;
    StereoEnvelopeFollower stereo;
    follower.prepare(48000.0f);
    stereo.prepare(48000.0f);

    require_no_allocation([&] {
        for (int i = 0; i < 4096; ++i) {
            (void)follower.process(0.25f);
            (void)follower.current_db();
            (void)stereo.process(0.25f, -0.5f);
            (void)GainReduction::from_signed_db(-3.0).linear_gain();
        }
    });
}
