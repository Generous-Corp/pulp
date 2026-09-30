#pragma once

/// @file callback_cost_probe.hpp
/// Per-callback cost records for offline renders.
///
/// An offline render has no deadline, so a callback that takes 4 ms against a
/// 2.67 ms budget renders perfectly and still drops a buffer in a DAW. This
/// probe records what every block of a render cost, with two instruments:
///
/// - operation counts (`pulp::signal::rt::RtWorkCounts`: FFT executions and
///   counted transcendental calls) — deterministic, so a gate on them cannot
///   flake and may hold a required lane;
/// - thread CPU time — the time the render thread itself spent, so a shared
///   runner preempting the thread does not register. Still load-sensitive
///   (cache, frequency scaling), so gates on it are advisory (`performance`).
///
/// Test/tool layer only — renders happen entirely off the audio thread.

#include <pulp/signal/rt_work_counter.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace pulp::test::audio {

/// Cost of one processed block.
struct BlockCost {
    std::int64_t start_frame = 0;
    int frames = 0;
    /// Thread CPU time of the render thread across the process call, in
    /// nanoseconds. On Windows the unit is CPU cycles
    /// (`QueryThreadCycleTime`), because thread times there are quantized to
    /// the scheduler tick; only ratios of this field are meaningful across
    /// platforms.
    std::int64_t cpu = 0;
    /// Wall-clock time across the process call, in nanoseconds.
    std::int64_t wall_ns = 0;
    /// Counted operations executed during the process call.
    pulp::signal::rt::RtWorkCounts ops;
    /// Parameter steps applied immediately before this block.
    int param_steps = 0;
};

/// Summary of a range of block costs (CPU field).
struct CostSummary {
    std::size_t blocks = 0;
    std::int64_t median = 0;
    std::int64_t p95 = 0;
    std::int64_t max = 0;
    std::size_t max_index = 0;  ///< index into the summarized span
    double max_over_median = 0.0;
};

/// Monotonic thread CPU time of the calling thread (see BlockCost::cpu for
/// the unit). Returns 0 where the platform offers no per-thread clock.
std::int64_t thread_cpu_now() noexcept;

/// Monotonic wall-clock time in nanoseconds.
std::int64_t wall_now_ns() noexcept;

/// Summarize the CPU field of `costs`. An empty span yields a zero summary.
CostSummary summarize_cpu(std::span<const BlockCost> costs);

/// Per-counter maximum over `costs`.
pulp::signal::rt::RtWorkCounts max_ops(std::span<const BlockCost> costs);

} // namespace pulp::test::audio
