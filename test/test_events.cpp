#include <catch2/catch_test_macros.hpp>
#include <pulp/events/events.hpp>
#include <atomic>
#include <thread>
#include <chrono>

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
        std::this_thread::sleep_for(50ms);
        REQUIRE(value.load() == 42);
    }

    SECTION("Multiple dispatches execute in order") {
        std::vector<int> results;
        std::mutex mtx;

        for (int i = 0; i < 10; ++i) {
            loop.dispatch([&, i] {
                std::lock_guard lock(mtx);
                results.push_back(i);
            });
        }

        std::this_thread::sleep_for(100ms);
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
        std::this_thread::sleep_for(200ms);
        REQUIRE(count.load() == 400);
    }
}

TEST_CASE("EventLoop dispatch_after", "[events][event_loop]") {
    EventLoop loop;

    SECTION("Delayed task executes after delay") {
        std::atomic<bool> executed{false};
        auto start = std::chrono::steady_clock::now();

        loop.dispatch_after(50ms, [&] { executed.store(true); });

        REQUIRE(wait_until([&] { return executed.load(); }, 500ms));
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

    std::this_thread::sleep_for(50ms);
    REQUIRE(v1.load() == 1);
    REQUIRE(v2.load() == 2);
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
}
