#pragma once

/// @file audio_assertions.hpp
/// Reusable signal assertions over rendered buffers (harness PR 1A).
///
/// Each assertion returns a `CheckResult` whose message is written for audio
/// developers per the harness plan's failure-message policy: expected vs
/// actual in dBFS/Hz/cents plus the relevant scenario facts, never bare
/// floats. Framework-agnostic — use from Catch2 as:
///
/// @code
/// auto result = assert_not_silent(metrics, -60.0);
/// INFO(result.message);
/// REQUIRE(result.passed);
/// @endcode
///
/// Tolerances are explicit parameters (named per the plan's Threshold
/// Policy); none of these assertions embeds a hidden magic number.

#include "audio_metrics.hpp"

namespace pulp::test::audio {

/// Outcome of one signal assertion. `message` always describes what was
/// measured, including on success (useful in CAPTURE/INFO logs).
struct CheckResult {
    bool passed = false;
    std::string message;
    explicit operator bool() const { return passed; }
};

/// Fails if any channel contains NaN or ±Inf samples.
CheckResult assert_no_nan_inf(const BufferMetrics& metrics);

/// Fails if any channel's peak reaches `ceiling_dbfs` or any sample counted
/// as clipped during analysis.
CheckResult assert_not_clipped(const BufferMetrics& metrics,
                               double ceiling_dbfs = -0.1);

/// Fails if any channel's RMS is at or above `threshold_dbfs`.
CheckResult assert_silent(const BufferMetrics& metrics,
                          double threshold_dbfs = -90.0);

/// Fails if every channel's RMS is below `min_rms_dbfs`.
CheckResult assert_not_silent(const BufferMetrics& metrics,
                              double min_rms_dbfs = -60.0);

/// Fails unless every channel's peak lies within [min_dbfs, max_dbfs].
CheckResult assert_peak_between(const BufferMetrics& metrics,
                                double min_dbfs, double max_dbfs);

/// Fails unless every channel's RMS lies within [min_dbfs, max_dbfs].
CheckResult assert_rms_between(const BufferMetrics& metrics,
                               double min_dbfs, double max_dbfs);

/// Fails unless the zero-crossing frequency estimate is within
/// `tolerance_cents` of `expected_hz`. Estimator limits are documented on
/// `estimate_frequency`; this also fails when no estimate is possible.
CheckResult assert_frequency_near(std::span<const float> samples,
                                  double sample_rate, double expected_hz,
                                  double tolerance_cents = 5.0);

/// Null test: fails if the per-sample residual |a−b| peak is at or above
/// `tolerance_dbfs`. Buffers must have identical shape.
CheckResult assert_null_near(const pulp::audio::BufferView<const float>& a,
                             const pulp::audio::BufferView<const float>& b,
                             double tolerance_dbfs = -120.0);

/// @copydoc assert_null_near
/// Convenience overload over owning buffers.
CheckResult assert_null_near(const pulp::audio::Buffer<float>& a,
                             const pulp::audio::Buffer<float>& b,
                             double tolerance_dbfs = -120.0);

/// Fails if any two channels are sample-identical (within 1e-9), which
/// usually means a channel-routing bug duplicated one channel.
CheckResult assert_channels_independent(
    const pulp::audio::BufferView<const float>& buffer);

/// @copydoc assert_channels_independent
CheckResult assert_channels_independent(
    const pulp::audio::Buffer<float>& buffer);

} // namespace pulp::test::audio
