#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/osc/phase.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

using namespace pulp::signal::osc;
using Catch::Matchers::WithinAbs;

namespace {

/// Distance between two phases on the unit circle. A linear difference reports
/// ~1.0 for the pair (1e-12, 1 - 1e-12), which are neighbours, not opposites.
double circular_distance(double a, double b) {
    const double d = std::fabs(a - b);
    return d > 0.5 ? 1.0 - d : d;
}

/// Exact wrapped phase after `n` accumulations of `increment`, computed in
/// integer arithmetic.
///
/// Every double is exactly k / 2^p for integers k, p. `frexp` recovers them, so
/// the true phase after n steps is (n*k mod 2^p) / 2^p. Accumulating k modulo
/// 2^p in uint64 is exact — this is ground truth derived independently of the
/// accumulator, not a second run of it.
///
/// Requires 0 < increment < 1 and a starting phase of 0.
double exact_phase_after(double increment, int n) {
    int exponent = 0;
    const double mantissa = std::frexp(increment, &exponent); // in [0.5, 1)
    const std::uint64_t k = static_cast<std::uint64_t>(std::ldexp(mantissa, 53));
    const int p = 53 - exponent;
    REQUIRE(p > 0);
    REQUIRE(p < 63); // 2^p, and acc + k < 2^(p+1), must fit in uint64

    const std::uint64_t modulus = std::uint64_t{1} << p;
    REQUIRE(k < modulus); // increment < 1

    std::uint64_t acc = 0;
    for (int i = 0; i < n; ++i) {
        acc += k;
        if (acc >= modulus) acc -= modulus;
    }
    return static_cast<double>(acc) / static_cast<double>(modulus);
}

} // namespace

TEST_CASE("PhaseAccumulator reports the fractional position of a forward wrap", "[signal][osc]") {
    PhaseAccumulator phase;
    phase.reset(0.75);

    // Starting at 0.75 and moving 0.5 per sample, phase reaches 1.0 exactly
    // halfway through the sample. Every value here is dyadic, so the expected
    // crossing is representable and the assertion can be exact.
    REQUIRE(phase.advance(0.5) == 1);
    REQUIRE(phase.events().size() == 1);

    const PhaseEvent& e = phase.events()[0];
    CHECK(e.kind == PhaseEventKind::wrap_forward);
    CHECK(e.frac == 0.5);
    CHECK(e.phase_before == 1.0);
    CHECK(e.phase_after == 0.0);
    CHECK(phase.phase() == 0.25);
    CHECK_FALSE(phase.truncated());
}

TEST_CASE("PhaseAccumulator advances without an event when no boundary is crossed", "[signal][osc]") {
    PhaseAccumulator phase;
    phase.reset(0.25);

    CHECK(phase.advance(0.5) == 0);
    CHECK(phase.events().empty());
    CHECK(phase.phase() == 0.75);
}

TEST_CASE("PhaseAccumulator wraps backward on a negative increment", "[signal][osc]") {
    // The through-zero FM case: instantaneous frequency goes negative, so the
    // phase must run backward and wrap through 0 up to 1.
    PhaseAccumulator phase;
    phase.reset(0.25);

    // From 0.25 at -0.5 per sample, phase hits 0 halfway through the sample.
    REQUIRE(phase.advance(-0.5) == 1);
    REQUIRE(phase.events().size() == 1);

    const PhaseEvent& e = phase.events()[0];
    CHECK(e.kind == PhaseEventKind::wrap_backward);
    CHECK(e.frac == 0.5);
    CHECK(e.phase_before == 0.0);
    CHECK(e.phase_after == 1.0);
    CHECK(phase.phase() == 0.75);
}

TEST_CASE("PhaseAccumulator holds phase in range across a sign reversal", "[signal][osc]") {
    // A modulator sweeping the increment through zero and back — the shape of a
    // through-zero FM excursion. Phase must stay in [0, 1) throughout.
    PhaseAccumulator phase;
    phase.reset(0.1);

    for (int i = -200; i <= 200; ++i) {
        const double increment = static_cast<double>(i) * 0.011;
        phase.advance(increment);
        REQUIRE(phase.phase() >= 0.0);
        REQUIRE(phase.phase() < 1.0);
        for (const PhaseEvent& e : phase.events()) {
            REQUIRE(e.frac >= 0.0);
            REQUIRE(e.frac <= 1.0);
        }
    }
}

TEST_CASE("PhaseAccumulator reports every wrap when the increment exceeds one", "[signal][osc]") {
    SECTION("forward") {
        PhaseAccumulator phase;
        phase.reset(0.0);

        // 2.5 cycles in one sample: boundaries at 1.0 and 2.0, reached 40% and
        // 80% of the way through.
        REQUIRE(phase.advance(2.5) == 2);
        CHECK(phase.events()[0].frac == 0.4);
        CHECK(phase.events()[1].frac == 0.8);
        CHECK(phase.events()[0].kind == PhaseEventKind::wrap_forward);
        CHECK(phase.events()[1].kind == PhaseEventKind::wrap_forward);
        CHECK(phase.phase() == 0.5);
        CHECK_FALSE(phase.truncated());
    }

    SECTION("backward") {
        PhaseAccumulator phase;
        phase.reset(0.5);

        // -2.5 cycles: crosses 0 at 20% and -1 at 60%.
        REQUIRE(phase.advance(-2.5) == 2);
        CHECK(phase.events()[0].frac == 0.2);
        CHECK(phase.events()[1].frac == 0.6);
        CHECK(phase.events()[0].kind == PhaseEventKind::wrap_backward);
        CHECK(phase.events()[1].kind == PhaseEventKind::wrap_backward);
        CHECK(phase.phase() == 0.0);
    }
}

TEST_CASE("PhaseAccumulator bounds its event list and keeps the phase exact", "[signal][osc][rt-safety]") {
    PhaseAccumulator phase;
    phase.reset(0.0);

    // 20.5 cycles in one sample would be 20 events. The list caps; the phase
    // does not — a bounded event list must not cost accumulator correctness.
    const int n = phase.advance(20.5);
    CHECK(n == PhaseAccumulator::max_events_per_sample);
    CHECK(phase.truncated());
    CHECK(phase.phase() == 0.5);

    // The retained events are the chronologically first ones.
    CHECK_THAT(phase.events().front().frac, WithinAbs(1.0 / 20.5, 1e-15));

    // A subsequent normal advance clears the flag rather than latching it.
    phase.advance(0.25);
    CHECK_FALSE(phase.truncated());
    CHECK(phase.phase() == 0.75);
}

TEST_CASE("PhaseAccumulator handles a zero increment", "[signal][osc]") {
    PhaseAccumulator phase;
    phase.reset(0.3);

    CHECK(phase.advance(0.0) == 0);
    CHECK(phase.events().empty());
    CHECK(phase.phase() == 0.3);

    // Zero increment at phase 0 is the case where a wrap test written as
    // `phase <= 0` would fire spuriously and divide by the increment.
    phase.reset(0.0);
    CHECK(phase.advance(0.0) == 0);
    CHECK(phase.phase() == 0.0);
}

TEST_CASE("PhaseAccumulator resolves the wrap boundary exactly", "[signal][osc]") {
    SECTION("forward crossing landing on the sample boundary") {
        PhaseAccumulator phase;
        phase.reset(0.5);

        // Phase reaches exactly 1.0 at the end of the sample. The wrap belongs
        // to this sample: deferring it to the next one, which starts at 0.0,
        // would lose it entirely.
        REQUIRE(phase.advance(0.5) == 1);
        CHECK(phase.events()[0].kind == PhaseEventKind::wrap_forward);
        CHECK(phase.events()[0].frac == 1.0);
        CHECK(phase.phase() == 0.0);
    }

    SECTION("backward crossing landing on the sample start") {
        PhaseAccumulator phase;
        phase.reset(0.0);

        // Phase is at 0 and immediately goes negative, so the wrap is at frac 0.
        REQUIRE(phase.advance(-1.0) == 1);
        CHECK(phase.events()[0].kind == PhaseEventKind::wrap_backward);
        CHECK(phase.events()[0].frac == 0.0);
        CHECK(phase.phase() == 0.0);
    }

    SECTION("an increment of exactly one wraps once") {
        PhaseAccumulator phase;
        phase.reset(0.0);
        REQUIRE(phase.advance(1.0) == 1);
        CHECK(phase.events()[0].frac == 1.0);
        CHECK(phase.phase() == 0.0);
    }

    SECTION("a wrap rounding up to 1.0 snaps into the domain") {
        PhaseAccumulator phase;
        phase.reset(0.0);

        // raw = -1e-20, so floor is -1 and (raw - floor) rounds to exactly 1.0
        // in double — outside the half-open domain, and the value a naive wrap
        // would store. 0.0 is the same point on the circle.
        REQUIRE(phase.advance(-1e-20) == 1);
        CHECK(phase.phase() < 1.0);
        CHECK(phase.phase() == 0.0);
        CHECK(phase.events()[0].kind == PhaseEventKind::wrap_backward);
    }

    SECTION("reset wraps its argument into the domain") {
        PhaseAccumulator phase;
        phase.reset(1.0);
        CHECK(phase.phase() == 0.0);

        phase.reset(-0.25);
        CHECK(phase.phase() == 0.75);

        phase.reset(3.5);
        CHECK(phase.phase() == 0.5);
    }
}

TEST_CASE("PhaseAccumulator absorbs a non-finite increment", "[signal][osc][rt-safety]") {
    // A non-finite increment is a caller bug, but it must not persist in the
    // phase for the lifetime of the voice, must not reach a NaN-to-int
    // conversion, and must not report events.
    //
    // The event list is the load-bearing assertion, not the phase. A surviving
    // phase says nothing: the phase snaps to 0 on its own here, so checking
    // only `isfinite(phase())` passes while the accumulator hands a corrector a
    // full budget of wraps at positions nothing crossed. Both infinities are
    // covered because they fail differently from NaN — NaN loses every internal
    // comparison and drops out unaided, while floor(inf) is inf, which reads as
    // a wrap count past the budget.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    for (double bad : {nan, inf, -inf}) {
        PhaseAccumulator phase;
        phase.reset(0.4);

        CHECK(phase.advance(bad) == 0);
        CHECK(phase.events().empty());
        CHECK_FALSE(phase.truncated());
        CHECK(phase.phase() == 0.0);
    }

    // The accumulator recovers: the next real increment behaves normally.
    PhaseAccumulator phase;
    phase.reset(0.4);
    phase.advance(nan);
    phase.reset(0.25);
    CHECK(phase.advance(0.5) == 0);
    CHECK(phase.phase() == 0.75);
}

TEST_CASE("PhaseAccumulator stays within its rounding bound over a long run", "[signal][osc]") {
    // 1/pi cycles per sample: not dyadic, so every accumulation rounds and the
    // errors have somewhere to go.
    constexpr int kSamples = 1'000'000;
    const double increment = std::numbers::inv_pi;

    PhaseAccumulator phase;
    phase.reset(0.0);
    for (int i = 0; i < kSamples; ++i)
        phase.advance(increment);

    const double expected = exact_phase_after(increment, kSamples);
    const double error = circular_distance(phase.phase(), expected);

    // Bound derivation: phase in [0, 1) plus an increment in (0, 1) gives a raw
    // sum below 2, whose half-ulp is 2^-53 ~ 1.11e-16 — the entire error of one
    // step. The wrap subtraction itself is exact: raw is in [1, 2) whenever it
    // fires, so Sterbenz's lemma applies to `raw - 1`. Errors therefore only
    // accumulate, never amplify, and after n steps the bound is n * 2^-53.
    // For 1e6 samples that is 1.11e-10. The float accumulator this supersedes
    // has a per-step half-ulp of 2^-24, ~6e7 times worse, which is why the
    // equivalent bound there is a large fraction of a cycle.
    const double bound = static_cast<double>(kSamples) * 1.12e-16;
    INFO("error " << error << " against bound " << bound);
    CHECK(error <= bound);
}

TEST_CASE("PhaseAccumulator hard-syncs at a fractional position", "[signal][osc]") {
    SECTION("reset inside a sample that does not otherwise wrap") {
        PhaseAccumulator phase;
        phase.reset(0.0);

        // Half a sample at 0.5 cycles/sample reaches 0.25, then the reset puts
        // the phase back to 0 and the remaining half sample reaches 0.25 again.
        REQUIRE(phase.advance_synced(0.5, 0.5, 0.0) == 1);
        const PhaseEvent& e = phase.events()[0];
        CHECK(e.kind == PhaseEventKind::sync);
        CHECK(e.frac == 0.5);
        CHECK(e.phase_before == 0.25);
        CHECK(e.phase_after == 0.0);
        CHECK(phase.phase() == 0.25);
    }

    SECTION("a natural wrap and a reset in the same sample, in order") {
        PhaseAccumulator phase;
        phase.reset(0.8);

        // The first half sample advances 0.25 from 0.8, crossing 1.0 at 80% of
        // that half — frac 0.4. The reset then fires at 0.5.
        REQUIRE(phase.advance_synced(0.5, 0.5, 0.0) == 2);
        CHECK(phase.events()[0].kind == PhaseEventKind::wrap_forward);
        CHECK_THAT(phase.events()[0].frac, WithinAbs(0.4, 1e-15));
        CHECK(phase.events()[1].kind == PhaseEventKind::sync);
        CHECK(phase.events()[1].frac == 0.5);
        CHECK_THAT(phase.events()[1].phase_before, WithinAbs(0.05, 1e-15));
        CHECK(phase.events()[1].phase_after == 0.0);
        CHECK_THAT(phase.phase(), WithinAbs(0.25, 1e-15));
    }

    SECTION("reset to a non-zero phase") {
        PhaseAccumulator phase;
        phase.reset(0.5);

        REQUIRE(phase.advance_synced(0.0, 0.25, 0.9) == 1);
        CHECK(phase.events()[0].phase_before == 0.5);
        CHECK(phase.events()[0].phase_after == 0.9);
        CHECK(phase.phase() == 0.9);
    }

    SECTION("reset at the edges of the sample") {
        PhaseAccumulator phase;

        phase.reset(0.5);
        REQUIRE(phase.advance_synced(0.25, 0.0, 0.0) == 1);
        CHECK(phase.events()[0].frac == 0.0);
        CHECK(phase.phase() == 0.25); // whole sample advances after the reset

        phase.reset(0.5);
        REQUIRE(phase.advance_synced(0.25, 1.0, 0.0) == 1);
        CHECK(phase.events()[0].frac == 1.0);
        CHECK(phase.phase() == 0.0); // whole sample advances before the reset

        // Out-of-range sync positions clamp rather than escaping the sample.
        phase.reset(0.5);
        phase.advance_synced(0.25, 2.0, 0.0);
        CHECK(phase.events()[0].frac == 1.0);
    }

    SECTION("a backward wrap and a reset in the same sample") {
        PhaseAccumulator phase;
        phase.reset(0.1);

        // Half a sample at -0.5 crosses 0 at 40% of the half — frac 0.2. The
        // reset to 0 then fires at 0.5, and because the phase keeps running
        // backward it leaves 0 in the same instant: the sync and the wrap it
        // provokes are coincident and compose to 0.85 -> 1.0.
        REQUIRE(phase.advance_synced(-0.5, 0.5, 0.0) == 3);
        CHECK(phase.events()[0].kind == PhaseEventKind::wrap_backward);
        CHECK_THAT(phase.events()[0].frac, WithinAbs(0.2, 1e-15));

        CHECK(phase.events()[1].kind == PhaseEventKind::sync);
        CHECK(phase.events()[1].frac == 0.5);
        CHECK_THAT(phase.events()[1].phase_before, WithinAbs(0.85, 1e-15));
        CHECK(phase.events()[1].phase_after == 0.0);

        CHECK(phase.events()[2].kind == PhaseEventKind::wrap_backward);
        CHECK(phase.events()[2].frac == 0.5);
        CHECK(phase.events()[2].phase_before == 0.0);
        CHECK(phase.events()[2].phase_after == 1.0);

        CHECK_THAT(phase.phase(), WithinAbs(0.75, 1e-15));
    }
}

TEST_CASE("PhaseAccumulator events chain into a continuous trajectory", "[signal][osc]") {
    SECTION("touching the boundary and reversing cancels to no net jump") {
        // Phase rises to exactly 1.0 at the sample boundary, then the increment
        // goes negative. Nothing is actually discontinuous — the phase touched
        // the top and turned around. The forward wrap and the backward wrap are
        // coincident and must sum to zero, or a corrector cancels a step that
        // never happened.
        PhaseAccumulator phase;
        phase.reset(0.5);

        REQUIRE(phase.advance(0.5) == 1);
        REQUIRE(phase.events()[0].kind == PhaseEventKind::wrap_forward);
        const double up = phase.events()[0].phase_after - phase.events()[0].phase_before;

        REQUIRE(phase.advance(-0.5) == 1);
        REQUIRE(phase.events()[0].kind == PhaseEventKind::wrap_backward);
        CHECK(phase.events()[0].frac == 0.0);
        const double down = phase.events()[0].phase_after - phase.events()[0].phase_before;

        CHECK(up + down == 0.0);
        CHECK(phase.phase() == 0.5);
    }

    SECTION("each event resumes where the previous one left off") {
        PhaseAccumulator phase;
        phase.reset(0.7);

        // Two forward wraps plus a sync, so the chain covers both kinds.
        const int n = phase.advance_synced(2.4, 0.9, 0.35);
        REQUIRE(n >= 2);

        // Walking the events must land on the phase the accumulator reports:
        // start + total increment - (sum of every jump) == final phase.
        double jumps = 0.0;
        for (const PhaseEvent& e : phase.events())
            jumps += e.phase_after - e.phase_before;

        // The sync discards the phase accrued before it, so reconstruct across
        // the reset instead: the last event's phase_after plus the increment
        // remaining after it.
        const PhaseEvent& last = phase.events().back();
        const double expected = last.phase_after + 2.4 * (1.0 - last.frac);
        CHECK_THAT(phase.phase(), WithinAbs(expected - std::floor(expected), 1e-15));
        CHECK(jumps != 0.0);
    }
}

TEST_CASE("PhaseAccumulator renders bit-identically from the same inputs", "[signal][osc][rt-safety]") {
    // Determinism is scoped to same-binary bit-exactness. The accumulator holds
    // no hidden state and calls no transcendental, so the render is a pure
    // function of the input increments.
    const auto render = [] {
        std::vector<double> out;
        out.reserve(4096 * 3);

        PhaseAccumulator phase;
        phase.reset(0.123456789);

        std::uint32_t rng = 0x9E3779B9u;
        for (int i = 0; i < 4096; ++i) {
            rng = rng * 1664525u + 1013904223u;
            // Increments spanning both signs and both sides of 1.0, so the run
            // covers the wrap, through-zero, and multi-wrap paths.
            const double increment =
                (static_cast<double>(rng >> 8) / 16777216.0 - 0.5) * 6.0;

            if ((rng & 0x3Fu) == 0)
                phase.advance_synced(increment, 0.5, 0.0);
            else
                phase.advance(increment);

            out.push_back(phase.phase());
            out.push_back(static_cast<double>(phase.events().size()));
            out.push_back(phase.events().empty() ? -1.0 : phase.events()[0].frac);
        }
        return out;
    };

    const std::vector<double> a = render();
    const std::vector<double> b = render();

    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        REQUIRE(a[i] == b[i]); // bit-exact, not approximate
}
