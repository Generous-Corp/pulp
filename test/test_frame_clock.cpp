#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/frame_clock.hpp>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

TEST_CASE("FrameClock initial state", "[view][frame_clock]") {
    FrameClock clock;
    REQUIRE_THAT(clock.time(), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(clock.dt(), WithinAbs(0.0, 0.001));
    REQUIRE(clock.frame() == 0);
    REQUIRE_FALSE(clock.has_active_subscribers());
}

TEST_CASE("FrameClock tick advances time", "[view][frame_clock]") {
    FrameClock clock;

    clock.tick(0.016f);
    REQUIRE_THAT(clock.time(), WithinAbs(0.016, 0.001));
    REQUIRE_THAT(clock.dt(), WithinAbs(0.016, 0.001));
    REQUIRE(clock.frame() == 1);

    clock.tick(0.016f);
    REQUIRE_THAT(clock.time(), WithinAbs(0.032, 0.001));
    REQUIRE(clock.frame() == 2);
}

TEST_CASE("FrameClock subscriber called on tick", "[view][frame_clock]") {
    FrameClock clock;
    int call_count = 0;
    float last_dt = 0;

    clock.subscribe([&](float dt) {
        call_count++;
        last_dt = dt;
        return true; // keep subscribed
    });

    REQUIRE(clock.has_active_subscribers());

    clock.tick(0.016f);
    REQUIRE(call_count == 1);
    REQUIRE_THAT(last_dt, WithinAbs(0.016, 0.001));

    clock.tick(0.033f);
    REQUIRE(call_count == 2);
    REQUIRE_THAT(last_dt, WithinAbs(0.033, 0.001));
}

TEST_CASE("FrameClock subscriber auto-removes on false", "[view][frame_clock]") {
    FrameClock clock;
    int call_count = 0;

    clock.subscribe([&](float) {
        call_count++;
        return call_count < 3; // unsubscribe after 3 calls
    });

    for (int i = 0; i < 5; i++) clock.tick(0.016f);
    REQUIRE(call_count == 3);
    REQUIRE_FALSE(clock.has_active_subscribers());
}

TEST_CASE("FrameClock unsubscribe by ID", "[view][frame_clock]") {
    FrameClock clock;
    int call_count = 0;

    int id = clock.subscribe([&](float) { call_count++; return true; });
    REQUIRE(clock.has_active_subscribers());

    clock.tick(0.016f);
    REQUIRE(call_count == 1);

    clock.unsubscribe(id);
    clock.tick(0.016f);
    REQUIRE(call_count == 1); // not called again
    REQUIRE_FALSE(clock.has_active_subscribers());
}

TEST_CASE("FrameClock multiple subscribers", "[view][frame_clock]") {
    FrameClock clock;
    int a = 0, b = 0;

    clock.subscribe([&](float) { a++; return true; });
    clock.subscribe([&](float) { b++; return true; });

    clock.tick(0.016f);
    REQUIRE(a == 1);
    REQUIRE(b == 1);
}

TEST_CASE("FrameClock reset clears all state", "[view][frame_clock]") {
    FrameClock clock;
    clock.subscribe([](float) { return true; });
    clock.tick(1.0f);

    clock.reset();
    REQUIRE_THAT(clock.time(), WithinAbs(0.0, 0.001));
    REQUIRE(clock.frame() == 0);
    REQUIRE_FALSE(clock.has_active_subscribers());
}

TEST_CASE("FrameClock negative dt clamped to zero", "[view][frame_clock]") {
    FrameClock clock;
    clock.tick(-1.0f);
    REQUIRE_THAT(clock.time(), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(clock.dt(), WithinAbs(0.0, 0.001));
    REQUIRE(clock.frame() == 1); // frame still increments
}

TEST_CASE("FrameClock zero dt is valid", "[view][frame_clock]") {
    FrameClock clock;
    int called = 0;
    clock.subscribe([&](float dt) { called++; (void)dt; return true; });
    clock.tick(0.0f);
    REQUIRE(called == 1);
    REQUIRE_THAT(clock.time(), WithinAbs(0.0, 0.001));
}
