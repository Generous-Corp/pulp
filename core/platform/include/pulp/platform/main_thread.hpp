#pragma once

/// @file main_thread.hpp
/// Main-thread posting and timers that keep running inside nested run loops.
///
/// An editor's UI work runs on the host's main thread. Two properties matter
/// there, and Grand Central Dispatch's main queue gives neither:
///
/// 1. **Work keeps running while the run loop is nested.** Menu tracking, live
///    window resize and modal panels run the main run loop in their own modes.
///    The main dispatch queue is drained in the common modes, but it is serial:
///    while one main-queue block runs a nested loop (a synchronous menu, a
///    modal panel, a resize started from a block), no other main-queue block or
///    dispatch timer runs. Code here uses a `CFRunLoopSource` and
///    `CFRunLoopTimer`s added to `kCFRunLoopCommonModes`, which the run loop
///    services from any nesting depth.
/// 2. **Nothing runs after its owner is gone.** A plug-in binary can be
///    unloaded right after its editor closes. A queued dispatch block cannot be
///    withdrawn, so it would still call into the unloaded image. Destroying a
///    MainThreadScope invalidates its run-loop source and timers, so the run
///    loop holds no callback into this binary afterwards, and drops every task
///    still queued.
///
/// The common modes include `NSEventTrackingRunLoopMode` and
/// `NSModalPanelRunLoopMode` once AppKit's NSApplication exists, which is
/// always true inside a host.
///
/// Off Apple platforms there is no backend yet: main_thread_supported() is
/// false, post() returns false and timers never fire.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace pulp::platform {

/// Whether this platform has a main-thread backend for MainThreadScope and
/// MainThreadTimer.
bool main_thread_supported() noexcept;

/// Whether the calling thread is the process main thread. False on platforms
/// without a backend.
bool is_main_thread() noexcept;

namespace detail {
struct MainThreadScopeState;
struct MainThreadTimerState;
/// How many times the run loop has called into this layer (source performs
/// plus timer fires), process-wide. Lets a test prove an idle scope costs the
/// main thread nothing without counting unrelated run-loop wake-ups.
std::uint64_t main_thread_callouts() noexcept;
} // namespace detail

/// Owner of the work one client (typically one editor) schedules on the main
/// thread.
///
/// Construct and destroy it on the main thread. When the destructor returns,
/// no task posted through it and no timer bound to it will run again, and no
/// run-loop callback into this binary remains. Destroying the scope from
/// inside one of its own tasks or timers is allowed; the remaining queued
/// tasks are dropped.
class MainThreadScope {
  public:
    MainThreadScope();
    ~MainThreadScope();
    MainThreadScope(const MainThreadScope&) = delete;
    MainThreadScope& operator=(const MainThreadScope&) = delete;

    /// Run `fn` on the main thread, in the common run-loop modes, after the
    /// current task returns. Callable from any thread. Tasks run in the order
    /// they were posted. Returns false, without keeping `fn`, when the scope is
    /// gone or the platform has no backend. A task still queued when the scope
    /// dies is dropped and never runs. A task that throws is contained.
    bool post(std::function<void()> fn);

    /// Copyable, thread-safe weak reference for code that may outlive the
    /// scope (a worker thread, a completion handler).
    class Handle {
      public:
        Handle() = default;
        /// As MainThreadScope::post; false once the scope is gone.
        bool post(std::function<void()> fn) const;
        /// Whether the scope still exists. A snapshot: it can turn false right
        /// after it returns true.
        bool alive() const;

      private:
        friend class MainThreadScope;
        explicit Handle(std::weak_ptr<detail::MainThreadScopeState> state);
        std::weak_ptr<detail::MainThreadScopeState> state_;
    };

    Handle handle() const;

  private:
    friend class MainThreadTimer;
    std::shared_ptr<detail::MainThreadScopeState> state_;
};

/// A main-thread timer bound to a MainThreadScope, firing in the common
/// run-loop modes with zero tolerance (millisecond granularity).
///
/// It is one-shot and reschedulable: fire_after() replaces any pending fire.
/// fire_every() repeats until cancel() or another fire_after(). All members
/// run on the main thread. Cancelling or destroying the timer, or destroying
/// its scope, from inside `on_fire` is allowed. When the scope dies first the
/// timer stops for good and its calls do nothing.
class MainThreadTimer {
  public:
    MainThreadTimer(MainThreadScope& scope, std::function<void()> on_fire);
    ~MainThreadTimer();
    MainThreadTimer(const MainThreadTimer&) = delete;
    MainThreadTimer& operator=(const MainThreadTimer&) = delete;

    /// Fire once, `delay` from now (0 means on the next run-loop pass).
    /// Replaces a pending fire or a repeating schedule.
    void fire_after(std::chrono::milliseconds delay);
    /// Fire every `period` (at least 1 ms), the first time one period from now.
    void fire_every(std::chrono::milliseconds period);
    /// Stop. `on_fire` is not called again until the next fire_after or
    /// fire_every.
    void cancel();
    /// Whether a fire is scheduled.
    bool pending() const;

    /// When the run loop will fire this timer next, read back from the
    /// run-loop timer itself; nullopt when nothing is pending.
    std::optional<std::chrono::steady_clock::time_point> next_fire() const;
    /// The tolerance the run loop was given for this timer (zero by design).
    std::chrono::microseconds tolerance() const;

  private:
    std::shared_ptr<detail::MainThreadTimerState> state_;
};

} // namespace pulp::platform
