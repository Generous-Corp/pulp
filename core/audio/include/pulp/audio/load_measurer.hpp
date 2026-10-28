#pragma once

/// @file load_measurer.hpp
/// Real-time CPU load measurement for audio callbacks.

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

#if defined(__APPLE__) && !defined(__aarch64__)
#include <mach/mach_time.h>
#endif

namespace pulp::audio {

/// Snapshot published by AudioProcessLoadMeasurer.
///
/// All values are cheap telemetry for UI, inspector, and diagnostic polling.
/// They are intentionally latest-value counters, not an exact trace stream.
struct AudioProcessLoadSnapshot {
    float load = 0.0f;          ///< Smoothed CPU load. 1.0 == full buffer.
    float peak_load = 0.0f;     ///< Highest raw load since the last reset.
    float last_load = 0.0f;     ///< Most recent raw load sample.
    std::int64_t elapsed_ns = 0;
    std::int64_t available_ns = 0;
    std::uint64_t callback_count = 0;
    std::uint64_t overload_count = 0;
};

enum class AudioRuntimeOverloadSeverity : uint8_t {
    Nominal = 0,
    Watch,
    Overloaded,
    Critical,
};

struct AudioRuntimeOverloadPolicy {
    float watch_load = 0.75f;
    float overload_load = 1.0f;
    float critical_load = 1.25f;
    std::uint64_t watch_xruns = 1;
    std::uint64_t critical_xruns = 3;
    std::uint64_t watch_overloads = 1;
    std::uint64_t critical_overloads = 3;
};

struct AudioRuntimeOverloadReport {
    AudioRuntimeOverloadSeverity severity = AudioRuntimeOverloadSeverity::Nominal;
    bool should_shed_optional_work = false;
    bool should_bypass_optional_work = false;
    bool validation_failure = false;
    const char* action = "normal";
};

inline const char* to_string(AudioRuntimeOverloadSeverity severity) noexcept {
    switch (severity) {
        case AudioRuntimeOverloadSeverity::Nominal:    return "nominal";
        case AudioRuntimeOverloadSeverity::Watch:      return "watch";
        case AudioRuntimeOverloadSeverity::Overloaded: return "overloaded";
        case AudioRuntimeOverloadSeverity::Critical:   return "critical";
    }
    return "critical";
}

inline AudioRuntimeOverloadReport evaluate_audio_runtime_overload(
    const AudioProcessLoadSnapshot& load,
    std::uint64_t xrun_count,
    const AudioRuntimeOverloadPolicy& policy = {}) noexcept {
    const float watch_load =
        std::isfinite(policy.watch_load) && policy.watch_load > 0.0f
            ? policy.watch_load
            : 0.75f;
    const float overload_load =
        std::isfinite(policy.overload_load) && policy.overload_load > watch_load
            ? policy.overload_load
            : std::max(watch_load, 1.0f);
    const float critical_load =
        std::isfinite(policy.critical_load) && policy.critical_load > overload_load
            ? policy.critical_load
            : std::max(overload_load, 1.25f);
    const float observed_load =
        std::max({load.load, load.peak_load, load.last_load});

    AudioRuntimeOverloadReport report;
    if (observed_load >= critical_load
        || (policy.critical_xruns > 0 && xrun_count >= policy.critical_xruns)
        || (policy.critical_overloads > 0
            && load.overload_count >= policy.critical_overloads)) {
        report.severity = AudioRuntimeOverloadSeverity::Critical;
        report.should_shed_optional_work = true;
        report.should_bypass_optional_work = true;
        report.validation_failure = true;
        report.action = "bypass-optional-work";
        return report;
    }

    if (observed_load >= overload_load
        || (policy.watch_xruns > 0 && xrun_count >= policy.watch_xruns)
        || (policy.watch_overloads > 0
            && load.overload_count >= policy.watch_overloads)) {
        report.severity = AudioRuntimeOverloadSeverity::Overloaded;
        report.should_shed_optional_work = true;
        report.action = "shed-optional-work";
        return report;
    }

    if (observed_load >= watch_load) {
        report.severity = AudioRuntimeOverloadSeverity::Watch;
        report.action = "monitor";
        return report;
    }

    return report;
}

/// Measures the CPU load of an audio processing callback.
///
/// Call begin() at the start and end() at the end of your process() method.
/// The load is the fraction of available time consumed by processing.
///
/// The audio-thread hot path performs fixed arithmetic and relaxed atomic
/// stores only. It does not allocate, lock, or block. Polling threads may read
/// load(), peak_load(), or snapshot() concurrently; multi-field snapshots are
/// latest-value telemetry and may mix adjacent callbacks.
///
/// @code
/// void MyPlugin::process(BufferView& buffer) {
///     load_measurer_.begin(buffer.num_frames(), sample_rate_);
///     // ... DSP work ...
///     load_measurer_.end();
///     float cpu = load_measurer_.load(); // 0.0 to 1.0+
/// }
/// @endcode
class AudioProcessLoadMeasurer {
public:
    /// Call at the start of the audio callback.
    /// @param num_frames Number of samples in this buffer.
    /// @param sample_rate Current sample rate in Hz.
    void begin(int num_frames, float sample_rate) noexcept {
#if defined(PULP_AUDIOWORKLET_DISABLE_LOAD_TIMING)
        // AudioWorkletGlobalScope does not provide the clock import used by
        // steady_clock. Leave telemetry explicitly unavailable in that profile.
        (void)num_frames;
        (void)sample_rate;
        return;
#else
        start_time_ = clock::now();

        if (num_frames <= 0 || !(sample_rate > 0.0f)) {
            available_ns_ = 0;
            return;
        }

        const double available_ns =
            static_cast<double>(num_frames) / static_cast<double>(sample_rate) * 1e9;

        if (!std::isfinite(available_ns) || available_ns <= 0.0 ||
            available_ns > static_cast<double>(std::numeric_limits<int64_t>::max())) {
            available_ns_ = 0;
            return;
        }

        available_ns_ = static_cast<int64_t>(available_ns);
#endif
    }

    /// Call at the end of the audio callback.
    void end() noexcept {
#if defined(PULP_AUDIOWORKLET_DISABLE_LOAD_TIMING)
        return;
#else
        auto elapsed = clock::now() - start_time_;
        const int64_t elapsed_ns = std::max<int64_t>(
            0, std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());

        if (available_ns_ > 0) {
            float raw = static_cast<float>(elapsed_ns) / static_cast<float>(available_ns_);
            // Smooth with exponential moving average
            const float previous_load = load_.load(std::memory_order_relaxed);
            const float next_load = previous_load + smoothing_ * (raw - previous_load);
            const float previous_peak = peak_load_.load(std::memory_order_relaxed);
            load_.store(next_load, std::memory_order_relaxed);
            peak_load_.store(std::max(previous_peak, raw), std::memory_order_relaxed);
            last_load_.store(raw, std::memory_order_relaxed);
            last_elapsed_ns_.store(elapsed_ns, std::memory_order_relaxed);
            last_available_ns_.store(available_ns_, std::memory_order_relaxed);
            callback_count_.fetch_add(1, std::memory_order_relaxed);
            if (raw >= overload_threshold_) {
                overload_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }
#endif
    }

    /// Current smoothed CPU load (0.0 = idle, 1.0 = full buffer, >1.0 = overrun).
    float load() const noexcept { return load_.load(std::memory_order_relaxed); }

    /// Peak load since last reset.
    float peak_load() const noexcept { return peak_load_.load(std::memory_order_relaxed); }

    /// Last raw load sample.
    float last_load() const noexcept { return last_load_.load(std::memory_order_relaxed); }

    /// Most recent measured callback duration, in nanoseconds (0 before the first
    /// valid window). Cheap relaxed read; safe to poll while the audio thread runs.
    std::int64_t last_elapsed_ns() const noexcept {
        return last_elapsed_ns_.load(std::memory_order_relaxed);
    }

    /// Number of valid callback windows measured since the last reset.
    std::uint64_t callback_count() const noexcept {
        return callback_count_.load(std::memory_order_relaxed);
    }

    /// Number of valid callback windows whose raw load reached the overload threshold.
    std::uint64_t overload_count() const noexcept {
        return overload_count_.load(std::memory_order_relaxed);
    }

    /// Latest telemetry snapshot. Safe to poll from UI/diagnostic threads.
    AudioProcessLoadSnapshot snapshot() const noexcept {
        return AudioProcessLoadSnapshot{
            .load = load_.load(std::memory_order_relaxed),
            .peak_load = peak_load_.load(std::memory_order_relaxed),
            .last_load = last_load_.load(std::memory_order_relaxed),
            .elapsed_ns = last_elapsed_ns_.load(std::memory_order_relaxed),
            .available_ns = last_available_ns_.load(std::memory_order_relaxed),
            .callback_count = callback_count_.load(std::memory_order_relaxed),
            .overload_count = overload_count_.load(std::memory_order_relaxed),
        };
    }

    /// Reset peak load tracker.
    void reset_peak() noexcept { peak_load_.store(0.0f, std::memory_order_relaxed); }

    /// Reset all state.
    void reset() noexcept {
        load_.store(0.0f, std::memory_order_relaxed);
        peak_load_.store(0.0f, std::memory_order_relaxed);
        last_load_.store(0.0f, std::memory_order_relaxed);
        last_elapsed_ns_.store(0, std::memory_order_relaxed);
        last_available_ns_.store(0, std::memory_order_relaxed);
        callback_count_.store(0, std::memory_order_relaxed);
        overload_count_.store(0, std::memory_order_relaxed);
        available_ns_ = 0;
        start_time_ = {};
    }

    /// Set smoothing factor (0 = no smoothing, 1 = no averaging).
    /// Default is 0.1 (responsive but stable).
    void set_smoothing(float alpha) noexcept {
        smoothing_ = std::clamp(alpha, 0.0f, 1.0f);
    }

    /// Set the raw-load threshold that increments overload_count().
    /// Default is 1.0, meaning the callback consumed the full buffer budget.
    void set_overload_threshold(float load) noexcept {
        overload_threshold_ = (std::isfinite(load) && load > 0.0f) ? load : 1.0f;
    }

  private:
    // A monotonic clock read twice per measured span. On Apple platforms
    // std::chrono::steady_clock goes through clock_gettime() and
    // mach_continuous_time(), which showed as about 8% of a small SignalGraph
    // block for the always-on whole-graph span alone. Continuous time also counts
    // system sleep, which a callback span never contains, so a cheaper uptime
    // counter measures the same durations:
    // - Apple arm64 reads the architectural virtual counter directly. An ISB
    //   orders the counter read without the heavier mach_absolute_time() path,
    //   which cost about 10 ns per read inside a graph block. Its rate comes
    //   from CNTFRQ_EL0, read once: it differs across chips (24 MHz on M1,
    //   1 GHz on M5) and need not match mach_absolute_time()'s timebase. Both
    //   ends of a span use the same ordered counter, and a span that would
    //   come out negative is clamped to zero.
    // - Other Apple targets use mach_absolute_time().
    struct clock {
        using duration = std::chrono::nanoseconds;
        using rep = duration::rep;
        using period = duration::period;
        using time_point = std::chrono::time_point<clock, duration>;
        static constexpr bool is_steady = true;

        static time_point now() noexcept {
#if defined(__APPLE__) && defined(__aarch64__)
            // Read the counter frequency on each call. This is one register
            // read and avoids function-local static initialization on the first
            // audio callback. The architectural counter is stable for the life
            // of the system; a zero frequency is treated as an unavailable
            // clock rather than dividing by zero.
            std::uint64_t frequency = 0;
            asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
            std::uint64_t ticks = 0;
            asm volatile("isb" ::: "memory");
            asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
            if (frequency == 0)
                return time_point{};
            const auto nanos = (static_cast<unsigned __int128>(ticks) * 1000000000u) /
                               static_cast<unsigned __int128>(frequency);
            return time_point(duration(static_cast<rep>(nanos)));
#elif defined(__APPLE__)
            // Avoid guarded function-local initialization on the first
            // callback. mach_timebase_info is a read-only kernel query; its
            // result is stable, and this path is used only on non-arm64 Apple
            // targets where the callback budget is less constrained.
            mach_timebase_info_data_t timebase{};
            mach_timebase_info(&timebase);
            const std::uint64_t ticks = mach_absolute_time();
            return time_point(duration(static_cast<rep>(ticks * timebase.numer / timebase.denom)));
#else
            return time_point(std::chrono::duration_cast<duration>(
                std::chrono::steady_clock::now().time_since_epoch()));
#endif
        }
    };
    using time_point = clock::time_point;

    time_point start_time_{};
    int64_t available_ns_ = 0;
    std::atomic<float> load_{0.0f};
    std::atomic<float> peak_load_{0.0f};
    std::atomic<float> last_load_{0.0f};
    std::atomic<std::int64_t> last_elapsed_ns_{0};
    std::atomic<std::int64_t> last_available_ns_{0};
    std::atomic<std::uint64_t> callback_count_{0};
    std::atomic<std::uint64_t> overload_count_{0};
    float smoothing_ = 0.1f;
    float overload_threshold_ = 1.0f;
};

} // namespace pulp::audio
