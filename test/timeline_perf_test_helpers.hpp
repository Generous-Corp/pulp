#pragma once

/// Measurement helpers shared by the timeline scale suites.
///
/// Two kinds of assertion live here, and the distinction is the point.
///
/// An absolute wall-clock ceiling measures `work / host_throughput`. It is only
/// admissible when the ceiling sits far enough above the observed time that no
/// plausible host can cross it; on a shared runner a ceiling within a small
/// multiple of the observed time reports the host, not the code. Those cases
/// belong in `measure_growth` instead.
///
/// A growth measurement runs the same operation at two input sizes and compares
/// them. Host throughput appears in both terms and cancels, so the result is a
/// property of the algorithm. It is measured in processor time rather than wall
/// time, and the minimum over interleaved rounds is kept, for the reasons given
/// on `cpu_now` and `measure_growth`.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string_view>

namespace pulp::test::timeline_perf {

inline bool strict_performance() {
    const auto* value = std::getenv("PULP_PERF_STRICT");
    return value && value[0] && value[0] != '0';
}

inline std::optional<std::chrono::milliseconds> performance_budget(const char* name) {
    const auto* value = std::getenv(name);
    if (!value || !value[0]) {
        INFO("missing performance budget: " << name);
        REQUIRE_FALSE(strict_performance());
        return std::nullopt;
    }

    const std::string_view text(value);
    std::chrono::milliseconds::rep milliseconds = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), milliseconds);
    INFO("invalid performance budget " << name << '=' << text);
    REQUIRE(parsed.ec == std::errc{});
    REQUIRE(parsed.ptr == text.data() + text.size());
    REQUIRE(milliseconds > 0);
    return std::chrono::milliseconds(milliseconds);
}

/// Assert an absolute ceiling. Reserved for measurements whose ceiling carries
/// so much headroom that host speed cannot reach it.
template <class Rep, class Period>
void enforce_performance_budget(const char* name, std::chrono::duration<Rep, Period> elapsed) {
    const auto budget = performance_budget(name);
    if (!budget)
        return;
    INFO(name << " elapsed_us="
              << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()
              << " budget_ms=" << budget->count());
    REQUIRE(elapsed <= *budget);
}

/// Processor time consumed so far, as a duration.
///
/// Growth is measured in processor time, not wall time. Wall time on a
/// contended host also counts the intervals in which the operation was
/// descheduled, and a longer operation is exposed to more of them, so
/// contention inflates the large-size term more than the small-size one and
/// biases the measured exponent upward -- the one direction the assertion
/// cares about. Processor time counts only the cycles actually spent on the
/// work. The operations measured here are single-threaded and the process is
/// otherwise idle while they run, so process time is the operation's own time.
inline std::chrono::steady_clock::duration cpu_now() {
    const auto seconds = static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(seconds));
}

/// The least-contended observation of one operation at two input sizes.
struct GrowthSample {
    std::size_t small_size = 0;
    std::size_t large_size = 0;
    std::chrono::steady_clock::duration small_min{};
    std::chrono::steady_clock::duration large_min{};

    /// How many times longer the large input took than the small one. A linear
    /// operation returns `size_ratio()`.
    double time_ratio() const {
        const auto small_ns = std::chrono::duration<double, std::nano>(small_min).count();
        const auto large_ns = std::chrono::duration<double, std::nano>(large_min).count();
        return small_ns > 0.0 ? large_ns / small_ns : 0.0;
    }

    double size_ratio() const {
        return small_size > 0 ? static_cast<double>(large_size) / static_cast<double>(small_size)
                              : 0.0;
    }

    /// The measured exponent `k` in `time ~ size^k`. Linear is 1.0, quadratic
    /// 2.0. Independent of host speed and of the sizes chosen.
    double exponent() const {
        const auto sizes = size_ratio();
        const auto times = time_ratio();
        return (sizes > 1.0 && times > 0.0) ? std::log(times) / std::log(sizes) : 0.0;
    }
};

/// Run `measure(size)` at both sizes, `rounds` times, alternating sizes each
/// round, and keep the smallest duration seen at each size.
///
/// `measure` must return the processor time (see `cpu_now`) of one full run at
/// the requested size and must not carry state between calls, or a later round
/// would measure a warmed cache instead of the operation. The minimum over
/// rounds is kept because contention is one-sided -- it can only make a run
/// cost more, never less -- and rounds are interleaved so a load that drifts
/// during the test cannot settle on one size.
template <class Measure>
GrowthSample measure_growth(std::size_t small_size, std::size_t large_size, int rounds,
                            Measure&& measure) {
    REQUIRE(small_size > 0);
    REQUIRE(large_size > small_size);
    REQUIRE(rounds >= 1);

    GrowthSample sample;
    sample.small_size = small_size;
    sample.large_size = large_size;
    sample.small_min = std::chrono::steady_clock::duration::max();
    sample.large_min = std::chrono::steady_clock::duration::max();

    for (int round = 0; round < rounds; ++round) {
        sample.small_min = std::min(sample.small_min, measure(small_size));
        sample.large_min = std::min(sample.large_min, measure(large_size));
    }
    return sample;
}

/// Assert the operation did not grow faster than `max_exponent` allows.
///
/// `max_exponent` is a property of the algorithm and belongs in the test as a
/// named constant, so the assertion holds in every lane rather than only where
/// a workflow happened to export a ceiling.
inline void require_growth_within(const char* name, const GrowthSample& sample,
                                  double max_exponent) {
    const auto small_us =
        std::chrono::duration_cast<std::chrono::microseconds>(sample.small_min).count();
    const auto large_us =
        std::chrono::duration_cast<std::chrono::microseconds>(sample.large_min).count();
    INFO(name << " n=" << sample.small_size << " -> " << sample.large_size << " (x"
              << sample.size_ratio() << ")  min_cpu_us=" << small_us << " -> " << large_us << " (x"
              << sample.time_ratio() << ")  exponent=" << sample.exponent()
              << " max_exponent=" << max_exponent);
    // A run too short to resolve would make the ratio meaningless, so require
    // the small size to be measurable before reading anything into the growth.
    REQUIRE(small_us > 0);
    REQUIRE(sample.exponent() <= max_exponent);
}

} // namespace pulp::test::timeline_perf
