#include <catch2/catch_test_macros.hpp>
#include <pulp/events/events.hpp>
#include <pulp/events/async_updater.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace pulp::events;
using namespace std::chrono_literals;

namespace {

template <typename Predicate>
bool wait_until(Predicate&& predicate, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

struct RecordingMultiTimer : MultiTimer {
    void timer_callback(int timer_id) override {
        callbacks.push_back(timer_id);
    }

    std::vector<int> callbacks;
};

} // namespace

TEST_CASE("EventLoop dispatch", "[events][event_loop]") {
    EventLoop loop;

    SECTION("Dispatch executes task") {
        std::atomic<int> value{0};
        loop.dispatch([&] { value.store(42); });
        REQUIRE(wait_until([&] { return value.load() == 42; }, 2000ms));
    }

    SECTION("Multiple dispatches execute in order") {
        std::atomic<int> done_count{0};
        std::vector<int> results;
        std::mutex mtx;

        for (int i = 0; i < 10; ++i) {
            loop.dispatch([&, i] {
                std::lock_guard lock(mtx);
                results.push_back(i);
                done_count.fetch_add(1);
            });
        }

        REQUIRE(wait_until([&] { return done_count.load() == 10; }, 2000ms));
        std::lock_guard lock(mtx);
        REQUIRE(results.size() == 10);
        for (int i = 0; i < 10; ++i) {
            REQUIRE(results[i] == i);
        }
    }

    SECTION("Dispatch from multiple threads") {
        std::atomic<int> count{0};
        std::vector<std::thread> threads;

        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 100; ++i) {
                    loop.dispatch([&] { count.fetch_add(1); });
                }
            });
        }

        for (auto& t : threads) t.join();
        REQUIRE(wait_until([&] { return count.load() == 400; }, 5000ms));
    }
}

TEST_CASE("EventLoop dispatch_after", "[events][event_loop]") {
    EventLoop loop;

    SECTION("Delayed task executes after delay") {
        std::atomic<bool> executed{false};
        auto start = std::chrono::steady_clock::now();

        loop.dispatch_after(50ms, [&] { executed.store(true); });

        REQUIRE(wait_until([&] { return executed.load(); }, 2000ms));
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        REQUIRE(elapsed >= 10ms);
    }
}

TEST_CASE("EventLoop is non-singleton", "[events][event_loop]") {
    EventLoop loop1;
    EventLoop loop2;

    std::atomic<int> v1{0}, v2{0};
    loop1.dispatch([&] { v1.store(1); });
    loop2.dispatch([&] { v2.store(2); });

    REQUIRE(wait_until([&] { return v1.load() == 1 && v2.load() == 2; }, 2000ms));
}

TEST_CASE("Timer basic operation", "[events][timer]") {
    EventLoop loop;

    SECTION("Repeating timer fires multiple times") {
        std::atomic<int> count{0};
        Timer timer(loop, 20ms, [&] { count.fetch_add(1); }, true);
        timer.start();

        REQUIRE(wait_until([&] { return count.load() >= 3; }, 500ms));
        timer.stop();
    }

    SECTION("One-shot timer fires once") {
        std::atomic<int> count{0};
        Timer timer(loop, 20ms, [&] { count.fetch_add(1); }, false);
        timer.start();

        REQUIRE(wait_until([&] { return count.load() >= 1; }, 300ms));
        REQUIRE(count.load() == 1);
    }

    SECTION("Stopped timer does not fire") {
        std::atomic<int> count{0};
        Timer timer(loop, 10ms, [&] { count.fetch_add(1); }, true);
        timer.start();
        timer.stop();

        std::this_thread::sleep_for(50ms);
        REQUIRE(count.load() == 0);
    }

    SECTION("Timer resumes firing after stop + start") {
        std::atomic<int> count{0};
        Timer timer(loop, 10ms, [&] { count.fetch_add(1); }, true);
        timer.start();
        REQUIRE(wait_until([&] { return count.load() >= 2; }, 500ms));
        timer.stop();
        auto captured = count.load();
        std::this_thread::sleep_for(50ms);
        REQUIRE(count.load() == captured);  // stopped: no more ticks

        timer.start();
        REQUIRE(wait_until([&] { return count.load() > captured; }, 500ms));
        timer.stop();
    }

    SECTION("start() is idempotent when already running (#438 P1 / #428)") {
        // Before the fix, an unconditional `alive_ = make_shared<>`
        // in start() races against schedule_next()'s `auto alive = alive_;`
        // if a second start() lands while the timer is already active.
        // After the fix, start() early-returns when active_ is true.
        //
        // The test can't deterministically prove the race is gone (TSan
        // does that on the CI sanitizer job), but it can assert that
        // calling start() twice in a row doesn't break repeat behavior
        // or double-schedule.
        std::atomic<int> count{0};
        Timer timer(loop, 10ms, [&] { count.fetch_add(1); }, true);
        timer.start();
        timer.start();  // second start — should no-op, not reschedule
        REQUIRE(wait_until([&] { return count.load() >= 3; }, 500ms));
        timer.stop();
        auto captured = count.load();
        std::this_thread::sleep_for(50ms);
        // Still stopped after the double-start + single-stop pair:
        REQUIRE(count.load() == captured);
    }
}

// #687 — stop()/start() pairs repeatedly on the owner thread while the
// event-loop thread is running timer dispatches. Pre-fix, start()'s
// `alive_ = make_shared<>(...)` raced with schedule_next()'s
// `auto alive = alive_;` read on the loop thread. Post-fix, alive_ is
// gone — stop() bumps generation_ (atomic) so stale dispatches early
// return. Under TSan this loop reliably surfaced the old race within
// a couple of iterations; 50 iterations is a comfortable margin.
TEST_CASE("Timer stop+start hammer (TSan-clean, #687)",
          "[events][timer][tsan][issue-687]") {
    EventLoop loop;
    std::atomic<int> count{0};
    Timer timer(loop, 1ms, [&] { count.fetch_add(1); }, true);
    for (int i = 0; i < 50; ++i) {
        timer.start();
        // Let the loop run at least one dispatch before stopping.
        std::this_thread::sleep_for(3ms);
        timer.stop();
        // Sleep long enough that any in-flight dispatch from this
        // cycle fires (and hits the generation/active checks) before
        // we hand start() a fresh cycle.
        std::this_thread::sleep_for(2ms);
    }
    // No functional claim about count — this test exists for TSan.
    SUCCEED("stop+start hammer completed without races");
}

// #716 — Timer destroyed while its EventLoop still has a queued dispatch.
// Pre-fix, schedule_next captured a raw `self` pointer; if the user-visible
// Timer dropped before the lambda fired, the lambda read `self->active_` /
// `self->generation_` from freed memory. ASan + TSan would have caught it
// eventually but neither test targeted the destroy-mid-dispatch window
// specifically. Post-fix, dispatch lambdas hold a shared_ptr<Impl> that
// keeps state alive past Timer destruction.
TEST_CASE("Timer destroy-while-dispatched is UAF-free (#716)",
          "[events][timer][asan][issue-716]") {
    EventLoop loop;
    auto count = std::make_shared<std::atomic<int>>(0);

    // 50 iterations so timing-window regressions surface under any of
    // ASan, TSan, or UBSan. Each iteration: start a 1ms timer, let it
    // dispatch once, then destroy it while the NEXT dispatch may still
    // be queued. Without the shared_ptr<Impl> fix this would UAF.
    for (int i = 0; i < 50; ++i) {
        {
            Timer t(loop, 1ms, [count] { count->fetch_add(1); }, true);
            t.start();
            std::this_thread::sleep_for(3ms);
            // t.~Timer() runs here; any already-dispatched lambdas still
            // pending in loop_ must finish safely.
        }
        // Give the loop thread a chance to fire the queued lambda after
        // Timer destruction. With the fix, it no-ops via generation
        // check; without the fix, it'd be a UAF.
        std::this_thread::sleep_for(3ms);
    }
    SUCCEED("destroy-while-dispatched completed without UAF");
}

// ── EventLoop lifecycle edges ───────────────────────────────────────────

TEST_CASE("EventLoop destructor tolerates pending dispatches",
          "[events][event_loop][lifecycle]") {
    // Build a loop, dispatch tasks without waiting, then let the
    // destructor run. Must not crash or hang. A counter is captured
    // by pointer to outlive the loop so any late dispatch doesn't UB.
    auto counter = std::make_shared<std::atomic<int>>(0);
    {
        EventLoop loop;
        for (int i = 0; i < 100; ++i) {
            loop.dispatch([counter] { counter->fetch_add(1); });
        }
        // Loop goes out of scope immediately — some tasks may run,
        // others may not; exit path must not leak.
    }
    SUCCEED("EventLoop destructor completed with pending work queued");
}

// ── AsyncUpdater coalescing contract ────────────────────────────────────

TEST_CASE("AsyncUpdater coalesces rapid triggers into one handle call",
          "[events][async_updater][coalesce]") {
    // The whole point of AsyncUpdater is that trigger_async_update()
    // from any thread between process_pending() calls collapses to a
    // single handle_async_update() dispatch.
    std::atomic<int> handles{0};
    LambdaAsyncUpdater u([&] { handles.fetch_add(1); });

    for (int i = 0; i < 1000; ++i) u.trigger_async_update();
    REQUIRE(u.is_update_pending());

    u.process_pending();
    REQUIRE(handles.load() == 1);
    REQUIRE_FALSE(u.is_update_pending());
}

TEST_CASE("AsyncUpdater cancel_pending_update drops the queued handle",
          "[events][async_updater][cancel]") {
    std::atomic<int> handles{0};
    LambdaAsyncUpdater u([&] { handles.fetch_add(1); });

    u.trigger_async_update();
    REQUIRE(u.is_update_pending());
    u.cancel_pending_update();
    REQUIRE_FALSE(u.is_update_pending());

    u.process_pending();
    REQUIRE(handles.load() == 0);
}

TEST_CASE("AsyncUpdater trigger then process_pending runs once even from N threads",
          "[events][async_updater][threading]") {
    std::atomic<int> handles{0};
    LambdaAsyncUpdater u([&] { handles.fetch_add(1); });

    constexpr int kWorkers = 8;
    constexpr int kTriggersPerWorker = 500;
    std::vector<std::thread> threads;
    threads.reserve(kWorkers);
    for (int t = 0; t < kWorkers; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kTriggersPerWorker; ++i) {
                u.trigger_async_update();
            }
        });
    }
    for (auto& t : threads) t.join();

    u.process_pending();
    // Because every thread may have won the CAS at different times,
    // handle count is either 0 (no trigger between joins and process)
    // or 1 (at least one trigger landed). It CANNOT exceed 1.
    REQUIRE(handles.load() <= 1);
    REQUIRE(handles.load() >= 1);   // there were definitely triggers
    REQUIRE_FALSE(u.is_update_pending());
}

TEST_CASE("AsyncUpdater re-arms after process_pending and fires again",
          "[events][async_updater][rearm]") {
    std::atomic<int> handles{0};
    LambdaAsyncUpdater u([&] { handles.fetch_add(1); });

    u.trigger_async_update();
    u.process_pending();
    REQUIRE(handles.load() == 1);

    // Second trigger after handling must not be ignored.
    u.trigger_async_update();
    REQUIRE(u.is_update_pending());
    u.process_pending();
    REQUIRE(handles.load() == 2);
}

TEST_CASE("AsyncUpdater process_pending with no trigger is a no-op",
          "[events][async_updater][idempotence]") {
    std::atomic<int> handles{0};
    LambdaAsyncUpdater u([&] { handles.fetch_add(1); });

    u.process_pending();
    u.process_pending();
    u.process_pending();
    REQUIRE(handles.load() == 0);
}

TEST_CASE("MultiTimer tracks timer lifecycle by id",
          "[events][async_updater][multi_timer]") {
    RecordingMultiTimer timers;

    REQUIRE_FALSE(timers.is_timer_running(1));

    timers.start_timer(1, 20);
    REQUIRE(timers.is_timer_running(1));

    timers.start_timer(1, 5);
    REQUIRE(timers.is_timer_running(1));

    timers.start_timer(2, 10);
    REQUIRE(timers.is_timer_running(2));

    timers.stop_timer(1);
    REQUIRE_FALSE(timers.is_timer_running(1));
    REQUIRE(timers.is_timer_running(2));

    timers.stop_timer(99);
    REQUIRE_FALSE(timers.is_timer_running(99));
}

TEST_CASE("MultiTimer stop_all_timers clears every active timer",
          "[events][async_updater][multi_timer]") {
    RecordingMultiTimer timers;

    timers.start_timer(1, 20);
    timers.start_timer(2, 10);
    timers.start_timer(3, 5);
    REQUIRE(timers.is_timer_running(1));
    REQUIRE(timers.is_timer_running(2));
    REQUIRE(timers.is_timer_running(3));

    timers.stop_all_timers();
    REQUIRE_FALSE(timers.is_timer_running(1));
    REQUIRE_FALSE(timers.is_timer_running(2));
    REQUIRE_FALSE(timers.is_timer_running(3));
}

TEST_CASE("ActionBroadcaster adds, removes, and notifies listeners",
          "[events][async_updater][action_broadcaster]") {
    ActionBroadcaster broadcaster;
    std::vector<std::string> seen;

    auto first = broadcaster.add_listener(
        [&](std::string_view action) { seen.emplace_back(action); });
    auto second = broadcaster.add_listener(
        [&](std::string_view action) { seen.emplace_back("second:" + std::string(action)); });

    broadcaster.send_action("refresh");
    REQUIRE(seen == std::vector<std::string>{"refresh", "second:refresh"});

    broadcaster.remove_listener(first);
    broadcaster.send_action("rebuild");
    REQUIRE(seen == std::vector<std::string>{
        "refresh", "second:refresh", "second:rebuild"});

    broadcaster.remove_listener(second);
    broadcaster.send_action("ignored");
    REQUIRE(seen.size() == 3);
}

TEST_CASE("ScopedLowPowerModeDisabler is constructible as an RAII guard",
          "[events][async_updater][power]") {
    {
        ScopedLowPowerModeDisabler guard;
        SUCCEED("construction completed");
    }

    SUCCEED("destruction completed");
}
