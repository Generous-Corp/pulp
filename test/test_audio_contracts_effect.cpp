// test_audio_contracts_effect.cpp — PulpEffect contracts.
// Separate TU within pulp-test-audio-contracts because pulp_effect.hpp and
// pulp_gain.hpp define colliding unscoped enumerators (kBypass).
//
// Analyzer Determinism Contract: identical to test_audio_contracts.cpp —
// deterministic sine/silence stimulus (no seed needed), pure-arithmetic
// metrics over stated windows, zero-crossing pitch estimator, tolerance
// classes stated per expectation. Filter steady-state claims discard a
// stated 100 ms warm-up window before measuring.

#include <catch2/catch_test_macros.hpp>

#include "support/audio_contracts.hpp"

#include "pulp_effect.hpp"

using namespace pulp::test::audio;
using namespace pulp::examples;

namespace {

constexpr int kPartitionBlocks[] = {64, 128, 256};
constexpr double kSampleRate = 48000.0;
constexpr int kFrames = 24000;       // 500 ms render
constexpr std::size_t kWarmup = 4800; // 100 ms transient discarded

// Steady-state RMS facts after the warm-up window (metrics layer only).
BufferMetrics steady_metrics(const ScenarioResult& result) {
    return analyze(result.output.view().slice(kWarmup, kFrames - kWarmup),
                   result.sample_rate);
}

RenderScenario lowpass_scenario(const char* name, float cutoff_hz,
                                float stimulus_hz) {
    return RenderScenario(create_pulp_effect)
        .name(name)
        .sample_rate(kSampleRate)
        .block_size(256)
        .input(make_sine(2, kFrames, stimulus_hz, kSampleRate, 0.5f))
        .set_param(kFrequency, cutoff_hz)
        .set_param(kResonance, 0.707f)
        .set_param(kFilterType, 0.0f) // lowpass
        .set_param(kMix, 100.0f);
}

} // namespace

TEST_CASE("Contract: PulpEffect lowpass attenuates high frequencies",
          "[audio][contract][pulpeffect]") {
    // Golden semantics: a 200 Hz lowpass must drop an 8 kHz, 0.5-amp sine
    // (RMS −9.05 dBFS) by ≥ 20 dB in steady state — measured after the
    // stated 100 ms warm-up (numeric). The biquad is per-sample state with
    // no mid-render parameter changes, so partitioning is exact.
    auto scenario = lowpass_scenario("pulpeffect.lowpass-attenuates-highs",
                                     200.0f, 8000.0f);
    AudioContract contract("pulpeffect.lowpass-attenuates-highs", scenario);
    contract.expect(assert_rms_between(steady_metrics(contract.result()),
                                       kSilenceFloorDb, -29.05))
            .expect(expect_finite_and_unclipped(contract.result()))
            .expect(assert_block_partition_invariant(scenario,
                                                     kPartitionBlocks));
    const auto verdict = contract.verify();
    INFO(verdict.message);
    CHECK(verdict.passed);
}

TEST_CASE("Contract: PulpEffect lowpass preserves low frequencies",
          "[audio][contract][pulpeffect]") {
    // A 5 kHz lowpass passes a 100 Hz, 0.5-amp sine (RMS −9.05 dBFS)
    // within 1 dB in steady state (numeric; matches the golden suite's
    // "within ~2 dB" claim with margin).
    AudioContract contract(
        "pulpeffect.lowpass-preserves-lows",
        lowpass_scenario("pulpeffect.lowpass-preserves-lows", 5000.0f,
                         100.0f));
    contract.expect(assert_rms_between(steady_metrics(contract.result()),
                                       -10.05, -8.55))
            .expect(expect_finite_and_unclipped(contract.result()));
    const auto verdict = contract.verify();
    INFO(verdict.message);
    CHECK(verdict.passed);
}

TEST_CASE("Contract: PulpEffect bypass is transparent",
          "[audio][contract][pulpeffect]") {
    // Bypass copies samples and must win over an aggressive filter setup
    // (tolerance: exact).
    const auto input = make_sine(2, 9600, 1000.0f, kSampleRate, 0.5f);
    AudioContract contract(
        "pulpeffect.bypass",
        RenderScenario(create_pulp_effect)
            .name("pulpeffect.bypass")
            .sample_rate(kSampleRate)
            .block_size(128)
            .input(input)
            .set_param(kFrequency, 200.0f)
            .set_param(kResonance, 10.0f)
            .set_param(kBypass, 1.0f));
    contract.expect(expect_passthrough(contract.result(), input))
            .expect(expect_finite_and_unclipped(contract.result()));
    const auto verdict = contract.verify();
    INFO(verdict.message);
    CHECK(verdict.passed);
}

TEST_CASE("Contract: PulpEffect keeps silence silent",
          "[audio][contract][pulpeffect]") {
    // Zero input through a zero-state biquad stays exactly zero — no
    // self-noise, no denormal rumble (threshold −90 dBFS).
    AudioContract contract(
        "pulpeffect.silence",
        RenderScenario(create_pulp_effect)
            .name("pulpeffect.silence")
            .sample_rate(kSampleRate)
            .block_size(128)
            .input(make_silence(2, 9600)));
    contract.expect(expect_silence_preserved(contract.result(), -90.0));
    const auto verdict = contract.verify();
    INFO(verdict.message);
    CHECK(verdict.passed);
}
