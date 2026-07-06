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

TEST_CASE("FrameClock unsubscribe is idempotent for inactive or unknown IDs", "[view][frame_clock]") {
    FrameClock clock;
    int call_count = 0;

    int id = clock.subscribe([&](float) { call_count++; return true; });
    clock.unsubscribe(id);
    clock.unsubscribe(id);
    clock.unsubscribe(id + 100);

    REQUIRE_FALSE(clock.has_active_subscribers());

    clock.tick(0.016f);
    REQUIRE(call_count == 0);
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

TEST_CASE("FrameClock subscriber can remove a later subscriber during tick", "[view][frame_clock]") {
    FrameClock clock;
    int first_calls = 0;
    int second_calls = 0;
    int second_id = 0;

    clock.subscribe([&](float) {
        first_calls++;
        clock.unsubscribe(second_id);
        return true;
    });
    second_id = clock.subscribe([&](float) {
        second_calls++;
        return true;
    });

    clock.tick(0.016f);
    REQUIRE(first_calls == 1);
    REQUIRE(second_calls == 0);
    REQUIRE(clock.has_active_subscribers());

    clock.tick(0.016f);
    REQUIRE(first_calls == 2);
    REQUIRE(second_calls == 0);
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

TEST_CASE("FrameClock reset restarts subscription IDs", "[view][frame_clock]") {
    FrameClock clock;

    int first_id = clock.subscribe([](float) { return true; });
    REQUIRE(first_id == 1);

    clock.reset();
    int after_reset_id = clock.subscribe([](float) { return true; });
    REQUIRE(after_reset_id == 1);
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

TEST_CASE("FrameClock subscriber added during tick starts on the next frame",
          "[view][frame_clock]") {
    FrameClock clock;
    int first_calls = 0;
    int added_calls = 0;
    bool added = false;

    clock.subscribe([&](float) {
        ++first_calls;
        if (!added) {
            added = true;
            clock.subscribe([&](float) {
                ++added_calls;
                return true;
            });
        }
        return true;
    });

    clock.tick(0.010f);
    REQUIRE(first_calls == 1);
    REQUIRE(added_calls == 0);
    REQUIRE(clock.has_active_subscribers());

    clock.tick(0.020f);
    REQUIRE(first_calls == 2);
    REQUIRE(added_calls == 1);
}

TEST_CASE("FrameClock subscriber can unsubscribe itself while returning true",
          "[view][frame_clock]") {
    FrameClock clock;
    int id = 0;
    int calls = 0;
    id = clock.subscribe([&](float) {
        ++calls;
        clock.unsubscribe(id);
        return true;
    });

    clock.tick(0.016f);
    REQUIRE(calls == 1);
    REQUIRE_FALSE(clock.has_active_subscribers());

    clock.tick(0.016f);
    REQUIRE(calls == 1);
}

TEST_CASE("FrameClock false-return compaction preserves later active subscribers",
          "[view][frame_clock]") {
    FrameClock clock;
    int first = 0;
    int second = 0;
    int third = 0;

    clock.subscribe([&](float) {
        ++first;
        return false;
    });
    clock.subscribe([&](float) {
        ++second;
        return true;
    });
    clock.subscribe([&](float) {
        ++third;
        return false;
    });

    clock.tick(0.016f);
    REQUIRE(first == 1);
    REQUIRE(second == 1);
    REQUIRE(third == 1);
    REQUIRE(clock.has_active_subscribers());

    clock.tick(0.016f);
    REQUIRE(first == 1);
    REQUIRE(second == 2);
    REQUIRE(third == 1);
}

// ── Activity channel (wake-from-idle probes) ────────────────────────────────

TEST_CASE("FrameClock activity probe fires on pump_activity, not tick",
          "[view][frame_clock][activity]") {
    FrameClock clock;
    int activity_calls = 0;
    float last_dt = -1;
    clock.subscribe_activity([&](float dt) { activity_calls++; last_dt = dt; });

    REQUIRE(clock.has_activity_subscribers());
    // The key invariant: an activity probe is NOT render-liveness.
    REQUIRE_FALSE(clock.has_active_subscribers());

    clock.pump_activity(0.02f);
    REQUIRE(activity_calls == 1);
    REQUIRE_THAT(last_dt, WithinAbs(0.02, 0.001));

    // tick() must NOT fire activity probes (they are a separate channel), and
    // pump_activity must NOT advance the render clock.
    clock.tick(0.016f);
    REQUIRE(activity_calls == 1);
    REQUIRE(clock.frame() == 1);
    clock.pump_activity(0.02f);
    REQUIRE(activity_calls == 2);
    REQUIRE(clock.frame() == 1);  // pump_activity never advances frame/time
    REQUIRE_THAT(clock.time(), WithinAbs(0.016, 0.001));
}

TEST_CASE("FrameClock render subscriber does not fire on pump_activity",
          "[view][frame_clock][activity]") {
    FrameClock clock;
    int render_calls = 0;
    clock.subscribe([&](float) { render_calls++; return true; });

    clock.pump_activity(0.02f);
    REQUIRE(render_calls == 0);       // render channel untouched by activity pump
    clock.tick(0.016f);
    REQUIRE(render_calls == 1);
}

TEST_CASE("FrameClock unsubscribe_activity removes the probe",
          "[view][frame_clock][activity]") {
    FrameClock clock;
    int calls = 0;
    int id = clock.subscribe_activity([&](float) { calls++; });
    clock.pump_activity(0.02f);
    REQUIRE(calls == 1);

    clock.unsubscribe_activity(id);
    REQUIRE_FALSE(clock.has_activity_subscribers());
    clock.pump_activity(0.02f);
    REQUIRE(calls == 1);              // no longer fired
}

TEST_CASE("FrameClock activity probe may unsubscribe itself during pump",
          "[view][frame_clock][activity]") {
    FrameClock clock;
    int calls = 0;
    int id = -1;
    id = clock.subscribe_activity([&](float) {
        calls++;
        clock.unsubscribe_activity(id);  // re-entrant removal is safe
    });
    clock.pump_activity(0.02f);
    REQUIRE(calls == 1);
    clock.pump_activity(0.02f);
    REQUIRE(calls == 1);              // compacted after the first pump
    REQUIRE_FALSE(clock.has_activity_subscribers());
}

TEST_CASE("FrameClock reset clears activity probes",
          "[view][frame_clock][activity]") {
    FrameClock clock;
    clock.subscribe_activity([](float) {});
    REQUIRE(clock.has_activity_subscribers());
    clock.reset();
    REQUIRE_FALSE(clock.has_activity_subscribers());
}
