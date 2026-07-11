// latency_evidence.cpp — pure measured-versus-reported latency evaluators.
// See latency_evidence.hpp for the contract. The rule that shapes every branch
// here: a delay this code is not sure of is `not_measurable`, never a match.

#include <pulp/audio/analysis/latency_evidence.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

namespace pulp::test::audio {

namespace {

// Shortest common shape of two buffers — the region where a comparison is
// meaningful at all.
struct CommonShape {
    std::size_t channels = 0;
    std::int64_t frames = 0;
};

CommonShape common_shape(const pulp::audio::Buffer<float>& a,
                         const pulp::audio::Buffer<float>& b) {
    CommonShape s;
    s.channels = std::min(a.num_channels(), b.num_channels());
    s.frames = static_cast<std::int64_t>(
        std::min(a.num_samples(), b.num_samples()));
    return s;
}

// RMS across all channels of `buffer` over [0, frames).
double buffer_rms(const pulp::audio::Buffer<float>& buffer,
                  std::size_t channels, std::int64_t frames) {
    if (channels == 0 || frames <= 0) return 0.0;
    double acc = 0.0;
    for (std::size_t ch = 0; ch < channels; ++ch) {
        const auto samples = buffer.channel(ch);
        for (std::int64_t n = 0; n < frames; ++n) {
            const double v = static_cast<double>(samples[static_cast<std::size_t>(n)]);
            acc += v * v;
        }
    }
    return std::sqrt(acc / (static_cast<double>(frames) * static_cast<double>(channels)));
}

// Any NaN/Inf makes every downstream number meaningless.
bool has_non_finite(const pulp::audio::Buffer<float>& buffer,
                    std::size_t channels, std::int64_t frames) {
    for (std::size_t ch = 0; ch < channels; ++ch) {
        const auto samples = buffer.channel(ch);
        for (std::int64_t n = 0; n < frames; ++n)
            if (!std::isfinite(samples[static_cast<std::size_t>(n)])) return true;
    }
    return false;
}

LatencyEvidence not_measurable(LatencyPolicy policy, std::string reason,
                               std::optional<int> reported) {
    LatencyEvidence e;
    e.policy = policy;
    e.reported_samples = reported;
    e.measurement_status = LatencyMeasurementStatus::not_measurable;
    e.contract_outcome = LatencyContractOutcome::inconclusive;
    e.reason = std::move(reason);
    return e;
}

// Compare a measured delay against the report and settle the verdict. Shared by
// both policies so `match` can never mean two different things.
void settle_verdict(LatencyEvidence& e, std::int64_t measured,
                    std::optional<int> reported, std::int64_t tolerance) {
    e.measured_samples = measured;
    e.reported_samples = reported;
    e.tolerance_samples = tolerance;

    if (!reported.has_value()) {
        e.measurement_status = LatencyMeasurementStatus::not_measurable;
        e.contract_outcome = LatencyContractOutcome::inconclusive;
        e.reason = "the delay was measured but there is no latency report to "
                   "check it against";
        return;
    }
    if (*reported < 0) {
        e.measurement_status = LatencyMeasurementStatus::not_measurable;
        e.contract_outcome = LatencyContractOutcome::inconclusive;
        e.reason = "latency report is negative (" + std::to_string(*reported) +
                   "), which is not a valid delay";
        return;
    }

    const std::int64_t delta = measured - static_cast<std::int64_t>(*reported);
    e.delta_samples = delta;
    if (std::llabs(static_cast<long long>(delta)) <= tolerance) {
        e.measurement_status = LatencyMeasurementStatus::match;
        e.contract_outcome = LatencyContractOutcome::satisfied;
        return;
    }
    e.measurement_status = LatencyMeasurementStatus::mismatch;
    e.contract_outcome = LatencyContractOutcome::violated;
    std::ostringstream why;
    why << "reported " << *reported << " samples of latency but the audio is "
        << "delayed by " << measured << " (delta " << (delta > 0 ? "+" : "")
        << delta << ", tolerance " << tolerance << ")";
    e.reason = why.str();
}

} // namespace

LatencyEvidence measure_delayed_passthrough(const pulp::audio::Buffer<float>& input,
                                            const pulp::audio::Buffer<float>& output,
                                            std::optional<int> reported_samples,
                                            const DelayedNullOptions& options) {
    constexpr auto kPolicy = LatencyPolicy::delayed_passthrough_null;
    const auto shape = common_shape(input, output);

    if (shape.channels == 0 || shape.frames <= 0)
        return not_measurable(kPolicy, "input or output buffer is empty",
                              reported_samples);

    if (has_non_finite(output, shape.channels, shape.frames))
        return not_measurable(kPolicy,
                              "output contains NaN or Inf; no delay can be "
                              "measured from it",
                              reported_samples);

    // The search window must be IDENTICAL for every candidate delay, or a large
    // delay would be scored over fewer (or easier) samples than a small one and
    // the comparison would be biased. Start every candidate's window at
    // max_delay so all of them read `input[n - D]` from valid frames.
    const std::int64_t max_delay =
        std::min(options.max_delay_samples, shape.frames - 1);
    if (max_delay < 0)
        return not_measurable(kPolicy, "render is too short to measure a delay",
                              reported_samples);

    const std::int64_t window_start = max_delay;
    const std::int64_t window_frames = shape.frames - window_start;
    constexpr std::int64_t kMinWindowFrames = 64;
    if (window_frames < kMinWindowFrames) {
        std::ostringstream why;
        why << "render is too short for the delay search: " << window_frames
            << " frames remain after reserving " << max_delay
            << " for the search window (need at least " << kMinWindowFrames
            << "). Render longer, or lower max_delay_samples.";
        return not_measurable(kPolicy, why.str(), reported_samples);
    }

    const double input_rms = buffer_rms(input, shape.channels, shape.frames);
    if (to_dbfs(input_rms) <= -90.0)
        return not_measurable(kPolicy,
                              "input stimulus is silent; a silent input cannot "
                              "reveal a delay",
                              reported_samples);

    // Residual of output against input delayed by D, relative to the input level.
    // Lower is a better explanation of the output.
    const auto residual_db_at = [&](std::int64_t delay) {
        double acc = 0.0;
        for (std::size_t ch = 0; ch < shape.channels; ++ch) {
            const auto in_ch = input.channel(ch);
            const auto out_ch = output.channel(ch);
            for (std::int64_t n = window_start; n < shape.frames; ++n) {
                const double d =
                    static_cast<double>(out_ch[static_cast<std::size_t>(n)]) -
                    static_cast<double>(in_ch[static_cast<std::size_t>(n - delay)]);
                acc += d * d;
            }
        }
        const double residual_rms = std::sqrt(
            acc / (static_cast<double>(window_frames) *
                   static_cast<double>(shape.channels)));
        return to_dbfs(residual_rms / input_rms);
    };

    std::int64_t best_delay = 0;
    double best_db = std::numeric_limits<double>::infinity();
    std::vector<double> residuals(static_cast<std::size_t>(max_delay) + 1);
    for (std::int64_t d = 0; d <= max_delay; ++d) {
        const double db = residual_db_at(d);
        residuals[static_cast<std::size_t>(d)] = db;
        if (db < best_db) {
            best_db = db;
            best_delay = d;
        }
    }

    // Is the output a delayed copy of the input AT ALL? If the best delay still
    // leaves a large residual, this processor is not passing audio through and
    // the policy does not apply. Say so rather than reporting the least-bad
    // delay as if it were the answer.
    if (best_db > options.null_floor_db) {
        std::ostringstream why;
        why << "output is not a delayed copy of the input (best residual "
            << best_db << " dB at delay " << best_delay << ", needs <= "
            << options.null_floor_db
            << " dB). The delayed-null policy requires the processor to be in a "
               "declared identity / bypass / fully-dry mode.";
        return not_measurable(kPolicy, why.str(), reported_samples);
    }

    // Could a DIFFERENT delay explain the output just as well? A periodic
    // stimulus (a pure sine nulls just as cleanly one period late) makes the
    // delay genuinely unknowable, and a confident wrong answer here is worse
    // than no answer. Ignore the immediate neighbours of the winner — those are
    // the same peak, not a competing one.
    double competitor_db = std::numeric_limits<double>::infinity();
    std::int64_t competitor_delay = -1;
    for (std::int64_t d = 0; d <= max_delay; ++d) {
        if (std::llabs(static_cast<long long>(d - best_delay)) <= 1) continue;
        if (residuals[static_cast<std::size_t>(d)] < competitor_db) {
            competitor_db = residuals[static_cast<std::size_t>(d)];
            competitor_delay = d;
        }
    }
    if (competitor_delay >= 0 &&
        competitor_db - best_db < options.ambiguity_margin_db) {
        std::ostringstream why;
        why << "delay is ambiguous: " << best_delay << " nulls to " << best_db
            << " dB but " << competitor_delay << " nulls to " << competitor_db
            << " dB, within the " << options.ambiguity_margin_db
            << " dB margin. A periodic stimulus cannot pin a delay down — use a "
               "broadband, aperiodic one (an impulse or noise).";
        return not_measurable(kPolicy, why.str(), reported_samples);
    }

    LatencyEvidence e;
    e.policy = kPolicy;
    settle_verdict(e, best_delay, reported_samples, options.tolerance_samples);
    return e;
}

LatencyEvidence measure_marker_offset(const pulp::audio::Buffer<float>& input,
                                      const pulp::audio::Buffer<float>& output,
                                      std::optional<int> reported_samples,
                                      const MarkerOffsetOptions& options) {
    constexpr auto kPolicy = LatencyPolicy::marker_offset;
    const auto shape = common_shape(input, output);

    if (shape.channels == 0 || shape.frames <= 0)
        return not_measurable(kPolicy, "input or output buffer is empty",
                              reported_samples);

    if (has_non_finite(output, shape.channels, shape.frames))
        return not_measurable(kPolicy,
                              "output contains NaN or Inf; no onset can be "
                              "located in it",
                              reported_samples);

    const float threshold = static_cast<float>(options.onset_threshold);

    // Peak across channels at frame n — the marker may live on any channel.
    const auto peak_at = [&](const pulp::audio::Buffer<float>& buffer,
                             std::int64_t n) {
        float peak = 0.0f;
        for (std::size_t ch = 0; ch < shape.channels; ++ch)
            peak = std::max(peak, std::fabs(
                buffer.channel(ch)[static_cast<std::size_t>(n)]));
        return peak;
    };

    // Count rising edges through the threshold. Exactly one is required: a
    // second marker makes "which onset is the response?" unanswerable.
    std::int64_t input_onsets = 0;
    std::int64_t first_input_onset = -1;
    bool above = false;
    for (std::int64_t n = 0; n < shape.frames; ++n) {
        const bool now_above = peak_at(input, n) >= threshold;
        if (now_above && !above) {
            ++input_onsets;
            if (first_input_onset < 0) first_input_onset = n;
        }
        above = now_above;
    }

    if (input_onsets == 0)
        return not_measurable(kPolicy,
                              "no marker found in the input above the onset "
                              "threshold",
                              reported_samples);
    if (input_onsets > 1) {
        std::ostringstream why;
        why << "input marker is not unique: " << input_onsets
            << " onsets cross the threshold, so the output onset cannot be "
               "attributed to one of them. Use a single-marker stimulus.";
        return not_measurable(kPolicy, why.str(), reported_samples);
    }

    // The author DECLARED where the marker is. If the stimulus disagrees, the
    // scenario is misconfigured and every number downstream would be wrong.
    if (first_input_onset != options.input_marker_frame) {
        std::ostringstream why;
        why << "declared input_marker_frame is " << options.input_marker_frame
            << " but the input's only onset is at frame " << first_input_onset
            << "; the scenario and the stimulus disagree";
        return not_measurable(kPolicy, why.str(), reported_samples);
    }

    std::int64_t output_onset = -1;
    for (std::int64_t n = 0; n < shape.frames; ++n) {
        if (peak_at(output, n) >= threshold) {
            output_onset = n;
            break;
        }
    }
    if (output_onset < 0)
        return not_measurable(kPolicy,
                              "output has no onset above the threshold — the "
                              "processor may be silent, or it suppressed the "
                              "marker (a nonlinear or gating processor cannot "
                              "be measured this way)",
                              reported_samples);

    const std::int64_t measured = output_onset - options.input_marker_frame -
                                  options.intrinsic_response_offset;
    if (measured < 0) {
        std::ostringstream why;
        why << "measured a negative delay (" << measured
            << "): the output onset at frame " << output_onset
            << " arrives before the declared marker at "
            << options.input_marker_frame << " plus intrinsic offset "
            << options.intrinsic_response_offset
            << ". A causal processor cannot do this — check "
               "intrinsic_response_offset.";
        return not_measurable(kPolicy, why.str(), reported_samples);
    }

    LatencyEvidence e;
    e.policy = kPolicy;
    settle_verdict(e, measured, reported_samples, options.tolerance_samples);
    return e;
}

LatencyEvidence& apply_report_observation(LatencyEvidence& evidence) {
    if (evidence.report_observation != LatencyReportObservation::changed)
        return evidence;

    // The report moved mid-render, so there is no single "the" latency to prove.
    // This holds even when the value returns to where it started and even when
    // the measurement happened to agree: a fixed-latency proof cannot stand over
    // a moving report.
    evidence.contract_outcome = LatencyContractOutcome::inconclusive;
    std::ostringstream why;
    why << "the latency report changed during the render";
    if (evidence.reported_samples && evidence.final_reported_samples)
        why << " (" << *evidence.reported_samples << " -> "
            << *evidence.final_reported_samples << ")";
    why << ", so a fixed-latency proof does not apply. Verifying an intentional "
           "latency change is a separate contract.";
    if (!evidence.reason.empty()) why << " Prior finding: " << evidence.reason;
    evidence.reason = why.str();
    return evidence;
}

// ── Serialization ───────────────────────────────────────────────────────────

const char* to_string(LatencyReportStatus status) {
    switch (status) {
        case LatencyReportStatus::available:    return "available";
        case LatencyReportStatus::unsupported:  return "unsupported";
        case LatencyReportStatus::query_failed: return "query_failed";
        case LatencyReportStatus::invalid:      return "invalid";
    }
    return "unsupported";
}

const char* to_string(LatencyReportObservation observation) {
    switch (observation) {
        case LatencyReportObservation::stable:       return "stable";
        case LatencyReportObservation::changed:      return "changed";
        case LatencyReportObservation::unobservable: return "unobservable";
    }
    return "unobservable";
}

const char* to_string(LatencyObservationMode mode) {
    switch (mode) {
        case LatencyObservationMode::processor_flag_and_poll:
            return "processor_flag_and_poll";
        case LatencyObservationMode::per_block_poll: return "per_block_poll";
        case LatencyObservationMode::none:           return "none";
    }
    return "none";
}

const char* to_string(LatencyPolicy policy) {
    switch (policy) {
        case LatencyPolicy::none: return "none";
        case LatencyPolicy::delayed_passthrough_null:
            return "delayed_passthrough_null";
        case LatencyPolicy::marker_offset: return "marker_offset";
    }
    return "none";
}

const char* to_string(LatencyMeasurementStatus status) {
    switch (status) {
        case LatencyMeasurementStatus::match:          return "match";
        case LatencyMeasurementStatus::mismatch:       return "mismatch";
        case LatencyMeasurementStatus::not_measurable: return "not_measurable";
        case LatencyMeasurementStatus::not_requested:  return "not_requested";
    }
    return "not_requested";
}

const char* to_string(LatencyContractOutcome outcome) {
    switch (outcome) {
        case LatencyContractOutcome::satisfied:     return "satisfied";
        case LatencyContractOutcome::violated:      return "violated";
        case LatencyContractOutcome::inconclusive:  return "inconclusive";
        case LatencyContractOutcome::not_requested: return "not_requested";
    }
    return "not_requested";
}

std::string latency_evidence_to_json(const LatencyEvidence& evidence) {
    auto root = choc::value::createObject("LatencyEvidence");
    root.setMember("schema_version", kLatencyEvidenceSchemaVersion);

    root.setMember("report_status", to_string(evidence.report_status));
    if (evidence.reported_samples)
        root.setMember("reported_samples",
                       static_cast<std::int64_t>(*evidence.reported_samples));
    if (evidence.final_reported_samples)
        root.setMember("final_reported_samples",
                       static_cast<std::int64_t>(*evidence.final_reported_samples));
    root.setMember("report_observation", to_string(evidence.report_observation));
    root.setMember("observation_mode", to_string(evidence.observation_mode));

    root.setMember("policy", to_string(evidence.policy));
    root.setMember("measurement_status", to_string(evidence.measurement_status));
    if (evidence.measured_samples)
        root.setMember("measured_samples", *evidence.measured_samples);
    if (evidence.delta_samples)
        root.setMember("delta_samples", *evidence.delta_samples);
    root.setMember("tolerance_samples", evidence.tolerance_samples);

    root.setMember("contract_outcome", to_string(evidence.contract_outcome));
    root.setMember("gates_failure", evidence.gates_failure());
    root.setMember("reason", evidence.reason);

    return choc::json::toString(root, /*multiLine=*/true);
}

std::string latency_evidence_summary(const LatencyEvidence& evidence) {
    std::ostringstream s;
    s << "latency: ";

    if (evidence.reported_samples)
        s << "reported " << *evidence.reported_samples;
    else
        s << "reported " << to_string(evidence.report_status);

    if (evidence.measured_samples) {
        s << ", measured " << *evidence.measured_samples;
        if (evidence.delta_samples)
            s << " (delta " << (*evidence.delta_samples > 0 ? "+" : "")
              << *evidence.delta_samples << ")";
    }

    s << " — " << to_string(evidence.contract_outcome);
    if (!evidence.reason.empty()) s << ": " << evidence.reason;
    return s.str();
}

} // namespace pulp::test::audio
