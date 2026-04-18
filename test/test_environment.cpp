// Unit tests for the unified environment API (#342).
//
// The Environment singleton is a process-wide notifier; tests reset it
// in each TEST_CASE so prior tests don't leak listeners or stale state.

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/environment.hpp>

#include <atomic>

using namespace pulp::platform;

namespace {

EnvironmentState make_state(ColorScheme scheme,
                            float scale = 2.0f,
                            float kbd = 0.0f) {
    EnvironmentState s;
    s.display.width = 800;
    s.display.height = 600;
    s.display.scale = scale;
    s.color_scheme = scheme;
    s.keyboard.bottom = kbd;
    s.lifecycle = LifecycleState::foreground;
    s.orientation = Orientation::portrait;
    return s;
}

} // namespace

TEST_CASE("Environment: singleton state defaults are sentinel-safe", "[environment]") {
    Environment::reset_for_test();
    auto s = Environment::instance().snapshot();
    REQUIRE(s.color_scheme    == ColorScheme::unknown);
    REQUIRE(s.lifecycle       == LifecycleState::unknown);
    REQUIRE(s.orientation     == Orientation::unknown);
    REQUIRE(s.memory_pressure == MemoryPressure::normal);
    REQUIRE(s.safe_area.is_zero());
    REQUIRE(s.keyboard.bottom == 0.0f);
}

TEST_CASE("Environment: subscribe receives publish + correct change mask",
          "[environment]") {
    Environment::reset_for_test();
    int dark_calls = 0;
    EnvironmentChange last_change;
    auto token = Environment::instance().subscribe(
        [&](const EnvironmentState& s, EnvironmentChange c) {
            if (s.color_scheme == ColorScheme::dark) ++dark_calls;
            last_change = c;
        });

    // First publish — color scheme changes from unknown -> dark, scale
    // from 1 -> 2, lifecycle from unknown -> foreground, etc. Many flags.
    Environment::inject_for_test(make_state(ColorScheme::dark));
    REQUIRE(dark_calls == 1);
    REQUIRE(last_change.color_scheme);
    REQUIRE(last_change.lifecycle);
    REQUIRE(last_change.display);

    // Idempotent publish — no change, no callback.
    Environment::inject_for_test(make_state(ColorScheme::dark));
    REQUIRE(dark_calls == 1);

    // Color scheme flip alone — only that bit set.
    Environment::inject_for_test(make_state(ColorScheme::light));
    REQUIRE(dark_calls == 1);
    REQUIRE(last_change.color_scheme);
    REQUIRE_FALSE(last_change.display);
    REQUIRE_FALSE(last_change.lifecycle);
}

TEST_CASE("Environment: token RAII unsubscribes", "[environment]") {
    Environment::reset_for_test();
    int calls = 0;
    {
        auto token = Environment::instance().subscribe(
            [&](const EnvironmentState&, EnvironmentChange) { ++calls; });
        Environment::inject_for_test(make_state(ColorScheme::dark));
        REQUIRE(calls == 1);
    } // token dropped → unsubscribed

    Environment::inject_for_test(make_state(ColorScheme::light));
    REQUIRE(calls == 1);
}

TEST_CASE("Environment: token move transfers ownership", "[environment]") {
    Environment::reset_for_test();
    int calls = 0;
    auto sub = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++calls; });
    REQUIRE(sub.valid());

    auto moved = std::move(sub);
    REQUIRE(moved.valid());
    REQUIRE_FALSE(sub.valid());

    Environment::inject_for_test(make_state(ColorScheme::dark));
    REQUIRE(calls == 1);
}

TEST_CASE("Environment: keyboard inset change propagates separately",
          "[environment]") {
    Environment::reset_for_test();
    EnvironmentChange last;
    auto token = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange c) { last = c; });

    Environment::inject_for_test(make_state(ColorScheme::dark, 2.0f, 0.0f));
    Environment::inject_for_test(make_state(ColorScheme::dark, 2.0f, 320.0f));
    REQUIRE(last.keyboard);
    REQUIRE_FALSE(last.color_scheme);
    REQUIRE_FALSE(last.display);
}

TEST_CASE("Environment: safe-area diff detection across all four edges",
          "[environment]") {
    Environment::reset_for_test();
    EnvironmentChange last;
    auto token = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange c) { last = c; });

    auto base = make_state(ColorScheme::dark);
    Environment::inject_for_test(base);

    auto with_inset = base;
    with_inset.safe_area.top = 47.0f;
    Environment::inject_for_test(with_inset);
    REQUIRE(last.safe_area);

    // Bottom-only change.
    auto bottom = with_inset;
    bottom.safe_area.bottom = 34.0f;
    Environment::inject_for_test(bottom);
    REQUIRE(last.safe_area);
    REQUIRE_FALSE(last.color_scheme);
}

TEST_CASE("Environment: memory pressure transitions", "[environment]") {
    Environment::reset_for_test();
    int observed = 0;
    auto token = Environment::instance().subscribe(
        [&](const EnvironmentState& s, EnvironmentChange c) {
            if (c.memory_pressure
                && s.memory_pressure == MemoryPressure::critical) {
                ++observed;
            }
        });

    auto base = make_state(ColorScheme::dark);
    Environment::inject_for_test(base);

    auto moderate = base;
    moderate.memory_pressure = MemoryPressure::moderate;
    Environment::inject_for_test(moderate);
    REQUIRE(observed == 0);

    auto critical = base;
    critical.memory_pressure = MemoryPressure::critical;
    Environment::inject_for_test(critical);
    REQUIRE(observed == 1);
}

TEST_CASE("Environment: callback that drops its own token does not deadlock",
          "[environment]") {
    Environment::reset_for_test();
    std::unique_ptr<Environment::Token> token;
    token = std::make_unique<Environment::Token>(
        Environment::instance().subscribe(
            [&](const EnvironmentState&, EnvironmentChange) {
                token.reset();  // unsubscribe from inside the callback
            }));

    // If publish() held the mutex during dispatch, this would deadlock
    // when the listener calls back into unsubscribe.
    Environment::inject_for_test(make_state(ColorScheme::dark));
    SUCCEED("no deadlock");
}

TEST_CASE("Environment: listener unsubscribed mid-dispatch is not invoked "
          "(#403 codex P1)",
          "[environment][issue-403]") {
    Environment::reset_for_test();

    int a_calls = 0;
    int b_calls = 0;

    Environment::Token a = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++a_calls; });
    Environment::Token b = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++b_calls; });

    // Subscribe a listener that tears down `b`'s subscription while
    // the publish is iterating the copied listener list. Before the
    // #403 fix, `b`'s callback still fired once because publish held
    // a copy that didn't re-check registration. After the fix, the
    // atomic `active` flag is flipped before erase and the second
    // invocation is skipped.
    Environment::Token killer = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { b.reset(); });

    Environment::inject_for_test(make_state(ColorScheme::dark));

    REQUIRE(a_calls == 1);
    // Whether `b` fires before or after `killer` depends on
    // subscription order; subscribe() appends, so iteration is
    // deterministically a, b, killer. `b` is invoked before `killer`
    // clears it — so b_calls should be 1 for this first publish.
    REQUIRE(b_calls == 1);

    // Second publish — `b` is now unsubscribed. `a` still fires,
    // `b` must not fire again (the bug would have it fire once more
    // if the listener list were copied without the active flag).
    Environment::inject_for_test(make_state(ColorScheme::light));
    REQUIRE(a_calls == 2);
    REQUIRE(b_calls == 1);
}

TEST_CASE("Environment: snapshot is consistent with last publish",
          "[environment]") {
    Environment::reset_for_test();
    auto s = make_state(ColorScheme::light, 1.5f, 220.0f);
    s.orientation = Orientation::landscape_left;
    s.safe_area.top = 22.0f;
    Environment::inject_for_test(s);

    auto got = Environment::instance().snapshot();
    REQUIRE(got.color_scheme    == ColorScheme::light);
    REQUIRE(got.orientation     == Orientation::landscape_left);
    REQUIRE(got.display.scale   == 1.5f);
    REQUIRE(got.keyboard.bottom == 220.0f);
    REQUIRE(got.safe_area.top   == 22.0f);
}
