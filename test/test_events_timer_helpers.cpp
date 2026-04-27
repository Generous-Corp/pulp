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
