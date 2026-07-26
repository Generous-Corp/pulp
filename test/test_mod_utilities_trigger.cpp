// Trigger-kit contracts split from the shared mod-utilities suite so the
// individual utility families remain independently navigable.

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/trigger_kit.hpp>
#include <pulp/signal/units.hpp>

#include <cmath>

using namespace pulp::signal;

namespace {
constexpr double kSr = 48000.0;
}

TEST_CASE("TriggerDetect fires once per edge and hysteresis stops chatter",
          "[trigger][mod-utilities]") {
    HystereticTriggerDetect det;
    det.reset();

    REQUIRE(det.process(1.0f));
    REQUIRE_FALSE(det.process(1.0f));
    REQUIRE_FALSE(det.process(0.4f));
    REQUIRE_FALSE(det.process(1.0f));
    REQUIRE_FALSE(det.process(0.1f));
    REQUIRE(det.process(1.0f));
}

TEST_CASE("ClockDivider passes the first edge after reset", "[trigger][mod-utilities]") {
    ClockDivider div;
    div.set_division(4);
    div.reset();

    int passes = 0;
    bool first_passed = false;
    for (int edge = 0; edge < 16; ++edge) {
        const bool p = div.process(1.0f);
        div.process(0.0f);
        if (edge == 0) first_passed = p;
        if (p) ++passes;
    }
    REQUIRE(first_passed);
    REQUIRE(passes == 4);
}

TEST_CASE("trigger-kit factor setters enforce their declared ranges",
          "[trigger][mod-utilities][contract]") {
    ClockDivider div;
    div.set_division(-1);
    REQUIRE(div.division() == ClockDivider::kMinDivision);
    div.set_division(ClockDivider::kMaxDivision + 1);
    REQUIRE(div.division() == ClockDivider::kMaxDivision);

    SignalClockMult mult;
    mult.set_multiple(-1);
    REQUIRE(mult.multiple() == SignalClockMult::kMinMultiple);
    mult.set_multiple(SignalClockMult::kMaxMultiple + 1);
    REQUIRE(mult.multiple() == SignalClockMult::kMaxMultiple);
}

TEST_CASE("trigger-kit time and count setters enforce declared ranges",
          "[trigger][mod-utilities][contract]") {
    GateGen gate;
    gate.set_length_ms(-1.0);
    REQUIRE(gate.length_ms() == GateGen::kMinLengthMs);
    gate.set_length_ms(20000.0);
    REQUIRE(gate.length_ms() == GateGen::kMaxLengthMs);

    TrigDelay delay;
    delay.set_delay_ms(-1.0);
    REQUIRE(delay.delay_ms() == TrigDelay::kMinDelayMs);
    delay.set_delay_ms(20000.0);
    REQUIRE(delay.delay_ms() == TrigDelay::kMaxDelayMs);

    BurstGen burst;
    burst.set_count(0);
    REQUIRE(burst.count() == BurstGen::kMinCount);
    burst.set_count(256);
    REQUIRE(burst.count() == BurstGen::kMaxCount);
    burst.set_interval_ms(0.0);
    REQUIRE(burst.interval_ms() == BurstGen::kMinIntervalMs);
    burst.set_interval_ms(3000.0);
    REQUIRE(burst.interval_ms() == BurstGen::kMaxIntervalMs);
}

TEST_CASE("SignalClockMult emits the stated number of triggers per steady period",
          "[trigger][mod-utilities]") {
    SignalClockMult mult;
    mult.prepare(kSr);
    mult.set_multiple(4);
    mult.reset();

    constexpr int period = 4800;
    int fired = 0;
    for (int p = 0; p < 6; ++p) {
        for (int i = 0; i < period; ++i) {
            const bool f = mult.process(i == 0 ? 1.0f : 0.0f);
            if (p >= 2 && f) ++fired;
        }
    }
    REQUIRE(fired == 4 * 4);
}

TEST_CASE("SignalClockMult treats a stopped clock as stopped, not as a very slow one",
          "[trigger][mod-utilities]") {
    SignalClockMult mult;
    mult.prepare(kSr);
    mult.set_multiple(4);
    mult.reset();

    mult.process(1.0f);
    const int gap =
        static_cast<int>(units::ms_to_samples(SignalClockMult::kMaxPeriodMs, kSr)) * 2;
    int spurious = 0;
    for (int i = 0; i < gap; ++i)
        if (mult.process(0.0f)) ++spurious;
    REQUIRE(spurious == 0);

    REQUIRE(mult.process(1.0f));
    int after = 0;
    for (int i = 0; i < 48000; ++i)
        if (mult.process(0.0f)) ++after;
    REQUIRE(after == 0);
}

TEST_CASE("SignalClockMult factor changes cannot emit subdivisions from the old grid",
          "[trigger][mod-utilities]") {
    SignalClockMult mult;
    mult.prepare(kSr);
    mult.set_multiple(8);
    mult.reset();

    constexpr int period = 800;
    REQUIRE(mult.process(1.0f));
    for (int i = 1; i < period; ++i) REQUIRE_FALSE(mult.process(0.0f));
    REQUIRE(mult.process(1.0f));

    for (int i = 1; i < period / 3; ++i) (void)mult.process(0.0f);
    mult.set_multiple(1);
    for (int i = period / 3; i < period; ++i) REQUIRE_FALSE(mult.process(0.0f));
    REQUIRE(mult.process(1.0f));
}

TEST_CASE("SignalClockMult factor increases schedule only future points of the new grid",
          "[trigger][mod-utilities]") {
    SignalClockMult mult;
    mult.prepare(kSr);
    mult.set_multiple(2);
    mult.reset();

    constexpr int period = 800;
    REQUIRE(mult.process(1.0f));
    for (int i = 1; i < period; ++i) (void)mult.process(0.0f);
    REQUIRE(mult.process(1.0f));

    constexpr int change_at = 250;
    for (int i = 1; i <= change_at; ++i) REQUIRE_FALSE(mult.process(0.0f));
    mult.set_multiple(8);

    int future_subdivisions = 0;
    for (int i = change_at + 1; i < period; ++i)
        if (mult.process(0.0f)) ++future_subdivisions;
    REQUIRE(future_subdivisions == 5);
    REQUIRE(mult.process(1.0f));
}

TEST_CASE("GateGen opens for its stated length and retrigger restarts it",
          "[trigger][mod-utilities]") {
    GateGen gate;
    gate.prepare(kSr);
    gate.set_length_ms(10.0);
    gate.reset();

    const int expected = static_cast<int>(std::llround(units::ms_to_samples(10.0, kSr)));
    int high = 0;
    gate.process(1.0f);
    ++high;
    for (int i = 0; i < expected * 3; ++i)
        if (gate.process(0.0f) > 0.5f) ++high;
    REQUIRE(high == expected);
}

TEST_CASE("TrigDelay delays by its stated time", "[trigger][mod-utilities]") {
    TrigDelay del;
    del.prepare(kSr);
    del.set_delay_ms(5.0);
    del.reset();

    const int expected = static_cast<int>(std::llround(units::ms_to_samples(5.0, kSr)));
    REQUIRE_FALSE(del.process(1.0f));
    int n = 0;
    bool fired = false;
    for (int i = 0; i < expected * 3 && !fired; ++i) {
        fired = del.process(0.0f);
        ++n;
    }
    REQUIRE(fired);
    REQUIRE(n == expected);
}

TEST_CASE("BurstGen emits exactly its count at its interval", "[trigger][mod-utilities]") {
    BurstGen burst;
    burst.prepare(kSr);
    burst.set_count(5);
    burst.set_interval_ms(10.0);
    burst.reset();

    const int interval = static_cast<int>(std::llround(units::ms_to_samples(10.0, kSr)));
    int fired = burst.process(1.0f) ? 1 : 0;
    for (int i = 0; i < interval * 8; ++i)
        if (burst.process(0.0f)) ++fired;
    REQUIRE(fired == 5);
    REQUIRE_FALSE(burst.busy());
}

TEST_CASE("trigger-kit utilities allocate nothing on the audio thread",
          "[trigger][mod-utilities][rt-safety]") {
    HystereticTriggerDetect trigger;
    Comparator comparator;
    GateGen gate;
    ClockDivider divider;
    SignalClockMult multiplier;
    TrigDelay delay;
    BurstGen burst;

    gate.prepare(kSr);
    multiplier.prepare(kSr);
    delay.prepare(kSr);
    burst.prepare(kSr);

    pulp::test::RtAllocationProbe probe;
    for (int i = 0; i < 512; ++i) {
        const float clock = (i % 64) == 0 ? 1.0f : 0.0f;
        (void)trigger.process(clock);
        (void)comparator.process(clock);
        (void)gate.process(clock);
        (void)divider.process(clock);
        (void)multiplier.process(clock);
        (void)delay.process(clock);
        (void)burst.process(clock);
    }
    REQUIRE(probe.allocation_count() == 0);
}
