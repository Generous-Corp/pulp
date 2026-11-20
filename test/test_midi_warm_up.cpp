// MIDI backend warm-up: the one-time slow first call moves to a worker, runs
// once, never blocks the caller, and leaves enumeration results unchanged.
#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/device.hpp>
#include "../core/midi/src/warm_up_test_access.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

double elapsed_ms(Clock::time_point since) {
    return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

// Stands in for a backend whose first call connects to a server: the first
// call pays `cold_cost`, later calls are instant.
struct ColdBackend {
    explicit ColdBackend(std::chrono::milliseconds cold_cost) : cold_cost_(cold_cost) {}
    std::vector<int> enumerate() {
        std::call_once(connected_, [&] { std::this_thread::sleep_for(cold_cost_); });
        return {1, 2, 3};
    }
    std::chrono::milliseconds cold_cost_;
    std::once_flag connected_;
};

struct ResetGuard {
    ResetGuard() { pulp::midi::detail::reset_warm_up_for_testing(); }
    ~ResetGuard() { pulp::midi::detail::reset_warm_up_for_testing(); }
};

} // namespace

TEST_CASE("MIDI warm-up returns at once and finishes in the background", "[midi][warm-up]") {
    ResetGuard guard;
    CHECK_FALSE(pulp::midi::warm_up_done());
    pulp::midi::wait_for_warm_up(); // nothing started: must not block

    std::atomic<int> runs{0};
    const auto start = Clock::now();
    REQUIRE(pulp::midi::detail::start_warm_up([&] {
        std::this_thread::sleep_for(300ms);
        runs.fetch_add(1);
    }));
    CHECK(elapsed_ms(start) < 150.0);
    CHECK_FALSE(pulp::midi::warm_up_done());

    pulp::midi::wait_for_warm_up();
    CHECK(elapsed_ms(start) >= 250.0);
    CHECK(pulp::midi::warm_up_done());
    CHECK(runs.load() == 1);
}

TEST_CASE("MIDI warm-up runs once however many threads ask", "[midi][warm-up]") {
    ResetGuard guard;
    std::atomic<int> runs{0};
    std::atomic<int> starters{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&] {
            if (pulp::midi::detail::start_warm_up([&] { runs.fetch_add(1); }))
                starters.fetch_add(1);
        });
    for (auto& t : threads)
        t.join();
    pulp::midi::wait_for_warm_up();

    CHECK(starters.load() == 1);
    CHECK(runs.load() == 1);
    // A later ask is a no-op.
    CHECK_FALSE(pulp::midi::detail::start_warm_up([&] { runs.fetch_add(1); }));
    CHECK(runs.load() == 1);
}

TEST_CASE("MIDI warm-up survives a failing probe", "[midi][warm-up]") {
    ResetGuard guard;
    REQUIRE(pulp::midi::detail::start_warm_up([] { throw std::runtime_error("no server"); }));
    pulp::midi::wait_for_warm_up();
    CHECK(pulp::midi::warm_up_done());
}

TEST_CASE("a warmed first call is a small fraction of a cold first call", "[midi][warm-up]") {
    ResetGuard guard;
    constexpr auto kColdCost = 300ms;

    // Control: no warm-up, the first call pays the whole connection cost.
    ColdBackend cold(kColdCost);
    auto t0 = Clock::now();
    const auto cold_result = cold.enumerate();
    const double cold_ms = elapsed_ms(t0);
    REQUIRE(cold_ms >= 250.0);

    // Warmed: the worker pays it, then the caller's first call is cheap and
    // returns the same list.
    ColdBackend warmed(kColdCost);
    REQUIRE(pulp::midi::detail::start_warm_up([&] { (void)warmed.enumerate(); }));
    pulp::midi::wait_for_warm_up();
    t0 = Clock::now();
    const auto warm_result = warmed.enumerate();
    const double warm_ms = elapsed_ms(t0);
    CHECK(warm_ms < cold_ms / 5.0);
    CHECK(warm_result == cold_result);
}

#if defined(__APPLE__)
TEST_CASE("platform warm-up leaves port enumeration unchanged", "[midi][warm-up]") {
    ResetGuard guard;
    pulp::midi::warm_up();
    pulp::midi::wait_for_warm_up();
    CHECK(pulp::midi::warm_up_done());

    auto system = pulp::midi::create_midi_system();
    REQUIRE(system);
    const auto first = system->enumerate_inputs();
    const auto second = system->enumerate_inputs();
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i)
        CHECK(first[i].id == second[i].id);
}
#endif
