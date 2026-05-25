#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/high_resolution_timer.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace pulp::runtime;

TEST_CASE("HighResolutionTimer DedicatedThread mode fires callbacks", "[runtime][hrt]") {
    HighResolutionTimer timer;
    std::atomic<int> ticks{0};
    timer.start(std::chrono::milliseconds(2),
                [&] { ticks.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(timer.is_running());
    REQUIRE(timer.active_mode() == TimerMode::DedicatedThread);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    timer.stop();
    REQUIRE(timer.is_running() == false);

    // Conservative bound: at 2 ms cadence over 100 ms we expect roughly 50 ticks.
    // Allow a wide window for CI variance.
    REQUIRE(ticks.load() >= 5);
}

#ifdef __APPLE__
TEST_CASE("HighResolutionTimer OsTimerQueue mode fires callbacks on Apple",
          "[runtime][hrt][apple]") {
    HighResolutionTimer timer;
    std::atomic<int> ticks{0};
    timer.start(std::chrono::milliseconds(2),
                [&] { ticks.fetch_add(1, std::memory_order_relaxed); },
                TimerMode::OsTimerQueue);
    REQUIRE(timer.is_running());
    REQUIRE(timer.active_mode() == TimerMode::OsTimerQueue);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    timer.stop();
    REQUIRE(timer.is_running() == false);

    REQUIRE(ticks.load() >= 5);
}

TEST_CASE("HighResolutionTimer OS queue cleans up on destruction",
          "[runtime][hrt][apple]") {
    std::atomic<int> ticks{0};
    {
        HighResolutionTimer timer;
        timer.start(std::chrono::milliseconds(5),
                    [&] { ticks.fetch_add(1, std::memory_order_relaxed); },
                    TimerMode::OsTimerQueue);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    } // destructor runs stop()
    const int final_ticks = ticks.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // After destruction, ticks must not advance.
    REQUIRE(ticks.load() == final_ticks);
}
#endif

TEST_CASE("HighResolutionTimer OS queue falls back to dedicated thread off-Apple",
          "[runtime][hrt]") {
    HighResolutionTimer timer;
    std::atomic<int> ticks{0};
    timer.start(std::chrono::milliseconds(2),
                [&] { ticks.fetch_add(1, std::memory_order_relaxed); },
                TimerMode::OsTimerQueue);
    REQUIRE(timer.is_running());

#ifndef __APPLE__
    REQUIRE(timer.active_mode() == TimerMode::DedicatedThread);
#endif

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    timer.stop();
}

TEST_CASE("HighResolutionTimer stop is idempotent", "[runtime][hrt]") {
    HighResolutionTimer timer;
    timer.stop();
    timer.stop();
    REQUIRE(timer.is_running() == false);

    std::atomic<int> ticks{0};
    timer.start(std::chrono::milliseconds(1),
                [&] { ticks.fetch_add(1, std::memory_order_relaxed); });
    timer.stop();
    timer.stop();
    REQUIRE(timer.is_running() == false);
}
