// Compile-time and behavioral compatibility contracts for event-domain modulation.
#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/trigger.hpp>

#include <concepts>

using namespace pulp::signal;

namespace {

constexpr double kSampleRate = 48000.0;

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
