// test_audio_contracts.cpp — named contracts for the example plugins
// (PulpGain effect + PulpTone instrument here; PulpEffect lives in
// test_audio_contracts_effect.cpp because pulp_gain.hpp and pulp_effect.hpp
// define colliding unscoped enumerators — same reason the golden suites are
// split). Acceptance: a new effect copies one of these fixtures; failures
// name the contract that broke.
//
// Analyzer Determinism Contract (uniform for every contract in this TU):
// - stimulus: documented deterministic generators (sine / silence / MIDI
//   note scripts); no noise, so no seed; no random_device, no clocks
// - analysis: pure-arithmetic audio_metrics (no FFT, no windowing); the
//   pitch estimator is the documented zero-crossing interpolator; no
//   warm-up trim except where a held window is stated in the contract
// - DC removal: none (DC is part of the measured facts)
// - sample rate / block size / channels: stated per scenario
// - tolerance classes: stated at each expectation (exact = bit-identical
//   via kExactPartitionToleranceDb, numeric = stated dB/cents bounds)

#include <catch2/catch_test_macros.hpp>

#include "support/audio_contracts.hpp"

#include <filesystem>
#include <sstream>

#include "pulp_gain.hpp"
#include "pulp_tone.hpp"

using namespace pulp::test::audio;

namespace {

constexpr int kPartitionBlocks[] = {64, 128, 256};

// One-line windowed RMS via the metrics layer (no duplicated math).
double window_rms_dbfs(const ScenarioResult& result, std::size_t start,
                       std::size_t length) {
    return analyze(result.output.view().slice(start, length),
                   result.sample_rate)
        .channels[0]
        .rms_dbfs();
}

} // namespace

TEST_CASE("AudioContract verdict mechanics",
          "[audio][contract]") {
    auto scenario = RenderScenario(pulp::examples::create_pulp_gain)
        .name("contract-meta")
        .sample_rate(48000.0)
        .block_size(128)
        .input(make_sine(2, 4800, 440.0f, 48000.0, 0.5f));

    SECTION("failures carry the contract name, scenario facts, artifact") {
        AudioContract contract("meta.failing", scenario);
        contract.expect(assert_silent(contract.result().metrics, -90.0))
                .expect(assert_no_nan_inf(contract.result().metrics));
        const auto verdict = contract.verify();
        REQUIRE_FALSE(verdict.passed);
        // Only the failing expectation is listed, prefixed with the name.
        CHECK(verdict.message.find("contract 'meta.failing': expected"
                                   " silence") != std::string::npos);
        CHECK(verdict.message.find("no NaN/Inf") == std::string::npos);
        CHECK(verdict.message.find("scenario: contract-meta sr=48000"
                                   " block=128 in=2 out=2 frames=4800") !=
              std::string::npos);
        const auto marker = verdict.message.find("artifact: ");
        REQUIRE(marker != std::string::npos);
        const auto path = verdict.message.substr(marker + 10);
        CHECK(std::filesystem::exists(path));
    }

    SECTION("passing contracts summarize, zero expectations fail") {
        AudioContract passing("meta.passing", scenario);
        passing.expect(assert_not_silent(passing.result().metrics, -60.0));
        const auto verdict = passing.verify();
        CHECK(verdict.passed);
        CHECK(verdict.message.find("contract 'meta.passing': 1") !=
              std::string::npos);

        CHECK_FALSE(AudioContract("meta.vacuous", scenario).verify().passed);
    }
}

TEST_CASE("Contract: PulpGain unity settings are transparent",
          "[audio][contract][pulpgain]") {
    // Default gains (0 dB in / 0 dB out) multiply by exactly 1.0f, so the
    // output must be bit-identical to the stimulus (tolerance: exact).
    const auto input = make_sine(2, 9600, 440.0f, 48000.0, 0.5f);
    AudioContract contract(
        "pulpgain.unity",
        RenderScenario(pulp::examples::create_pulp_gain)
            .name("pulpgain.unity")
            .sample_rate(48000.0)
            .block_size(128)
            .input(input));
    contract.expect(expect_passthrough(contract.result(), input))
            .expect(expect_finite_and_unclipped(contract.result()));
    const auto verdict = contract.verify();
    INFO(verdict.message);
    CHECK(verdict.passed);
}

TEST_CASE("Contract: PulpGain scales level by the configured gain",
          "[audio][contract][pulpgain]") {
    // -12 dBFS-peak sine has RMS -15.05 dBFS; -6 dB input gain and -6 dB
    // output gain compose to -12 dB → RMS -27.05 dBFS (numeric ±0.5 dB).
    // Gain is stateless per sample, so partitioning is exact.
    auto scenario = RenderScenario(pulp::examples::create_pulp_gain)
        .name("pulpgain.gain-scaling")
        .sample_rate(48000.0)
        .block_size(128)
        .input(make_sine(2, 9600, 440.0f, 48000.0,
                         static_cast<float>(from_dbfs(-12.0))))
        .set_param(pulp::examples::kInputGain, -6.0f)
        .set_param(pulp::examples::kOutputGain, -6.0f);
    AudioContract contract("pulpgain.gain-scaling", scenario);
    contract.expect(assert_rms_between(contract.result().metrics,
                                       -27.55, -26.55))
            .expect(expect_finite_and_unclipped(contract.result()))
            .expect(assert_block_partition_invariant(scenario,
                                                     kPartitionBlocks));
    const auto verdict = contract.verify();
    INFO(verdict.message);
    CHECK(verdict.passed);
}

TEST_CASE("Contract: PulpGain bypass is transparent",
          "[audio][contract][pulpgain]") {
    // Bypass copies samples and must win over extreme gain settings
    // (tolerance: exact).
    const auto input = make_sine(2, 9600, 220.0f, 48000.0, 0.5f);
    AudioContract contract(
        "pulpgain.bypass",
        RenderScenario(pulp::examples::create_pulp_gain)
            .name("pulpgain.bypass")
            .sample_rate(48000.0)
            .block_size(128)
            .input(input)
            .set_param(pulp::examples::kInputGain, 12.0f)
            .set_param(pulp::examples::kOutputGain, 12.0f)
            .set_param(pulp::examples::kBypass, 1.0f));
    contract.expect(expect_passthrough(contract.result(), input))
            .expect(expect_finite_and_unclipped(contract.result()));
    const auto verdict = contract.verify();
    INFO(verdict.message);
    CHECK(verdict.passed);
}

TEST_CASE("Contract: PulpGain keeps silence silent",
          "[audio][contract][pulpgain]") {
    // Silence in → silence out: a gain stage adds no self-noise even at
    // +24 dB (threshold −90 dBFS, the metrics silence floor).
    AudioContract contract(
        "pulpgain.silence",
        RenderScenario(pulp::examples::create_pulp_gain)
            .name("pulpgain.silence")
            .sample_rate(48000.0)
            .block_size(128)
            .input(make_silence(2, 9600))
            .set_param(pulp::examples::kInputGain, 24.0f));
    contract.expect(expect_silence_preserved(contract.result(), -90.0));
    const auto verdict = contract.verify();
    INFO(verdict.message);
    CHECK(verdict.passed);
}

TEST_CASE("Contract: PulpTone note-on produces the claimed tone",
          "[audio][contract][pulptone]") {
    // A4 (note 69, velocity 100) at default −6 dB volume: amplitude
    // 0.5012 · (100/127) ≈ 0.395 → held RMS ≈ −11.2 dBFS including the
    // 10 ms attack (numeric ±1.3 dB window); pitch 440 Hz ±5 cents.
    // Per-sample voice state with no mid-render events partitions exactly.
    auto scenario = RenderScenario(pulp::examples::create_pulp_tone)
        .name("pulptone.note-a4")
        .sample_rate(48000.0)
        .block_size(128)
        .channels(0, 2)
        .duration_ms(400.0)
        .midi(make_note_script(69, 100, 0, 9600));
    AudioContract contract("pulptone.note-a4", scenario);
    contract.expect(expect_tone(contract.result(),
                                {.window_start = 0,
                                 .window_frames = 9600,
                                 .expected_hz = 440.0,
                                 .tolerance_cents = 5.0,
                                 .min_rms_dbfs = -12.5,
                                 .max_rms_dbfs = -10.5}))
            .expect(expect_finite_and_unclipped(contract.result()))
            .expect(assert_block_partition_invariant(scenario,
                                                     kPartitionBlocks));
    const auto verdict = contract.verify();
    INFO(verdict.message);
    CHECK(verdict.passed);
}

TEST_CASE("Contract: PulpTone is silent without MIDI",
          "[audio][contract][pulptone]") {
    // An instrument given no events must not self-oscillate or leak
    // voice state (threshold −90 dBFS).
    AudioContract contract(
        "pulptone.silent-without-midi",
        RenderScenario(pulp::examples::create_pulp_tone)
            .name("pulptone.silent-without-midi")
            .sample_rate(48000.0)
            .block_size(128)
            .channels(0, 2)
            .duration_ms(200.0));
    contract.expect(expect_silence_preserved(contract.result(), -90.0));
    const auto verdict = contract.verify();
    INFO(verdict.message);
    CHECK(verdict.passed);
}

TEST_CASE("Contract: PulpTone release tail decays",
          "[audio][contract][pulptone]") {
    // Note-off at 200 ms with the default 200 ms release: the final 50 ms
    // of a 400 ms render must sit ≥ 20 dB below the held level (numeric;
    // a hand-built CheckResult — the vocabulary is open to custom claims).
    AudioContract contract(
        "pulptone.release-decays",
        RenderScenario(pulp::examples::create_pulp_tone)
            .name("pulptone.release-decays")
            .sample_rate(48000.0)
            .block_size(128)
            .channels(0, 2)
            .duration_ms(400.0)
            .midi(make_note_script(69, 100, 0, 9600)));
    const double held = window_rms_dbfs(contract.result(), 0, 9600);
    const double tail = window_rms_dbfs(contract.result(), 16800, 2400);
    std::ostringstream decay;
    decay << "release decay: held RMS " << held << " dBFS, final-50 ms tail "
          << tail << " dBFS, required drop >= 20 dB";
    contract.expect({tail < held - 20.0, decay.str()})
            .expect(expect_finite_and_unclipped(contract.result()));
    const auto verdict = contract.verify();
    INFO(verdict.message);
    CHECK(verdict.passed);
}
