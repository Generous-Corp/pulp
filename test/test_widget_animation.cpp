#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/frame_clock.hpp>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

// Helper: advance many frames to settle animations
static void settle(auto& widget, int frames = 30) {
    for (int i = 0; i < frames; i++)
        widget.advance_animations(0.016f);
}

// ── Toggle animation tests ──────────────────────────────────────────────────

TEST_CASE("Toggle thumb animates on set_on", "[view][widget_animation]") {
    Toggle toggle;
    toggle.set_bounds({0, 0, 50, 30});

    // Initially off, thumb at 0
    REQUIRE_THAT(toggle.thumb_position(), WithinAbs(0.0, 0.01));

    // Turn on — starts animating
    toggle.set_on(true);
    REQUIRE(toggle.is_on());

    // After one frame, thumb should have moved but not reached 1
    toggle.advance_animations(0.016f);
    REQUIRE(toggle.thumb_position() > 0.0f);
    REQUIRE(toggle.thumb_position() < 1.0f);

    // After settling, thumb should be at 1
    settle(toggle);
    REQUIRE_THAT(toggle.thumb_position(), WithinAbs(1.0, 0.01));
}

TEST_CASE("Toggle thumb animates back on set_on(false)", "[view][widget_animation]") {
    Toggle toggle;
    toggle.set_bounds({0, 0, 50, 30});

    // Start on, settle
    toggle.set_on(true);
    settle(toggle);
    REQUIRE_THAT(toggle.thumb_position(), WithinAbs(1.0, 0.01));

    // Turn off
    toggle.set_on(false);
    toggle.advance_animations(0.016f);
    REQUIRE(toggle.thumb_position() < 1.0f);

    settle(toggle);
    REQUIRE_THAT(toggle.thumb_position(), WithinAbs(0.0, 0.01));
}

TEST_CASE("Toggle hover opacity animates", "[view][widget_animation]") {
    Toggle toggle;
    toggle.set_bounds({0, 0, 50, 30});
    REQUIRE_THAT(toggle.hover_opacity(), WithinAbs(0.0, 0.01));

    toggle.set_hovered(true);
    settle(toggle);
    REQUIRE_THAT(toggle.hover_opacity(), WithinAbs(1.0, 0.01));

    toggle.set_hovered(false);
    settle(toggle);
    REQUIRE_THAT(toggle.hover_opacity(), WithinAbs(0.0, 0.01));
}

TEST_CASE("Toggle set_on same value does not re-animate", "[view][widget_animation]") {
    Toggle toggle;
    toggle.set_bounds({0, 0, 50, 30});
    toggle.set_on(true);
    settle(toggle);

    float pos_before = toggle.thumb_position();
    toggle.set_on(true); // same value
    toggle.advance_animations(0.016f);
    REQUIRE_THAT(toggle.thumb_position(), WithinAbs(pos_before, 0.01));
}

// ── Knob animation tests ────────────────────────────────────────────────────

TEST_CASE("Knob hover glow animates", "[view][widget_animation]") {
    Knob knob;
    knob.set_bounds({0, 0, 48, 48});
    REQUIRE_THAT(knob.hover_glow(), WithinAbs(0.0, 0.01));

    knob.set_hovered(true);
    settle(knob);
    REQUIRE_THAT(knob.hover_glow(), WithinAbs(1.0, 0.01));

    knob.set_hovered(false);
    settle(knob);
    REQUIRE_THAT(knob.hover_glow(), WithinAbs(0.0, 0.01));
}

// ── Fader animation tests ───────────────────────────────────────────────────

TEST_CASE("Fader hover scales thumb", "[view][widget_animation]") {
    Fader fader;
    fader.set_bounds({0, 0, 24, 200});
    REQUIRE_THAT(fader.hover_scale(), WithinAbs(1.0, 0.01));

    fader.set_hovered(true);
    settle(fader);
    REQUIRE_THAT(fader.hover_scale(), WithinAbs(1.3, 0.05));

    fader.set_hovered(false);
    settle(fader);
    REQUIRE_THAT(fader.hover_scale(), WithinAbs(1.0, 0.01));
}

// ── Tooltip animation tests ─────────────────────────────────────────────────

TEST_CASE("Tooltip fades in on show_at", "[view][widget_animation]") {
    Tooltip tip("Hello");
    REQUIRE_THAT(tip.opacity(), WithinAbs(0.0, 0.01));

    tip.show_at({100, 100});
    REQUIRE(tip.visible());

    // After one frame, opacity should be increasing
    tip.advance_animations(0.016f);
    REQUIRE(tip.opacity() > 0.0f);

    // After settling, should be fully opaque
    settle(tip);
    REQUIRE_THAT(tip.opacity(), WithinAbs(1.0, 0.01));
}

TEST_CASE("Tooltip fades out on hide", "[view][widget_animation]") {
    Tooltip tip("Hello");
    tip.show_at({100, 100});
    settle(tip);
    REQUIRE_THAT(tip.opacity(), WithinAbs(1.0, 0.01));

    tip.hide();
    tip.advance_animations(0.016f);
    REQUIRE(tip.opacity() < 1.0f);

    settle(tip);
    REQUIRE_THAT(tip.opacity(), WithinAbs(0.0, 0.01));
    REQUIRE_FALSE(tip.visible()); // auto-hides when opacity reaches 0
}

// ── Motion token tests ──────────────────────────────────────────────────────

TEST_CASE("Widgets use motion tokens from theme", "[view][widget_animation]") {
    // Set up a theme with custom fast motion duration
    Theme theme = Theme::dark();
    theme.dimensions["motion.duration.fast"] = 0.5f; // slower than default

    Toggle toggle;
    toggle.set_bounds({0, 0, 50, 30});
    toggle.set_theme(theme);

    // With a 0.5s hover duration, after 0.08s (default fast) the animation
    // should NOT be finished
    toggle.set_hovered(true);
    for (int i = 0; i < 5; i++) toggle.advance_animations(0.016f); // ~0.08s
    REQUIRE(toggle.hover_opacity() < 0.5f); // still early in a 0.5s animation
}

// ── ScrollView animation tests ──────────────────────────────────────────────

TEST_CASE("ScrollView bar fades in on hover", "[view][widget_animation]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 500}); // taller than view = needs scroll bar
    REQUIRE_THAT(sv.bar_opacity(), WithinAbs(0.0, 0.01));
    REQUIRE_THAT(sv.bar_width(), WithinAbs(4.0, 0.1));

    sv.set_hovered(true);
    settle(sv);
    REQUIRE_THAT(sv.bar_opacity(), WithinAbs(1.0, 0.01));
    REQUIRE_THAT(sv.bar_width(), WithinAbs(8.0, 0.1));
}

TEST_CASE("ScrollView smooth scroll animates position", "[view][widget_animation]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 500});
    REQUIRE_THAT(sv.scroll_y(), WithinAbs(0.0, 0.01));

    sv.scroll_by(0, 50); // scroll down 50px
    REQUIRE_THAT(sv.target_scroll_y(), WithinAbs(50.0, 0.1));

    // After one frame, position should be moving but not there yet
    sv.advance_animations(0.016f);
    REQUIRE(sv.scroll_y() > 0.0f);
    REQUIRE(sv.scroll_y() < 50.0f);

    // After settling, should reach target
    settle(sv);
    REQUIRE_THAT(sv.scroll_y(), WithinAbs(50.0, 0.5));
}

TEST_CASE("ScrollView smooth scroll clamps to bounds", "[view][widget_animation]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 500}); // max scroll = 400

    sv.scroll_by(0, 9999); // way past end
    settle(sv);
    REQUIRE_THAT(sv.scroll_y(), WithinAbs(400.0, 0.5));

    sv.scroll_by(0, -9999); // way past start
    settle(sv);
    REQUIRE_THAT(sv.scroll_y(), WithinAbs(0.0, 0.5));
}

TEST_CASE("ScrollView set_scroll snaps immediately", "[view][widget_animation]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 500});

    sv.set_scroll(0, 100);
    REQUIRE_THAT(sv.scroll_y(), WithinAbs(100.0, 0.01)); // immediate, no animation
}

TEST_CASE("ScrollView bar fades out on leave", "[view][widget_animation]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 500});

    sv.set_hovered(true);
    settle(sv);

    sv.set_hovered(false);
    settle(sv);
    REQUIRE_THAT(sv.bar_opacity(), WithinAbs(0.3, 0.01));
    REQUIRE_THAT(sv.bar_width(), WithinAbs(4.0, 0.1));
}

// ── View hover dispatch tests ───────────────────────────────────────────────

TEST_CASE("View simulate_hover sets hovered state", "[view][widget_animation]") {
    View root;
    root.set_bounds({0, 0, 200, 100});

    auto knob = std::make_unique<Knob>();
    knob->set_bounds({10, 10, 48, 48});
    auto* knob_ptr = knob.get();
    root.add_child(std::move(knob));

    REQUIRE_FALSE(knob_ptr->is_hovered());

    root.simulate_hover({30, 30}); // inside knob
    REQUIRE(knob_ptr->is_hovered());

    root.simulate_hover({150, 50}); // outside knob
    REQUIRE_FALSE(knob_ptr->is_hovered());
}

// ── FrameClock integration test ─────────────────────────────────────────────

TEST_CASE("View frame_clock walks parent chain", "[view][frame_clock]") {
    FrameClock clock;
    View root;
    root.set_bounds({0, 0, 200, 100});
    root.set_frame_clock(&clock);

    auto child = std::make_unique<View>();
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    REQUIRE(root.frame_clock() == &clock);
    REQUIRE(child_ptr->frame_clock() == &clock);
}
