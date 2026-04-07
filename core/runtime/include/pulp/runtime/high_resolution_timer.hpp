#pragma once

// High-resolution periodic timer for sub-millisecond callbacks.
// Uses platform-specific high-precision timer APIs.

#include <atomic>
#include <functional>
#include <thread>
#include <chrono>

namespace pulp::runtime {

class HighResolutionTimer {
public:
    HighResolutionTimer() = default;
    ~HighResolutionTimer();

    /// Start the timer with the given interval. Callback runs on a dedicated thread.
    void start(std::chrono::microseconds interval, std::function<void()> callback);

    /// Stop the timer.
    void stop();

    /// Whether the timer is currently running.
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // No copy or move
    HighResolutionTimer(const HighResolutionTimer&) = delete;
    HighResolutionTimer& operator=(const HighResolutionTimer&) = delete;

private:
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::function<void()> callback_;
};

}  // namespace pulp::runtime
