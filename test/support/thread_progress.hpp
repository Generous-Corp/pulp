#pragma once

// Deadline-bounded waits for a worker thread to make observable progress.
//
// A test that spawns a worker and then stops it on a fixed main-thread budget —
// a block count, a loop count, a sleep — has nothing ordering that worker's
// first iteration before the budget runs out. Under CPU saturation the whole
// budget can be spent before the worker is ever scheduled, leaving its counters
// at zero; the assertion that reads those counters then fails as `0 > 0` and
// reads as a product regression when the code under test was never entered.
//
// Assert on the worker's progress instead of on the budget: spend the budget,
// then wait for the outcome the assertion depends on. The deadline is what keeps
// that wait honest — a worker that genuinely never makes progress must fail the
// caller's assertion at the deadline, never hang the suite, and never let the
// test pass merely by waiting longer.
//
// These return a bool rather than asserting: Catch2 macros are not thread-safe
// and the result belongs in a CHECK/REQUIRE on the calling (test) thread.

#include <atomic>
#include <chrono>
#include <thread>

namespace pulp::test {

// Long enough that a saturated host still gets its worker scheduled, short
// enough that a genuinely stalled worker fails well inside a CTest timeout.
inline constexpr std::chrono::seconds kProgressDeadline{10};

// Poll `predicate` until it holds or the deadline passes; returns whether it
// held. Sleeps rather than spins — a busy yield loop can repeatedly win the
// scheduler on a low-core host and starve the very worker it waits for.
template <typename Predicate>
bool wait_for_condition(Predicate&& predicate,
                        std::chrono::steady_clock::duration timeout = kProgressDeadline) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// The common case: a counter the worker increments must become non-zero.
template <typename T>
bool wait_for_progress(const std::atomic<T>& counter,
                       std::chrono::steady_clock::duration timeout = kProgressDeadline) {
    return wait_for_condition(
        [&counter] { return counter.load(std::memory_order_relaxed) != T{}; }, timeout);
}

// For a test whose main thread is itself half the race — an audio render loop the
// worker publishes against. Sleeping would stop the contention the test exists to
// create, so `pump` runs once per poll iteration instead and supplies the yield.
template <typename Predicate, typename Pump>
bool pump_until(Predicate&& predicate, Pump&& pump,
                std::chrono::steady_clock::duration timeout = kProgressDeadline) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        pump();
    }
    return true;
}

}  // namespace pulp::test
