// Unit tests for the unified environment API (#342).
//
// The Environment singleton is a process-wide notifier; tests reset it
// in each TEST_CASE so prior tests don't leak listeners or stale state.

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/environment.hpp>

#include <atomic>
#include <memory>
#include <utility>

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

TEST_CASE("Environment: empty listener subscription is inert",
          "[environment][issue-640]") {
    Environment::reset_for_test();

    auto token = Environment::instance().subscribe({});
    REQUIRE_FALSE(token.valid());
    token.reset();

    Environment::inject_for_test(make_state(ColorScheme::dark));
    auto s = Environment::instance().snapshot();
    REQUIRE(s.color_scheme == ColorScheme::dark);
}

TEST_CASE("EnvironmentChange: any reflects individual flags",
          "[environment][issue-640]") {
    EnvironmentChange change;
    REQUIRE_FALSE(change.any());

    change.display = true;
    REQUIRE(change.any());

    change = {};
    change.safe_area = true;
    REQUIRE(change.any());

    change = {};
    change.keyboard = true;
    REQUIRE(change.any());

    change = {};
    change.orientation = true;
    REQUIRE(change.any());

    change = {};
    change.color_scheme = true;
    REQUIRE(change.any());

    change = {};
    change.lifecycle = true;
    REQUIRE(change.any());

    change = {};
    change.memory_pressure = true;
    REQUIRE(change.any());
}

TEST_CASE("SafeAreaInsets zero detection checks all edges",
          "[environment][coverage][issue-640]") {
    SafeAreaInsets insets;
    REQUIRE(insets.is_zero());

    insets.top = 1.0f;
    REQUIRE_FALSE(insets.is_zero());
    insets = {};
    insets.bottom = 1.0f;
    REQUIRE_FALSE(insets.is_zero());
    insets = {};
    insets.left = 1.0f;
    REQUIRE_FALSE(insets.is_zero());
    insets = {};
    insets.right = 1.0f;
    REQUIRE_FALSE(insets.is_zero());
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

TEST_CASE("Environment: reset clears listeners held by live tokens",
          "[environment][issue-640]") {
    Environment::reset_for_test();
    int calls = 0;
    auto token = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++calls; });
    REQUIRE(token.valid());

    Environment::reset_for_test();
    Environment::inject_for_test(make_state(ColorScheme::dark));
    REQUIRE(calls == 0);

    token.reset();
    REQUIRE_FALSE(token.valid());

    Environment::inject_for_test(make_state(ColorScheme::light));
    REQUIRE(calls == 0);
}

TEST_CASE("Environment: token reset is idempotent and preserves other listeners",
          "[environment][coverage][issue-640]") {
    Environment::reset_for_test();
    int first_calls = 0;
    int second_calls = 0;

    auto first = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++first_calls; });
    auto second = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++second_calls; });

    first.reset();
    first.reset();
    REQUIRE_FALSE(first.valid());
    REQUIRE(second.valid());

    Environment::inject_for_test(make_state(ColorScheme::dark));
    REQUIRE(first_calls == 0);
    REQUIRE(second_calls == 1);

    second.reset();
    Environment::inject_for_test(make_state(ColorScheme::light));
    REQUIRE(first_calls == 0);
    REQUIRE(second_calls == 1);
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

TEST_CASE("Environment: publish without listeners still updates snapshot",
          "[environment][coverage][issue-649]") {
    Environment::reset_for_test();

    auto state = make_state(ColorScheme::light, 1.25f, 48.0f);
    state.safe_area.bottom = 12.0f;
    state.memory_pressure = MemoryPressure::moderate;
    Environment::instance().publish(state);

    auto got = Environment::instance().snapshot();
    REQUIRE(got.color_scheme == ColorScheme::light);
    REQUIRE(got.display.scale == 1.25f);
    REQUIRE(got.keyboard.bottom == 48.0f);
    REQUIRE(got.safe_area.bottom == 12.0f);
    REQUIRE(got.memory_pressure == MemoryPressure::moderate);
}

TEST_CASE("Environment: token reset and self-move assignment are idempotent",
          "[environment][coverage][issue-649]") {
    Environment::reset_for_test();
    int calls = 0;

    auto token = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++calls; });
    REQUIRE(token.valid());

    auto* same_token = &token;
    token = std::move(*same_token);
    REQUIRE(token.valid());
    Environment::inject_for_test(make_state(ColorScheme::dark));
    REQUIRE(calls == 1);

    token.reset();
    REQUIRE_FALSE(token.valid());
    token.reset();
    REQUIRE_FALSE(token.valid());
    Environment::inject_for_test(make_state(ColorScheme::light));
    REQUIRE(calls == 1);
}

TEST_CASE("Environment: token move assignment replaces prior subscription",
          "[environment][issue-640]") {
    Environment::reset_for_test();
    int first_calls = 0;
    int second_calls = 0;

    auto token = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++first_calls; });
    auto replacement = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++second_calls; });

    token = std::move(replacement);
    REQUIRE(token.valid());
    REQUIRE_FALSE(replacement.valid());

    Environment::inject_for_test(make_state(ColorScheme::dark));
    REQUIRE(first_calls == 0);
    REQUIRE(second_calls == 1);

    token.reset();
    Environment::inject_for_test(make_state(ColorScheme::light));
    REQUIRE(first_calls == 0);
    REQUIRE(second_calls == 1);
}

TEST_CASE("Environment: token self move assignment preserves subscription",
          "[environment][issue-640]") {
    Environment::reset_for_test();
    int calls = 0;
    auto token = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++calls; });
    REQUIRE(token.valid());

    Environment::Token& alias = token;
    token = std::move(alias);
    REQUIRE(token.valid());

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

TEST_CASE("Environment: keyboard animation duration diffs separately",
          "[environment][issue-640]") {
    Environment::reset_for_test();
    EnvironmentChange last;
    EnvironmentState last_state;
    auto token = Environment::instance().subscribe(
        [&](const EnvironmentState& s, EnvironmentChange c) {
            last = c;
            last_state = s;
        });

    auto base = make_state(ColorScheme::dark, 2.0f, 128.0f);
    base.keyboard.animation_duration = 0.1f;
    Environment::inject_for_test(base);

    auto animated = base;
    animated.keyboard.animation_duration = 0.25f;
    Environment::inject_for_test(animated);
    REQUIRE(last.keyboard);
    REQUIRE_FALSE(last.display);
    REQUIRE_FALSE(last.color_scheme);
    REQUIRE(last_state.keyboard.bottom == 128.0f);
    REQUIRE(last_state.keyboard.animation_duration == 0.25f);
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

    auto left = bottom;
    left.safe_area.left = 12.0f;
    Environment::inject_for_test(left);
    REQUIRE(last.safe_area);
    REQUIRE_FALSE(last.display);

    auto right = left;
    right.safe_area.right = 18.0f;
    Environment::inject_for_test(right);
    REQUIRE(last.safe_area);
    REQUIRE_FALSE(last.keyboard);
}

TEST_CASE("Environment: display metadata changes are display-only",
          "[environment][issue-640]") {
    Environment::reset_for_test();
    EnvironmentChange last;
    EnvironmentState last_state;
    auto token = Environment::instance().subscribe(
        [&](const EnvironmentState& s, EnvironmentChange c) {
            last = c;
            last_state = s;
        });

    auto base = make_state(ColorScheme::dark);
    Environment::inject_for_test(base);

    auto next = base;
    next.display.physical_width = 3024;
    next.display.physical_height = 1964;
    next.display.refresh_hz = 120.0f;
    next.display.name = "Built-in Display";
    Environment::inject_for_test(next);

    REQUIRE(last.display);
    REQUIRE_FALSE(last.safe_area);
    REQUIRE_FALSE(last.keyboard);
    REQUIRE_FALSE(last.orientation);
    REQUIRE_FALSE(last.color_scheme);
    REQUIRE_FALSE(last.lifecycle);
    REQUIRE_FALSE(last.memory_pressure);
    REQUIRE(last_state.display.physical_width == 3024);
    REQUIRE(last_state.display.physical_height == 1964);
    REQUIRE(last_state.display.refresh_hz == 120.0f);
    REQUIRE(last_state.display.name == "Built-in Display");
}

TEST_CASE("Environment: orientation and lifecycle emit isolated diffs",
          "[environment][issue-640]") {
    Environment::reset_for_test();
    EnvironmentChange last;
    EnvironmentState last_state;
    auto token = Environment::instance().subscribe(
        [&](const EnvironmentState& s, EnvironmentChange c) {
            last = c;
            last_state = s;
        });

    auto base = make_state(ColorScheme::dark);
    Environment::inject_for_test(base);

    auto rotated = base;
    rotated.orientation = Orientation::landscape_right;
    Environment::inject_for_test(rotated);
    REQUIRE(last.orientation);
    REQUIRE_FALSE(last.display);
    REQUIRE_FALSE(last.lifecycle);
    REQUIRE(last_state.orientation == Orientation::landscape_right);

    auto inactive = rotated;
    inactive.lifecycle = LifecycleState::inactive;
    Environment::inject_for_test(inactive);
    REQUIRE(last.lifecycle);
    REQUIRE_FALSE(last.display);
    REQUIRE_FALSE(last.orientation);
    REQUIRE(last_state.lifecycle == LifecycleState::inactive);
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

// Note: this case exercises the listener contract (diff is emitted
// on normal-recovery) via inject_for_test, which bypasses the platform
// dispatch source. The platform-side mask that makes that recovery
// event actually fire on macOS is guarded by static_assert in
// core/platform/platform/mac/environment_mac.mm — see #466 follow-up
// for the layered coverage decision.
TEST_CASE("Environment: memory pressure recovery to normal fires diff",
          "[environment][issue-404]") {
    Environment::reset_for_test();
    EnvironmentChange last_change{};
    EnvironmentState  last_state{};
    auto token = Environment::instance().subscribe(
        [&](const EnvironmentState& s, EnvironmentChange c) {
            last_change = c;
            last_state  = s;
        });

    auto base = make_state(ColorScheme::dark);
    base.memory_pressure = MemoryPressure::critical;
    Environment::inject_for_test(base);

    last_change = {};
    auto recovered = base;
    recovered.memory_pressure = MemoryPressure::normal;
    Environment::inject_for_test(recovered);

    REQUIRE(last_change.memory_pressure);
    REQUIRE(last_state.memory_pressure == MemoryPressure::normal);
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

TEST_CASE("Environment: listener removed before its dispatch turn is skipped",
          "[environment][issue-640]") {
    Environment::reset_for_test();

    int victim_calls = 0;
    int survivor_calls = 0;
    Environment::Token victim;

    auto killer = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { victim.reset(); });
    victim = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++victim_calls; });
    auto survivor = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++survivor_calls; });

    Environment::inject_for_test(make_state(ColorScheme::dark));

    REQUIRE(killer.valid());
    REQUIRE_FALSE(victim.valid());
    REQUIRE(survivor.valid());
    REQUIRE(victim_calls == 0);
    REQUIRE(survivor_calls == 1);
}

TEST_CASE("Environment: reset during dispatch clears later callbacks",
          "[environment][issue-640]") {
    Environment::reset_for_test();

    int resetter_calls = 0;
    int later_calls = 0;
    auto resetter = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) {
            ++resetter_calls;
            Environment::reset_for_test();
        });
    auto later = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++later_calls; });

    Environment::inject_for_test(make_state(ColorScheme::dark));

    REQUIRE(resetter_calls == 1);
    REQUIRE(later_calls == 0);
    REQUIRE(resetter.valid());
    REQUIRE(later.valid());
    REQUIRE(Environment::instance().snapshot().color_scheme
            == ColorScheme::unknown);

    resetter.reset();
    later.reset();
    REQUIRE_FALSE(resetter.valid());
    REQUIRE_FALSE(later.valid());

    Environment::inject_for_test(make_state(ColorScheme::light));
    REQUIRE(resetter_calls == 1);
    REQUIRE(later_calls == 0);
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

TEST_CASE("Environment: publish without listeners still updates snapshot",
          "[environment][coverage][phase3]") {
    Environment::reset_for_test();
    auto state = make_state(ColorScheme::dark, 1.25f, 48.0f);
    state.lifecycle = LifecycleState::background;

    Environment::inject_for_test(state);

    auto got = Environment::instance().snapshot();
    REQUIRE(got.color_scheme == ColorScheme::dark);
    REQUIRE(got.display.scale == 1.25f);
    REQUIRE(got.keyboard.bottom == 48.0f);
    REQUIRE(got.lifecycle == LifecycleState::background);
}

TEST_CASE("Environment: multiple listeners receive the same change snapshot",
          "[environment][coverage][phase3]") {
    Environment::reset_for_test();
    int first_calls = 0;
    int second_calls = 0;
    EnvironmentChange first_change{};
    EnvironmentChange second_change{};
    EnvironmentState first_state{};
    EnvironmentState second_state{};

    auto first = Environment::instance().subscribe(
        [&](const EnvironmentState& s, EnvironmentChange c) {
            ++first_calls;
            first_state = s;
            first_change = c;
        });
    auto second = Environment::instance().subscribe(
        [&](const EnvironmentState& s, EnvironmentChange c) {
            ++second_calls;
            second_state = s;
            second_change = c;
        });

    auto state = make_state(ColorScheme::light, 2.5f);
    Environment::inject_for_test(state);

    REQUIRE(first_calls == 1);
    REQUIRE(second_calls == 1);
    REQUIRE(first_change.display);
    REQUIRE(second_change.display);
    REQUIRE(first_change.color_scheme);
    REQUIRE(second_change.color_scheme);
    REQUIRE(first_state.display.scale == 2.5f);
    REQUIRE(second_state.display.scale == 2.5f);
}

TEST_CASE("Environment: default token reset is inert",
          "[environment][coverage][phase3]") {
    Environment::reset_for_test();
    Environment::Token token;
    REQUIRE_FALSE(token.valid());

    token.reset();
    REQUIRE_FALSE(token.valid());

    Environment::inject_for_test(make_state(ColorScheme::dark));
    REQUIRE(Environment::instance().snapshot().color_scheme == ColorScheme::dark);
}

TEST_CASE("Environment: token self move assignment preserves subscription",
          "[environment][coverage][phase3]") {
    Environment::reset_for_test();
    int calls = 0;
    auto token = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++calls; });
    REQUIRE(token.valid());

    auto* same = &token;
    token = std::move(*same);
    REQUIRE(token.valid());

    Environment::inject_for_test(make_state(ColorScheme::dark));
    REQUIRE(calls == 1);
}

TEST_CASE("Environment: move assigning an empty token unsubscribes existing listener",
          "[environment][coverage][phase3]") {
    Environment::reset_for_test();
    int calls = 0;
    auto token = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) { ++calls; });
    REQUIRE(token.valid());

    Environment::Token empty;
    token = std::move(empty);
    REQUIRE_FALSE(token.valid());
    REQUIRE_FALSE(empty.valid());

    Environment::inject_for_test(make_state(ColorScheme::dark));
    REQUIRE(calls == 0);
}

TEST_CASE("Environment: listener subscribed during dispatch waits for next publish",
          "[environment][coverage][phase3]") {
    Environment::reset_for_test();
    int first_calls = 0;
    int late_calls = 0;
    std::unique_ptr<Environment::Token> late_token;

    auto first = Environment::instance().subscribe(
        [&](const EnvironmentState&, EnvironmentChange) {
            ++first_calls;
            if (!late_token) {
                late_token = std::make_unique<Environment::Token>(
                    Environment::instance().subscribe(
                        [&](const EnvironmentState&, EnvironmentChange) {
                            ++late_calls;
                        }));
            }
        });

    Environment::inject_for_test(make_state(ColorScheme::dark));
    REQUIRE(first_calls == 1);
    REQUIRE(late_calls == 0);
    REQUIRE(late_token);
    REQUIRE(late_token->valid());

    Environment::inject_for_test(make_state(ColorScheme::light));
    REQUIRE(first_calls == 2);
    REQUIRE(late_calls == 1);
}

TEST_CASE("Environment: reset restores defaults after published state",
          "[environment][coverage][phase3]") {
    Environment::reset_for_test();
    auto state = make_state(ColorScheme::dark, 3.0f, 240.0f);
    state.safe_area.bottom = 12.0f;
    Environment::inject_for_test(state);

    Environment::reset_for_test();

    auto got = Environment::instance().snapshot();
    REQUIRE(got.color_scheme == ColorScheme::unknown);
    REQUIRE(got.lifecycle == LifecycleState::unknown);
    REQUIRE(got.orientation == Orientation::unknown);
    REQUIRE(got.display.scale == 1.0f);
    REQUIRE(got.keyboard.bottom == 0.0f);
    REQUIRE(got.safe_area.is_zero());
}
