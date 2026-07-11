#pragma once

/// @file audio_contracts.hpp
/// Named audio contracts over RenderScenario.
///
/// A contract is a named, human-readable claim ("PulpGain bypass is
/// transparent") bound to one rendered scenario plus a list of
/// expectations. Failures carry the contract name, the expected-vs-actual
/// message of each failing expectation, the scenario facts, and a metrics
/// artifact path — per the harness plan's failure-message policy, a
/// developer reads the contract that broke, never a raw sample index.
///
/// @code
/// auto scenario = RenderScenario(create_my_effect)
///     .name("myeffect.bypass")
///     .input(stimulus)
///     .set_param(kBypass, 1.0f);
/// AudioContract contract("myeffect.bypass", scenario);
/// contract.expect(expect_passthrough(contract.result(), stimulus))
///         .expect(expect_finite_and_unclipped(contract.result()))
///         .expect(assert_block_partition_invariant(scenario, kBlocks));
/// const auto verdict = contract.verify();
/// INFO(verdict.message);
/// CHECK(verdict.passed);
/// @endcode
///
/// Expectations are plain `CheckResult`s, so the vocabulary is open: the
/// `expect_*` helpers below cover the contract families the examples
/// exercise, the lower-layer `assert_*` helpers slot in directly (block
/// partition invariance IS `assert_block_partition_invariant`), and a
/// hand-built CheckResult expresses anything else.
///
/// Layering (see README.md): contracts are the top layer, above scenarios.
/// Nothing below may include this header. No new DSP or measurement code
/// lives here — every helper composes existing metrics/assertions.

#include <pulp/audio/analysis/audio_artifacts.hpp>

#include "render_scenario.hpp"

namespace pulp::test::audio {

/// One named claim over one rendered scenario.
class AudioContract {
public:
    /// Render `scenario` once and bind the result to the claim `name`.
    AudioContract(std::string name, const RenderScenario& scenario)
        : name_(std::move(name)), result_(scenario.render()) {}

    /// Bind an already-rendered result (e.g. when one render feeds
    /// several contracts, or the contract compares two renders).
    AudioContract(std::string name, ScenarioResult result)
        : name_(std::move(name)), result_(std::move(result)) {}

    const std::string& name() const { return name_; }
    /// The rendered scenario the expectations are evaluated against.
    const ScenarioResult& result() const { return result_; }

    /// Accumulate one evaluated expectation. Chainable.
    AudioContract& expect(CheckResult check) {
        checks_.push_back(std::move(check));
        return *this;
    }

    /// All-or-nothing verdict. On failure the message lists every failing
    /// expectation prefixed `contract '<name>':`, then the scenario facts
    /// and the path of the metrics artifact written for this run. A
    /// contract with zero expectations fails (vacuous claims are bugs).
    CheckResult verify() const;

private:
    std::string name_;
    ScenarioResult result_;
    std::vector<CheckResult> checks_;
};

// ── Contract-family helpers ─────────────────────────────────────────────
// Only the families the examples exercise. Each composes existing
// assert_* / metrics helpers; tolerances are explicit parameters per the
// plan's Threshold Policy (class named at each call site).

/// Pass-through transparency (unity-settings and bypass claims): the
/// rendered output nulls against the input stimulus within
/// `tolerance_dbfs`. `input` must have the same shape as the render
/// (scenario duration defaulted from this buffer guarantees that).
CheckResult expect_passthrough(const ScenarioResult& result,
                               const pulp::audio::Buffer<float>& input,
                               double tolerance_dbfs = kExactPartitionToleranceDb);

/// Silence-in-silence-out (effects fed a silent stimulus) and
/// silent-without-events (instruments given no MIDI): the whole render is
/// silent below `threshold_dbfs` and free of NaN/Inf.
CheckResult expect_silence_preserved(const ScenarioResult& result,
                                     double threshold_dbfs = -90.0);

/// Claim parameters for expect_tone(). The window should cover the held
/// portion of the note — exclude attack/release per the stated claim.
struct ToneClaim {
    std::int64_t window_start = 0;   ///< First frame of the held window.
    std::int64_t window_frames = 0;  ///< Window length in frames.
    double expected_hz = 0.0;        ///< Claimed pitch.
    double tolerance_cents = 5.0;    ///< Numeric tolerance class.
    double min_rms_dbfs = -60.0;     ///< Amplitude-range claim (window RMS).
    double max_rms_dbfs = 0.0;
};

/// Generator/instrument tone claim: over the held window the output is
/// non-silent, its RMS lies in [min, max] dBFS, and the channel-0
/// zero-crossing pitch estimate is within `tolerance_cents` of
/// `expected_hz` (estimator limits documented on `estimate_frequency`).
CheckResult expect_tone(const ScenarioResult& result, const ToneClaim& claim);

/// Output hygiene: no NaN/Inf anywhere and no channel peak at or above
/// `ceiling_dbfs`.
CheckResult expect_finite_and_unclipped(const ScenarioResult& result,
                                        double ceiling_dbfs = -0.1);

// ── Reported-latency proof ──────────────────────────────────────────────
// A processor with nonzero latency tells the host a number, and the host
// slides the whole track by it. Nothing checks the number. These contracts
// do: they measure the delay actually present in the rendered audio and
// compare it against what the processor reported.
//
// Both are opt-in and neither ever guesses. A stimulus that cannot pin the
// delay down — silence, a pure sine, an output that is not a delayed copy
// of the input — comes back inconclusive, and an inconclusive contract that
// was ASKED for is a failure, not a pass. See latency_evidence.hpp.

/// Prove the reported latency against a delayed-null comparison — the
/// strongest policy, because it checks every sample rather than one onset.
///
/// Requires the processor to be in a declared identity / bypass / fully-dry
/// mode for this render, so its output genuinely IS its input, delayed. Feed
/// it a BROADBAND stimulus (an impulse or noise). A tone whose period is a
/// whole number of samples cannot pin a delay down — delays one period apart
/// produce bit-identical output — and is rejected as ambiguous by design
/// rather than producing a confident wrong answer.
///
/// @code
/// auto scenario = RenderScenario(create_my_lookahead_limiter)
///     .name("limiter.latency")
///     .input(make_noise(2, 48000, /*seed=*/1))
///     .set_param(kBypass, 1.0f);          // declared identity mode
/// AudioContract contract("limiter reports its true latency", scenario);
/// contract.expect(expect_reported_latency(contract.result()));
/// CHECK(contract.verify().passed);
/// @endcode
CheckResult expect_reported_latency(const ScenarioResult& result,
                                    const DelayedNullOptions& options = {});

/// Prove the reported latency from a single input marker — for processors
/// that reshape the signal (a convolver, a filter) where nulling against the
/// input is meaningless. Weaker than the delayed-null policy: it checks one
/// onset, not every sample.
///
/// The stimulus must contain exactly one onset, at `options.input_marker_frame`.
/// Declare any delay the processor adds for reasons other than latency (leading
/// silence in a known IR, a known attack) in `options.intrinsic_response_offset`.
CheckResult expect_reported_latency_from_marker(const ScenarioResult& result,
                                                const MarkerOffsetOptions& options);

/// The structured evidence behind `expect_reported_latency()` — the report
/// facts, the measurement, and the verdict — for a caller that wants to inspect
/// or serialize it rather than assert on a pass/fail.
LatencyEvidence evaluate_reported_latency(const ScenarioResult& result,
                                          const DelayedNullOptions& options = {});

/// @copydoc evaluate_reported_latency
LatencyEvidence evaluate_reported_latency_from_marker(
    const ScenarioResult& result, const MarkerOffsetOptions& options);

} // namespace pulp::test::audio
