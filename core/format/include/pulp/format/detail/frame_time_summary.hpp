// Frame-time summary for a finite standalone run (the headless screenshot
// path). Pure functions over a list of per-frame milliseconds so the
// percentile and budget arithmetic is unit-testable without a window.
//
// The formatted line is a contract with support tooling that greps for it:
//   frame_ms frames=60 p50=2.10 p95=4.80 max=31.00 over16=1 over33=0
// Add fields at the end; never rename or reorder existing ones.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

namespace pulp::format::detail {

/// A 60 Hz frame budget (1000 / 60 ms).
inline constexpr double kFrameBudget60HzMs = 1000.0 / 60.0;
/// A 30 Hz frame budget (1000 / 30 ms).
inline constexpr double kFrameBudget30HzMs = 1000.0 / 30.0;

struct FrameTimeSummary {
    std::size_t frames = 0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double max_ms = 0.0;
    /// Frames strictly longer than the 60 Hz budget.
    std::size_t over_16ms = 0;
    /// Frames strictly longer than the 30 Hz budget.
    std::size_t over_33ms = 0;
};

/// Nearest-rank percentile of an ascending-sorted, non-empty list:
/// the smallest sample with at least `fraction` of the samples at or below it.
inline double nearest_rank_percentile(const std::vector<double>& sorted, double fraction) {
    const auto n = sorted.size();
    const auto rank = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(n)));
    const std::size_t index = rank == 0 ? 0 : std::min(rank, n) - 1;
    return sorted[index];
}

/// Summarize per-frame durations in milliseconds. Non-finite and negative
/// samples are dropped rather than skewing the percentiles; an empty input
/// yields frames=0 and zeroed statistics.
inline FrameTimeSummary summarize_frame_times(const std::vector<double>& samples_ms) {
    std::vector<double> sorted;
    sorted.reserve(samples_ms.size());
    for (const double ms : samples_ms) {
        if (std::isfinite(ms) && ms >= 0.0)
            sorted.push_back(ms);
    }
    FrameTimeSummary summary;
    summary.frames = sorted.size();
    if (sorted.empty())
        return summary;
    std::sort(sorted.begin(), sorted.end());
    summary.p50_ms = nearest_rank_percentile(sorted, 0.50);
    summary.p95_ms = nearest_rank_percentile(sorted, 0.95);
    summary.max_ms = sorted.back();
    for (const double ms : sorted) {
        if (ms > kFrameBudget60HzMs)
            ++summary.over_16ms;
        if (ms > kFrameBudget30HzMs)
            ++summary.over_33ms;
    }
    return summary;
}

/// The summary as one greppable line body (callers add their own prefix,
/// e.g. "Standalone: "). Milliseconds carry two decimals: an idle
/// host tick costs microseconds, which one decimal would print as 0.0.
inline std::string format_frame_time_summary(const FrameTimeSummary& s) {
    return std::format("frame_ms frames={} p50={:.2f} p95={:.2f} max={:.2f} over16={} over33={}",
                       s.frames, s.p50_ms, s.p95_ms, s.max_ms, s.over_16ms, s.over_33ms);
}

} // namespace pulp::format::detail
