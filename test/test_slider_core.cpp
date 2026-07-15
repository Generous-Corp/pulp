// Tests for pulp::view::SliderCore — the value engine the continuous controls
// share.
//
// Each case below pins a behavior a control genuinely depends on, and several
// pin a bug that shipped:
//
//   * A wheel notch on a quantized control used to nudge the NORMALIZED value by
//     a fixed 0.004. With an interval set, every notch quantized straight back to
//     the step it started on — thirty notches moved the value by exactly zero.
//   * A programmatic write always fired the change callback, so syncing a control
//     from the thing it had just notified echoed the change back at it. That is
//     what Notify::none exists to stop.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/slider_core.hpp>

#include <cmath>
#include <vector>

using pulp::view::Notify;
using pulp::view::SliderCore;

namespace {

// A decibel-ish range with a half-step: the shape a real gain control has.
SliderCore quantized_db() {
    SliderCore s;
    s.set_range(-61.0, 13.0, 0.5);
    return s;
}

bool lands_on_a_step(double v, double min, double interval) {
    const double steps = (v - min) / interval;
    return std::abs(steps - std::round(steps)) < 1e-9;
}

} // namespace

TEST_CASE("SliderCore quantizes every value in the range to a legal step",
          "[view][slider-core]") {
    auto s = quantized_db();

    // Sweep the whole range at a resolution finer than the interval, so most
    // samples land BETWEEN steps and have to be snapped.
    for (int i = 0; i <= 1000; ++i) {
        const double raw = -61.0 + (74.0 * static_cast<double>(i) / 1000.0);
        s.set_value(raw, Notify::none);
        REQUIRE(lands_on_a_step(s.value(), -61.0, 0.5));
        REQUIRE(s.value() >= -61.0);
        REQUIRE(s.value() <= 13.0);
    }
}

TEST_CASE("SliderCore clamps to the range rather than extrapolating",
          "[view][slider-core]") {
    auto s = quantized_db();

    s.set_value(-1000.0, Notify::none);
    REQUIRE(s.value() == -61.0);

    s.set_value(1000.0, Notify::none);
    REQUIRE(s.value() == 13.0);
}

TEST_CASE("SliderCore skew of 1.0 is linear", "[view][slider-core]") {
    SliderCore s;
    s.set_range(0.0, 100.0);
    s.set_skew(1.0);

    s.set_proportion(0.5, Notify::none);
    REQUIRE_THAT(s.value(), Catch::Matchers::WithinAbs(50.0, 1e-9));
}

TEST_CASE("SliderCore skew_from_midpoint puts the requested value at half travel",
          "[view][slider-core]") {
    // A frequency control: half the travel should land on 632 Hz, not on the
    // arithmetic middle of 20..20000.
    SliderCore s;
    s.set_range(20.0, 20000.0);
    s.set_skew_from_midpoint(632.0);

    s.set_proportion(0.5, Notify::none);
    REQUIRE_THAT(s.value(), Catch::Matchers::WithinRel(632.0, 1e-9));

    // And the curve still spans the ends exactly.
    s.set_proportion(0.0, Notify::none);
    REQUIRE_THAT(s.value(), Catch::Matchers::WithinAbs(20.0, 1e-9));
    s.set_proportion(1.0, Notify::none);
    REQUIRE_THAT(s.value(), Catch::Matchers::WithinAbs(20000.0, 1e-9));
}

TEST_CASE("SliderCore skew round-trips value and proportion",
          "[view][slider-core]") {
    SliderCore s;
    s.set_range(20.0, 20000.0);
    s.set_skew_from_midpoint(632.0);

    for (int i = 0; i <= 100; ++i) {
        const double p = static_cast<double>(i) / 100.0;
        const double v = s.value_for_proportion(p);
        REQUIRE_THAT(s.proportion_for(v), Catch::Matchers::WithinAbs(p, 1e-9));
    }
}

TEST_CASE("SliderCore drag travel is absolute: the pointer and the control stay locked",
          "[view][slider-core]") {
    SliderCore s;
    s.set_range(-24.0, 12.0);          // 36 units of span
    s.set_drag_sensitivity(200);       // 200px crosses the whole range
    s.set_value(-24.0, Notify::none);

    // Half the sensitivity distance = half the range.
    s.drag_to(100.0, /*proportion_at_press=*/0.0, /*fine=*/false, Notify::none);
    REQUIRE_THAT(s.value(), Catch::Matchers::WithinAbs(-6.0, 1e-9));

    // Absolute, not incremental: replaying the SAME travel from the SAME press
    // proportion must land in the same place, never drift.
    s.drag_to(100.0, 0.0, false, Notify::none);
    REQUIRE_THAT(s.value(), Catch::Matchers::WithinAbs(-6.0, 1e-9));
}

TEST_CASE("SliderCore fine drag divides the travel", "[view][slider-core]") {
    SliderCore s;
    s.set_range(0.0, 100.0);
    s.set_drag_sensitivity(100);
    s.set_fine_divisor(10.0);

    s.drag_to(50.0, 0.0, /*fine=*/false, Notify::none);
    REQUIRE_THAT(s.value(), Catch::Matchers::WithinAbs(50.0, 1e-9));

    s.drag_to(50.0, 0.0, /*fine=*/true, Notify::none);
    REQUIRE_THAT(s.value(), Catch::Matchers::WithinAbs(5.0, 1e-9));
}

TEST_CASE("SliderCore reset_to_default returns to the authored default",
          "[view][slider-core]") {
    auto s = quantized_db();
    s.set_default_value(0.0);
    REQUIRE(s.has_default());

    s.set_value(-30.0, Notify::none);
    REQUIRE(s.value() == -30.0);

    s.reset_to_default(Notify::none);
    REQUIRE_THAT(s.value(), Catch::Matchers::WithinAbs(0.0, 1e-9));
}

TEST_CASE("SliderCore opens exactly one gesture per gesture",
          "[view][slider-core]") {
    SliderCore s;
    int begins = 0, ends = 0;
    s.on_gesture_begin = [&] { ++begins; };
    s.on_gesture_end   = [&] { ++ends; };

    // A widget that funnels both a rich event and a legacy pointer callback into
    // the same handler will call begin twice. The bracket is edge-triggered, so
    // the host still records ONE edit, not two.
    s.begin_gesture();
    s.begin_gesture();
    REQUIRE(s.gesture_active());

    for (int i = 0; i < 50; ++i)
        s.drag_to(static_cast<double>(i), 0.0, false, Notify::none);

    s.end_gesture();
    s.end_gesture();

    REQUIRE(begins == 1);
    REQUIRE(ends == 1);
    REQUIRE_FALSE(s.gesture_active());
}

TEST_CASE("SliderCore Notify::none writes silently", "[view][slider-core]") {
    SliderCore s;
    s.set_range(0.0, 100.0);

    int fired = 0;
    s.on_value_change = [&](double) { ++fired; };

    for (int i = 1; i <= 10; ++i)
        s.set_value(static_cast<double>(i), Notify::none);

    // The value moved; nobody was told. This is what stops a programmatic sync
    // from echoing the change back at whatever just made it.
    REQUIRE(s.value() == 10.0);
    REQUIRE(fired == 0);
}

TEST_CASE("SliderCore Notify::sync fires once per real change",
          "[view][slider-core]") {
    SliderCore s;
    s.set_range(0.0, 100.0);

    std::vector<double> seen;
    s.on_value_change = [&](double v) { seen.push_back(v); };

    for (int i = 1; i <= 10; ++i)
        s.set_value(static_cast<double>(i), Notify::sync);

    REQUIRE(seen.size() == 10);
    REQUIRE_THAT(seen.back(), Catch::Matchers::WithinAbs(10.0, 1e-9));
}

TEST_CASE("SliderCore does not notify when the value did not actually change",
          "[view][slider-core]") {
    SliderCore s;
    s.set_range(0.0, 100.0);
    s.set_value(42.0, Notify::none);

    int fired = 0;
    s.on_value_change = [&](double) { ++fired; };

    s.set_value(42.0, Notify::sync);   // same value
    s.set_value(42.0, Notify::sync);   // and again

    REQUIRE(fired == 0);
}

TEST_CASE("SliderCore Notify::async defers to the flush and fires once",
          "[view][slider-core]") {
    SliderCore s;
    s.set_range(0.0, 100.0);

    int fired = 0;
    double last = -1.0;
    s.on_value_change = [&](double v) { ++fired; last = v; };

    s.set_value(7.0, Notify::async);
    REQUIRE(fired == 0);               // nothing yet — it is queued

    pulp::view::flush_async_notifications();
    REQUIRE(fired == 1);
    REQUIRE_THAT(last, Catch::Matchers::WithinAbs(7.0, 1e-9));

    // A second flush with nothing queued is a no-op, not a replay.
    pulp::view::flush_async_notifications();
    REQUIRE(fired == 1);
}

TEST_CASE("SliderCore async notification queued during a flush runs on the NEXT flush",
          "[view][slider-core]") {
    // A listener that re-queues itself must not be able to spin the pump.
    int fired = 0;
    pulp::view::queue_async_notification([&] {
        ++fired;
        if (fired < 3)
            pulp::view::queue_async_notification([&] { ++fired; });
    });

    pulp::view::flush_async_notifications();
    REQUIRE(fired == 1);               // the re-queued one did NOT run in this flush

    pulp::view::flush_async_notifications();
    REQUIRE(fired == 2);
}
