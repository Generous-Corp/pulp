// Modulation toolkit, event domain: the trigger/gate kit and the envelope
// family.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/envelope.hpp>
#include <pulp/signal/mod_tools.hpp>
#include <pulp/signal/trigger.hpp>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kSampleRate = 48000.0;

/// Sample indices at which a per-sample boolean process fires, over `n`
/// samples, given an input trigger at the sample indices in `triggers`.
template <typename Fn> std::vector<int> fire_times(int n, Fn&& step) {
    std::vector<int> times;
    for (int i = 0; i < n; ++i)
        if (step(i))
            times.push_back(i);
    return times;
}

template <typename Multiplier>
concept AcceptsIntegerClock = requires(Multiplier multiplier) { multiplier.process(1); };

template <typename Multiplier>
concept HasEventFactor = requires(Multiplier multiplier) { multiplier.set_multiplier(4); };

template <typename Multiplier>
concept HasSignalFactor = requires(Multiplier multiplier) { multiplier.set_multiple(4); };

template <typename Detector>
concept AcceptsIntegerTrigger = requires(Detector detector) { detector.process(1); };

template <typename Detector>
concept AcceptsFloatTrigger = requires(Detector detector) { detector.process(1.0f); };

template <typename Detector>
concept AcceptsExplicitSignalTrigger =
    requires(Detector detector) { detector.process_signal(1.0f); };

static_assert(AcceptsIntegerClock<ClockMult>);
static_assert(!AcceptsIntegerClock<SignalClockMult>);
static_assert(HasEventFactor<ClockMult>);
static_assert(!HasSignalFactor<ClockMult>);
static_assert(!HasEventFactor<SignalClockMult>);
static_assert(HasSignalFactor<SignalClockMult>);
static_assert(std::same_as<ClockMultT, ClockMult>);
static_assert(std::same_as<ClockMultT, ClockMult64>);
static_assert(std::same_as<EventClockMult64, ClockMult64>);
static_assert(std::same_as<round2::ClockMultT<float>, ClockMult>);
static_assert(std::same_as<round2::ClockMultT<double>, ClockMult>);
static_assert(!AcceptsIntegerTrigger<TriggerDetect>);
static_assert(!AcceptsFloatTrigger<TriggerDetect>);
static_assert(AcceptsExplicitSignalTrigger<TriggerDetect>);
static_assert(AcceptsFloatTrigger<HystereticTriggerDetect>);
static_assert(!AcceptsExplicitSignalTrigger<HystereticTriggerDetect>);

} // namespace

// ── TriggerDetectT ───────────────────────────────────────────────────────────

TEST_CASE("TriggerDetect fires on the rising edge only", "[signal][mod][trigger]") {
    TriggerDetect detect;
    detect.prepare(kSampleRate);
    detect.set_refractory_ms(0.0);

    REQUIRE(detect.process(true));
    REQUIRE_FALSE(detect.process(true));
    REQUIRE_FALSE(detect.process(false));
    REQUIRE(detect.process(true));
}

TEST_CASE("TriggerDetect refractory window is sample exact", "[signal][mod][trigger]") {
    TriggerDetect detect;
    detect.prepare(kSampleRate);
    detect.set_refractory_ms(1.0); // 48 samples

    // Alternate high/low every sample so an edge is available constantly.
    const auto times = fire_times(500, [&](int i) { return detect.process(i % 2 == 0); });

    REQUIRE(times.size() >= 5);
    REQUIRE(times[0] == 0);
    for (std::size_t i = 1; i < times.size(); ++i)
        REQUIRE(times[i] - times[i - 1] == 48);
}

TEST_CASE("TriggerDetect thresholds a continuous signal", "[signal][mod][trigger]") {
    TriggerDetect detect;
    detect.prepare(kSampleRate);
    detect.set_refractory_ms(0.0);
    detect.set_threshold(0.5f);

    REQUIRE_FALSE(detect.process_signal(0.4f));
    REQUIRE(detect.process_signal(0.6f));
    REQUIRE_FALSE(detect.process_signal(0.9f));
    REQUIRE_FALSE(detect.process_signal(0.1f));
    REQUIRE(detect.process_signal(0.7f));
}

TEST_CASE("TriggerDetect process_signal preserves strict threshold and refractory",
          "[signal][mod][trigger][compatibility]") {
    TriggerDetect detect;
    detect.prepare(1000.0);
    detect.set_refractory_ms(10.0);
    detect.set_threshold(0.5f);

    REQUIRE_FALSE(detect.process_signal(0.5f));
    REQUIRE(detect.process_signal(0.6f));
    REQUIRE_FALSE(detect.process_signal(0.0f));
    REQUIRE_FALSE(detect.process_signal(0.6f));

    // Let the ten-sample refractory expire, then prove recovery on the next
    // strict crossing.
    REQUIRE_FALSE(detect.process_signal(0.0f));
    for (int i = 0; i < 7; ++i) REQUIRE_FALSE(detect.process_signal(0.0f));
    REQUIRE(detect.process_signal(0.6f));
}

TEST_CASE("HystereticTriggerDetect rearms at the low threshold",
          "[signal][mod][trigger][hysteresis]") {
    HystereticTriggerDetect detect;
    detect.set_thresholds(0.75, 0.25);

    REQUIRE(detect.process(0.8f));
    REQUIRE_FALSE(detect.process(0.2f));
    REQUIRE(detect.process(0.8f));
}

TEST_CASE("HystereticTriggerDetect reports the state owned by its process lane",
          "[signal][mod][trigger][hysteresis]") {
    HystereticTriggerDetect detector;
    detector.set_thresholds(0.75, 0.25);
    detector.reset();
    for (float sample : {0.0f, 0.8f, 0.7f, 0.2f, 0.9f, 0.1f}) {
        const bool was_high = detector.high();
        const bool rising = detector.process(sample);
        REQUIRE(rising == (!was_high && detector.high()));
    }
}

// ── GateGenT ─────────────────────────────────────────────────────────────────

TEST_CASE("GateGen holds the gate for exactly the set length", "[signal][mod][trigger]") {
    GateGen gate;
    gate.prepare(kSampleRate);
    gate.set_length_ms(10.0); // 480 samples

    int high = 0;
    for (int i = 0; i < 1000; ++i)
        if (gate.process(i == 0))
            ++high;
    REQUIRE(high == 480);
}

TEST_CASE("GateGen retrigger policies differ as documented", "[signal][mod][trigger]") {
    auto count_high = [](GateGen::Retrigger policy) {
        GateGen gate;
        gate.prepare(kSampleRate);
        gate.set_length_samples(100.0);
        gate.set_retrigger(policy);
        int high = 0;
        // Two triggers 50 samples apart.
        for (int i = 0; i < 400; ++i)
            if (gate.process(i == 0 || i == 50))
                ++high;
        return high;
    };

    REQUIRE(count_high(GateGen::Retrigger::restart) == 150); // 50 + a fresh 100
    REQUIRE(count_high(GateGen::Retrigger::ignore) == 100);  // second dropped
    REQUIRE(count_high(GateGen::Retrigger::extend) == 200);  // 50 + 50 + 100
}

// ── ClockDividerT / ClockMultT ───────────────────────────────────────────────

TEST_CASE("ClockDivider passes every Nth trigger from the first", "[signal][mod][trigger]") {
    ClockDivider divider;
    divider.set_division(3);

    const auto times = fire_times(31, [&](int i) { return divider.process(i % 3 == 0); });
    // Input triggers at 0, 3, 6, ...; every third passes: 0, 9, 18, 27.
    REQUIRE(times == std::vector<int>{0, 9, 18, 27});

    divider.reset_phase();
    REQUIRE(divider.process(true));
}

TEST_CASE("ClockMult emits N evenly spaced ticks per measured period", "[signal][mod][trigger]") {
    ClockMult multiplier;
    multiplier.set_multiplier(4);

    // Input triggers every 400 samples.
    const auto times = fire_times(1600, [&](int i) { return multiplier.process(i % 400 == 0); });

    REQUIRE(multiplier.period_samples() == 400);
    // The first period is only the measurement; multiplied ticks start after
    // the second input trigger.
    REQUIRE(times.front() == 0);
    const std::vector<int> expected_after_measurement{400,  500,  600,  700,  800,  900,
                                                      1000, 1100, 1200, 1300, 1400, 1500};
    std::vector<int> after;
    for (int t : times)
        if (t >= 400)
            after.push_back(t);
    REQUIRE(after == expected_after_measurement);
}

TEST_CASE("ClockMult at 1x passes the input through", "[signal][mod][trigger]") {
    ClockMult multiplier;
    multiplier.set_multiplier(1);
    const auto times = fire_times(1000, [&](int i) { return multiplier.process(i % 250 == 0); });
    REQUIRE(times == std::vector<int>{0, 250, 500, 750});
}

TEST_CASE("ClockMultT preserves concrete type-position and integral event compatibility",
          "[signal][mod][trigger][compatibility]") {
    const auto accepts_concrete = [](ClockMultT& multiplier) {
        multiplier.set_multiplier(1);
        return multiplier.process(1);
    };

    ClockMultT multiplier;
    REQUIRE(accepts_concrete(multiplier));
    REQUIRE_FALSE(multiplier.process(0));

    round2::ClockMultT<double> round2_spelling;
    REQUIRE(round2_spelling.process(1));
}

TEST_CASE("event and signal clock multipliers expose one scheduler each",
          "[signal][mod][trigger][regression]") {
    EventClockMult events;
    events.set_multiplier(4);

    SignalClockMult signal;
    signal.prepare(kSampleRate);
    signal.set_multiple(4);

    // The event adapter consumes a deliberately decoded bool. The signal
    // adapter consumes a deliberately floating-point CV sample. The signal
    // adapter rejects integers; the concrete event adapter preserves its
    // established implicit bool conversion because no overload ambiguity
    // remains after the type split.
    REQUIRE(events.process(true));
    REQUIRE(signal.process(1.0f));

    // The established precision-specialized event names remain source- and
    // behavior-compatible while still accepting only decoded bool events.
    ClockMult64 precise_events;
    precise_events.set_multiplier(4);
    REQUIRE(precise_events.process(true));
}

// ── BurstGenT ────────────────────────────────────────────────────────────────

TEST_CASE("BurstGen even spacing matches the closed form", "[signal][mod][trigger]") {
    BurstGen burst;
    burst.prepare(kSampleRate);
    burst.set_count(4);
    burst.set_spacing_ms(10.0); // 480 samples between hits when the curve is flat
    burst.set_spacing_curve(0.0f);

    std::vector<int> times;
    for (int i = 0; i < 3000; ++i) {
        const auto hit = burst.process(i == 0);
        if (hit.fired)
            times.push_back(i);
    }
    REQUIRE(times == std::vector<int>{0, 480, 960, 1440});
}

TEST_CASE("BurstGen spacing curve accelerates and decelerates", "[signal][mod][trigger]") {
    auto burst_times = [](float curve) {
        BurstGen burst;
        burst.prepare(kSampleRate);
        burst.set_count(5);
        burst.set_spacing_ms(10.0);
        burst.set_spacing_curve(curve);
        std::vector<int> times;
        for (int i = 0; i < 5000; ++i) {
            const auto hit = burst.process(i == 0);
            if (hit.fired)
                times.push_back(i);
        }
        return times;
    };

    auto gaps = [](const std::vector<int>& times) {
        std::vector<int> out;
        for (std::size_t i = 1; i < times.size(); ++i)
            out.push_back(times[i] - times[i - 1]);
        return out;
    };

    // Closed form: hit i lands at span * stage_curve(i / (count - 1), curve),
    // with span = (count - 1) * spacing in samples.
    const double span = 4.0 * 480.0;
    for (float curve : {-1.0f, 0.0f, 1.0f}) {
        const auto times = burst_times(curve);
        REQUIRE(times.size() == 5);
        for (int i = 0; i < 5; ++i) {
            const auto expected = static_cast<int>(std::llround(
                span * static_cast<double>(stage_curve(static_cast<float>(i) / 4.0f, curve))));
            REQUIRE(times[static_cast<std::size_t>(i)] == expected);
        }
    }

    const auto accelerating = gaps(burst_times(1.0f));
    for (std::size_t i = 1; i < accelerating.size(); ++i)
        REQUIRE(accelerating[i] < accelerating[i - 1]);

    const auto decelerating = gaps(burst_times(-1.0f));
    for (std::size_t i = 1; i < decelerating.size(); ++i)
        REQUIRE(decelerating[i] > decelerating[i - 1]);
}

TEST_CASE("BurstGen ramps the per-hit level", "[signal][mod][trigger]") {
    BurstGen burst;
    burst.prepare(kSampleRate);
    burst.set_count(5);
    burst.set_spacing_ms(5.0);
    burst.set_levels(1.0f, 0.2f);

    std::vector<float> levels;
    for (int i = 0; i < 3000; ++i) {
        const auto hit = burst.process(i == 0);
        if (hit.fired)
            levels.push_back(hit.level);
    }
    REQUIRE(levels.size() == 5);
    REQUIRE_THAT(levels.front(), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(levels.back(), WithinAbs(0.2f, 1e-6f));
    for (std::size_t i = 1; i < levels.size(); ++i)
        REQUIRE(levels[i] < levels[i - 1]);
}

TEST_CASE("BurstGen count change on an idle generator fires nothing", "[signal][mod][trigger]") {
    BurstGen burst;
    burst.prepare(kSampleRate);
    burst.set_count(4);
    burst.set_spacing_ms(10.0);

    // Complete one burst, then only turn the count knob up. The finished
    // marker is next_index_ == count_, so growing the count must not resurrect
    // the burst.
    int fired = 0;
    for (int i = 0; i < 4800; ++i)
        if (burst.process(i == 0).fired)
            ++fired;
    REQUIRE(fired == 4);
    REQUIRE_FALSE(burst.active());

    burst.set_count(8);
    REQUIRE_FALSE(burst.active());
    for (int i = 0; i < 48000; ++i)
        REQUIRE_FALSE(burst.process(false).fired);

    // A never-triggered generator stays inert through the same change.
    BurstGen fresh;
    fresh.prepare(kSampleRate);
    fresh.set_count(8);
    for (int i = 0; i < 48000; ++i)
        REQUIRE_FALSE(fresh.process(false).fired);
}

// ── TrigDelayT ───────────────────────────────────────────────────────────────

TEST_CASE("TrigDelay delays by exactly the set time", "[signal][mod][trigger]") {
    TrigDelay delay;
    delay.prepare(kSampleRate);
    delay.set_delay_ms(15.0); // 720 samples

    const auto times = fire_times(2000, [&](int i) { return delay.process(i == 0); });
    REQUIRE(times == std::vector<int>{720});
    REQUIRE(delay.dropped() == 0);
}

TEST_CASE("TrigDelay zero delay passes through", "[signal][mod][trigger]") {
    TrigDelay delay;
    delay.prepare(kSampleRate);
    delay.set_delay_ms(0.0);
    const auto times = fire_times(100, [&](int i) { return delay.process(i == 10); });
    REQUIRE(times == std::vector<int>{10});
}

TEST_CASE("TrigDelay drops rather than overwrites when full", "[signal][mod][trigger]") {
    TrigDelay delay;
    delay.prepare(kSampleRate);
    delay.set_delay_samples(5000.0);

    int fired = 0;
    for (int i = 0; i < 100; ++i)
        if (delay.process(true))
            ++fired;
    REQUIRE(fired == 0);
    REQUIRE(delay.dropped() == 100 - TrigDelay::kCapacity);
}

TEST_CASE("TrigDelay is inert when zero-initialized", "[signal][mod][trigger]") {
    // The queue's empty marker is 0 so a zero-initialized instance holds no
    // pending triggers; the alternative marker would fire kCapacity ghosts.
    TrigDelay delay{};
    for (int i = 0; i < 100; ++i)
        REQUIRE_FALSE(delay.process(false));
}

// ── envelope family: stage timing ────────────────────────────────────────────

TEST_CASE("Ar sustains at one while gated and releases on the falling edge",
          "[signal][mod][envelope]") {
    Ar envelope;
    envelope.prepare(kSampleRate);
    envelope.set_attack_ms(10.0);  // 480 samples
    envelope.set_release_ms(20.0); // 960 samples

    envelope.gate(true);
    for (int i = 0; i < 479; ++i) {
        REQUIRE(envelope.stage() == EnvelopeStage::attack);
        (void)envelope.next();
    }
    (void)envelope.next();
    REQUIRE(envelope.stage() == EnvelopeStage::sustain);
    for (int i = 0; i < 5000; ++i)
        REQUIRE_THAT(envelope.next(), WithinAbs(1.0f, 1e-6f));

    envelope.gate(false);
    for (int i = 0; i < 959; ++i) {
        REQUIRE(envelope.stage() == EnvelopeStage::release);
        (void)envelope.next();
    }
    (void)envelope.next();
    REQUIRE(envelope.stage() == EnvelopeStage::idle);
    REQUIRE_THAT(envelope.next(), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("Ar gate is edge-driven so it can be called every sample", "[signal][mod][envelope]") {
    Ar held;
    Ar edged;
    for (auto* envelope : {&held, &edged}) {
        envelope->prepare(kSampleRate);
        envelope->set_attack_ms(10.0);
        envelope->set_release_ms(10.0);
    }
    edged.gate(true);
    for (int i = 0; i < 1000; ++i) {
        held.gate(true); // repeated, must not restart the attack
        REQUIRE_THAT(held.next(), WithinAbs(edged.next(), 1e-6f));
    }
}

TEST_CASE("Ad completes attack then decay and ignores gate semantics", "[signal][mod][envelope]") {
    Ad envelope;
    envelope.prepare(kSampleRate);
    envelope.set_attack_ms(5.0); // 240 samples
    envelope.set_decay_ms(10.0); // 480 samples
    envelope.trigger();

    for (int i = 0; i < 240; ++i)
        (void)envelope.next();
    REQUIRE(envelope.stage() == EnvelopeStage::decay);
    for (int i = 0; i < 480; ++i)
        (void)envelope.next();
    REQUIRE(envelope.stage() == EnvelopeStage::idle);
    REQUIRE_THAT(envelope.current(), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("Ahd holds at full level for exactly the hold time", "[signal][mod][envelope]") {
    Ahd envelope;
    envelope.prepare(kSampleRate);
    envelope.set_attack_ms(5.0); // 240
    envelope.set_hold_ms(20.0);  // 960
    envelope.set_decay_ms(10.0); // 480
    envelope.trigger();

    for (int i = 0; i < 240; ++i)
        (void)envelope.next();
    REQUIRE(envelope.stage() == EnvelopeStage::hold);
    for (int i = 0; i < 960; ++i) {
        REQUIRE(envelope.stage() == EnvelopeStage::hold);
        REQUIRE_THAT(envelope.next(), WithinAbs(1.0f, 1e-6f));
    }
    REQUIRE(envelope.stage() == EnvelopeStage::decay);
}

TEST_CASE("Dahdsr walks every stage in order", "[signal][mod][envelope]") {
    Dahdsr envelope;
    envelope.prepare(kSampleRate);
    envelope.set_delay_ms(5.0);  // 240
    envelope.set_attack_ms(5.0); // 240
    envelope.set_hold_ms(5.0);   // 240
    envelope.set_decay_ms(10.0); // 480
    envelope.set_sustain(0.5f);
    envelope.set_release_ms(10.0); // 480
    envelope.note_on();

    auto run_stage = [&](EnvelopeStage expected, int samples) {
        for (int i = 0; i < samples; ++i) {
            REQUIRE(envelope.stage() == expected);
            (void)envelope.next();
        }
    };

    run_stage(EnvelopeStage::delay, 240);
    run_stage(EnvelopeStage::attack, 240);
    run_stage(EnvelopeStage::hold, 240);
    run_stage(EnvelopeStage::decay, 480);
    REQUIRE(envelope.stage() == EnvelopeStage::sustain);
    REQUIRE_THAT(envelope.next(), WithinAbs(0.5f, 1e-6f));

    envelope.note_off();
    run_stage(EnvelopeStage::release, 480);
    REQUIRE(envelope.stage() == EnvelopeStage::idle);
}

TEST_CASE("Dahdsr output stays zero for the whole delay stage", "[signal][mod][envelope]") {
    Dahdsr envelope;
    envelope.prepare(kSampleRate);
    envelope.set_delay_ms(10.0);
    envelope.set_attack_ms(1.0);
    envelope.note_on();
    for (int i = 0; i < 480; ++i)
        REQUIRE(envelope.next() == 0.0f);
    REQUIRE(envelope.stage() == EnvelopeStage::attack);
    // The first attack sample sits at progress 0, so the rise starts on the
    // one after it.
    REQUIRE(envelope.next() == 0.0f);
    REQUIRE(envelope.next() > 0.0f);
}

TEST_CASE("Dahdsr retrigger with a delay stage holds the current level",
          "[signal][mod][envelope]") {
    Dahdsr envelope;
    envelope.prepare(kSampleRate);
    envelope.set_delay_ms(10.0); // 480 samples
    envelope.set_attack_ms(10.0);
    envelope.set_decay_ms(10.0);
    envelope.set_sustain(0.7f);

    envelope.note_on();
    float level = 0.0f;
    for (int i = 0; i < 4800; ++i)
        level = envelope.next();
    REQUIRE_THAT(level, WithinAbs(0.7f, 1e-4f));

    // Retrigger-from-current-level: the delay must hold the captured level,
    // not snap to zero and then jump back for the attack.
    envelope.note_on();
    for (int i = 0; i < 480; ++i)
        REQUIRE_THAT(envelope.next(), WithinAbs(0.7f, 1e-4f));
    REQUIRE(envelope.stage() == EnvelopeStage::attack);
}

TEST_CASE("Dahdsr velocity scales the peak and the sustain floor", "[signal][mod][envelope]") {
    Dahdsr envelope;
    envelope.prepare(kSampleRate);
    envelope.set_attack_ms(1.0);
    envelope.set_hold_ms(1.0);
    envelope.set_decay_ms(1.0);
    envelope.set_sustain(0.5f);
    envelope.note_on(0.6f);

    float peak = 0.0f;
    for (int i = 0; i < 200; ++i)
        peak = std::max(peak, envelope.next());
    REQUIRE_THAT(peak, WithinAbs(0.6f, 1e-5f));
    for (int i = 0; i < 500; ++i)
        (void)envelope.next();
    REQUIRE(envelope.stage() == EnvelopeStage::sustain);
    REQUIRE_THAT(envelope.next(), WithinAbs(0.3f, 1e-5f)); // sustain * velocity
}

// ── envelope family: curves ──────────────────────────────────────────────────

TEST_CASE("Envelope stage curves follow the shared law", "[signal][mod][envelope]") {
    auto attack_midpoint = [](float curve) {
        Ad envelope;
        envelope.prepare(kSampleRate);
        envelope.set_attack_ms(10.0); // 480 samples
        envelope.set_decay_ms(10.0);
        envelope.set_attack_curve(curve);
        envelope.trigger();
        for (int i = 0; i < 240; ++i)
            (void)envelope.next();
        return envelope.current();
    };

    // The 240th sample sits at progress 239/480 — a stage's first sample is at
    // progress 0, so the midpoint sample is half a step short of 0.5.
    constexpr float kMidProgress = 239.0f / 480.0f;
    REQUIRE_THAT(attack_midpoint(0.0f), WithinAbs(kMidProgress, 1e-5f));
    REQUIRE_THAT(attack_midpoint(1.0f), WithinAbs(curve_rise(kMidProgress, 1.0f), 1e-5f));
    REQUIRE_THAT(attack_midpoint(-1.0f), WithinAbs(curve_rise(kMidProgress, -1.0f), 1e-5f));
    // An exponential attack is slow to leave zero; a logarithmic one is not.
    REQUIRE(attack_midpoint(1.0f) < attack_midpoint(0.0f));
    REQUIRE(attack_midpoint(-1.0f) > attack_midpoint(0.0f));
}

TEST_CASE("Envelope decay curve sign matches the musical convention", "[signal][mod][envelope]") {
    auto decay_midpoint = [](float curve) {
        Ad envelope;
        envelope.prepare(kSampleRate);
        envelope.set_attack_ms(0.0);
        envelope.set_decay_ms(10.0); // 480 samples
        envelope.set_decay_curve(curve);
        envelope.trigger();
        for (int i = 0; i < 240; ++i)
            (void)envelope.next();
        return envelope.current();
    };

    // Exponential decay: fast drop, long tail.
    REQUIRE(decay_midpoint(1.0f) < decay_midpoint(0.0f));
    REQUIRE(decay_midpoint(-1.0f) > decay_midpoint(0.0f));
    REQUIRE_THAT(decay_midpoint(0.0f), WithinAbs(curve_fall(239.0f / 480.0f, 0.0f), 1e-5f));
}

// ── envelope family: looping ─────────────────────────────────────────────────

TEST_CASE("Looping envelope period is exact", "[signal][mod][envelope]") {
    Ad envelope;
    envelope.prepare(kSampleRate);
    envelope.set_attack_ms(10.0); // 480
    envelope.set_decay_ms(10.0);  // 480 -> 960-sample period
    envelope.set_loop(true, 0);
    envelope.trigger();

    // Every loop restart re-enters the attack stage.
    std::vector<int> restarts;
    EnvelopeStage previous = envelope.stage();
    for (int i = 0; i < 5000; ++i) {
        (void)envelope.next();
        if (previous == EnvelopeStage::decay && envelope.stage() == EnvelopeStage::attack)
            restarts.push_back(i);
        previous = envelope.stage();
    }
    REQUIRE(restarts.size() >= 4);
    for (std::size_t i = 1; i < restarts.size(); ++i)
        REQUIRE(restarts[i] - restarts[i - 1] == 960);
}

TEST_CASE("Looping envelope honours a finite loop count", "[signal][mod][envelope]") {
    Ahd envelope;
    envelope.prepare(kSampleRate);
    envelope.set_attack_ms(1.0);
    envelope.set_hold_ms(1.0);
    envelope.set_decay_ms(1.0); // 144-sample period
    envelope.set_loop(true, 3);
    envelope.trigger();

    for (int i = 0; i < 3 * 144; ++i)
        (void)envelope.next();
    REQUIRE(envelope.loops_completed() == 3);
    REQUIRE_FALSE(envelope.active());
    for (int i = 0; i < 500; ++i)
        REQUIRE(envelope.next() == 0.0f);
}

TEST_CASE("Looping Dahdsr skips the sustain hold", "[signal][mod][envelope]") {
    Dahdsr envelope;
    envelope.prepare(kSampleRate);
    envelope.set_delay_ms(0.0);
    envelope.set_attack_ms(2.0);
    envelope.set_hold_ms(0.0);
    envelope.set_decay_ms(2.0);
    envelope.set_sustain(0.5f);
    envelope.set_loop(true, 0);
    envelope.note_on();

    for (int i = 0; i < 5000; ++i) {
        (void)envelope.next();
        REQUIRE(envelope.stage() != EnvelopeStage::sustain);
    }
    REQUIRE(envelope.loops_completed() > 5);
}

TEST_CASE("A note-off ends a looping envelope instead of restarting it",
          "[signal][mod][envelope]") {
    // A zero-length release used to fall through the loop-restart path, so a
    // note-off started the shape over — the opposite of releasing it.
    for (double release_ms : {0.0, 5.0}) {
        Dahdsr envelope;
        envelope.prepare(kSampleRate);
        envelope.set_attack_ms(2.0);
        envelope.set_decay_ms(2.0);
        envelope.set_release_ms(release_ms);
        envelope.set_loop(true, 0);
        envelope.note_on();
        for (int i = 0; i < 500; ++i)
            (void)envelope.next();
        REQUIRE(envelope.active());

        envelope.note_off();
        for (int i = 0; i < 500; ++i)
            (void)envelope.next();
        REQUIRE_FALSE(envelope.active());
        for (int i = 0; i < 500; ++i)
            REQUIRE(envelope.next() == 0.0f);
    }
}

TEST_CASE("A zero-length shape cannot loop forever", "[signal][mod][envelope]") {
    // Every stage is zero: looping must terminate rather than spin.
    Ad envelope;
    envelope.prepare(kSampleRate);
    envelope.set_attack_ms(0.0);
    envelope.set_decay_ms(0.0);
    envelope.set_loop(true, 0);
    envelope.trigger();
    REQUIRE_FALSE(envelope.active());
    REQUIRE(envelope.next() == 0.0f);
}

// ── ModEnvT ──────────────────────────────────────────────────────────────────

TEST_CASE("ModEnv applies a signed depth without touching the raw level",
          "[signal][mod][envelope]") {
    ModEnv envelope;
    envelope.prepare(kSampleRate);
    envelope.set_attack_ms(1.0);
    envelope.set_hold_ms(2.0);
    envelope.set_decay_ms(1.0);
    envelope.set_depth(-0.75f);
    envelope.trigger();

    float most_negative = 0.0f;
    float highest_level = 0.0f;
    for (int i = 0; i < 500; ++i) {
        const float level = envelope.next();
        highest_level = std::max(highest_level, level);
        most_negative = std::min(most_negative, envelope.modulation());
        REQUIRE(level >= 0.0f);
    }
    REQUIRE_THAT(highest_level, WithinAbs(1.0f, 1e-5f));
    REQUIRE_THAT(most_negative, WithinAbs(-0.75f, 1e-5f));
}

// ── TransientDetectorT ───────────────────────────────────────────────────────

TEST_CASE("TransientDetector output is independent of input level", "[signal][mod][envelope]") {
    auto detect_peak = [](float scale) {
        TransientDetector detector;
        detector.prepare(static_cast<float>(kSampleRate));
        detector.reset();
        float peak = 0.0f;
        for (int i = 0; i < 20000; ++i) {
            // A percussive burst every 5000 samples: sharp attack, exponential
            // tail, at 1 kHz.
            const int phase = i % 5000;
            const float amplitude = std::exp(-static_cast<float>(phase) / 400.0f);
            const float value = scale * amplitude *
                                std::sin(6.2831853f * 1000.0f * static_cast<float>(i) / 48000.0f);
            peak = std::max(peak, detector.process(value));
        }
        return peak;
    };

    const float loud = detect_peak(0.501f);   // -6 dBFS
    const float quiet = detect_peak(0.0316f); // -30 dBFS
    REQUIRE(loud > 0.5f);
    REQUIRE_THAT(quiet, WithinRel(loud, 1e-3f));
}

TEST_CASE("TransientDetector reads near zero on steady material", "[signal][mod][envelope]") {
    TransientDetector detector;
    detector.prepare(static_cast<float>(kSampleRate));
    detector.reset();
    float peak_after_settle = 0.0f;
    for (int i = 0; i < 48000; ++i) {
        const float value = 0.5f * std::sin(6.2831853f * 220.0f * static_cast<float>(i) / 48000.0f);
        const float out = detector.process(value);
        if (i > 24000)
            peak_after_settle = std::max(peak_after_settle, out);
    }
    REQUIRE(peak_after_settle < 0.2f);
}
