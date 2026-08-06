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
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kFollowerTolerance = 1e-8;
constexpr double kTimeToleranceSamples = 2.0;

double reference_coefficient(double milliseconds, double sample_rate) {
    const double interval_samples = milliseconds * 0.001 * sample_rate;
    return 1.0 - std::exp(-std::log(9.0) / interval_samples);
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

void require_unity_meter(const GainReduction& meter) {
    REQUIRE(meter.db() == 0.0);
    REQUIRE(meter.signed_db() == 0.0);
    REQUIRE(meter.linear_gain() == 1.0);
}

void require_signed_legacy_meter(const GainReduction& meter, double legacy_db) {
    REQUIRE(legacy_db < 0.0);
    REQUIRE(meter.db() > 0.0);
    REQUIRE(meter.db() == -legacy_db);
    REQUIRE_THAT(meter.linear_gain(), WithinAbs(std::pow(10.0, legacy_db / 20.0), 1e-14));
}

void require_magnitude_legacy_meter(const GainReduction& meter, double legacy_db) {
    REQUIRE(legacy_db > 0.0);
    REQUIRE(meter.db() > 0.0);
    REQUIRE(meter.db() == legacy_db);
    REQUIRE_THAT(meter.linear_gain(), WithinAbs(std::pow(10.0, -legacy_db / 20.0), 1e-14));
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
    REQUIRE(EnvelopeFollower64::coefficient_for_time_ms(
                10.0, std::numeric_limits<double>::infinity()) == 0.0);
    REQUIRE(EnvelopeFollower64::coefficient_for_time_ms(std::numeric_limits<double>::quiet_NaN(),
                                                        48000.0) == 0.0);
}

TEST_CASE("Peak envelope attack and release use stated time-to-target semantics",
          "[signal][dynamics]") {
    for (double sample_rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (double milliseconds : {12.5, 250.0}) {
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
}

TEST_CASE("RMS envelope timing applies to its mean-square power state", "[signal][dynamics]") {
    for (double sample_rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (double milliseconds : {12.5, 250.0}) {
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
}

TEST_CASE("Legacy BallisticsFilter valid-input render hash remains bit-identical",
          "[signal][dynamics][compat]") {
    constexpr double sample_rate = 192000.0;
    constexpr double attack_ms = 7.5;
    constexpr double release_ms = 123.0;
    const auto legacy_coefficient = [](double milliseconds) {
        return 1.0 - std::exp(static_cast<double>(-2.2f) /
                              (milliseconds * static_cast<double>(0.001f) * sample_rate));
    };

    BallisticsFilter64 follower;
    follower.prepare(sample_rate);
    follower.set_attack_ms(attack_ms);
    follower.set_release_ms(release_ms);

    Xorshift32 rng(0x8f3a21c7u);
    double reference_state = 0.0;
    std::uint64_t subject_hash = 1469598103934665603ull;
    std::uint64_t reference_hash = subject_hash;
    for (int i = 0; i < 16384; ++i) {
        const bool rms = i >= 8192;
        if (i == 8192)
            follower.set_mode(BallisticsFilter64::Mode::rms);
        const double input = rng.next_bipolar<double>();
        const double value = rms ? input * input : std::abs(input);
        const double coefficient = value > reference_state ? legacy_coefficient(attack_ms)
                                                           : legacy_coefficient(release_ms);
        reference_state = reference_state + coefficient * (value - reference_state);
        const double expected = rms ? std::sqrt(reference_state) : reference_state;
        const double actual = follower.process(input);

        const auto actual_bits = std::bit_cast<std::uint64_t>(actual);
        const auto expected_bits = std::bit_cast<std::uint64_t>(expected);
        subject_hash = (subject_hash ^ actual_bits) * 1099511628211ull;
        reference_hash = (reference_hash ^ expected_bits) * 1099511628211ull;
    }
    REQUIRE(subject_hash == reference_hash);
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

TEST_CASE("RMS envelope bounds finite square overflow and sanitizes dB floors",
          "[signal][dynamics]") {
    EnvelopeFollower64 follower;
    follower.prepare(192000.0);
    follower.set_attack_ms(0.0);
    follower.set_release_ms(250.0);
    follower.set_mode(EnvelopeFollower64::Mode::rms);

    const double huge = follower.process(std::numeric_limits<double>::max());
    REQUIRE(std::isfinite(huge));
    REQUIRE(huge > 0.0);
    REQUIRE(std::isfinite(follower.current()));
    REQUIRE(std::isfinite(follower.current_db()));
    REQUIRE(std::isfinite(follower.process(0.0)));

    follower.reset();
    REQUIRE(follower.current_db(12.0) == -160.0);
    REQUIRE(follower.current_db(std::numeric_limits<double>::quiet_NaN()) == -160.0);
    REQUIRE(follower.current_db(-std::numeric_limits<double>::infinity()) == -160.0);
    REQUIRE(follower.current_db(-96.0) == -96.0);
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

    linked.set_link(1.0);
    const auto transitioned = linked.process(0.1, 0.1);
    REQUIRE(transitioned[0] == transitioned[1]);
    REQUIRE(linked.current()[0] == linked.current()[1]);

    linked.set_link(0.0);

    const auto isolated_fault = linked.process(std::numeric_limits<double>::quiet_NaN(), -0.5);
    REQUIRE(isolated_fault[0] == 0.0);
    REQUIRE(isolated_fault[1] > independent[1]);

    linked.set_link(1.0);
    const auto shared_fault = linked.process(0.5, std::numeric_limits<double>::infinity());
    constexpr std::array<double, 2> silent{0.0, 0.0};
    REQUIRE(shared_fault == silent);
    REQUIRE(linked.current() == silent);
}

TEST_CASE("Stereo RMS supports partial detector links and finite extreme input",
          "[signal][dynamics]") {
    StereoEnvelopeFollower64 follower;
    follower.prepare(192000.0);
    follower.set_attack_ms(0.0);
    follower.set_release_ms(250.0);
    follower.set_mode(StereoEnvelopeFollower64::Mode::rms);
    follower.set_link(0.5);

    const auto partial = follower.process(1.0, -0.25);
    REQUIRE_THAT(partial[0], WithinAbs(1.0, 1e-15));
    REQUIRE_THAT(partial[1], WithinAbs(0.625, 1e-15));

    const auto extreme = follower.process(1.0, std::numeric_limits<double>::max());
    REQUIRE(std::isfinite(extreme[0]));
    REQUIRE(std::isfinite(extreme[1]));
    REQUIRE(extreme[0] > 0.0);
    REQUIRE(extreme[1] > 0.0);
    REQUIRE(std::isfinite(follower.current()[0]));
    REQUIRE(std::isfinite(follower.current()[1]));
}

TEST_CASE("Stereo link transitions synchronize detector history before unlinking",
          "[signal][dynamics]") {
    StereoEnvelopeFollower64 follower;
    follower.prepare(48000.0);
    follower.set_attack_ms(10.0);
    follower.set_release_ms(100.0);
    follower.set_link(0.0);

    for (int i = 0; i < 1000; ++i)
        follower.process(1.0, 0.1);
    const auto independent = follower.current();
    REQUIRE(independent[0] > independent[1] * 5.0);

    follower.set_link(1.0);
    REQUIRE(follower.current()[0] == follower.current()[1]);
    for (int i = 0; i < 100; ++i) {
        const auto linked = follower.process(0.2, 0.2);
        REQUIRE(linked[0] == linked[1]);
    }

    follower.set_link(0.5);
    const auto partial = follower.process(0.8, 0.2);
    REQUIRE(partial[0] > partial[1]);
    REQUIRE(partial[1] > 0.2);

    follower.set_link(1.0);
    REQUIRE(follower.current()[0] == follower.current()[1]);
    follower.set_link(0.0);
    const auto unlinked = follower.current();
    REQUIRE(unlinked[0] == unlinked[1]);

    const auto equal_input = follower.process(0.3, 0.3);
    REQUIRE(equal_input[0] == equal_input[1]);
    const auto downstream_gain = [](double envelope) {
        const double reduction_db = std::max(0.0, 20.0 * std::log10(envelope / 0.1));
        return GainReduction::from_magnitude_db(reduction_db).linear_gain();
    };
    REQUIRE(downstream_gain(equal_input[0]) == downstream_gain(equal_input[1]));
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
    const auto signed_mute =
        GainReduction::from_signed_db(-std::numeric_limits<double>::infinity());
    const auto magnitude_mute =
        GainReduction::from_magnitude_db(std::numeric_limits<double>::infinity());
    REQUIRE(signed_mute.db() == std::numeric_limits<double>::infinity());
    REQUIRE(magnitude_mute.db() == std::numeric_limits<double>::infinity());
    REQUIRE(signed_mute.linear_gain() == 0.0);
    REQUIRE(magnitude_mute.linear_gain() == 0.0);
    REQUIRE(GainReduction::from_signed_db(std::numeric_limits<double>::quiet_NaN()).db() == 0.0);

    FeedforwardCompressor64 feedforward;
    feedforward.prepare(48000.0);
    require_unity_meter(feedforward.gain_reduction());
    feedforward.set_auto_makeup(false);
    feedforward.set_threshold_db(-30.0);
    feedforward.set_ratio(10.0);
    feedforward.set_attack_ms(0.05);
    for (int i = 0; i < 48000; ++i)
        feedforward.process(1.0);
    require_magnitude_legacy_meter(feedforward.gain_reduction(), feedforward.gain_reduction_db());

    VcaCompressor64 vca;
    vca.prepare(48000.0);
    require_unity_meter(vca.gain_reduction());
    vca.set_threshold_db(-30.0);
    vca.set_ratio(10.0);
    for (int i = 0; i < 48000; ++i)
        vca.process(1.0);
    require_signed_legacy_meter(vca.gain_reduction(), vca.gain_reduction_db());
    REQUIRE_THAT(vca.gain_reduction().linear_gain(), WithinAbs(vca.current_gain_linear(), 1e-14));

    FetCompressor64 fet;
    fet.prepare(48000.0);
    require_unity_meter(fet.gain_reduction());
    fet.set_input_gain_db(20.0);
    for (int i = 0; i < 48000; ++i)
        fet.process(1.0);
    require_magnitude_legacy_meter(fet.gain_reduction(), fet.gain_reduction_db());

    DiodeBridgeCompressor64 diode;
    diode.prepare(48000.0);
    require_unity_meter(diode.gain_reduction());
    diode.set_threshold_db(-30.0);
    diode.set_ratio(10.0);
    for (int i = 0; i < 96000; ++i)
        diode.process(std::sin(2.0 * std::numbers::pi * 1000.0 * i / 48000.0));
    require_signed_legacy_meter(diode.gain_reduction(), diode.gain_reduction_db());

    Compressor64 legacy;
    require_unity_meter(legacy.gain_reduction());
    Compressor64::Params params;
    params.threshold_db = -30.0;
    params.ratio = 10.0;
    params.knee_db = 0.0;
    params.attack_ms = 0.0;
    legacy.set_params(params);
    legacy.set_sample_rate(48000.0);
    const double compressed = legacy.process(1.0);
    require_signed_legacy_meter(legacy.gain_reduction(), legacy.gain_reduction_db());
    REQUIRE_THAT(legacy.gain_reduction().linear_gain(), WithinAbs(compressed, 1e-14));

    Limiter64 limiter;
    require_unity_meter(limiter.gain_reduction());
    limiter.set_threshold_db(-12.0);
    const double limited = limiter.process(1.0);
    REQUIRE(limiter.gain_reduction().db() > 0.0);
    REQUIRE_THAT(limiter.gain_reduction().linear_gain(), WithinAbs(limited, 1e-14));

    limiter.reset();
    limiter.set_threshold_db(-std::numeric_limits<double>::infinity());
    const double muted = limiter.process(1.0);
    REQUIRE(muted == 0.0);
    REQUIRE(limiter.gain_reduction().db() == std::numeric_limits<double>::infinity());
    REQUIRE(limiter.gain_reduction().linear_gain() == muted);

    NoiseGate64 gate;
    require_unity_meter(gate.gain_reduction());
    NoiseGate64::Params gate_params;
    gate_params.threshold_db = -20.0;
    gate_params.ratio = 10.0;
    gate_params.attack_ms = 0.0;
    gate_params.release_ms = 0.0;
    gate.set_params(gate_params);
    const double gate_input = 0.001;
    const double gated = gate.process(gate_input);
    REQUIRE(gate.gain_reduction().db() > 0.0);
    const double actual_gate_gain = std::abs(gated / gate_input);
    REQUIRE_THAT(gate.gain_reduction().linear_gain(), WithinAbs(actual_gate_gain, 1e-14));
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
