#pragma once

// trace_timeout.hpp — the owned, interruptible timeout behind Perfetto's
// auto-flush (WAH-4).
//
// It lives in its own header, compiled UNCONDITIONALLY, for the same reason
// `tracing_reminder_first_time()` does: Pulp's default build is
// PULP_TRACING=OFF, so anything defined only inside `#if PULP_TRACING_ENABLED`
// is never exercised by CI. The lifetime rules here are exactly the ones that
// were wrong, so they need a gate that actually runs.
//
// What it replaces: a detached `std::thread` that slept for N seconds and then
// called back into process-global tracing state. Two failures, both reachable
// from an ordinary DAW session:
//
//   * The plug-in module can be unloaded (`FreeLibrary` / `dlclose`) while that
//     thread still sleeps. It then wakes into freed code — on Windows, inside
//     the loader lock, which is a hard crash rather than a diagnosable fault.
//   * A timer armed for one capture could stop a LATER one. Close and reopen an
//     editor inside the `PULP_TRACE_SECONDS` window and the second capture was
//     silently truncated by the first one's timer.
//
// So: cancellation is immediate (a condition variable, not "after the remaining
// sleep"), the thread is joinable and joined by the final detach, and every
// timeout carries the session generation it was armed for.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace pulp::runtime::detail {

/// True when a timeout armed for `armed_for` may still act on the session
/// currently identified by `current`.
///
/// Pure so the rule is testable without a live Perfetto session. `0` means "no
/// session", which nothing may act on — a timeout that fires after the last
/// detach has nothing left to flush.
constexpr bool timeout_targets_current_session(std::uint64_t armed_for,
                                               std::uint64_t current) noexcept {
    return armed_for != 0 && armed_for == current;
}

/// A single owned, cancellable, joinable timeout.
///
/// Not thread-safe against concurrent `arm()` calls — the caller serializes
/// them (trace.cpp holds a dedicated mutex). `cancel_and_join()` is safe to
/// call from any thread EXCEPT the worker itself, which would self-join.
class TimeoutController {
public:
    TimeoutController() = default;
    TimeoutController(const TimeoutController&) = delete;
    TimeoutController& operator=(const TimeoutController&) = delete;
    /// Joins any armed timeout. A controller must never outlive its worker.
    ~TimeoutController() { cancel_and_join(); }

    /// Arm `on_expire(generation)` to run after `delay`. Any previously armed
    /// timeout is cancelled and joined first, so two arms cannot leave two
    /// timers running against one session.
    void arm(std::chrono::milliseconds delay, std::uint64_t generation,
             std::function<void(std::uint64_t)> on_expire) {
        cancel_and_join();
        {
            std::lock_guard<std::mutex> lk(mu_);
            cancelled_ = false;
        }
        worker_ = std::thread([this, delay, generation,
                               on_expire = std::move(on_expire)] {
            {
                std::unique_lock<std::mutex> lk(mu_);
                // The predicate makes this immune to spurious wakeups AND to a
                // cancel that lands before the wait even starts — a real race,
                // because arm() releases mu_ before the thread runs.
                if (cv_.wait_for(lk, delay, [this] { return cancelled_; }))
                    return;
            }
            // Called with mu_ RELEASED: the callback re-enters tracing state
            // and may block, and holding mu_ here would make cancel_and_join()
            // wait on it.
            if (on_expire) on_expire(generation);
        });
    }

    /// Cancel an armed timeout and join its thread. No-op when nothing is
    /// armed. Returns only once the worker is guaranteed not to run again —
    /// which is what makes module unload safe.
    void cancel_and_join() {
        if (!worker_.joinable()) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cancelled_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }

    /// True while a timeout thread exists (armed, running, or finished but not
    /// yet joined).
    bool armed() const noexcept { return worker_.joinable(); }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool cancelled_ = false;
    std::thread worker_;
};

}  // namespace pulp::runtime::detail
