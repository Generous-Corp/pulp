// audio_contracts.cpp — named audio contracts.
// See audio_contracts.hpp for the vocabulary and message policy.

#include "audio_contracts.hpp"

#include <sstream>

namespace pulp::test::audio {
namespace {

// Combine sub-checks of one contract-family helper into one CheckResult.
// `claim` names the family in the message; on failure only the failing
// sub-messages are kept (each already states expected vs actual).
CheckResult combine(std::string_view claim,
                    std::initializer_list<CheckResult> checks) {
    CheckResult out;
    out.passed = true;
    std::string detail;
    for (const auto& check : checks) {
        if (check.passed)
            continue;
        out.passed = false;
        if (!detail.empty())
            detail += "; ";
        detail += check.message;
    }
    if (out.passed) {
        for (const auto& check : checks) {
            if (!detail.empty())
                detail += "; ";
            detail += check.message;
        }
    }
    out.message = std::string(claim) + ": " + detail;
    return out;
}

} // namespace

CheckResult AudioContract::verify() const {
    std::ostringstream msg;
    if (checks_.empty()) {
        msg << "contract '" << name_ << "': declares no expectations — a"
            << " claim without checks is vacuously true and therefore a bug"
            << " (" << result_.scenario << ")";
        return {false, msg.str()};
    }

    std::size_t failed = 0;
    for (const auto& check : checks_)
        failed += check.passed ? 0u : 1u;

    if (failed == 0) {
        msg << "contract '" << name_ << "': " << checks_.size()
            << " expectation(s) hold (" << result_.scenario << ")";
        return {true, msg.str()};
    }

    // Leave a machine-readable record of what the signal looked like. NOTE:
    // this artifact always reflects the FULL-render metrics (result_.metrics),
    // even when the failing check was a windowed / block-partition sub-view
    // (e.g. expect_tone's held window) — the per-check message names the window
    // it judged, so read the message for the sub-view and this artifact for the
    // whole-render context.
    const auto artifact = write_metrics_artifact(result_.metrics,
                                                 result_.scenario);
    for (const auto& check : checks_) {
        if (!check.passed)
            msg << "contract '" << name_ << "': " << check.message << "\n";
    }
    msg << "  scenario: " << result_.scenario << "\n";
    if (!artifact.empty())
        msg << "  artifact: " << artifact.string();
    else
        msg << "  artifact: (write failed)";
    return {false, msg.str()};
}

CheckResult expect_passthrough(const ScenarioResult& result,
                               const pulp::audio::Buffer<float>& input,
                               double tolerance_dbfs) {
    return combine("pass-through (output vs input stimulus)",
                   {assert_null_near(result.output, input, tolerance_dbfs)});
}

CheckResult expect_silence_preserved(const ScenarioResult& result,
                                     double threshold_dbfs) {
    return combine("silence preserved",
                   {assert_silent(result.metrics, threshold_dbfs),
                    assert_no_nan_inf(result.metrics)});
}

CheckResult expect_tone(const ScenarioResult& result, const ToneClaim& claim) {
    const auto total = static_cast<std::int64_t>(result.output.num_samples());
    if (claim.window_frames <= 0 || claim.window_start < 0 ||
        claim.window_start + claim.window_frames > total) {
        std::ostringstream msg;
        msg << "tone claim window [" << claim.window_start << ", "
            << claim.window_start + claim.window_frames
            << ") does not fit the " << total << "-frame render";
        return {false, msg.str()};
    }

    const auto window = result.output.view().slice(
        static_cast<std::size_t>(claim.window_start),
        static_cast<std::size_t>(claim.window_frames));
    const auto metrics = analyze(window, result.sample_rate);

    std::ostringstream label;
    label << "tone over held window [" << claim.window_start << ", "
          << claim.window_start + claim.window_frames << ")";
    // assert_rms_between requires EVERY channel ≥ min_rms_dbfs, which already
    // subsumes assert_not_silent (≥ min on at least one channel), so the
    // not-silent check would be redundant here — the range claim is strictly
    // stronger.
    return combine(label.str(),
                   {assert_rms_between(metrics, claim.min_rms_dbfs,
                                       claim.max_rms_dbfs),
                    assert_frequency_near(window.channel(0),
                                          result.sample_rate,
                                          claim.expected_hz,
                                          claim.tolerance_cents)});
}

CheckResult expect_finite_and_unclipped(const ScenarioResult& result,
                                        double ceiling_dbfs) {
    return combine("finite and unclipped",
                   {assert_no_nan_inf(result.metrics),
                    assert_not_clipped(result.metrics, ceiling_dbfs)});
}

} // namespace pulp::test::audio
