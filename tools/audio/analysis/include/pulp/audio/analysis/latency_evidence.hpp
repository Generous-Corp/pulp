#pragma once

/// @file latency_evidence.hpp
/// Measured-versus-reported latency: the evidence schema and its pure evaluators.
///
/// A processor with nonzero latency tells its host a number
/// (`Processor::latency_samples()`), and the host slides the whole track by that
/// number to keep it aligned with everything else in the session. Nothing
/// verifies the number. If the true delay through the processor and the reported
/// delay drift apart — someone changes an FFT size, adds a filter stage, tweaks
/// an oversampling ratio — the plugin quietly misaligns against every other
/// track, and no test fails. This header makes "the reported latency is honest"
/// a machine-checkable claim.
///
/// **Report, measurement, and verdict are three separate things** and are never
/// collapsed into one another:
///
/// - `report_status` / `reported_samples` are FACTS read from the processor or
///   host. A supported zero is not the same as an unsupported query, which is
///   not the same as a failed one.
/// - `measurement_status` is a CONCLUSION drawn by an explicit probe policy.
/// - `contract_outcome` is the VERDICT, and it is the only field that gates.
///
/// **Ambiguity is never a pass.** A stimulus that cannot pin the delay down —
/// silence, a tone whose period is a whole number of samples (delays one period
/// apart then produce bit-identical output), an output that is not a delayed
/// copy of the input at all — yields `not_measurable` and `inconclusive`, never
/// `match`. Instruments, generators, and arbitrary nonlinear or time-varying
/// effects are `not_measurable` unless their author supplies a scenario that
/// makes the delay observable.
///
/// The guard is not blanket paranoia about tones: a 440 Hz sine at 48 kHz has a
/// 109.09-sample period, so no other integer delay reproduces it and the delay
/// IS determined. It is integer-sample periodicity that destroys the
/// information. A broadband, aperiodic stimulus (an impulse or noise) sidesteps
/// the question entirely and is what these policies want.
///
/// The evaluators here are pure functions over buffers: no processor, no host,
/// no I/O. Tests, the `pulp` CLI, and MCP all call these same functions and
/// serialize this same struct, so a verdict cannot mean one thing in a test and
/// another on the command line.
///
/// Test/tool layer only — never realtime-safe, never called from an audio
/// callback.

#include <pulp/audio/analysis/audio_metrics.hpp>
#include <pulp/audio/buffer.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace pulp::test::audio {

/// Whether a latency report could be read at all, and whether it was sane.
enum class LatencyReportStatus {
    available,     ///< A value was read. It may legitimately be zero.
    unsupported,   ///< This format/host cannot expose latency. NOT a zero.
    query_failed,  ///< The query was attempted and errored.
    invalid,       ///< A value was read but is nonsensical (e.g. negative).
};

/// Whether the report held still for the duration of the render.
enum class LatencyReportObservation {
    stable,        ///< Same value at every observation point.
    changed,       ///< The value moved, or a change was flagged, mid-render.
    unobservable,  ///< The surface offers no way to watch it.
};

/// How the report was watched while the render ran.
enum class LatencyObservationMode {
    processor_flag_and_poll,  ///< Direct Processor: poll + the latency-changed flag.
    per_block_poll,           ///< Hosted slot: poll between blocks (no flag surface).
    none,                     ///< Not watched.
};

/// The probe used to turn audio into a delay measurement.
enum class LatencyPolicy {
    none,
    /// Compare the output against the input delayed by D, sweeping D. Requires
    /// the processor to be in a declared identity / bypass / fully-dry mode:
    /// the output must actually BE a delayed copy of the input. This is the
    /// strongest policy — it checks every sample, not just an onset.
    delayed_passthrough_null,
    /// The author declares a unique input marker and any intrinsic response
    /// offset (e.g. leading silence in a known IR). Measures
    /// output_marker - input_marker - intrinsic_offset.
    marker_offset,
};

/// What the probe concluded.
enum class LatencyMeasurementStatus {
    match,           ///< Measured delay agrees with the report, within tolerance.
    mismatch,        ///< Measured delay disagrees with the report.
    not_measurable,  ///< The response cannot pin a delay down. See `reason`.
    not_requested,   ///< No measurement was asked for.
};

/// The gating verdict.
enum class LatencyContractOutcome {
    satisfied,      ///< Proven: the report matches the audio.
    violated,       ///< Disproven: they disagree.
    inconclusive,   ///< A measurement was requested but could not be drawn.
    not_requested,  ///< Neutral — nothing was claimed, so nothing failed.
};

/// One processor's latency evidence: the facts, the measurement, the verdict.
struct LatencyEvidence {
    // ── Facts read from the processor / host ────────────────────────────────
    LatencyReportStatus report_status = LatencyReportStatus::unsupported;
    /// The report at the start of the render. Null unless `report_status` is
    /// `available`. A present zero means "reports zero", not "unknown".
    std::optional<int> reported_samples;
    /// The report at the end of the render, when observable.
    std::optional<int> final_reported_samples;
    LatencyReportObservation report_observation = LatencyReportObservation::unobservable;
    LatencyObservationMode observation_mode = LatencyObservationMode::none;

    // ── The measurement ─────────────────────────────────────────────────────
    LatencyPolicy policy = LatencyPolicy::none;
    LatencyMeasurementStatus measurement_status = LatencyMeasurementStatus::not_requested;
    /// The delay actually observed in the audio, in samples.
    std::optional<std::int64_t> measured_samples;
    /// measured - reported. Null unless both are known.
    std::optional<std::int64_t> delta_samples;
    /// How far |delta| may stray and still count as a match.
    std::int64_t tolerance_samples = 0;

    // ── How much to trust the measurement ───────────────────────────────────
    //
    // Both are populated by the delayed-null policy only; the marker policy
    // finds an onset rather than nulling, so neither number exists for it.
    //
    // They are the difference between "the measurement passed" and "the
    // measurement passed, and here is how far it was from being a coin flip."
    // An agent reading the artifact should treat a 2 dB margin as a hair's
    // breadth from ambiguous even though it technically cleared the bar.

    /// Residual level at the winning delay, relative to the input, in dB. How
    /// completely the delayed input explains the output. A pure delay line
    /// nulls to the float noise floor; DSP that reconstructs the signal (an
    /// STFT, an oversampler) leaves a real residual and nulls less deeply.
    std::optional<double> null_depth_db;

    /// How much worse the best COMPETING delay nulled, in dB — the winner's
    /// margin of victory. Large means the delay is pinned; small means another
    /// delay explained the audio nearly as well, and the answer is a guess
    /// dressed up as a fact. Below `DelayedNullOptions::ambiguity_margin_db`
    /// the measurement is refused outright as `not_measurable`.
    std::optional<double> ambiguity_margin_db;

    /// An optional INTENDED latency, independent of what the processor reports.
    ///
    /// The rest of this struct proves self-consistency: the audio is delayed by
    /// exactly as much as the processor claims. That is the property a host
    /// cares about — a processor that reports 4096 and delays 4096 is compensated
    /// correctly and sounds right.
    ///
    /// But it is not the only thing that can regress. Someone doubles an FFT
    /// size, the true delay doubles, `latency_samples()` dutifully reports the
    /// new value, and self-consistency still holds — while your plugin quietly
    /// grew 43 ms of latency. Set this to pin the value itself and catch that.
    std::optional<int> expected_samples;

    // ── The verdict ─────────────────────────────────────────────────────────
    LatencyContractOutcome contract_outcome = LatencyContractOutcome::not_requested;
    /// Why the outcome is inconclusive, or why the measurement was not possible.
    /// Empty when the outcome is satisfied. This is the field a developer reads.
    std::string reason;

    /// True when the evidence must fail a gate: a disproven report, or a
    /// measurement that was asked for and could not be drawn. A requested
    /// measurement that comes back inconclusive is NOT a pass — an unprovable
    /// claim is a failed claim. `not_requested` is neutral and does not gate.
    bool gates_failure() const {
        return contract_outcome == LatencyContractOutcome::violated ||
               contract_outcome == LatencyContractOutcome::inconclusive;
    }
};

/// Knobs for `measure_delayed_passthrough`. The defaults are deliberately
/// conservative: they would rather refuse to measure than report a delay they
/// are not sure of.
struct DelayedNullOptions {
    /// Largest delay considered, in samples. The search window is
    /// [0, max_delay_samples]; a true delay beyond it reads as not_measurable.
    std::int64_t max_delay_samples = 8192;
    /// The residual at the best delay must be at least this far below the input
    /// level for the output to count as "a delayed copy of the input". If it is
    /// not, the processor is doing something other than passing audio through
    /// and the policy reports not_measurable rather than guessing.
    double null_floor_db = -60.0;
    /// The best delay must beat every non-adjacent competing delay by at least
    /// this margin. This is what stops a periodic stimulus (a pure sine nulls
    /// just as well one period late) from producing a confident wrong answer.
    double ambiguity_margin_db = 12.0;
    /// |measured - reported| may not exceed this and still be a match.
    std::int64_t tolerance_samples = 0;
};

/// Sweep the delay D that best explains `output` as a copy of `input`, and
/// compare it against `reported_samples`.
///
/// The processor must be in a declared identity / bypass / fully-dry mode, so
/// that its output genuinely is its input, delayed. Every sample is checked, so
/// a one-sample misreport fails.
///
/// Returns evidence with the policy, measurement, and verdict filled in.
/// `report_status` / `reported_samples` / observation fields are the caller's to
/// supply — this function does not touch a processor. When `reported_samples` is
/// null the delay is still measured and reported, but the outcome is
/// `inconclusive` (there is nothing to check it against).
LatencyEvidence measure_delayed_passthrough(const pulp::audio::Buffer<float>& input,
                                            const pulp::audio::Buffer<float>& output,
                                            std::optional<int> reported_samples,
                                            const DelayedNullOptions& options = {});

/// Knobs for `measure_marker_offset`.
struct MarkerOffsetOptions {
    /// Frame of the marker in the input. The input must have exactly one onset
    /// crossing `onset_threshold` — a second one makes the measurement ambiguous.
    std::int64_t input_marker_frame = 0;
    /// Delay the processor adds for reasons other than latency — e.g. leading
    /// silence in a known IR, or a known attack time. Subtracted from the raw
    /// offset before comparing against the report.
    std::int64_t intrinsic_response_offset = 0;
    /// |sample| at or above this linear level counts as the onset.
    double onset_threshold = 1e-3;
    std::int64_t tolerance_samples = 0;
};

/// Measure delay as `output_onset - input_marker - intrinsic_response_offset`
/// and compare it against `reported_samples`.
///
/// Weaker than the delayed-null policy — it checks one onset rather than every
/// sample — but it works for processors that reshape the signal (a convolver,
/// a filter) where a null comparison is meaningless. It refuses to measure when
/// the input marker is not unique or the output has no detectable onset.
LatencyEvidence measure_marker_offset(const pulp::audio::Buffer<float>& input,
                                      const pulp::audio::Buffer<float>& output,
                                      std::optional<int> reported_samples,
                                      const MarkerOffsetOptions& options = {});

/// Fold a report-observation fact into already-measured evidence.
///
/// A processor whose latency moved mid-render has no single "the" latency, so a
/// fixed-latency proof cannot stand even if the measured and reported values
/// happen to agree at the end — including when the value moves away and comes
/// back. Call this after a measurement to demote such a result to
/// `inconclusive`.
LatencyEvidence& apply_report_observation(LatencyEvidence& evidence);

/// Additionally require the latency to equal `expected`, pinning the value
/// rather than only its self-consistency. Violates a passing result whose
/// reported latency is not the intended one. Call after a measurement.
///
/// Use it to regression-pin a known-good latency: it is the only thing that
/// catches a processor whose true delay AND report both moved together.
LatencyEvidence& apply_expected_samples(LatencyEvidence& evidence, int expected,
                                        std::int64_t tolerance_samples = 0);

// ── Serialization ───────────────────────────────────────────────────────────
// One schema, shared by tests, the CLI, and MCP, so a verdict cannot drift
// between surfaces.

const char* to_string(LatencyReportStatus status);
const char* to_string(LatencyReportObservation observation);
const char* to_string(LatencyObservationMode mode);
const char* to_string(LatencyPolicy policy);
const char* to_string(LatencyMeasurementStatus status);
const char* to_string(LatencyContractOutcome outcome);

/// Schema version of the emitted object. Bump on any breaking field change.
inline constexpr int kLatencyEvidenceSchemaVersion = 1;

/// Serialize evidence to a JSON object string.
std::string latency_evidence_to_json(const LatencyEvidence& evidence);

/// One-line human summary, suitable for a test failure message.
/// e.g. "reported 512, measured 448 (delta -64) — VIOLATED".
std::string latency_evidence_summary(const LatencyEvidence& evidence);

} // namespace pulp::test::audio
