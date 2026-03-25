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
