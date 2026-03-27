#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/animation.hpp>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

TEST_CASE("Tween linear interpolation", "[view][animation]") {
    Tween t(0.0f, 100.0f, 1.0f, easing::linear);

    t.advance(0.5f);
    REQUIRE_THAT(t.current(), WithinAbs(50.0, 0.1));
    REQUIRE_FALSE(t.finished());

    t.advance(0.5f);
    REQUIRE_THAT(t.current(), WithinAbs(100.0, 0.1));
    REQUIRE(t.finished());
}

TEST_CASE("Tween ease_out_quad", "[view][animation]") {
    Tween t(0.0f, 1.0f, 1.0f, easing::ease_out_quad);

    t.advance(0.5f);
    // ease_out_quad at t=0.5 = 0.5*(2-0.5) = 0.75
    REQUIRE(t.current() > 0.7f);
}

TEST_CASE("Tween reset", "[view][animation]") {
    Tween t(10.0f, 20.0f, 0.5f);
    t.advance(0.5f);
    REQUIRE(t.finished());

    t.reset();
    REQUIRE_THAT(t.current(), WithinAbs(10.0, 0.1));
    REQUIRE_FALSE(t.finished());
}

TEST_CASE("AnimationManager runs and completes", "[view][animation]") {
    AnimationManager mgr;

    float value = 0;
    bool completed = false;

    mgr.animate(0.0f, 1.0f, 0.1f, easing::linear,
        [&](float v) { value = v; },
        [&]() { completed = true; });

    REQUIRE(mgr.active_count() == 1);

    mgr.tick(0.05f);
    REQUIRE(value > 0.4f);
    REQUIRE_FALSE(completed);

    mgr.tick(0.06f);
    REQUIRE(completed);
    REQUIRE(mgr.active_count() == 0);
}

TEST_CASE("AnimationManager cancel", "[view][animation]") {
    AnimationManager mgr;
    float value = 0;

    auto id = mgr.animate(0.0f, 100.0f, 10.0f, easing::linear,
        [&](float v) { value = v; });

    mgr.tick(1.0f);
    REQUIRE(value > 0);

    mgr.cancel(id);
    REQUIRE(mgr.active_count() == 0);
}

TEST_CASE("Easing functions produce valid output", "[view][animation]") {
    // All easing functions should map 0->0 and 1->1
    REQUIRE_THAT(easing::linear(0.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(easing::linear(1.0f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(easing::ease_in_quad(0.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(easing::ease_in_quad(1.0f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(easing::ease_out_cubic(0.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(easing::ease_out_cubic(1.0f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(easing::ease_out_bounce(0.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(easing::ease_out_bounce(1.0f), WithinAbs(1.0, 0.001));
}

// ── ValueAnimation tests ────────────────────────────────────────────────────

TEST_CASE("ValueAnimation initial value", "[view][animation]") {
    ValueAnimation a(0.5f);
    REQUIRE_THAT(a.value(), WithinAbs(0.5, 0.001));
    REQUIRE_FALSE(a.animating());
}

TEST_CASE("ValueAnimation animate_to reaches target", "[view][animation]") {
    ValueAnimation a(0.0f);
    a.animate_to(1.0f, 0.1f, easing::linear);
    REQUIRE(a.animating());

    // Advance halfway
    a.advance(0.05f);
    REQUIRE_THAT(a.value(), WithinAbs(0.5, 0.05));
    REQUIRE(a.animating());

    // Finish
    a.advance(0.05f);
    REQUIRE_THAT(a.value(), WithinAbs(1.0, 0.001));
    REQUIRE_FALSE(a.animating());
}

TEST_CASE("ValueAnimation set snaps immediately", "[view][animation]") {
    ValueAnimation a(0.0f);
    a.animate_to(1.0f, 1.0f); // long animation
    a.advance(0.01f);
    REQUIRE(a.animating());

    a.set(0.75f);
    REQUIRE_THAT(a.value(), WithinAbs(0.75, 0.001));
    REQUIRE_FALSE(a.animating());
}

TEST_CASE("ValueAnimation cancel stops mid-animation", "[view][animation]") {
    ValueAnimation a(0.0f);
    a.animate_to(1.0f, 1.0f, easing::linear);
    a.advance(0.5f);
    float mid = a.value();
    REQUIRE(mid > 0.0f);
    REQUIRE(mid < 1.0f);

    a.cancel();
    REQUIRE_FALSE(a.animating());
    REQUIRE_THAT(a.value(), WithinAbs(mid, 0.001));

    // Further advance does nothing
    a.advance(1.0f);
    REQUIRE_THAT(a.value(), WithinAbs(mid, 0.001));
}

TEST_CASE("ValueAnimation chained: animate_to while already animating", "[view][animation]") {
    ValueAnimation a(0.0f);
    a.animate_to(1.0f, 1.0f, easing::linear);
    a.advance(0.3f); // partway through
    float mid = a.value();

    // Start new animation from current position
    a.animate_to(0.0f, 0.1f, easing::linear);
    REQUIRE(a.animating());
    REQUIRE_THAT(a.value(), WithinAbs(mid, 0.01)); // starts from where we were

    a.advance(0.1f);
    REQUIRE_THAT(a.value(), WithinAbs(0.0, 0.01)); // reaches new target
}

TEST_CASE("ValueAnimation zero duration snaps", "[view][animation]") {
    ValueAnimation a(0.0f);
    a.animate_to(1.0f, 0.0f);
    REQUIRE_THAT(a.value(), WithinAbs(1.0, 0.001));
    REQUIRE_FALSE(a.animating());
}

TEST_CASE("ValueAnimation advance returns false when not animating", "[view][animation]") {
    ValueAnimation a(0.0f);
    REQUIRE_FALSE(a.advance(0.016f));

    a.animate_to(1.0f, 0.01f);
    a.advance(0.1f); // finish
    REQUIRE_FALSE(a.advance(0.016f)); // no longer animating
}

// ── easing_by_name tests ────────────────────────────────────────────────────

TEST_CASE("easing_by_name resolves known easings", "[view][animation]") {
    REQUIRE(easing_by_name("linear") == easing::linear);
    REQUIRE(easing_by_name("ease_in_quad") == easing::ease_in_quad);
    REQUIRE(easing_by_name("ease_out_quad") == easing::ease_out_quad);
    REQUIRE(easing_by_name("ease_in_out_quad") == easing::ease_in_out_quad);
    REQUIRE(easing_by_name("ease_in_cubic") == easing::ease_in_cubic);
    REQUIRE(easing_by_name("ease_out_cubic") == easing::ease_out_cubic);
    REQUIRE(easing_by_name("ease_in_out_cubic") == easing::ease_in_out_cubic);
    REQUIRE(easing_by_name("ease_in_expo") == easing::ease_in_expo);
    REQUIRE(easing_by_name("ease_out_expo") == easing::ease_out_expo);
    REQUIRE(easing_by_name("ease_out_elastic") == easing::ease_out_elastic);
    REQUIRE(easing_by_name("ease_out_bounce") == easing::ease_out_bounce);
}

TEST_CASE("easing_by_name returns linear for unknown", "[view][animation]") {
    REQUIRE(easing_by_name("nonexistent") == easing::linear);
    REQUIRE(easing_by_name("") == easing::linear);
}
