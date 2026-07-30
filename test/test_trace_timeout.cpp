// test_trace_timeout.cpp — the Perfetto auto-flush timeout's lifetime and
// generation contract (WAH-4).
//
// What was wrong: a detached `std::thread` slept for `PULP_TRACE_SECONDS` and
// then called back into process-global tracing state. Nothing joined it, so the
// plug-in module could be unloaded (`FreeLibrary` / `dlclose`) while it still
// slept — it then woke into freed code. And nothing tagged it, so a timer armed
// for one capture could stop a LATER one: close and reopen an editor inside the
// window and the second capture was silently truncated.
//
// These tests run in the DEFAULT PULP_TRACING=OFF build, which is the only
// build CI exercises — that is why TimeoutController lives in a header rather
// than inside `#if PULP_TRACING_ENABLED`, where no gate would ever reach it.

#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/trace_timeout.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using pulp::runtime::detail::TimeoutController;
using pulp::runtime::detail::timeout_targets_current_session;
using namespace std::chrono_literals;

namespace {

/// Spin until `pred` holds or `limit` elapses. Returns whether it held.
/// Bounded so a broken timeout fails the test instead of hanging CI.
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds limit = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

}  // namespace

// ── The generation guard ────────────────────────────────────────────────────

TEST_CASE("a timeout only targets the session it was armed for",
          "[trace-timeout][wah-4]") {
    REQUIRE(timeout_targets_current_session(7, 7));
    // The exact case that truncated a re-opened editor's capture.
    REQUIRE_FALSE(timeout_targets_current_session(7, 8));
    REQUIRE_FALSE(timeout_targets_current_session(8, 7));
}

TEST_CASE("no session is never a valid target", "[trace-timeout][wah-4]") {
    // Generation 0 means "no session active". A timeout that fires after the
    // last detach has nothing to flush and must not try.
    REQUIRE_FALSE(timeout_targets_current_session(0, 0));
    REQUIRE_FALSE(timeout_targets_current_session(1, 0));
    REQUIRE_FALSE(timeout_targets_current_session(0, 1));
}

// ── Cancellation and joining ────────────────────────────────────────────────

TEST_CASE("cancelling before the deadline suppresses the callback",
          "[trace-timeout][wah-4]") {
    TimeoutController timer;
    std::atomic<int> fired{0};

    timer.arm(10s, /*generation*/ 1, [&](std::uint64_t) { ++fired; });
    REQUIRE(timer.armed());
    timer.cancel_and_join();

    // The whole point of the condition variable: cancel returns immediately
    // rather than after the remaining 10 seconds, and the callback never runs.
    REQUIRE(fired.load() == 0);
    REQUIRE_FALSE(timer.armed());
}

TEST_CASE("cancel_and_join returns only once the worker cannot run again",
          "[trace-timeout][wah-4]") {
    // This is the property that makes module unload safe. If cancel returned
    // while the worker was still runnable, `dlclose` could pull the code out
    // from under it.
    TimeoutController timer;
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};

    timer.arm(1ms, 1, [&](std::uint64_t) {
        running = true;
        std::this_thread::sleep_for(50ms);
        finished = true;
    });
    REQUIRE(wait_until([&] { return running.load(); }));

    timer.cancel_and_join();

    // Joined, so an in-flight callback has completed — not abandoned mid-way.
    REQUIRE(finished.load());
    REQUIRE_FALSE(timer.armed());
}

TEST_CASE("cancelling when nothing is armed is a no-op",
          "[trace-timeout][wah-4]") {
    TimeoutController timer;
    REQUIRE_FALSE(timer.armed());
    timer.cancel_and_join();  // must not hang or crash
    timer.cancel_and_join();
    REQUIRE_FALSE(timer.armed());
}

TEST_CASE("destroying a controller joins its armed timeout",
          "[trace-timeout][wah-4]") {
    std::atomic<int> fired{0};
    {
        TimeoutController timer;
        timer.arm(10s, 1, [&](std::uint64_t) { ++fired; });
    }  // ~TimeoutController must cancel + join, not detach and hope
    REQUIRE(fired.load() == 0);
}

// ── Expiry ──────────────────────────────────────────────────────────────────

TEST_CASE("an un-cancelled timeout fires with its own generation",
          "[trace-timeout][wah-4]") {
    TimeoutController timer;
    std::atomic<std::uint64_t> seen{0};

    timer.arm(1ms, /*generation*/ 42, [&](std::uint64_t g) { seen = g; });

    REQUIRE(wait_until([&] { return seen.load() != 0; }));
    REQUIRE(seen.load() == 42);
    timer.cancel_and_join();
}

TEST_CASE("re-arming replaces the previous timeout rather than adding one",
          "[trace-timeout][wah-4]") {
    // Restart-before-an-old-deadline: the first timer must be gone, not merely
    // outvoted, or two timers race to stop one session.
    TimeoutController timer;
    std::atomic<int> first{0};
    std::atomic<int> second{0};

    timer.arm(10s, 1, [&](std::uint64_t) { ++first; });
    timer.arm(1ms, 2, [&](std::uint64_t) { ++second; });

    REQUIRE(wait_until([&] { return second.load() == 1; }));
    REQUIRE(first.load() == 0);  // the 10s timer was cancelled, not left running
    timer.cancel_and_join();
    REQUIRE(second.load() == 1);  // and it fired exactly once
}

TEST_CASE("repeated arm/cancel cycles leave no thread behind",
          "[trace-timeout][wah-4]") {
    TimeoutController timer;
    std::atomic<int> fired{0};

    for (int i = 0; i < 20; ++i) {
        timer.arm(10s, static_cast<std::uint64_t>(i + 1),
                  [&](std::uint64_t) { ++fired; });
        timer.cancel_and_join();
        REQUIRE_FALSE(timer.armed());
    }

    REQUIRE(fired.load() == 0);
}

TEST_CASE("a timeout that already fired is still safely joinable",
          "[trace-timeout][wah-4]") {
    // The final detach calls cancel_and_join() unconditionally; it must cope
    // with a worker that has already run its callback and exited.
    TimeoutController timer;
    std::atomic<int> fired{0};
    timer.arm(1ms, 1, [&](std::uint64_t) { ++fired; });
    REQUIRE(wait_until([&] { return fired.load() == 1; }));

    timer.cancel_and_join();

    REQUIRE(fired.load() == 1);
    REQUIRE_FALSE(timer.armed());
}

TEST_CASE("an armed timeout with no callback is harmless",
          "[trace-timeout][wah-4]") {
    TimeoutController timer;
    timer.arm(1ms, 1, nullptr);
    std::this_thread::sleep_for(20ms);
    timer.cancel_and_join();
    SUCCEED("no crash");
}
