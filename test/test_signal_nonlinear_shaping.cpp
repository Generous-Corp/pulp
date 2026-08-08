#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/nonlinear_shaping.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kSampleRate = 48000.0;

template <typename Processor, typename Generator>
std::vector<double> render(Processor& processor, int count, Generator&& generator,
                           int discard = 1024) {
    std::vector<double> output;
    output.reserve(static_cast<std::size_t>(count));
    for (int sample = 0; sample < count + discard; ++sample) {
        const double value = processor.process(generator(sample));
        if (sample >= discard)
            output.push_back(value);
    }
    return output;
}

double magnitude_at(const std::vector<double>& samples, double hz) {
    double real = 0.0;
    double imaginary = 0.0;
    const double omega = 2.0 * std::numbers::pi * hz / kSampleRate;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        real += samples[i] * std::cos(omega * static_cast<double>(i));
        imaginary -= samples[i] * std::sin(omega * static_cast<double>(i));
    }
    return 2.0 * std::hypot(real, imaginary) / static_cast<double>(samples.size());
}

template <typename Processor> void require_partition_invariant(Processor configured) {
    constexpr std::size_t count = 997;
    std::array<double, count> input{};
    std::array<double, count> contiguous{};
    std::array<double, count> partitioned{};
    for (std::size_t i = 0; i < count; ++i)
        input[i] = 0.51 * std::sin(0.071 * static_cast<double>(i)) +
                   0.17 * std::cos(0.193 * static_cast<double>(i));

    Processor one = configured;
    Processor split = configured;
    one.process_block(input.data(), contiguous.data(), count);

    constexpr std::array<std::size_t, 8> block_sizes{1, 31, 7, 64, 3, 127, 11, 43};
    std::size_t offset = 0;
    std::size_t block = 0;
    while (offset < count) {
        const std::size_t size = std::min(block_sizes[block % block_sizes.size()], count - offset);
        split.process_block(input.data() + offset, partitioned.data() + offset, size);
        offset += size;
        ++block;
    }
    REQUIRE(contiguous == partitioned);
}

template <typename Processor> void require_fault_recovery(Processor configured) {
    Processor faulted = configured;
    Processor reference = configured;
    static_cast<void>(faulted.process(0.3));
    REQUIRE(faulted.process(std::numeric_limits<double>::infinity()) == 0.0);
    for (int i = 0; i < 300; ++i) {
        const double input = 0.4 * std::sin(0.13 * static_cast<double>(i));
        REQUIRE_THAT(faulted.process(input), WithinAbs(reference.process(input), 1.0e-12));
    }
}

template <typename Fn> void require_no_allocations(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

template <typename Processor> void require_exact_linear_timing(NonlinearAliasPolicy policy) {
    Processor processor;
    processor.prepare(kSampleRate);
    processor.set_alias_policy(policy);
    const int latency = processor.latency_samples();
    const int tail = processor.tail_samples();
    std::vector<double> impulse_response(static_cast<std::size_t>(tail + 2));
    impulse_response[0] = processor.process(1.0);
    for (int i = 1; i < tail + 2; ++i)
        impulse_response[static_cast<std::size_t>(i)] = processor.process(0.0);
    const auto peak =
        std::max_element(impulse_response.begin(), impulse_response.end(),
                         [](double a, double b) { return std::abs(a) < std::abs(b); });
    REQUIRE(std::distance(impulse_response.begin(), peak) == latency);
    REQUIRE(std::abs(impulse_response[static_cast<std::size_t>(tail)]) >
            std::numeric_limits<double>::min());
    REQUIRE(impulse_response.back() == 0.0);
}

} // namespace

TEST_CASE("nonlinear shapers expose an exact alias, latency, and tail contract",
          "[signal][nonlinear][contract]") {
    MultistageWavefolder64 folder;
    folder.prepare(kSampleRate);
    REQUIRE(folder.alias_policy() == NonlinearAliasPolicy::oversample_4x);
    REQUIRE(folder.oversample_factor() == 4);
    REQUIRE(folder.latency_samples() == 76);
    REQUIRE(folder.tail_samples() == 152);

    folder.set_alias_policy(NonlinearAliasPolicy::oversample_2x);
    REQUIRE(folder.oversample_factor() == 2);
    REQUIRE(folder.latency_samples() == 64);
    REQUIRE(folder.tail_samples() == 128);
    folder.set_alias_policy(NonlinearAliasPolicy::off);
    REQUIRE(folder.oversample_factor() == 1);
    REQUIRE(folder.latency_samples() == 0);
    REQUIRE(folder.tail_samples() == 0);

    ChebyshevHarmonicShaper shaper;
    NonlinearRingModulator ring;
    shaper.prepare(kSampleRate);
    ring.prepare(kSampleRate);
    REQUIRE(shaper.tail_samples() == 152);
    REQUIRE(ring.tail_samples() == 152);

    require_exact_linear_timing<MultistageWavefolder64>(NonlinearAliasPolicy::oversample_2x);
    require_exact_linear_timing<MultistageWavefolder64>(NonlinearAliasPolicy::oversample_4x);
}

TEST_CASE("nonlinear shaper configuration rejects invalid external values",
          "[signal][nonlinear][contract]") {
    MultistageWavefolder folder;
    folder.prepare(kSampleRate);
    folder.prepare(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(folder.sample_rate() == kSampleRate);
    folder.prepare(std::numeric_limits<double>::infinity());
    REQUIRE(folder.sample_rate() == kSampleRate);
    folder.prepare(999.0);
    REQUIRE(folder.sample_rate() == kSampleRate);
    folder.prepare(768001.0);
    REQUIRE(folder.sample_rate() == kSampleRate);

    folder.set_alias_policy(NonlinearAliasPolicy::off);
    folder.set_alias_policy(static_cast<NonlinearAliasPolicy>(255));
    REQUIRE(folder.alias_policy() == NonlinearAliasPolicy::off);
    REQUIRE(folder.oversample_factor() == 1);

    NonlinearRingModulator ring;
    ring.set_model(RingModulationModel::ideal_multiplier);
    ring.set_model(static_cast<RingModulationModel>(255));
    REQUIRE(ring.model() == RingModulationModel::ideal_multiplier);
    ring.set_carrier_waveform(RingCarrierWaveform::triangle);
    ring.set_carrier_waveform(static_cast<RingCarrierWaveform>(255));
    REQUIRE(ring.carrier_waveform() == RingCarrierWaveform::triangle);
    ring.set_carrier_mode(RingCarrierMode::unipolar);
    ring.set_carrier_mode(static_cast<RingCarrierMode>(255));
    REQUIRE(ring.carrier_mode() == RingCarrierMode::unipolar);
    ring.set_output_polarity(RingOutputPolarity::inverted);
    ring.set_output_polarity(static_cast<RingOutputPolarity>(255));
    REQUIRE(ring.output_polarity() == RingOutputPolarity::inverted);
}

TEST_CASE("wavefolder stages retain their stated DC and small-signal behavior",
          "[signal][nonlinear][wavefolder]") {
    MultistageWavefolder64 folder;
    folder.set_alias_policy(NonlinearAliasPolicy::off);
    folder.set_stages(4);
    folder.set_fold(0.6);

    REQUIRE(folder.dc_output() == 0.0);
    REQUIRE(folder.shape(0.0) == 0.0);
    constexpr double epsilon = 1.0e-8;
    REQUIRE_THAT(folder.shape(epsilon) / epsilon, WithinRel(folder.small_signal_gain(), 1.0e-10));
    REQUIRE(std::abs(folder.shape(1.0)) <= 1.0);

    MultistageWavefolder64 one_stage = folder;
    one_stage.set_stages(1);
    REQUIRE(std::abs(one_stage.shape(0.7) - folder.shape(0.7)) > 0.1);

    MultistageWavefolder64 controls;
    controls.set_alias_policy(NonlinearAliasPolicy::off);
    controls.set_stages(2);
    controls.set_fold(0.55);
    const double baseline_positive = controls.shape(0.45);
    const double baseline_negative = controls.shape(-0.45);
    controls.set_stage_offset(0, 0.2);
    REQUIRE(controls.shape(0.45) != baseline_positive);
    controls.set_stage_offset(0, 0.0);
    controls.set_stage_symmetry(0, 0.8);
    REQUIRE(std::abs(controls.shape(0.45) + controls.shape(-0.45)) > 0.05);
    REQUIRE(controls.shape(-0.45) != baseline_negative);

    controls.set_stage_symmetry(0, 0.0);
    controls.set_stage_offset(0, 0.3);
    controls.set_stage_offset(1, -0.2);
    controls.set_dc_coupling(1.0);
    REQUIRE(std::abs(controls.dc_output()) > 0.05);
    REQUIRE(controls.tail_samples() == -1);
    controls.set_dc_coupling(0.0);
    REQUIRE_THAT(controls.dc_output(), WithinAbs(0.0, 1.0e-15));
    REQUIRE(controls.tail_samples() == 0);
    controls.set_stage_dc_coupling(0, 1.0);
    const double first_stage_coupled = controls.dc_output();
    REQUIRE(std::abs(first_stage_coupled) > 0.05);
    REQUIRE(controls.stage_dc_coupling(0) == 1.0);
    REQUIRE(controls.stage_dc_coupling(1) == 0.0);
    controls.set_stage_dc_coupling(0, 0.0);
    controls.set_stage_dc_coupling(1, 1.0);
    const double second_stage_coupled = controls.dc_output();
    REQUIRE(std::abs(second_stage_coupled) > 0.05);
    REQUIRE(second_stage_coupled != first_stage_coupled);
}

TEST_CASE("Chebyshev coefficients select harmonics while silence remains silence",
          "[signal][nonlinear][chebyshev]") {
    ChebyshevHarmonicShaper64 shaper;
    shaper.set_alias_policy(NonlinearAliasPolicy::off);
    shaper.clear_harmonics();
    shaper.set_harmonic(2, 0.25);
    shaper.set_harmonic(3, 0.5);

    REQUIRE(shaper.dc_output() == 0.0);
    REQUIRE(shaper.shape(0.0) == 0.0);
    REQUIRE_THAT(shaper.small_signal_gain(), WithinAbs(-1.5, 1.0e-15));
    constexpr double epsilon = 1.0e-7;
    REQUIRE_THAT((shaper.shape(epsilon) - shaper.shape(-epsilon)) / (2.0 * epsilon),
                 WithinRel(shaper.small_signal_gain(), 1.0e-8));
    REQUIRE_THAT(shaper.worst_case_output(), WithinAbs(1.0, 1.0e-15));
    for (int step = -1000; step <= 1000; ++step)
        REQUIRE(std::abs(shaper.shape(static_cast<double>(step) / 1000.0)) <=
                shaper.worst_case_output() + 1.0e-12);

    shaper.clear_harmonics();
    shaper.set_harmonic(5, 0.7);
    const auto output = render(
        shaper, 4800,
        [](int sample) { return std::cos(2.0 * std::numbers::pi * 1000.0 * sample / kSampleRate); },
        0);
    REQUIRE_THAT(magnitude_at(output, 5000.0), WithinRel(0.7, 1.0e-10));
    REQUIRE(magnitude_at(output, 1000.0) < 1.0e-10);
}

TEST_CASE("ring modulation provides ideal and quasi-static diode-ring transfer laws",
          "[signal][nonlinear][ring-modulation]") {
    NonlinearRingModulator64 ring;
    ring.set_model(RingModulationModel::ideal_multiplier);
    ring.set_index(0.0);
    REQUIRE_THAT(ring.shape(0.4, -0.5), WithinAbs(0.4, 1.0e-15));
    ring.set_index(1.0);
    REQUIRE_THAT(ring.shape(0.4, -0.5), WithinAbs(-0.2, 1.0e-15));

    ring.set_model(RingModulationModel::diode_ring);
    ring.set_nonlinear_drive(2.5);
    const auto log_cosh = [](double value) {
        const double magnitude = std::abs(value);
        return magnitude + std::log1p(std::exp(-2.0 * magnitude)) - std::log(2.0);
    };
    const double expected =
        (log_cosh(2.5 * (-0.5 + 0.5 * 0.4)) - log_cosh(2.5 * (-0.5 - 0.5 * 0.4))) / 2.5;
    REQUIRE_THAT(ring.shape(0.4, -0.5), WithinAbs(expected, 1.0e-15));
    REQUIRE(ring.shape(0.0, 0.8) == 0.0);
    REQUIRE(ring.shape(0.4, 0.0) == 0.0);
    ring.set_nonlinear_drive(16.0);
    REQUIRE_THAT(ring.shape(0.2, 1.0), WithinAbs(0.2, 1.0e-7));
    REQUIRE_THAT(ring.shape(0.2, -1.0), WithinAbs(-0.2, 1.0e-7));

    ring.set_output_polarity(RingOutputPolarity::normal);
    const double normal = ring.shape(0.4, 0.5);
    ring.set_output_polarity(RingOutputPolarity::inverted);
    REQUIRE_THAT(ring.shape(0.4, 0.5), WithinAbs(-normal, 1.0e-15));
}

TEST_CASE("ring carrier defaults and an explicit sine selection both emit sine",
          "[signal][nonlinear][ring-modulation][contract]") {
    for (bool select_explicitly : {false, true}) {
        NonlinearRingModulator64 ring;
        ring.prepare(kSampleRate);
        ring.set_alias_policy(NonlinearAliasPolicy::off);
        ring.set_model(RingModulationModel::ideal_multiplier);
        ring.set_phase(0.25);
        if (select_explicitly)
            ring.set_carrier_waveform(RingCarrierWaveform::sine);
        REQUIRE(ring.carrier_waveform() == RingCarrierWaveform::sine);
        REQUIRE_THAT(ring.process(1.0), WithinAbs(1.0, 1.0e-15));
    }
}

TEST_CASE("ring carrier phase follows the input timeline through oversampling latency",
          "[signal][nonlinear][ring-modulation][timing]") {
    NonlinearRingModulator64 ring;
    ring.prepare(kSampleRate);
    ring.set_model(RingModulationModel::ideal_multiplier);
    ring.set_carrier_hz(kSampleRate / (2.0 * 76.0));
    ring.set_phase(0.25);
    REQUIRE_THAT(ring.phase(), WithinAbs(0.25, 1.0e-15));

    std::array<double, 153> response{};
    response[0] = ring.process(1.0);
    for (std::size_t i = 1; i < response.size(); ++i)
        response[i] = ring.process(0.0);
    REQUIRE(response[76] > 0.8);
}

TEST_CASE("bright nonlinear products are rejected before returning to base rate",
          "[signal][nonlinear][aliasing]") {
    ChebyshevHarmonicShaper64 raw_chebyshev;
    raw_chebyshev.clear_harmonics();
    raw_chebyshev.set_harmonic(9, 1.0);
    raw_chebyshev.set_alias_policy(NonlinearAliasPolicy::off);
    ChebyshevHarmonicShaper64 safe_chebyshev;
    safe_chebyshev.clear_harmonics();
    safe_chebyshev.set_harmonic(9, 1.0);
    safe_chebyshev.prepare(kSampleRate);

    const auto tone = [](int sample) {
        return std::cos(2.0 * std::numbers::pi * 4000.0 * sample / kSampleRate);
    };
    const auto raw = render(raw_chebyshev, 4800, tone, 0);
    const auto safe = render(safe_chebyshev, 4800, tone, 1024);
    const double raw_alias = magnitude_at(raw, 12000.0);
    const double safe_alias = magnitude_at(safe, 12000.0);
    REQUIRE(raw_alias > 0.9);
    REQUIRE(safe_alias < raw_alias * 0.01);

    NonlinearRingModulator64 raw_ring;
    raw_ring.prepare(kSampleRate);
    raw_ring.set_model(RingModulationModel::ideal_multiplier);
    raw_ring.set_carrier_hz(15000.0);
    raw_ring.set_alias_policy(NonlinearAliasPolicy::off);
    NonlinearRingModulator64 safe_ring;
    safe_ring.prepare(kSampleRate);
    safe_ring.set_model(RingModulationModel::ideal_multiplier);
    safe_ring.set_carrier_hz(15000.0);

    const auto bright = [](int sample) {
        return 0.5 * std::sin(2.0 * std::numbers::pi * 10000.0 * sample / kSampleRate) +
               0.5 * std::sin(2.0 * std::numbers::pi * 14000.0 * sample / kSampleRate);
    };
    const auto raw_modulated = render(raw_ring, 4800, bright, 0);
    const auto safe_modulated = render(safe_ring, 4800, bright, 1024);
    const double raw_folded_sideband =
        magnitude_at(raw_modulated, 19000.0) + magnitude_at(raw_modulated, 23000.0);
    const double safe_folded_sideband =
        magnitude_at(safe_modulated, 19000.0) + magnitude_at(safe_modulated, 23000.0);
    REQUIRE(raw_folded_sideband > 0.2);
    REQUIRE(safe_folded_sideband < raw_folded_sideband * 0.01);
    REQUIRE(magnitude_at(safe_modulated, 1000.0) > 0.1);
    REQUIRE(magnitude_at(safe_modulated, 5000.0) > 0.1);
}

TEST_CASE("ring carrier options are periodic, bandlimited, and mapped independently",
          "[signal][nonlinear][ring-modulation]") {
    for (RingCarrierWaveform waveform :
         {RingCarrierWaveform::sine, RingCarrierWaveform::triangle, RingCarrierWaveform::square}) {
        NonlinearRingModulator64 ring;
        ring.prepare(kSampleRate);
        ring.set_alias_policy(NonlinearAliasPolicy::off);
        ring.set_model(RingModulationModel::ideal_multiplier);
        ring.set_index(1.0);
        ring.set_carrier_hz(1000.0);
        ring.set_carrier_waveform(waveform);
        ring.set_carrier_mode(RingCarrierMode::bipolar);
        std::array<double, 144> bipolar{};
        bipolar.fill(1.0);
        ring.process_block(bipolar.data(), bipolar.data(), bipolar.size());
        for (std::size_t i = 96; i < bipolar.size(); ++i)
            REQUIRE_THAT(bipolar[i], WithinAbs(bipolar[i - 48], 1.0e-12));
        REQUIRE(*std::min_element(bipolar.begin() + 96, bipolar.end()) >= -1.1);
        REQUIRE(*std::max_element(bipolar.begin() + 96, bipolar.end()) <= 1.1);

        ring.reset();
        ring.set_carrier_mode(RingCarrierMode::unipolar);
        std::array<double, 144> unipolar{};
        unipolar.fill(1.0);
        ring.process_block(unipolar.data(), unipolar.data(), unipolar.size());
        double bipolar_mean = 0.0;
        double unipolar_mean = 0.0;
        for (std::size_t i = 96; i < bipolar.size(); ++i) {
            bipolar_mean += bipolar[i];
            unipolar_mean += unipolar[i];
            REQUIRE_THAT(unipolar[i], WithinAbs(0.5 * (bipolar[i] + 1.0), 1.0e-12));
        }
        REQUIRE(std::abs(bipolar_mean / 48.0) < 1.0e-12);
        REQUIRE_THAT(unipolar_mean / 48.0, WithinAbs(0.5, 1.0e-12));
    }
}

TEST_CASE("wavefolder bright-input residual spectrum benefits from its alias policy",
          "[signal][nonlinear][wavefolder][aliasing]") {
    MultistageWavefolder64 raw;
    raw.set_alias_policy(NonlinearAliasPolicy::off);
    raw.set_stages(3);
    raw.set_fold(0.8);
    MultistageWavefolder64 safe;
    safe.prepare(kSampleRate);
    safe.set_stages(3);
    safe.set_fold(0.8);

    const auto bright = [](int sample) {
        return 0.85 * std::sin(2.0 * std::numbers::pi * 7000.0 * sample / kSampleRate);
    };
    const auto raw_output = render(raw, 4800, bright, 0);
    const auto safe_output = render(safe, 4800, bright, 1024);
    const double raw_residual = magnitude_at(raw_output, 13000.0);
    const double safe_residual = magnitude_at(safe_output, 13000.0);
    REQUIRE(raw_residual > 1.0e-3);
    REQUIRE(safe_residual < raw_residual * 0.35);
}

TEST_CASE("nonlinear shapers recover from faults and ignore block partitioning",
          "[signal][nonlinear][determinism]") {
    MultistageWavefolder64 folder;
    folder.prepare(kSampleRate);
    folder.set_fold(0.65);
    require_partition_invariant(folder);
    require_fault_recovery(folder);

    ChebyshevHarmonicShaper64 shaper;
    shaper.prepare(kSampleRate);
    shaper.set_harmonic(5, 0.35);
    require_partition_invariant(shaper);
    require_fault_recovery(shaper);

    NonlinearRingModulator64 ring;
    ring.prepare(kSampleRate);
    ring.set_carrier_hz(713.0);
    require_partition_invariant(ring);
    require_fault_recovery(ring);
}

TEST_CASE("block processing supports in-place buffers and null inputs are no-ops",
          "[signal][nonlinear][contract]") {
    MultistageWavefolder64 folder;
    folder.set_alias_policy(NonlinearAliasPolicy::off);
    folder.set_fold(0.7);
    std::array<double, 5> input{-0.8, -0.2, 0.0, 0.3, 0.9};
    std::array<double, 5> expected{};
    folder.process_block(input.data(), expected.data(), expected.size());
    folder.reset();
    folder.process_block(input.data(), input.data(), input.size());
    REQUIRE(input == expected);

    folder.process_block(nullptr, input.data(), input.size());
    folder.process_block(input.data(), nullptr, input.size());
    folder.process_block(nullptr, nullptr, 0);

    ChebyshevHarmonicShaper64 shaper;
    shaper.set_alias_policy(NonlinearAliasPolicy::off);
    shaper.process_block(input.data(), input.data(), input.size());
    shaper.process_block(nullptr, input.data(), input.size());
    shaper.process_block(input.data(), nullptr, input.size());

    NonlinearRingModulator64 ring;
    ring.set_alias_policy(NonlinearAliasPolicy::off);
    ring.process_block(input.data(), input.data(), input.size());
    ring.process_block(nullptr, input.data(), input.size());
    ring.process_block(input.data(), nullptr, input.size());
}

TEST_CASE("alias policy changes reset filter and carrier phase deterministically",
          "[signal][nonlinear][determinism]") {
    NonlinearRingModulator64 ring;
    ring.prepare(kSampleRate);
    ring.set_carrier_hz(731.0);
    ring.set_phase(0.37);
    ring.set_alias_policy(NonlinearAliasPolicy::oversample_4x);
    REQUIRE_THAT(ring.phase(), WithinAbs(0.37, 1.0e-15));
    static_cast<void>(ring.process(0.5));
    ring.set_alias_policy(NonlinearAliasPolicy::oversample_2x);
    REQUIRE(ring.phase() == 0.0);

    NonlinearRingModulator64 reference;
    reference.prepare(kSampleRate);
    reference.set_carrier_hz(731.0);
    reference.set_alias_policy(NonlinearAliasPolicy::oversample_2x);
    for (int i = 0; i < 400; ++i) {
        const double input = std::sin(0.11 * static_cast<double>(i));
        REQUIRE(ring.process(input) == reference.process(input));
    }
}

TEST_CASE("ring waveform switches clear correction state and re-prepare restarts the carrier",
          "[signal][nonlinear][ring-modulation][determinism]") {
    NonlinearRingModulator64 switched;
    switched.prepare(kSampleRate);
    switched.set_alias_policy(NonlinearAliasPolicy::off);
    switched.set_carrier_hz(9000.0);
    switched.set_carrier_waveform(RingCarrierWaveform::square);
    for (int i = 0; i < 7; ++i)
        static_cast<void>(switched.process(1.0));
    const double switch_phase = switched.phase();
    switched.set_carrier_waveform(RingCarrierWaveform::triangle);
    REQUIRE_THAT(switched.phase(), WithinAbs(switch_phase, 1.0e-15));

    NonlinearRingModulator64 clean;
    clean.prepare(kSampleRate);
    clean.set_alias_policy(NonlinearAliasPolicy::off);
    clean.set_carrier_hz(9000.0);
    clean.set_carrier_waveform(RingCarrierWaveform::triangle);
    clean.set_phase(switch_phase);
    for (int i = 0; i < 32; ++i)
        REQUIRE(switched.process(1.0) == clean.process(1.0));

    switched.set_phase(0.37);
    switched.set_carrier_waveform(RingCarrierWaveform::triangle);
    REQUIRE_THAT(switched.phase(), WithinAbs(0.37, 1.0e-15));
    switched.prepare(kSampleRate);
    REQUIRE(switched.phase() == 0.0);

    NonlinearRingModulator64 restarted;
    restarted.prepare(kSampleRate);
    restarted.set_alias_policy(NonlinearAliasPolicy::off);
    restarted.set_carrier_hz(9000.0);
    restarted.set_carrier_waveform(RingCarrierWaveform::triangle);
    for (int i = 0; i < 32; ++i)
        REQUIRE(switched.process(1.0) == restarted.process(1.0));

    NonlinearRingModulator64 reclamped;
    reclamped.prepare(48000.0);
    reclamped.set_carrier_hz(20000.0);
    REQUIRE(reclamped.carrier_hz() == 20000.0);
    reclamped.prepare(8000.0);
    REQUIRE(reclamped.sample_rate() == 8000.0);
    REQUIRE(reclamped.carrier_hz() == 3920.0);
}

TEST_CASE("float and double nonlinear shapers remain finite and bounded",
          "[signal][nonlinear][precision]") {
    MultistageWavefolder float_folder;
    MultistageWavefolder64 double_folder;
    float_folder.set_alias_policy(NonlinearAliasPolicy::off);
    double_folder.set_alias_policy(NonlinearAliasPolicy::off);
    float_folder.set_fold(1.0);
    double_folder.set_fold(1.0);
    for (double input : {-1.0e12, -4.0, -0.1, 0.0, 0.1, 4.0, 1.0e12}) {
        const float single = float_folder.process(static_cast<float>(input));
        const double dual = double_folder.process(input);
        REQUIRE(std::isfinite(single));
        REQUIRE(std::isfinite(dual));
        REQUIRE(std::abs(single) <= 1.0f);
        REQUIRE(std::abs(dual) <= 1.0);
    }
    float_folder.set_stage_symmetry(0, 1.0);
    double_folder.set_stage_symmetry(0, 1.0);
    REQUIRE(std::isfinite(float_folder.shape(std::numeric_limits<float>::max())));
    REQUIRE(std::isfinite(double_folder.shape(std::numeric_limits<double>::max())));

    for (RingModulationModel model :
         {RingModulationModel::ideal_multiplier, RingModulationModel::diode_ring}) {
        NonlinearRingModulator64 ring;
        ring.set_model(model);
        ring.set_index(0.0);
        REQUIRE(ring.shape(std::numeric_limits<double>::max(), -1.0) ==
                std::numeric_limits<double>::max());
        ring.set_index(1.0);
        const double modulated = ring.shape(std::numeric_limits<double>::max(), -1.0);
        REQUIRE(std::isfinite(modulated));
        REQUIRE(modulated <= 0.0);

        NonlinearRingModulator float_ring;
        float_ring.set_model(model);
        float_ring.set_index(1.0);
        REQUIRE(std::isfinite(float_ring.shape(std::numeric_limits<float>::max(), -1.0)));
    }
}

TEST_CASE("nonlinear shaping audio paths allocate no memory", "[signal][nonlinear][rt-safety]") {
    MultistageWavefolder folder;
    ChebyshevHarmonicShaper shaper;
    NonlinearRingModulator ring;
    folder.prepare(kSampleRate);
    shaper.prepare(kSampleRate);
    ring.prepare(kSampleRate);
    std::array<float, 257> input{};
    std::array<float, 257> output{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = 0.5f * std::sin(0.1f * static_cast<float>(i));

    require_no_allocations([&] {
        folder.process_block(input.data(), output.data(), input.size());
        shaper.process_block(input.data(), output.data(), input.size());
        ring.process_block(input.data(), output.data(), input.size());
        folder.reset();
        shaper.reset();
        ring.reset();
    });
}
