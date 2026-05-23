#include <catch2/catch_test_macros.hpp>
#include <pulp/events/events.hpp>

#include <atomic>
#include <chrono>
#include <thread>

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
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

}  // namespace

TEST_CASE("Timer one-shot deactivates after firing and can be restarted",
          "[events][timer][issue-642]") {
    EventLoop loop;
    std::atomic<int> calls{0};
    Timer timer(loop, 5ms, [&] { calls.fetch_add(1); }, false);

    timer.start();
    REQUIRE(timer.is_active());
    REQUIRE(wait_until([&] { return calls.load() == 1; }, 500ms));
    REQUIRE_FALSE(timer.is_active());

    std::this_thread::sleep_for(25ms);
    REQUIRE(calls.load() == 1);

    timer.start();
    REQUIRE(timer.is_active());
    REQUIRE(wait_until([&] { return calls.load() == 2; }, 500ms));
    REQUIRE_FALSE(timer.is_active());
}

TEST_CASE("Timer start is idempotent while active",
          "[events][timer][coverage]") {
    EventLoop loop;
    std::atomic<int> calls{0};
    Timer timer(loop, 10ms, [&] { calls.fetch_add(1); }, false);

    timer.start();
    timer.start();
    timer.start();
    REQUIRE(timer.is_active());

    REQUIRE(wait_until([&] { return calls.load() == 1; }, 500ms));
    REQUIRE_FALSE(timer.is_active());

    std::this_thread::sleep_for(35ms);
    REQUIRE(calls.load() == 1);
}

TEST_CASE("Timer stop cancels queued one-shot callback",
          "[events][timer][coverage]") {
    EventLoop loop;
    std::atomic<int> calls{0};
    Timer timer(loop, 25ms, [&] { calls.fetch_add(1); }, false);

    timer.start();
    REQUIRE(timer.is_active());
    timer.stop();
    REQUIRE_FALSE(timer.is_active());

    std::this_thread::sleep_for(75ms);
    REQUIRE(calls.load() == 0);
}

TEST_CASE("Timer interval can be changed before restart",
          "[events][timer][coverage]") {
    EventLoop loop;
    std::atomic<int> calls{0};
    Timer timer(loop, 100ms, [&] { calls.fetch_add(1); }, false);

    REQUIRE(timer.interval() == 100ms);
    timer.set_interval(5ms);
    REQUIRE(timer.interval() == 5ms);

    timer.start();
    REQUIRE(wait_until([&] { return calls.load() == 1; }, 500ms));
    REQUIRE_FALSE(timer.is_active());
}

TEST_CASE("Timer with empty callback still tracks one-shot lifecycle",
          "[events][timer][coverage]") {
    EventLoop loop;
    Timer timer(loop, 5ms, {}, false);

    timer.start();
    REQUIRE(timer.is_active());
    REQUIRE(wait_until([&] { return !timer.is_active(); }, 500ms));
}

TEST_CASE("Timer restart invalidates stale queued dispatches",
          "[events][timer][coverage][issue-687]") {
    EventLoop loop;
    std::atomic<int> calls{0};
    Timer timer(loop, 75ms, [&] { calls.fetch_add(1); }, false);

    REQUIRE_FALSE(timer.is_active());
    timer.start();
    REQUIRE(timer.is_active());

    timer.stop();
    REQUIRE_FALSE(timer.is_active());
    timer.set_interval(5ms);
    REQUIRE(timer.interval() == 5ms);

    timer.start();
    REQUIRE(timer.is_active());
    REQUIRE(wait_until([&] { return calls.load() == 1; }, 500ms));
    REQUIRE_FALSE(timer.is_active());

    std::this_thread::sleep_for(100ms);
    REQUIRE(calls.load() == 1);
}

TEST_CASE("Repeating timer can stop itself from its callback",
          "[events][timer][coverage]") {
    EventLoop loop;
    std::atomic<int> calls{0};
    Timer* self = nullptr;
    Timer timer(loop, 5ms, [&] {
        calls.fetch_add(1);
        self->stop();
    }, true);
    self = &timer;

    REQUIRE_FALSE(timer.is_active());
    timer.start();
    REQUIRE(timer.is_active());

    REQUIRE(wait_until([&] { return calls.load() == 1; }, 500ms));
    REQUIRE_FALSE(timer.is_active());

    std::this_thread::sleep_for(30ms);
    REQUIRE(calls.load() == 1);
}
