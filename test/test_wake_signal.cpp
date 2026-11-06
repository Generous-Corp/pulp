#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/wake_signal.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using pulp::runtime::WakeSignal;

namespace {

// Spin (with yields) until `done()` or a generous wall-clock limit.
template <typename Done> bool spin_until(Done&& done, std::chrono::milliseconds limit = 5000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::yield();
    }
    return true;
}

} // namespace

TEST_CASE("WakeSignal: a busy consumer costs the producer no post",
          "[runtime][wake-signal][rt-safety]") {
    WakeSignal wake;
    std::atomic<int> published{0};
    for (int i = 0; i < 10000; ++i) {
        published.store(i + 1, std::memory_order_release);
        wake.notify();
    }
    REQUIRE(wake.posts() == 0);
    // Work already published: the consumer returns without parking.
    REQUIRE(wake.wait_for([&] { return published.load(std::memory_order_acquire) == 10000; }, 1s));
    REQUIRE(wake.wakes() == 0);
}

TEST_CASE("WakeSignal: a parked consumer is woken by exactly one post", "[runtime][wake-signal]") {
    WakeSignal wake;
    std::atomic<bool> ready{false};
    std::atomic<bool> woke{false};
    std::thread consumer([&] {
        woke.store(wake.wait_for([&] { return ready.load(std::memory_order_acquire); }, 10s));
    });
    REQUIRE(spin_until([&] { return wake.consumer_parked(); }));
    const auto start = std::chrono::steady_clock::now();
    ready.store(true, std::memory_order_release);
    wake.notify();
    consumer.join();
    REQUIRE(woke.load());
    REQUIRE(wake.posts() == 1);
    REQUIRE(wake.wakes() >= 1);
    // Woken by the post, not by the 10 s deadline.
    REQUIRE(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("WakeSignal: the deadline ends a wait with nothing published", "[runtime][wake-signal]") {
    WakeSignal wake;
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_FALSE(wake.wait_for([] { return false; }, 20ms));
    REQUIRE(std::chrono::steady_clock::now() - start >= 20ms);
    REQUIRE(wake.posts() == 0);
    REQUIRE_FALSE(wake.consumer_parked());
}

// Ping-pong: every handoff makes the consumer park before the producer
// publishes, which is the window a lost wakeup needs. A lost wakeup shows up
// as a wait that only ends at its deadline.
TEST_CASE("WakeSignal: no lost wakeup across many handoffs",
          "[runtime][wake-signal][concurrent][race]") {
    constexpr std::uint64_t kHandoffs = 100000;
    WakeSignal wake;
    std::atomic<std::uint64_t> published{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<std::uint64_t> missed{0};
    std::thread consumer([&] {
        for (std::uint64_t expected = 1; expected <= kHandoffs; ++expected) {
            if (!wake.wait_for(
                    [&] { return published.load(std::memory_order_acquire) >= expected; }, 1s)) {
                missed.fetch_add(1);
                return;
            }
            consumed.store(expected, std::memory_order_release);
        }
    });
    for (std::uint64_t i = 1; i <= kHandoffs; ++i) {
        while (consumed.load(std::memory_order_acquire) + 1 < i) {
            if (missed.load() != 0)
                break;
            std::this_thread::yield();
        }
        if (missed.load() != 0)
            break;
        published.store(i, std::memory_order_release);
        wake.notify();
    }
    consumer.join();
    REQUIRE(missed.load() == 0);
    REQUIRE(consumed.load() == kHandoffs);
    // The consumer parked for at least some handoffs, so the post path ran.
    REQUIRE(wake.posts() > 0);
}

TEST_CASE("WakeSignal: several producers, one consumer",
          "[runtime][wake-signal][concurrent][race]") {
    constexpr int kProducers = 4;
    constexpr std::uint64_t kEach = 20000;
    WakeSignal wake;
    std::atomic<std::uint64_t> published{0};
    std::atomic<bool> lost{false};
    std::thread consumer([&] {
        std::uint64_t seen = 0;
        while (seen < kProducers * kEach) {
            if (!wake.wait_for([&] { return published.load(std::memory_order_acquire) > seen; },
                               1s)) {
                lost.store(true);
                return;
            }
            seen = published.load(std::memory_order_acquire);
        }
    });
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p)
        producers.emplace_back([&] {
            for (std::uint64_t i = 0; i < kEach; ++i) {
                published.fetch_add(1, std::memory_order_release);
                wake.notify();
            }
        });
    for (auto& t : producers)
        t.join();
    consumer.join();
    REQUIRE_FALSE(lost.load());
}
