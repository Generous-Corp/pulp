// SidePanel slide-in animation tests (pulp #6.3 macos plugin-authoring plan).
//
// Validates the slide-in/out state machine + Tween wiring:
//   - opens from closed,
//   - advance_animations() interpolates progress toward 1.0,
//   - reaches `open` and freezes,
//   - close() runs in reverse and lands on `closed` with visible=false,
//   - toggle() switches direction,
//   - on_state_change fires the expected transition sequence.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/view/side_panel.hpp>
#include <pulp/view/motion_preferences.hpp>

using namespace pulp::view;

namespace {
// Drain a panel's tween to completion by ticking a generous chunk of time.
// We over-step rather than match the exact duration so the test stays robust
// to MotionPreferences scaling and easing tails.
void drain(SidePanel& p, float total_seconds = 1.0f) {
    constexpr float dt = 1.0f / 60.0f;
    int steps = static_cast<int>(total_seconds / dt) + 4;
    for (int i = 0; i < steps && p.is_animating(); ++i) {
        p.advance_animations(dt);
    }
}
} // namespace

TEST_CASE("SidePanel: defaults to closed and invisible-on-paint contract",
          "[side-panel]") {
    SidePanel p;
    REQUIRE(p.state() == SidePanel::State::closed);
    REQUIRE_FALSE(p.is_open());
    REQUIRE_FALSE(p.is_open_or_opening());
    REQUIRE(p.progress() == Catch::Approx(0.0f));
    REQUIRE(p.slide_offset() == Catch::Approx(0.0f));
    REQUIRE(p.edge() == SidePanel::Edge::right);
}

TEST_CASE("SidePanel: open() drives a slide-in tween to open", "[side-panel]") {
    SidePanel p;
    p.set_extent(200.0f);
    p.set_animation_duration(0.2f);
    p.open();
    REQUIRE(p.state() == SidePanel::State::opening);
    REQUIRE(p.is_open_or_opening());

    // Mid-tween: progress is strictly between 0 and 1.
    p.advance_animations(0.05f);
    REQUIRE(p.progress() > 0.0f);
    REQUIRE(p.progress() < 1.0f);
    REQUIRE(p.is_animating());

    drain(p, 0.5f);
    REQUIRE(p.state() == SidePanel::State::open);
    REQUIRE(p.progress() == Catch::Approx(1.0f));
    REQUIRE(p.slide_offset() == Catch::Approx(200.0f));
    REQUIRE_FALSE(p.is_animating());
    REQUIRE(p.visible());
}

TEST_CASE("SidePanel: close() runs in reverse and hides the view",
          "[side-panel]") {
    SidePanel p;
    p.set_animation_duration(0.2f);
    p.open();
    drain(p, 0.5f);
    REQUIRE(p.is_open());

    p.close();
    REQUIRE(p.state() == SidePanel::State::closing);
    p.advance_animations(0.05f);
    REQUIRE(p.progress() < 1.0f);
    REQUIRE(p.progress() > 0.0f);

    drain(p, 0.5f);
    REQUIRE(p.state() == SidePanel::State::closed);
    REQUIRE(p.progress() == Catch::Approx(0.0f));
    REQUIRE_FALSE(p.visible());  // hidden so it doesn't paint / hit-test
}

TEST_CASE("SidePanel: open()/close() are idempotent", "[side-panel]") {
    SidePanel p;
    p.set_animation_duration(0.2f);
    p.open();
    auto progress_after_first = p.progress();
    p.open();  // second open while already opening — should not reset
    REQUIRE(p.state() == SidePanel::State::opening);
    REQUIRE(p.progress() == Catch::Approx(progress_after_first));

    drain(p, 0.5f);
    p.close();
    p.close();  // double-close while already closing — should not reset
    REQUIRE(p.state() == SidePanel::State::closing);
}

TEST_CASE("SidePanel: toggle() flips direction", "[side-panel]") {
    SidePanel p;
    p.set_animation_duration(0.1f);
    p.toggle();
    REQUIRE(p.state() == SidePanel::State::opening);
    drain(p, 0.4f);
    REQUIRE(p.is_open());

    p.toggle();
    REQUIRE(p.state() == SidePanel::State::closing);
    drain(p, 0.4f);
    REQUIRE(p.state() == SidePanel::State::closed);
}

TEST_CASE("SidePanel: on_state_change emits the full transition sequence",
          "[side-panel]") {
    SidePanel p;
    p.set_animation_duration(0.05f);
    std::vector<SidePanel::State> seen;
    p.on_state_change = [&](SidePanel::State s) { seen.push_back(s); };

    p.open();
    drain(p, 0.2f);
    p.close();
    drain(p, 0.2f);

    // Expect: opening, open, closing, closed (in that order).
    REQUIRE(seen.size() == 4);
    REQUIRE(seen[0] == SidePanel::State::opening);
    REQUIRE(seen[1] == SidePanel::State::open);
    REQUIRE(seen[2] == SidePanel::State::closing);
    REQUIRE(seen[3] == SidePanel::State::closed);
}

TEST_CASE("SidePanel: extent controls slide_offset at progress=1",
          "[side-panel]") {
    SidePanel p;
    p.set_extent(320.0f);
    p.set_animation_duration(0.05f);
    p.open();
    drain(p, 0.3f);
    REQUIRE(p.slide_offset() == Catch::Approx(320.0f));

    // Changing extent mid-life updates slide_offset on next read; progress
    // stays at 1.0 because we haven't started a new tween.
    p.set_extent(160.0f);
    REQUIRE(p.slide_offset() == Catch::Approx(160.0f));
}

TEST_CASE("SidePanel: respects MotionPreferences Off (snap to target)",
          "[side-panel]") {
    MotionPreferences::instance().set_override(MotionPolicy::Off);

    SidePanel p;
    p.set_animation_duration(0.5f);
    p.open();
    // With motion off the Tween short-circuits to its target on tick 0.
    p.advance_animations(1.0f / 60.0f);
    REQUIRE(p.state() == SidePanel::State::open);
    REQUIRE(p.progress() == Catch::Approx(1.0f));

    MotionPreferences::instance().reset_for_tests();
}
