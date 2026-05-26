// TooltipWindow transient floating tooltip tests
// (closes the gap-doc P2 row "TooltipWindow").
//
// Validates the idle → pending → showing → hiding → idle state machine:
//   - show() waits for hover_delay before appearing,
//   - tick() drives the timer + fade animations,
//   - move_to / show on a different anchor re-anchors live,
//   - hide() runs a fade-out,
//   - cancel_pending bails before the tooltip ever shows,
//   - auto_hide elapses + drives hide() automatically,
//   - MotionPolicy::Off snaps fade animations to their target.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/view/tooltip_window.hpp>
#include <pulp/view/motion_preferences.hpp>

using namespace pulp::view;

namespace {
// Drain ticks until the tooltip's tick() reports "no more work" or we
// hit `max_seconds`. Always advances at least one tick.
void drain(TooltipWindow& t, float max_seconds = 1.5f) {
    constexpr float dt = 1.0f / 60.0f;
    int steps = static_cast<int>(max_seconds / dt) + 2;
    for (int i = 0; i < steps; ++i) {
        if (!t.tick(dt)) break;
    }
}
} // namespace

TEST_CASE("TooltipWindow: defaults to idle and invisible",
          "[tooltip-window]") {
    TooltipWindow t;
    REQUIRE(t.phase() == TooltipWindow::Phase::idle);
    REQUIRE_FALSE(t.visible());
    REQUIRE_FALSE(t.pending());
    REQUIRE(t.opacity() == Catch::Approx(0.0f));
}

TEST_CASE("TooltipWindow: show() waits for hover_delay before appearing",
          "[tooltip-window]") {
    TooltipWindow t;
    t.set_hover_delay(0.2f);
    t.set_fade_duration(0.05f);
    t.show("hello", {100, 100});

    REQUIRE(t.phase() == TooltipWindow::Phase::pending);
    REQUIRE_FALSE(t.visible());
    REQUIRE(t.text() == "hello");

    // Mid-delay: still pending.
    t.tick(0.1f);
    REQUIRE(t.phase() == TooltipWindow::Phase::pending);

    // After delay elapses: transitions to showing.
    t.tick(0.15f);
    REQUIRE(t.phase() == TooltipWindow::Phase::showing);
    REQUIRE(t.visible());

    drain(t, 0.2f);
    REQUIRE(t.opacity() == Catch::Approx(1.0f));
}

TEST_CASE("TooltipWindow: position applies cursor_offset to anchor",
          "[tooltip-window]") {
    TooltipWindow t;
    t.set_cursor_offset({4, 8});
    t.show("x", {100, 200});
    REQUIRE(t.position().x == Catch::Approx(104));
    REQUIRE(t.position().y == Catch::Approx(208));
    REQUIRE(t.last_anchor() == Point{100, 200});
}

TEST_CASE("TooltipWindow: move_to re-anchors while visible",
          "[tooltip-window]") {
    TooltipWindow t;
    t.set_hover_delay(0.05f);
    t.set_fade_duration(0.02f);
    t.set_cursor_offset({0, 0});
    t.show("x", {0, 0});
    drain(t, 0.2f);
    REQUIRE(t.visible());

    t.move_to({50, 75});
    REQUIRE(t.position() == Point{50, 75});
    REQUIRE(t.last_anchor() == Point{50, 75});
}

TEST_CASE("TooltipWindow: move_to is a no-op when idle", "[tooltip-window]") {
    TooltipWindow t;
    t.move_to({10, 10});
    REQUIRE(t.phase() == TooltipWindow::Phase::idle);
    REQUIRE_FALSE(t.visible());
}

TEST_CASE("TooltipWindow: hide() runs a fade-out and returns to idle",
          "[tooltip-window]") {
    TooltipWindow t;
    t.set_hover_delay(0.05f);
    t.set_fade_duration(0.05f);
    t.show("x", {0, 0});
    drain(t, 0.2f);
    REQUIRE(t.opacity() == Catch::Approx(1.0f));

    t.hide();
    REQUIRE(t.phase() == TooltipWindow::Phase::hiding);

    // Mid-fade: opacity is between 0 and 1.
    t.tick(0.02f);
    REQUIRE(t.opacity() > 0.0f);
    REQUIRE(t.opacity() < 1.0f);

    drain(t, 0.2f);
    REQUIRE(t.phase() == TooltipWindow::Phase::idle);
    REQUIRE_FALSE(t.visible());
    REQUIRE(t.opacity() == Catch::Approx(0.0f));
}

TEST_CASE("TooltipWindow: cancel_pending bails out of the hover delay",
          "[tooltip-window]") {
    TooltipWindow t;
    t.set_hover_delay(0.5f);
    t.show("x", {0, 0});
    REQUIRE(t.pending());

    t.cancel_pending();
    REQUIRE(t.phase() == TooltipWindow::Phase::idle);
    REQUIRE_FALSE(t.visible());

    // Future ticks do nothing.
    REQUIRE_FALSE(t.tick(1.0f));
}

TEST_CASE("TooltipWindow: hide() before delay elapses goes straight to idle",
          "[tooltip-window]") {
    TooltipWindow t;
    t.set_hover_delay(0.5f);
    t.show("x", {0, 0});
    REQUIRE(t.pending());
    t.hide();
    REQUIRE(t.phase() == TooltipWindow::Phase::idle);
}

TEST_CASE("TooltipWindow: auto_hide triggers hide after elapsed seconds",
          "[tooltip-window]") {
    TooltipWindow t;
    t.set_hover_delay(0.0f);
    t.set_fade_duration(0.02f);
    t.set_auto_hide(0.1f);
    t.show("x", {0, 0});
    t.tick(0.0001f);  // get past pending → showing transition
    REQUIRE(t.visible());

    // Tick past the auto-hide window — should flip into hiding.
    for (int i = 0; i < 20; ++i) t.tick(0.01f);
    REQUIRE((t.phase() == TooltipWindow::Phase::hiding ||
             t.phase() == TooltipWindow::Phase::idle));
}

TEST_CASE("TooltipWindow: showing a different anchor re-anchors live",
          "[tooltip-window]") {
    TooltipWindow t;
    t.set_hover_delay(0.05f);
    t.set_fade_duration(0.02f);
    t.set_cursor_offset({0, 0});
    t.show("hello", {10, 10});
    drain(t, 0.2f);
    REQUIRE(t.visible());

    // Live re-anchor + text update.
    t.show("world", {100, 200});
    REQUIRE(t.visible());
    REQUIRE(t.text() == "world");
    REQUIRE(t.position() == Point{100, 200});
}

TEST_CASE("TooltipWindow: MotionPolicy::Off snaps fade-in instantly",
          "[tooltip-window]") {
    MotionPreferences::instance().set_override(MotionPolicy::Off);

    TooltipWindow t;
    t.set_hover_delay(0.05f);
    t.set_fade_duration(0.5f);
    t.show("x", {0, 0});
    drain(t, 0.2f);
    REQUIRE(t.opacity() == Catch::Approx(1.0f));

    t.hide();
    drain(t, 0.2f);
    REQUIRE(t.opacity() == Catch::Approx(0.0f));
    REQUIRE_FALSE(t.visible());

    MotionPreferences::instance().reset_for_tests();
}
