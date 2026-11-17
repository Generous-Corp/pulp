#pragma once

// Fixed-origin deadline driver shared by every paced-timing fixture.
//
// Callback i targets origin + i * frames * 1e9 / rate nanoseconds, computed in
// integers so the schedule never drifts. A late callback keeps its original
// target: the driver never moves the origin or any later target to hide a
// miss, so lateness stays visible in the recorded start error.
//
// The default backend ("deadline-spin") waits in the kernel until a guard
// interval before the target, using an absolute deadline (mach_wait_until on
// macOS, clock_nanosleep with TIMER_ABSTIME on Linux), then spins on the
// measurement clock until the target. The guard is calibrated on the callback
// thread, after its scheduling class is applied, from the observed kernel
// wake overshoot, and is clamped to a bound derived from the block period.
// The spin is bounded too: a spin that outlives its limit falls back to a
// kernel wait for the target and is flagged as an overrun. Kernel-wait retries
// after the first return count as spin work: they share the spin limit and the
// spin CPU cost, because a wait on a deadline the kernel already considers
// passed returns at once and would otherwise busy-wait unaccounted.
//
// The kernel deadline is converted from the measurement clock through one
// mapping sampled at construction. The two clocks are read separately, so the
// mapping brackets the native read between two measurement reads and retries
// until the bracket is narrow; the residual skew is recorded so a run with a
// skewed mapping can be disqualified rather than trusted.
//
// "sleep-until" keeps the plain std::this_thread::sleep_until wait. It exists
// only as the A/B baseline for the deadline backend and never qualifies.
//
// For every callback the driver records three things separately: when the
// kernel wait returned, when the callback was released (its start), and the
// thread CPU time spent between the two (the spin cost). These waits belong in
// test drivers only; production DSP never waits like this.
//
// The clock, the kernel wait and the thread CPU clock come from a platform
// policy so the scheduling logic can be tested against a scripted clock.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <time.h>
#elif defined(__linux__)
#include <cerrno>
#include <time.h>
#endif

namespace pulp::test::paced {

enum class WakeBackend : std::uint8_t { deadline_spin, sleep_until };

inline std::string_view wake_backend_name(WakeBackend backend) {
    return backend == WakeBackend::sleep_until ? "sleep-until" : "deadline-spin";
}

// A measurement-clock time point for absolute nanoseconds from a tick.
inline std::chrono::steady_clock::time_point steady_at_ns(std::int64_t ns) {
    return std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(ns)));
}

// What the cell asked for. A negative guard requests calibration; a zero spin
// limit derives it from the guard.
struct WakeConfig {
    WakeBackend backend = WakeBackend::deadline_spin;
    std::int64_t guard_ns = -1;
    std::int64_t spin_limit_ns = 0;
    std::uint32_t calibration_samples = 64;
    // Injected lateness, a negative control: before waiting for every Nth
    // callback the driver sleeps this long, as if the thread had been
    // descheduled, so the callback can enter after its target.
    std::uint64_t inject_late_us = 0;
    std::uint64_t inject_late_every = 1;
};

// Guard bounds. The lower bound leaves room for one wake and one clock read;
// the upper bound keeps the spin to a fraction of a block.
inline constexpr std::int64_t kMinGuardNs = 20'000;
inline constexpr std::int64_t kMaxGuardNs = 2'000'000;
inline constexpr std::int64_t kGuardMarginNs = 10'000;
inline constexpr std::int64_t kCalibrationWaitNs = 200'000;
inline constexpr std::uint32_t kMaxCalibrationSamples = 4096;

// Widest accepted bracket around the native clock read when mapping the
// measurement clock onto the kernel's absolute-deadline clock.
inline constexpr std::int64_t kMaxClockMappingSkewNs = 20'000;
inline constexpr int kClockMappingAttempts = 16;

inline std::int64_t guard_bound_ns(std::int64_t period_ns) {
    return std::clamp<std::int64_t>(period_ns / 4, kMinGuardNs, kMaxGuardNs);
}

// What the driver actually used, printed once per cell.
struct WakeSettings {
    WakeBackend backend = WakeBackend::deadline_spin;
    std::string kernel_wait = "none";
    std::string guard_source = "none"; // "calibrated" | "fixed" | "none"
    std::int64_t guard_ns = 0;
    std::int64_t guard_bound_ns = 0;
    int guard_clamped = 0;
    std::int64_t spin_limit_ns = 0;
    std::uint32_t calibration_samples = 0;
    std::int64_t calibration_p50_ns = 0;
    std::int64_t calibration_p99_ns = 0;
    std::int64_t calibration_max_ns = 0;
    std::uint64_t timebase_numer = 1;
    std::uint64_t timebase_denom = 1;
    std::int64_t cpu_clock_resolution_ns = 0;
    std::uint64_t inject_late_us = 0;
    std::uint64_t inject_late_every = 1;
    int wait_failures = 0;
    // Width of the narrowest measurement-clock bracket around the native read
    // that fixed the kernel deadline mapping. Above kMaxClockMappingSkewNs the
    // mapping may bias every kernel deadline and the run is disqualified.
    std::int64_t clock_mapping_skew_ns = 0;
};

// A measurement-clock instant paired with a native-clock instant. The native
// read happened somewhere inside a bracket `skew_ns` wide; `steady_ns` is the
// bracket midpoint, so the pairing error is at most half the skew.
struct ClockMapping {
    std::int64_t steady_ns = 0;
    std::uint64_t native = 0;
    std::int64_t skew_ns = 0;
    int attempts = 0;
    bool ok = false;
};

// Samples steady, native, steady up to `attempts` times and keeps the narrowest
// bracket, stopping early once one is within `max_skew_ns`. `ok` is false when
// no bracket was narrow enough: a preemption landed inside every attempt.
template <class ReadSteady, class ReadNative>
ClockMapping bracket_clock_mapping(ReadSteady&& read_steady, ReadNative&& read_native,
                                   std::int64_t max_skew_ns = kMaxClockMappingSkewNs,
                                   int attempts = kClockMappingAttempts) {
    ClockMapping best;
    bool have = false;
    for (int i = 0; i < std::max(1, attempts); ++i) {
        const std::int64_t before = read_steady();
        const std::uint64_t native = read_native();
        const std::int64_t after = read_steady();
        const auto skew = std::max<std::int64_t>(0, after - before);
        best.attempts = i + 1;
        if (!have || skew < best.skew_ns) {
            have = true;
            best.steady_ns = before + skew / 2;
            best.native = native;
            best.skew_ns = skew;
        }
        if (skew <= max_skew_ns)
            break;
    }
    best.ok = best.skew_ns <= max_skew_ns;
    return best;
}

// How a callback met its target.
enum class Lateness : std::uint8_t {
    on_time = 0,         // the kernel woke at or before the target
    entered_late = 1,    // the target had passed before any wait began
    kernel_overshot = 2, // the kernel wait returned after the target
};

// One callback's wake record. Times are absolute measurement-clock
// nanoseconds; `target_offset_ns` is the target relative to the origin.
struct DeadlineTick {
    std::size_t index = 0;
    std::int64_t target_offset_ns = 0;
    std::int64_t target_ns = 0;
    std::int64_t kernel_wake_ns = 0;
    std::int64_t start_ns = 0;
    std::int64_t spin_cpu_ns = 0;
    Lateness lateness = Lateness::on_time;
    bool spin_overrun = false;
    // Kernel waits issued after the first one returned before its deadline.
    std::uint32_t kernel_retries = 0;
};

// Real platform: steady_clock is the measurement clock; the kernel wait uses
// the platform's absolute-deadline primitive, mapped from steady_clock once.
class SystemWakePlatform {
  public:
    SystemWakePlatform() {
#if defined(__APPLE__)
        mach_timebase_info_data_t timebase{};
        if (mach_timebase_info(&timebase) == KERN_SUCCESS && timebase.numer != 0 &&
            timebase.denom != 0) {
            numer_ = timebase.numer;
            denom_ = timebase.denom;
        }
        mapping_ = bracket_clock_mapping([this] { return now_ns(); },
                                         [] { return std::uint64_t{mach_absolute_time()}; });
        mach_origin_ = mapping_.native;
        steady_origin_ns_ = mapping_.steady_ns;
#elif defined(__linux__)
        mapping_ = bracket_clock_mapping(
            [this] { return now_ns(); },
            [] {
                timespec now{};
                clock_gettime(CLOCK_MONOTONIC, &now);
                return static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec);
            });
        monotonic_origin_ns_ = static_cast<std::int64_t>(mapping_.native);
        steady_origin_ns_ = mapping_.steady_ns;
#else
        mapping_.ok = true;
        mapping_.attempts = 1;
#endif
    }

    const ClockMapping& clock_mapping() const {
        return mapping_;
    }
    std::int64_t clock_mapping_skew_ns() const {
        return mapping_.skew_ns;
    }

    std::int64_t now_ns() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // Blocks until the measurement clock reaches `deadline_ns` (absolute).
    // Returns false when the kernel reports an error.
    bool kernel_wait_until(std::int64_t deadline_ns) const {
        const auto delta = deadline_ns - steady_origin_ns_;
#if defined(__APPLE__)
        if (delta <= 0)
            return true;
        const auto ticks =
            static_cast<std::uint64_t>(static_cast<unsigned __int128>(delta) * denom_ / numer_);
        kern_return_t result = KERN_SUCCESS;
        do {
            result = mach_wait_until(mach_origin_ + ticks);
        } while (result == KERN_ABORTED);
        return result == KERN_SUCCESS;
#elif defined(__linux__)
        const auto absolute = monotonic_origin_ns_ + delta;
        timespec when{};
        when.tv_sec = absolute / 1'000'000'000;
        when.tv_nsec = absolute % 1'000'000'000;
        int result = 0;
        do {
            result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &when, nullptr);
        } while (result == EINTR);
        return result == 0;
#else
        (void)delta;
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(deadline_ns))));
        return true;
#endif
    }

    void sleep_until_ns(std::int64_t deadline_ns) const {
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(deadline_ns))));
    }

    void sleep_for_ns(std::int64_t duration_ns) const {
        std::this_thread::sleep_for(std::chrono::nanoseconds(duration_ns));
    }

    std::int64_t thread_cpu_ns() const {
#if defined(__APPLE__) || defined(__linux__)
        timespec now{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0)
            return 0;
        return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
#else
        return 0;
#endif
    }

    std::int64_t cpu_clock_resolution_ns() const {
#if defined(__APPLE__) || defined(__linux__)
        timespec resolution{};
        if (clock_getres(CLOCK_THREAD_CPUTIME_ID, &resolution) != 0)
            return 0;
        return static_cast<std::int64_t>(resolution.tv_sec) * 1'000'000'000 + resolution.tv_nsec;
#else
        return 0;
#endif
    }

    std::string_view kernel_wait_name() const {
#if defined(__APPLE__)
        return "mach_wait_until";
#elif defined(__linux__)
        return "clock_nanosleep_abstime";
#else
        return "sleep_until";
#endif
    }

    std::uint64_t timebase_numer() const {
        return numer_;
    }
    std::uint64_t timebase_denom() const {
        return denom_;
    }

    void relax() const {}

  private:
    std::uint64_t numer_ = 1;
    std::uint64_t denom_ = 1;
    std::int64_t steady_origin_ns_ = 0;
    ClockMapping mapping_;
#if defined(__APPLE__)
    std::uint64_t mach_origin_ = 0;
#elif defined(__linux__)
    std::int64_t monotonic_origin_ns_ = 0;
#endif
};

template <class Platform> class BasicDeadlineDriver {
  public:
    BasicDeadlineDriver(Platform& platform, std::uint64_t rate, std::uint64_t frames,
                        WakeConfig config)
        : platform_(platform), rate_(rate), frames_(frames), config_(config) {
        if (rate == 0 || frames == 0 || config.inject_late_every == 0 ||
            config.calibration_samples > kMaxCalibrationSamples || config.spin_limit_ns < 0)
            throw std::invalid_argument("paced deadline driver configuration out of range");
        settings_.backend = config.backend;
        settings_.inject_late_us = config.inject_late_us;
        settings_.inject_late_every = config.inject_late_every;
        settings_.timebase_numer = platform.timebase_numer();
        settings_.timebase_denom = platform.timebase_denom();
        settings_.cpu_clock_resolution_ns = platform.cpu_clock_resolution_ns();
        settings_.guard_bound_ns = guard_bound_ns(period_ns());
        settings_.clock_mapping_skew_ns = platform.clock_mapping_skew_ns();
        if (config.backend == WakeBackend::sleep_until) {
            settings_.kernel_wait = "sleep_until";
            return;
        }
        settings_.kernel_wait = std::string(platform.kernel_wait_name());
        if (config.guard_ns >= 0) {
            settings_.guard_source = "fixed";
            settings_.guard_ns = config.guard_ns;
        } else {
            settings_.guard_source = "calibrated";
            calibrate();
        }
        settings_.spin_limit_ns = config.spin_limit_ns > 0
                                      ? config.spin_limit_ns
                                      : 2 * settings_.guard_ns + kGuardMarginNs;
    }

    std::int64_t period_ns() const {
        return static_cast<std::int64_t>(frames_ * 1'000'000'000ull / rate_);
    }

    // Integer schedule arithmetic: no accumulated floating drift.
    std::int64_t target_offset_ns(std::size_t index) const {
        return static_cast<std::int64_t>(static_cast<std::uint64_t>(index) * frames_ *
                                         1'000'000'000ull / rate_);
    }

    // Fixes the origin `lead_ns` after now. Call once, before the first wait.
    void start(std::int64_t lead_ns = 10'000'000) {
        origin_ns_ = platform_.now_ns() + lead_ns;
        started_ = true;
    }

    std::int64_t origin_ns() const {
        return origin_ns_;
    }
    const WakeSettings& settings() const {
        return settings_;
    }

    // Waits for callback `index` and returns its wake record. The target is
    // always origin + target_offset_ns(index), however late the caller is.
    DeadlineTick wait(std::size_t index) {
        if (!started_)
            throw std::logic_error("paced deadline driver used before start()");
        DeadlineTick tick;
        tick.index = index;
        tick.target_offset_ns = target_offset_ns(index);
        tick.target_ns = origin_ns_ + tick.target_offset_ns;
        if (config_.inject_late_us != 0 && index % config_.inject_late_every == 0)
            platform_.sleep_for_ns(static_cast<std::int64_t>(config_.inject_late_us) * 1000);
        const auto entry = platform_.now_ns();
        if (entry >= tick.target_ns) {
            tick.lateness = Lateness::entered_late;
            tick.kernel_wake_ns = entry;
            tick.start_ns = entry;
            return tick;
        }
        if (config_.backend == WakeBackend::sleep_until) {
            // No spin: the start is the wake, and the spin cost is zero.
            platform_.sleep_until_ns(tick.target_ns);
            tick.kernel_wake_ns = platform_.now_ns();
            tick.start_ns = tick.kernel_wake_ns;
            tick.lateness = tick.kernel_wake_ns > tick.target_ns ? Lateness::kernel_overshot
                                                                 : Lateness::on_time;
            return tick;
        }
        const auto wake_deadline = tick.target_ns - settings_.guard_ns;
        auto now = entry;
        bool kernel_ok = true;
        if (now < wake_deadline) {
            kernel_ok = platform_.kernel_wait_until(wake_deadline);
            if (!kernel_ok)
                ++settings_.wait_failures;
            now = platform_.now_ns();
        }
        // From the first kernel return to the release everything is spin
        // work: one budget, one CPU measurement.
        const auto spin_start = now;
        const auto cpu_before = platform_.thread_cpu_ns();
        // A kernel wait can return early; wait again until the guard deadline.
        // A deadline biased early by a skewed clock mapping makes each retry
        // return at once, so retries are bounded by the spin limit.
        while (kernel_ok && now < wake_deadline) {
            if (now - spin_start > settings_.spin_limit_ns) {
                tick.spin_overrun = true;
                break;
            }
            ++tick.kernel_retries;
            if (!platform_.kernel_wait_until(wake_deadline)) {
                ++settings_.wait_failures;
                break;
            }
            now = platform_.now_ns();
        }
        tick.kernel_wake_ns = now;
        tick.lateness =
            tick.kernel_wake_ns > tick.target_ns ? Lateness::kernel_overshot : Lateness::on_time;
        while (!tick.spin_overrun && now < tick.target_ns) {
            if (now - spin_start > settings_.spin_limit_ns) {
                tick.spin_overrun = true;
                break;
            }
            platform_.relax();
            now = platform_.now_ns();
        }
        if (tick.spin_overrun) {
            while (now < tick.target_ns) {
                platform_.sleep_until_ns(tick.target_ns);
                now = platform_.now_ns();
            }
        }
        tick.start_ns = now;
        // Read after the start timestamp, so the CPU clock read never delays
        // the recorded start; its cost lands inside the callback duration.
        tick.spin_cpu_ns = std::max<std::int64_t>(0, platform_.thread_cpu_ns() - cpu_before);
        return tick;
    }

  private:
    void calibrate() {
        const auto bound = settings_.guard_bound_ns;
        std::vector<std::int64_t> overshoot;
        overshoot.reserve(config_.calibration_samples);
        for (std::uint32_t i = 0; i < config_.calibration_samples; ++i) {
            const auto deadline = platform_.now_ns() + kCalibrationWaitNs;
            if (!platform_.kernel_wait_until(deadline)) {
                ++settings_.wait_failures;
                continue;
            }
            overshoot.push_back(std::max<std::int64_t>(0, platform_.now_ns() - deadline));
        }
        settings_.calibration_samples = static_cast<std::uint32_t>(overshoot.size());
        std::int64_t wanted = bound;
        if (!overshoot.empty()) {
            std::sort(overshoot.begin(), overshoot.end());
            const auto rank = [&](std::size_t numerator) {
                const auto n = overshoot.size();
                return overshoot[std::max<std::size_t>(1, (n * numerator + 99) / 100) - 1];
            };
            settings_.calibration_p50_ns = rank(50);
            settings_.calibration_p99_ns = rank(99);
            settings_.calibration_max_ns = overshoot.back();
            wanted = settings_.calibration_p99_ns + kGuardMarginNs;
        }
        settings_.guard_clamped = wanted > bound ? 1 : 0;
        settings_.guard_ns = std::clamp<std::int64_t>(wanted, kMinGuardNs, bound);
    }

    Platform& platform_;
    std::uint64_t rate_;
    std::uint64_t frames_;
    WakeConfig config_;
    WakeSettings settings_;
    std::int64_t origin_ns_ = 0;
    bool started_ = false;
};

using DeadlineDriver = BasicDeadlineDriver<SystemWakePlatform>;

inline std::uint64_t wake_env_integer(const char* key, std::uint64_t fallback) {
    const char* text = std::getenv(key);
    if (text == nullptr || *text == '\0')
        return fallback;
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0')
        throw std::invalid_argument(std::string("malformed ") + key);
    return static_cast<std::uint64_t>(value);
}

// Reads the wake request from the environment:
//   PULP_PACED_WAKE                "deadline-spin" (default) or "sleep-until"
//   PULP_PACED_WAKE_GUARD_NS       fixed guard; unset calibrates
//   PULP_PACED_WAKE_SPIN_LIMIT_NS  spin bound; unset derives it from the guard
//   PULP_PACED_WAKE_CALIBRATION    calibration samples (default 64)
//   PULP_PACED_INJECT_LATE_US      injected lateness negative (default 0)
//   PULP_PACED_INJECT_LATE_EVERY   apply it to every Nth callback (default 1)
inline WakeConfig read_wake_config() {
    WakeConfig config;
    const char* backend = std::getenv("PULP_PACED_WAKE");
    const std::string_view name = backend == nullptr ? std::string_view{} : backend;
    if (name.empty() || name == "deadline-spin")
        config.backend = WakeBackend::deadline_spin;
    else if (name == "sleep-until")
        config.backend = WakeBackend::sleep_until;
    else
        throw std::invalid_argument("malformed PULP_PACED_WAKE");
    if (const char* guard = std::getenv("PULP_PACED_WAKE_GUARD_NS"); guard && *guard)
        config.guard_ns =
            static_cast<std::int64_t>(wake_env_integer("PULP_PACED_WAKE_GUARD_NS", 0));
    config.spin_limit_ns =
        static_cast<std::int64_t>(wake_env_integer("PULP_PACED_WAKE_SPIN_LIMIT_NS", 0));
    config.calibration_samples = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        wake_env_integer("PULP_PACED_WAKE_CALIBRATION", 64), kMaxCalibrationSamples + 1));
    config.inject_late_us = wake_env_integer("PULP_PACED_INJECT_LATE_US", 0);
    config.inject_late_every = wake_env_integer("PULP_PACED_INJECT_LATE_EVERY", 1);
    if (config.calibration_samples > kMaxCalibrationSamples || config.inject_late_every == 0)
        throw std::invalid_argument("paced wake configuration out of range");
    return config;
}

} // namespace pulp::test::paced
