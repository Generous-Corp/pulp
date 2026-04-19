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
