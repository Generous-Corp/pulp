// MonotonicMidiClock — seconds-since-open MIDI timestamp source
// (#3327). The Linux ALSA raw-midi path used to stamp
// every event `timestamp = 0.0`; this helper restores real monotonic
// timing. The `now`-taking overloads are pure, so this suite verifies the
// conversion deterministically without touching the wall clock. Runs on all
// platforms because the helper is platform-neutral.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/midi/monotonic_timestamp.hpp>

#include <chrono>

using pulp::midi::MonotonicMidiClock;
using Clock = MonotonicMidiClock::clock;

TEST_CASE("MonotonicMidiClock reports 0 before the base is captured",
          "[midi][timestamp][issue-3327]") {
    MonotonicMidiClock c;
    REQUIRE_FALSE(c.has_base());
    // Even with a real time point, no base means no bogus huge value.
    REQUIRE(c.seconds_since_open(Clock::now()) == 0.0);
}

TEST_CASE("MonotonicMidiClock measures seconds elapsed since reset",
          "[midi][timestamp][issue-3327]") {
    MonotonicMidiClock c;
    const Clock::time_point t0 = Clock::now();
    c.reset(t0);
    REQUIRE(c.has_base());

    // At the base instant, zero elapsed.
    REQUIRE(c.seconds_since_open(t0) == 0.0);

    // 1.5s later → ~1.5s elapsed (exact: duration is integral ns in,
    // double seconds out).
    const auto t1 = t0 + std::chrono::milliseconds(1500);
    REQUIRE(c.seconds_since_open(t1) == Catch::Approx(1.5).epsilon(1e-9));

    const auto t2 = t0 + std::chrono::microseconds(250);
    REQUIRE(c.seconds_since_open(t2) == Catch::Approx(0.00025).epsilon(1e-9));
}

TEST_CASE("MonotonicMidiClock is monotonic non-decreasing for advancing time",
          "[midi][timestamp][issue-3327]") {
    MonotonicMidiClock c;
    const Clock::time_point t0 = Clock::now();
    c.reset(t0);

    double prev = c.seconds_since_open(t0);
    for (int i = 1; i <= 10; ++i) {
        const auto t = t0 + std::chrono::milliseconds(i * 7);
        const double s = c.seconds_since_open(t);
        REQUIRE(s >= prev);
        REQUIRE(s > 0.0);
        prev = s;
    }
}

TEST_CASE("MonotonicMidiClock re-reset rebases to a new origin",
          "[midi][timestamp][issue-3327]") {
    MonotonicMidiClock c;
    const Clock::time_point t0 = Clock::now();
    c.reset(t0);
    REQUIRE(c.seconds_since_open(t0 + std::chrono::seconds(5)) ==
            Catch::Approx(5.0).epsilon(1e-9));

    // Reopening the port rebases — the same wall instant now reads as a
    // smaller elapsed value relative to the new base.
    const auto t_new = t0 + std::chrono::seconds(3);
    c.reset(t_new);
    REQUIRE(c.seconds_since_open(t0 + std::chrono::seconds(5)) ==
            Catch::Approx(2.0).epsilon(1e-9));
}
