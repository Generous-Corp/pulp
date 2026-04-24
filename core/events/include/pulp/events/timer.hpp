#pragma once

#include <pulp/events/event_loop.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace pulp::events {

// Timer bound to an EventLoop. Supports one-shot and repeating.
//
// Thread-safety + lifetime design (#414 / #687 / #716):
//
// - Mutable state (active, generation, callback, interval, loop ref) lives
//   in a `shared_ptr<TimerImpl>` assigned exactly once at construction and
//   never reassigned. That single-assignment avoids the #414 TSan race on
//   the shared_ptr slot.
// - Dispatch lambdas capture the shared_ptr by value. If the user-visible
//   Timer is destroyed while the EventLoop still has a queued dispatch,
//   the lambda's captured shared_ptr keeps TimerImpl alive until the
//   lambda finishes. No UAF. (#716 — Codex P1 on #689.)
// - stop() bumps `generation` so any in-flight dispatch whose captured gen
//   no longer matches returns early before calling the user callback.
class Timer {
public:
    using Callback = std::function<void()>;

    Timer(EventLoop& loop, Duration interval, Callback callback, bool repeating = true);
    ~Timer();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    void start();
    void stop();
    bool is_active() const;

    void set_interval(Duration interval);
    Duration interval() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;

    // Static helpers keep the dispatch lambda free of `this` captures —
    // the lambda only holds a shared_ptr<Impl> so post-destruction firing
    // is safe.
    static void schedule_next(std::shared_ptr<Impl> impl, std::uint64_t gen);
};

} // namespace pulp::events
