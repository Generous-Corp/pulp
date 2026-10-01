// callback_cost_probe.cpp — per-block cost records for offline renders.

#include "callback_cost_probe.hpp"

#include <algorithm>
#include <chrono>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace pulp::test::audio {

std::int64_t thread_cpu_now() noexcept {
#if defined(_WIN32)
    ULONG64 cycles = 0;
    if (QueryThreadCycleTime(GetCurrentThread(), &cycles))
        return static_cast<std::int64_t>(cycles);
    return 0;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
        return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL +
               static_cast<std::int64_t>(ts.tv_nsec);
    return 0;
#else
    return 0;
#endif
}

std::int64_t wall_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

CostSummary summarize_cpu(std::span<const BlockCost> costs) {
    CostSummary summary;
    if (costs.empty())
        return summary;
    std::vector<std::int64_t> values;
    values.reserve(costs.size());
    for (std::size_t i = 0; i < costs.size(); ++i) {
        values.push_back(costs[i].cpu);
        if (costs[i].cpu > summary.max || i == 0) {
            summary.max = costs[i].cpu;
            summary.max_index = i;
        }
    }
    std::sort(values.begin(), values.end());
    summary.blocks = values.size();
    summary.median = values[values.size() / 2];
    const auto p95_index = std::min(values.size() - 1,
                                    (values.size() * 95) / 100);
    summary.p95 = values[p95_index];
    summary.max_over_median =
        summary.median > 0 ? static_cast<double>(summary.max) /
                                 static_cast<double>(summary.median)
                           : 0.0;
    return summary;
}

pulp::signal::rt::RtWorkCounts max_ops(std::span<const BlockCost> costs) {
    pulp::signal::rt::RtWorkCounts out;
    for (const auto& cost : costs) {
        out.fft = std::max(out.fft, cost.ops.fft);
        out.trig = std::max(out.trig, cost.ops.trig);
        out.bins = std::max(out.bins, cost.ops.bins);
    }
    return out;
}

} // namespace pulp::test::audio
