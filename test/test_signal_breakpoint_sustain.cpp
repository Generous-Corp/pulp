// Sustain point, release and legato retrigger of the breakpoint envelope,
// including how they interact with loops. A sample rate of 1000 Hz makes one
// millisecond of program time exactly one sample.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/breakpoint_envelope.hpp>

#include <array>
#include <cstddef>
#include <type_traits>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Env = BreakpointEnvelope64;
using Point = BreakpointEnvelopePoint64;

static_assert(std::is_trivially_copyable_v<Env>);

namespace {

constexpr double kSr = 1000.0;

// attack 0 -> 1 over 10, decay to the 0.6 sustain level at 30, release to 0
// over the final 20 samples.
Env adsr(std::size_t sustain = 2) {
    Env env;
    REQUIRE(env.prepare(kSr) == BreakpointEnvelopeStatus::ok);
    const std::array<Point, 4> points{
        {{0.0, 0.0, {}}, {10.0, 1.0, {}}, {30.0, 0.6, {}}, {50.0, 0.0, {}}}};
    REQUIRE(env.configure(points) == BreakpointEnvelopeStatus::ok);
    if (sustain != Env::kNoSustain)
        REQUIRE(env.set_sustain_point(sustain) == BreakpointEnvelopeStatus::ok);
    return env;
}

std::vector<double> run(Env& env, int samples) {
    std::vector<double> out;
    for (int i = 0; i < samples; ++i)
        out.push_back(env.next());
    return out;
}

// Loop program: P0(0,0) P1(10,1) P2(20,0.2) P3(30,0.8) P4(50,0).
Env loop_program() {
    Env env;
    REQUIRE(env.prepare(kSr) == BreakpointEnvelopeStatus::ok);
    const std::array<Point, 5> points{
        {{0.0, 0.0, {}}, {10.0, 1.0, {}}, {20.0, 0.2, {}}, {30.0, 0.8, {}}, {50.0, 0.0, {}}}};
    REQUIRE(env.configure(points) == BreakpointEnvelopeStatus::ok);
    return env;
}

int samples_until_inactive(Env& env, int limit) {
    int n = 0;
    while (env.active() && n < limit) {
        env.next();
        ++n;
    }
    return n;
}

} // namespace

TEST_CASE("Sustain holds at the sustain level while the gate is on",
          "[signal][breakpoint-envelope][sustain]") {
    Env env = adsr();
    env.trigger();
    const auto out = run(env, 400);
    // Attack and decay follow the authored segments.
    CHECK_THAT(out[4], WithinAbs(0.5, 1e-12));  // sample 5 of the attack
    CHECK_THAT(out[9], WithinAbs(1.0, 1e-12));  // reaches the peak at sample 10
    CHECK_THAT(out[19], WithinAbs(0.8, 1e-12)); // halfway down the decay
    CHECK_THAT(out[29], WithinAbs(0.6, 1e-12)); // arrives at the sustain level
    for (std::size_t i = 29; i < out.size(); ++i)
        CHECK(out[i] == 0.6); // exactly, for as long as the gate is on
    CHECK(env.holding());
    CHECK(env.active());
    CHECK(env.gate());
}

TEST_CASE("Release continues from the held level through the authored release segment",
          "[signal][breakpoint-envelope][sustain]") {
    Env env = adsr();
    env.trigger();
    run(env, 200);
    REQUIRE(env.holding());
    env.release();
    CHECK_FALSE(env.holding());
    CHECK_FALSE(env.gate());
    const auto tail = run(env, 25);
    // No jump: the first release sample is one step below the held 0.6.
    CHECK_THAT(tail[0], WithinAbs(0.6 * (1.0 - 1.0 / 20.0), 1e-12));
    CHECK_THAT(tail[9], WithinAbs(0.6 * 0.5, 1e-12));
    CHECK_THAT(tail[19], WithinAbs(0.0, 1e-12));
    CHECK_FALSE(env.active());
    CHECK(tail[24] == 0.0);
}

TEST_CASE("Releasing before the sustain point is reached cancels the hold without a jump",
          "[signal][breakpoint-envelope][sustain]") {
    for (const int release_at : {3, 10, 20, 29}) {
        Env sustained = adsr();
        Env plain = adsr(Env::kNoSustain);
        sustained.trigger();
        plain.trigger();
        std::vector<double> a, b;
        for (int i = 0; i < 80; ++i) {
            if (i == release_at)
                sustained.release();
            a.push_back(sustained.next());
            b.push_back(plain.next());
        }
        INFO("release at " << release_at);
        CHECK(a == b);
        CHECK_FALSE(sustained.holding());
    }
}

TEST_CASE("Legato retrigger keeps a gated envelope running and resumes a release without a jump",
          "[signal][breakpoint-envelope][sustain][legato]") {
    {
        // While holding: untouched.
        Env env = adsr();
        env.trigger();
        run(env, 100);
        env.trigger_legato();
        CHECK(env.holding());
        CHECK(env.next() == 0.6);
    }
    {
        // While rising: untouched, same as not calling it.
        Env a = adsr(), b = adsr();
        a.trigger();
        b.trigger();
        run(a, 4);
        run(b, 4);
        a.trigger_legato();
        CHECK(run(a, 60) == run(b, 60));
    }
    {
        // Mid-release: the attack restarts from the current level.
        Env env = adsr();
        env.trigger();
        run(env, 100);
        env.release();
        run(env, 10);
        const double level = env.current();
        REQUIRE_THAT(level, WithinAbs(0.3, 1e-12));
        env.trigger_legato();
        CHECK(env.gate());
        const double first = env.next();
        CHECK_THAT(first, WithinAbs(level + (1.0 - level) / 10.0, 1e-12));
        const auto rest = run(env, 9);
        CHECK_THAT(rest.back(), WithinAbs(1.0, 1e-12)); // peak after the attack length
        const auto onward = run(env, 100);
        CHECK(env.holding());
        CHECK(onward.back() == 0.6);
    }
    {
        // A plain trigger() from the same point jumps back to the start instead.
        Env env = adsr();
        env.trigger();
        run(env, 100);
        env.release();
        run(env, 10);
        env.trigger();
        CHECK_THAT(env.current(), WithinAbs(0.0, 1e-12));
    }
}

TEST_CASE("Sustain on the last point holds until release then finishes",
          "[signal][breakpoint-envelope][sustain]") {
    Env env;
    REQUIRE(env.prepare(kSr) == BreakpointEnvelopeStatus::ok);
    const std::array<Point, 2> points{{{0.0, 0.0, {}}, {10.0, 1.0, {}}}};
    REQUIRE(env.configure(points) == BreakpointEnvelopeStatus::ok);
    REQUIRE(env.set_sustain_point(1) == BreakpointEnvelopeStatus::ok);
    env.trigger();
    const auto out = run(env, 100);
    CHECK(out.back() == 1.0);
    CHECK(env.holding());
    env.release();
    CHECK_FALSE(env.active());
    CHECK(env.next() == 1.0);
}

TEST_CASE("Sustain on point zero holds the first level at once",
          "[signal][breakpoint-envelope][sustain]") {
    Env env = adsr(0);
    env.trigger();
    CHECK(env.holding());
    CHECK(run(env, 50).back() == 0.0);
    env.release();
    CHECK_THAT(run(env, 10).back(), WithinAbs(1.0, 1e-12)); // attack segment, after release
}

TEST_CASE("A loop after the sustain point's loop end runs its repeats, then holds",
          "[signal][breakpoint-envelope][sustain][loop]") {
    Env env = loop_program();
    REQUIRE(env.set_loop(1, 2, 2) == BreakpointEnvelopeStatus::ok);
    REQUIRE(env.set_sustain_point(3) == BreakpointEnvelopeStatus::ok);
    env.trigger();
    run(env, 300);
    CHECK(env.holding());
    CHECK(env.current() == 0.8);
    CHECK(env.loops_completed() == 2);
    env.release();
    CHECK(samples_until_inactive(env, 100) == 20);
}

TEST_CASE("An endless loop repeats until release, then plays out past the sustain point",
          "[signal][breakpoint-envelope][sustain][loop]") {
    Env env = loop_program();
    REQUIRE(env.set_loop(1, 2, Env::kLoopForever) == BreakpointEnvelopeStatus::ok);
    REQUIRE(env.set_sustain_point(3) == BreakpointEnvelopeStatus::ok);
    env.trigger();
    run(env, 1000);
    CHECK_FALSE(env.holding()); // the loop never lets it reach the sustain point
    CHECK(env.loops_completed() > 50);
    CHECK(env.active());

    env.release();
    const int remaining = samples_until_inactive(env, 200);
    // At most the rest of the current pass, the segment to point 3, and the
    // authored 20-sample release; it never parks at the sustain point.
    CHECK(remaining <= 10 + 10 + 20);
    CHECK(remaining >= 20);
    CHECK_FALSE(env.holding());
    CHECK(env.current() == 0.0);
}

TEST_CASE("A sustain point at the loop end wins over the loop",
          "[signal][breakpoint-envelope][sustain][loop]") {
    Env env = loop_program();
    REQUIRE(env.set_loop(1, 2, Env::kLoopForever) == BreakpointEnvelopeStatus::ok);
    REQUIRE(env.set_sustain_point(2) == BreakpointEnvelopeStatus::ok);
    env.trigger();
    run(env, 500);
    CHECK(env.holding());
    CHECK(env.current() == 0.2);
    CHECK(env.loops_completed() == 0);
    env.release();
    // Continues from the held point into 2 -> 3 -> 4 with no further repeats.
    CHECK(samples_until_inactive(env, 200) == 10 + 20);
    CHECK(env.loops_completed() == 0);
}

TEST_CASE("A sustain point before the loop holds first and the loop never repeats after release",
          "[signal][breakpoint-envelope][sustain][loop]") {
    Env env = loop_program();
    REQUIRE(env.set_loop(2, 3, Env::kLoopForever) == BreakpointEnvelopeStatus::ok);
    REQUIRE(env.set_sustain_point(1) == BreakpointEnvelopeStatus::ok);
    env.trigger();
    run(env, 100);
    CHECK(env.holding());
    CHECK(env.current() == 1.0);
    env.release();
    CHECK(samples_until_inactive(env, 500) == 10 + 10 + 20);
    CHECK(env.loops_completed() == 0);
}

TEST_CASE("Release on a loop without a sustain point ends the loop after the current pass",
          "[signal][breakpoint-envelope][sustain][loop]") {
    Env env = loop_program();
    REQUIRE(env.set_loop(1, 2, Env::kLoopForever) == BreakpointEnvelopeStatus::ok);
    env.trigger();
    run(env, 305);
    env.release();
    const int remaining = samples_until_inactive(env, 500);
    CHECK(remaining <= 10 + 10 + 20);
    CHECK(env.current() == 0.0);
}

TEST_CASE("Release at the instant a loop is about to wrap does not wrap",
          "[signal][breakpoint-envelope][sustain][loop]") {
    Env env = loop_program();
    REQUIRE(env.set_loop(1, 2, Env::kLoopForever) == BreakpointEnvelopeStatus::ok);
    env.trigger();
    // Run to the sample that emits the loop end point; the wrap is pending.
    run(env, 20);
    REQUIRE(env.current() == 0.2);
    env.release();
    // The next sample belongs to segment 2 -> 3, not a restart at point 1.
    CHECK_THAT(env.next(), WithinAbs(0.2 + (0.8 - 0.2) / 10.0, 1e-6));
    CHECK(samples_until_inactive(env, 100) == 9 + 20);
}

TEST_CASE("Sustain configuration is validated and reset with the program",
          "[signal][breakpoint-envelope][sustain]") {
    Env env = adsr();
    CHECK(env.set_sustain_point(4) == BreakpointEnvelopeStatus::invalid_sustain);
    CHECK(env.sustain_point() == 2); // a failed set leaves it alone
    const std::array<Point, 2> replacement{{{0.0, 0.0, {}}, {5.0, 1.0, {}}}};
    REQUIRE(env.configure(replacement) == BreakpointEnvelopeStatus::ok);
    CHECK(env.sustain_point() == Env::kNoSustain);
    env.trigger();
    run(env, 20);
    CHECK_FALSE(env.holding());
    CHECK_FALSE(env.active());
}

TEST_CASE("Moving or clearing the sustain point while holding resumes playback",
          "[signal][breakpoint-envelope][sustain]") {
    Env env = adsr();
    env.trigger();
    run(env, 100);
    REQUIRE(env.holding());
    env.clear_sustain_point();
    CHECK_FALSE(env.holding());
    CHECK(samples_until_inactive(env, 100) == 20);

    Env other = adsr();
    other.trigger();
    run(other, 100);
    REQUIRE(other.holding());
    REQUIRE(other.set_sustain_point(3) == BreakpointEnvelopeStatus::ok);
    CHECK_FALSE(other.holding()); // moved ahead of the playhead: plays on and holds at 3
    run(other, 50);
    CHECK(other.holding());
    CHECK(other.current() == 0.0);
}

TEST_CASE("Sustain, release and legato allocate nothing",
          "[signal][breakpoint-envelope][sustain][rt-safety]") {
    Env env = loop_program();
    REQUIRE(env.set_loop(1, 2, Env::kLoopForever) == BreakpointEnvelopeStatus::ok);
    REQUIRE(env.set_sustain_point(3) == BreakpointEnvelopeStatus::ok);
    double sink = 0.0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int note = 0; note < 50; ++note) {
            env.trigger_legato();
            for (int i = 0; i < 37; ++i)
                sink += env.next();
            env.release();
            for (int i = 0; i < 61; ++i)
                sink += env.next();
        }
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(sink > 0.0);
}
