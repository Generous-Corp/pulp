#pragma once

#include <pulp/events/event_loop.hpp>
#include <atomic>
#include <cstdint>
#include <functional>

namespace pulp::events {

// Timer bound to an EventLoop. Supports one-shot and repeating.
class Timer {
public:
    using Callback = std::function<void()>;

    Timer(EventLoop& loop, Duration interval, Callback callback, bool repeating = true);
    ~Timer();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    void start();
    void stop();
    bool is_active() const { return active_.load(std::memory_order_acquire); }

    void set_interval(Duration interval);
    Duration interval() const { return interval_; }

private:
    void schedule_next(std::uint64_t gen);

    EventLoop& loop_;
    Duration interval_;
    Callback callback_;
    bool repeating_;
    std::atomic<bool> active_{false};
    // Cycle generation — incremented by stop() so that stale dispatch
    // lambdas scheduled before stop() return early when they fire. Cheap
    // replacement for the previous shared_ptr<atomic<bool>> sentinel,
    // which raced on the shared_ptr slot between start() (main thread)
    // and schedule_next() (event-loop thread). See #687 / #414.
    std::atomic<std::uint64_t> generation_{0};
};

} // namespace pulp::events
