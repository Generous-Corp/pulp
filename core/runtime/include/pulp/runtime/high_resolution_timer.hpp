#pragma once

// High-resolution periodic timer for sub-millisecond callbacks.
// Uses platform-specific high-precision timer APIs.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>

namespace pulp::runtime {

/// Backend selection for the high-resolution timer.
///
/// - `DedicatedThread` (default): a Pulp-owned worker thread spins or
///   sleeps to the next deadline. Cross-platform; ~1 ms jitter under
///   load. Use when no OS timer queue is available or when the
///   workload runs outside an event loop.
/// - `OsTimerQueue`: backed by the platform's native high-resolution
///   timer queue (macOS / iOS: `dispatch_source_create` on a leeway-0
///   timer). No Pulp-owned thread; callbacks fire on the dispatch
///   queue. Lower jitter on supported platforms. Falls back to
///   `DedicatedThread` on platforms without an equivalent surface
///   (Linux desktop) so the contract holds.
enum class TimerMode {
    DedicatedThread,
    OsTimerQueue,
};

class HighResolutionTimer {
public:
    HighResolutionTimer() = default;
    ~HighResolutionTimer();

    /// Start the timer with the given interval. Callback runs on a dedicated thread.
    void start(std::chrono::microseconds interval, std::function<void()> callback);

    /// Start the timer in the requested mode.
    /// Falls back to `DedicatedThread` if the requested backend is
    /// unavailable on this platform.
    void start(std::chrono::microseconds interval,
               std::function<void()> callback,
               TimerMode mode);

    /// Stop the timer.
    void stop();

    /// Whether the timer is currently running.
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    /// The backend actually in use for the current run (may differ from
    /// the requested mode when the platform doesn't support it).
    TimerMode active_mode() const { return active_mode_; }

    // No copy or move
    HighResolutionTimer(const HighResolutionTimer&) = delete;
    HighResolutionTimer& operator=(const HighResolutionTimer&) = delete;

private:
    struct TimerState;

    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::thread thread_;
    std::shared_ptr<TimerState> state_;
    TimerMode active_mode_ = TimerMode::DedicatedThread;
    void* dispatch_source_ = nullptr; ///< `dispatch_source_t` cast to void* (Apple only).
};

}  // namespace pulp::runtime
