#pragma once

// Fixed-origin paced callback cell shared by the graph paced-timing matrix.
//
// A matrix cell drives an already-prepared fixture from one callback thread on
// a fixed schedule: callback i targets origin + i * frames / rate and a late
// callback never resets the origin. The schedule and the wait come from the
// shared deadline driver (paced_deadline_driver.hpp), which records the kernel
// wake, the callback start and the spin CPU cost of every callback. The
// harness records raw steady-clock offsets into preallocated storage and
// prints them only after the last callback, so the timed window contains no
// I/O. Percentiles, deadline misses
// and the qualification verdict are computed by
// tools/graph_paced_timing_matrix.py from the raw rows, not here.
//
// The cell is configured through environment variables so one hidden Catch2
// case per fixture can serve every matrix cell:
//   PULP_PACED_VARIANT       fixture-specific variant name
//   PULP_PACED_RATE          sample rate in Hz (default 48000)
//   PULP_PACED_FRAMES        frames per callback
//   PULP_PACED_CALLBACKS     callback count, 1..100000 (default 32)
//   PULP_PACED_PLANTED_SLEEP_US  planted negative: sleep inside the timed
//                                callback window (default 0)
//   PULP_PACED_PLANTED_EVERY     apply the planted sleep to every Nth callback
//   PULP_PACED_SCHED         callback-thread scheduling request: "none"
//                            (default QoS), "realtime" (a time-constraint
//                            thread on macOS, SCHED_FIFO on Linux), or
//                            "realtime-invalid" (a deliberately malformed
//                            real-time request the kernel must reject; the
//                            negative control for the refusal path)
//   PULP_PACED_WAKE*, PULP_PACED_INJECT_LATE_*  wake backend, guard, spin
//                            limit and injected lateness; see
//                            read_wake_config()
//
// A real-time request is read back from the kernel after it is applied. When
// the request fails or the read-back does not show the requested class, the
// cell refuses: no callback runs, the PACED_SCHED line records the reason and
// print_cell reports a failure. A cell never silently falls back to default
// scheduling.

#include "paced_deadline_driver.hpp"

#include <pulp/runtime/build_info.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#elif defined(__linux__)
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#endif

namespace pulp::test::paced {

inline constexpr std::string_view kCellHeader = "pulp_paced_timing_cell_v2";
inline constexpr std::uint64_t kMaxCallbacks = 100000;

enum class SchedRequest : std::uint8_t { none, realtime, realtime_invalid };

inline std::string_view sched_request_name(SchedRequest request) {
    switch (request) {
    case SchedRequest::realtime:
        return "realtime";
    case SchedRequest::realtime_invalid:
        return "realtime-invalid";
    case SchedRequest::none:
        break;
    }
    return "none";
}

// What the callback thread asked for and what the kernel reports it holds.
// `obtained` is the class read back after the request: "time_constraint" or
// "timeshare" on macOS, "fifo", "rr" or "other" on Linux, "unknown" elsewhere.
// Period, computation and constraint are the read-back values in nanoseconds
// (zero when the class carries none).
struct SchedReceipt {
    SchedRequest requested = SchedRequest::none;
    std::string obtained = "unknown";
    std::string status = "not_requested"; // "obtained" | "refused" | "not_requested"
    std::string reason = "none";          // one token; "none" unless refused
    std::int64_t period_ns = 0;
    std::int64_t computation_ns = 0;
    std::int64_t constraint_ns = 0;
    int preemptible = -1;
    int priority = -1;

    bool refused() const {
        return status == "refused";
    }
};

inline SchedRequest read_sched_request() {
    const char* text = std::getenv("PULP_PACED_SCHED");
    const std::string_view value = text == nullptr ? std::string_view{} : std::string_view(text);
    if (value.empty() || value == "none")
        return SchedRequest::none;
    if (value == "realtime")
        return SchedRequest::realtime;
    if (value == "realtime-invalid")
        return SchedRequest::realtime_invalid;
    throw std::invalid_argument("malformed PULP_PACED_SCHED");
}

#if defined(__APPLE__)
namespace detail {
inline std::int64_t mach_to_ns(std::uint64_t ticks) {
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    return static_cast<std::int64_t>(ticks * timebase.numer / timebase.denom);
}

inline std::uint32_t ns_to_mach(std::int64_t ns) {
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(ns) * timebase.denom /
                                      timebase.numer);
}
} // namespace detail
#endif

// Reads the calling thread's scheduling class back from the kernel.
inline void observe_thread_scheduling(SchedReceipt& receipt) {
#if defined(__APPLE__)
    const thread_port_t thread = pthread_mach_thread_np(pthread_self());
    thread_time_constraint_policy_data_t policy{};
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t get_default = FALSE;
    const auto result =
        thread_policy_get(thread, THREAD_TIME_CONSTRAINT_POLICY,
                          reinterpret_cast<thread_policy_t>(&policy), &count, &get_default);
    if (result == KERN_SUCCESS && !get_default) {
        receipt.obtained = "time_constraint";
        receipt.period_ns = detail::mach_to_ns(policy.period);
        receipt.computation_ns = detail::mach_to_ns(policy.computation);
        receipt.constraint_ns = detail::mach_to_ns(policy.constraint);
        receipt.preemptible = policy.preemptible ? 1 : 0;
    } else {
        receipt.obtained = result == KERN_SUCCESS ? "timeshare" : "unknown";
        receipt.period_ns = receipt.computation_ns = receipt.constraint_ns = 0;
        receipt.preemptible = -1;
    }
    int policy_kind = 0;
    sched_param param{};
    receipt.priority = pthread_getschedparam(pthread_self(), &policy_kind, &param) == 0
                           ? param.sched_priority
                           : -1;
#elif defined(__linux__)
    int policy_kind = 0;
    sched_param param{};
    if (pthread_getschedparam(pthread_self(), &policy_kind, &param) != 0) {
        receipt.obtained = "unknown";
        return;
    }
    receipt.obtained = policy_kind == SCHED_FIFO ? "fifo"
                       : policy_kind == SCHED_RR ? "rr"
                                                 : "other";
    receipt.priority = param.sched_priority;
#else
    receipt.obtained = "unknown";
#endif
}

// Applies `request` to the calling thread for a callback period of `period_ns`
// and returns the receipt. Call it on the callback thread before the first
// callback. A real-time request asks for a computation budget of half the
// period and a constraint of one full period; the readback must report that
// class or the request is refused.
inline SchedReceipt request_thread_scheduling(SchedRequest request, std::int64_t period_ns) {
    SchedReceipt receipt;
    receipt.requested = request;
    if (request == SchedRequest::none) {
        observe_thread_scheduling(receipt);
        return receipt;
    }
    const bool invalid = request == SchedRequest::realtime_invalid;
    const auto refuse = [&receipt](std::string reason) {
        receipt.status = "refused";
        receipt.reason = std::move(reason);
        observe_thread_scheduling(receipt);
        return receipt;
    };
    if (period_ns <= 0)
        return refuse("invalid_period");
#if defined(__APPLE__)
    // The kernel rejects a computation budget above its 50 ms real-time quantum.
    constexpr std::int64_t kMaxComputationNs = 50'000'000;
    thread_time_constraint_policy_data_t policy{};
    policy.period = detail::ns_to_mach(period_ns);
    policy.computation = detail::ns_to_mach(std::min(period_ns / 2, kMaxComputationNs));
    // The malformed request sets the constraint below the computation budget,
    // which the kernel refuses with KERN_INVALID_ARGUMENT.
    policy.constraint = invalid ? policy.computation / 2 : detail::ns_to_mach(period_ns);
    policy.preemptible = TRUE;
    const thread_port_t thread = pthread_mach_thread_np(pthread_self());
    const auto result = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
                                          reinterpret_cast<thread_policy_t>(&policy),
                                          THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (result != KERN_SUCCESS)
        return refuse("kern_return_" + std::to_string(result));
    observe_thread_scheduling(receipt);
    if (receipt.obtained != "time_constraint")
        return refuse("readback_" + receipt.obtained);
    const auto expected = [](std::uint32_t ticks) { return detail::mach_to_ns(ticks); };
    if (receipt.period_ns != expected(policy.period) ||
        receipt.computation_ns != expected(policy.computation) ||
        receipt.constraint_ns != expected(policy.constraint))
        return refuse("readback_mismatch");
#elif defined(__linux__)
    const int top = sched_get_priority_max(SCHED_FIFO);
    if (top < 0)
        return refuse("errno_" + std::to_string(errno));
    sched_param param{};
    // The malformed request names a priority above the policy maximum (EINVAL).
    param.sched_priority =
        invalid ? top + 1 : std::max(sched_get_priority_min(SCHED_FIFO), top - 10);
    const int result = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (result != 0)
        return refuse("errno_" + std::to_string(result));
    observe_thread_scheduling(receipt);
    if (receipt.obtained != "fifo" || receipt.priority != param.sched_priority)
        return refuse("readback_" + receipt.obtained);
    receipt.period_ns = period_ns;
#else
    (void)invalid;
    return refuse("unsupported_platform");
#endif
    receipt.status = "obtained";
    return receipt;
}

// One line in the format parsed by tools/graph_paced_timing_matrix.py.
inline void print_sched(const SchedReceipt& receipt) {
    std::cout << "PACED_SCHED requested=" << sched_request_name(receipt.requested)
              << " obtained=" << receipt.obtained << " status=" << receipt.status
              << " reason=" << receipt.reason << " period_ns=" << receipt.period_ns
              << " computation_ns=" << receipt.computation_ns
              << " constraint_ns=" << receipt.constraint_ns
              << " preemptible=" << receipt.preemptible << " priority=" << receipt.priority << '\n';
}

inline std::int64_t block_period_ns(std::uint64_t rate, std::uint64_t frames) {
    return static_cast<std::int64_t>(frames * 1'000'000'000ull / rate);
}

struct CellConfig {
    std::string variant;
    std::uint64_t rate = 48000;
    std::uint64_t frames = 0;
    std::uint64_t callbacks = 32;
    std::uint64_t planted_sleep_us = 0;
    std::uint64_t planted_every = 1;
    SchedRequest sched = SchedRequest::none;
    WakeConfig wake;
};

// Offsets are relative to the fixed origin. `wake_ns` is when the kernel wait
// returned, `start_ns` when the callback was released, and `spin_cpu_ns` the
// thread CPU time spent spinning between the two.
struct CellRow {
    std::int64_t target_ns = 0;
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;
    std::uint8_t ok = 0;
    std::int64_t wake_ns = 0;
    std::int64_t spin_cpu_ns = 0;
    std::uint8_t lateness = 0;
    std::uint8_t spin_overrun = 0;
};

// Raw rows plus the scheduling and wake receipts of the thread that produced
// them. A refused scheduling request leaves `rows` empty.
struct CellResult {
    std::vector<CellRow> rows;
    SchedReceipt sched;
    WakeSettings wake;
};

// One line in the format parsed by tools/graph_paced_timing_matrix.py.
inline void print_wake(const WakeSettings& wake) {
    std::cout << "PACED_WAKE backend=" << wake_backend_name(wake.backend)
              << " kernel_wait=" << wake.kernel_wait << " guard_source=" << wake.guard_source
              << " guard_ns=" << wake.guard_ns << " guard_bound_ns=" << wake.guard_bound_ns
              << " guard_clamped=" << wake.guard_clamped << " spin_limit_ns=" << wake.spin_limit_ns
              << " calibration_samples=" << wake.calibration_samples
              << " calibration_p50_ns=" << wake.calibration_p50_ns
              << " calibration_p99_ns=" << wake.calibration_p99_ns
              << " calibration_max_ns=" << wake.calibration_max_ns
              << " timebase_numer=" << wake.timebase_numer
              << " timebase_denom=" << wake.timebase_denom
              << " cpu_clock_resolution_ns=" << wake.cpu_clock_resolution_ns
              << " inject_late_us=" << wake.inject_late_us
              << " inject_late_every=" << wake.inject_late_every
              << " wait_failures=" << wake.wait_failures << '\n';
}

inline std::uint64_t env_integer(const char* key, std::uint64_t fallback) {
    const char* text = std::getenv(key);
    if (text == nullptr || *text == '\0')
        return fallback;
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0')
        throw std::invalid_argument(std::string("malformed ") + key);
    return static_cast<std::uint64_t>(value);
}

// Unset variables fall back to the fixture's defaults, so running a cell case
// directly (or through a tag filter) is a short smoke run rather than an error.
inline CellConfig read_config(std::string_view default_variant, std::uint64_t default_frames) {
    CellConfig config;
    const char* variant = std::getenv("PULP_PACED_VARIANT");
    config.variant = (variant == nullptr || *variant == '\0') ? std::string(default_variant)
                                                              : std::string(variant);
    config.rate = env_integer("PULP_PACED_RATE", 48000);
    config.frames = env_integer("PULP_PACED_FRAMES", default_frames);
    config.callbacks = env_integer("PULP_PACED_CALLBACKS", 32);
    config.planted_sleep_us = env_integer("PULP_PACED_PLANTED_SLEEP_US", 0);
    config.planted_every = env_integer("PULP_PACED_PLANTED_EVERY", 1);
    config.sched = read_sched_request();
    config.wake = read_wake_config();
    if (config.rate == 0 || config.frames == 0 || config.callbacks == 0 ||
        config.callbacks > kMaxCallbacks || config.planted_every == 0)
        throw std::invalid_argument("paced cell configuration out of range");
    return config;
}

// Runs `process(index, first_sample)` on a dedicated thread at the fixed-origin
// schedule kept by the shared deadline driver and times only that call (plus
// any planted sleep). `verify(index,
// first_sample)` then checks the block outside the timed window and returns the
// fixture's own oracle verdict; it still shares the callback period, so keep
// it table-driven and cheap. The worker applies `config.sched` before the
// first callback, then calibrates the wake guard under that class; when the
// request is refused no callback runs. Returns the raw rows and the
// scheduling and wake receipts.
template <class Process, class Verify>
CellResult run_cell(const CellConfig& config, Process&& process, Verify&& verify) {
    using clock = std::chrono::steady_clock;
    CellResult result;
    result.rows.resize(static_cast<std::size_t>(config.callbacks));
    // A refused cell still reports the wake request it carried.
    result.wake.backend = config.wake.backend;
    result.wake.inject_late_us = config.wake.inject_late_us;
    result.wake.inject_late_every = config.wake.inject_late_every;
    auto& rows = result.rows;
    const auto planted = std::chrono::microseconds(config.planted_sleep_us);
    std::thread worker([&] {
        result.sched =
            request_thread_scheduling(config.sched, block_period_ns(config.rate, config.frames));
        if (result.sched.refused())
            return;
        SystemWakePlatform platform;
        DeadlineDriver driver(platform, config.rate, config.frames, config.wake);
        driver.start();
        const auto origin = driver.origin_ns();
        for (std::uint64_t i = 0; i < config.callbacks; ++i) {
            const auto tick = driver.wait(static_cast<std::size_t>(i));
            process(static_cast<std::size_t>(i), i * config.frames);
            if (config.planted_sleep_us != 0 && i % config.planted_every == 0)
                std::this_thread::sleep_for(planted);
            const auto end = clock::now();
            const bool ok = verify(static_cast<std::size_t>(i), i * config.frames);
            auto& row = rows[static_cast<std::size_t>(i)];
            row.target_ns = tick.target_offset_ns;
            row.start_ns = tick.start_ns - origin;
            row.end_ns =
                static_cast<std::int64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch())
                        .count()) -
                origin;
            row.ok = ok ? 1 : 0;
            row.wake_ns = tick.kernel_wake_ns - origin;
            row.spin_cpu_ns = tick.spin_cpu_ns;
            row.lateness = static_cast<std::uint8_t>(tick.lateness);
            row.spin_overrun = tick.spin_overrun ? 1 : 0;
        }
        result.wake = driver.settings();
    });
    worker.join();
    if (result.sched.refused())
        rows.clear();
    return result;
}

// Prints one cell in the line format parsed by tools/graph_paced_timing_matrix.py
// and returns the number of blocks whose fixture oracle failed. A refused
// scheduling request counts as one failure so the cell's Catch2 case fails.
inline std::uint64_t print_cell(std::string_view fixture, const CellConfig& config,
                                const CellResult& result, std::string_view extra = {}) {
    const auto& rows = result.rows;
    std::uint64_t oracle_failures = 0;
    for (const auto& row : rows)
        oracle_failures += row.ok ? 0 : 1;
    std::cout << kCellHeader << '\n'
              << "PACED_CELL fixture=" << fixture << " variant=" << config.variant
              << " rate=" << config.rate << " frames=" << config.frames
              << " callbacks=" << config.callbacks
              << " planted_sleep_us=" << config.planted_sleep_us
              << " planted_every=" << config.planted_every
              << " compiled_source=" << pulp::runtime::kGitSha
              << " build_type=" << pulp::runtime::kBuildType
              << " build_dirty=" << pulp::runtime::kGitDirty;
    if (!extra.empty())
        std::cout << ' ' << extra;
    std::cout << '\n';
    print_sched(result.sched);
    print_wake(result.wake);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        std::cout << "P " << i << ' ' << row.target_ns << ' ' << row.start_ns << ' ' << row.end_ns
                  << ' ' << static_cast<int>(row.ok) << ' ' << row.wake_ns << ' ' << row.spin_cpu_ns
                  << ' ' << static_cast<int>(row.lateness) << ' '
                  << static_cast<int>(row.spin_overrun) << '\n';
    }
    std::cout << "PACED_END rows=" << rows.size() << " oracle_failures=" << oracle_failures << '\n';
    return oracle_failures + (result.sched.refused() ? 1 : 0);
}

} // namespace pulp::test::paced
