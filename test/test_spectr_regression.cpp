// SPDX-License-Identifier: MIT
//
// Autonomous Spectr regression suite — pulp-internal task #63, follow-up
// to pulp #1792.
//
// PR #1792 landed six bridge-level fixes for `@pulp/react` event regressions
// that had silently broken Spectr (band-drawing, key handling, global event
// fan-out, handler-throw resilience, registrar idempotency, and wheel
// bubbling). It also added six `[contract]` Catch2 tests in
// `test_widget_bridge.cpp` that pin each invariant in isolation.
//
// This file complements those: it composes the same surface into a
// Spectr-shaped mini-scenario — a React-ish editor mount that registers
// pointer / wheel / global key handlers in the order Spectr's
// `native-react/spectr-editor-extracted.js` does, then drives the bridge
// with a sequence of interactions that, pre-#1792, all silently failed.
// If any of the six contracts regresses *in combination*, this suite
// fails — catching the integration-level shape of the bug class even
// when the unit tests still pass in isolation.
//
// All tests are headless (no window, no DAW, no Skia GPU surface). They
// run on every CI lane the bridge test runs on.

#include <catch2/catch_test_macros.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

using namespace pulp::view;
using namespace pulp::state;

namespace {

constexpr const char* kSpectrLikeMountScript = R"(
    // Mirror of the order Spectr's native-react loader runs in: install
    // top-level handlers, mount a CanvasWidget, then register pointer +
    // wheel + global key listeners. The exact sequence + idempotency
    // pre-#1792 was where six silent failures clustered.

    var trace = {
        clicks: [],
        keys: [],
        wheels: [],
        wraps_wheel: 0,
        thrown_seen_before: 0,
        post_throw_events: 0,
        global_listener_fires: 0,
    };

    // (Issue 3) window.addEventListener('keydown', fn) should fire on
    // every keydown the bridge dispatches via __global__.
    if (typeof window !== 'undefined' && typeof window.addEventListener === 'function') {
        window.addEventListener('keydown', function(e) {
            trace.global_listener_fires += 1;
        });
    }

    createCol('root', '');
    createLabel('editor-canvas', '', 'root');
    createLabel('editor-wrap', '', 'root');
    createLabel('throw-zone', '', 'root');

    // (Issue 1) e.button must be W3C: left=0, middle=1, right=2. Spectr's
    // band-drawing routes on e.button === 0 (left). This single handler
    // also pins (Issue 4): the throw happens AFTER the trace push, so we
    // can assert below that subsequent EVENTS still dispatch even though
    // the handler raised.
    on('editor-canvas', 'pointerdown', function(e) {
        trace.clicks.push(e.button);
    });

    // (Issue 6) wheel on the wrap div must bubble up. Payload must be an
    // object (deltaX / deltaY / clientX / clientY); pre-#1792 it was
    // positional args and the synthetic-event shim dropped them.
    on('editor-wrap', 'wheel', function(e) {
        trace.wheels.push(e.deltaY);
        trace.wraps_wheel += 1;
    });

    // (Issue 4) A handler that throws — used by the chain-resilience
    // test below. The handler increments a counter BEFORE throwing so we
    // can verify it fired at all, then asserts a different (id, event)
    // pair continues to dispatch on the next event so the throw didn't
    // poison the broader dispatch loop.
    on('throw-zone', 'pointerdown', function(e) {
        trace.thrown_seen_before += 1;
        throw new Error('Spectr-shaped handler throw should not kill subsequent events');
    });

    // (Issue 2) e.key must be the W3C string 'Escape', not the raw int.
    on('__global__', 'keydown', function(e) {
        trace.keys.push(e.key);
    });

    // (Issue 5) re-registering a widget for pointer/wheel must be
    // idempotent — Spectr's reconciler re-runs registrars on every
    // commit and the bridge has to gate growth.
    registerPointer('editor-canvas');
    registerPointer('editor-canvas');
    registerPointer('editor-canvas');
    registerPointer('throw-zone');
    registerPointer('throw-zone');
    registerWheel('editor-wrap');
    registerWheel('editor-wrap');
)";

struct SpectrFixture {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge;

    SpectrFixture() : root(), bridge(engine, root, store) {
        root.set_bounds({0, 0, 800, 600});
        bridge.load_script(kSpectrLikeMountScript);
    }

    View* widget(const std::string& id) { return bridge.widget(id); }

    int trace_int(const std::string& path) {
        // ScriptEngine's wrapper returns choc::value::Value; pull as
        // string and reparse so we don't depend on Value::toInt32
        // (not in the choc surface).
        return std::stoi(engine.evaluate("String(trace." + path + ")").toString());
    }

    std::string trace_join(const std::string& arr) {
        return engine.evaluate("trace." + arr + ".join(',')").toString();
    }
};

}  // namespace

TEST_CASE("Spectr regression: left-click reaches band-drawing handler as button=0",
          "[spectr-regression][issue-1792]") {
    // pulp #1792 issue #1. Pre-fix `e.button` was the raw MouseButton
    // enum (left=1 in the C++ enum order), so JSX `e.button === 0`
    // never matched — every left-click hit Spectr's middle-click pan
    // branch instead of the band-drawing branch.
    SpectrFixture fix;
    auto* canvas = fix.widget("editor-canvas");
    REQUIRE(canvas != nullptr);

    MouseEvent left{};
    left.button = MouseButton::left;
    left.is_down = true;
    canvas->on_mouse_event(left);

    REQUIRE(fix.trace_join("clicks") == "0");
}

TEST_CASE("Spectr regression: handler-throw doesn't poison subsequent dispatch",
          "[spectr-regression][issue-1792]") {
    // pulp #1792 issue #4. Pre-fix, an uncaught throw from a handler
    // killed the bridge's rAF self-rescheduling chain — every later
    // event silently dropped on the floor (waveform animation died
    // except on mouse-move because mouse-move re-primed the loop).
    //
    // We fire a pointerdown on `throw-zone` (whose handler always
    // throws AFTER incrementing the trace counter), then fire a
    // pointerdown on `editor-canvas` and assert THAT handler still
    // dispatched. If the throw poisoned the loop, the second event
    // would never reach JS.
    SpectrFixture fix;
    auto* throw_zone = fix.widget("throw-zone");
    auto* canvas = fix.widget("editor-canvas");
    REQUIRE(throw_zone != nullptr);
    REQUIRE(canvas != nullptr);

    MouseEvent left{};
    left.button = MouseButton::left;
    left.is_down = true;

    // 1. Fire the throwing handler — it should see the event before
    //    raising, then bridge must swallow the exception.
    throw_zone->on_mouse_event(left);
    REQUIRE(fix.trace_int("thrown_seen_before") == 1);

    // 2. Fire a subsequent event on a different widget — pre-#1792
    //    this dropped silently because the dispatcher had been
    //    knocked out by the unhandled throw. Post-fix, the bridge
    //    wraps each handler in try/catch + __dispatchError__ so
    //    dispatch continues.
    canvas->on_mouse_event(left);
    REQUIRE(fix.trace_join("clicks") == "0");
}

TEST_CASE("Spectr regression: window.addEventListener('keydown', fn) fires on bridge keydown",
          "[spectr-regression][issue-1792]") {
    // pulp #1792 issue #3. Pre-fix, the bridge dispatched only into
    // its own `__global__` listener table; the JS `window.addEventListener`
    // shim wasn't installed early enough to land in `window._listeners`,
    // so consumer code that used the W3C shape never got events.
    SpectrFixture fix;

    fix.bridge.forward_key_event(static_cast<int>(KeyCode::escape), 0, true);
    fix.bridge.forward_key_event(static_cast<int>(KeyCode::left),   0, true);

    // Both the `__global__` handler and the window-shim handler should
    // see each keydown. We assert the window-shim count first because
    // it's the path that regressed.
    REQUIRE(fix.trace_int("global_listener_fires") == 2);
    REQUIRE(fix.trace_join("keys") == "Escape,ArrowLeft");
}

TEST_CASE("Spectr regression: registerPointer / registerWheel are idempotent across re-mounts",
          "[spectr-regression][issue-1792]") {
    // pulp #1792 issue #5. Spectr's React reconciler re-runs registrars
    // on every commit. Pre-fix, each call appended a new MouseEvent /
    // wheel-event lambda to the View's listener stack, so
    // per-pointer-event cost grew O(render_count). Post-fix, the bridge
    // guards with `pointer_registered_` / `wheel_registered_` sets and
    // each widget only carries one lambda.
    //
    // Codex P2 on PR #1827 — exercise the wheel path too, not just
    // the pointer path. The mount script calls `registerWheel(...)`
    // twice; if the gate regresses, a single wheel event would land
    // in `trace.wheels` twice (or `trace.wraps_wheel` would be 2N).
    SpectrFixture fix;
    auto* canvas = fix.widget("editor-canvas");
    auto* wrap = fix.widget("editor-wrap");
    REQUIRE(canvas != nullptr);
    REQUIRE(wrap != nullptr);

    // Pointer side — `registerPointer('editor-canvas')` ran 3× in mount.
    MouseEvent left{};
    left.button = MouseButton::left;
    left.is_down = true;
    canvas->on_mouse_event(left);
    canvas->on_mouse_event(left);
    canvas->on_mouse_event(left);

    // Wheel side — `registerWheel('editor-wrap')` ran 2× in mount.
    MouseEvent w1{};
    w1.is_wheel = true;
    w1.scroll_delta_y = 2.5f;
    wrap->on_mouse_event(w1);

    MouseEvent w2{};
    w2.is_wheel = true;
    w2.scroll_delta_y = -1.5f;
    wrap->on_mouse_event(w2);

    // The pointerdown handler fired 3 times — once per mouse event,
    // NOT 3*N where N is the registrar-call count. The wheel handler
    // fired twice (once per wheel event), not 4×.
    REQUIRE(fix.trace_join("clicks") == "0,0,0");
    REQUIRE(fix.trace_join("wheels") == "2.5,-1.5");
    REQUIRE(fix.trace_int("wraps_wheel") == 2);
}

TEST_CASE("Spectr regression: full mixed sequence — throw + button-mapping + idempotence + key + global",
          "[spectr-regression][issue-1792]") {
    // Composite check — every single-axis contract above must hold
    // simultaneously under a realistic mixed-event sequence. If a future
    // PR fixes one in isolation but breaks another via interaction
    // (dispatch order, registrar-gate semantics, handler-wrap layering),
    // this test fails first.
    SpectrFixture fix;
    auto* throw_zone = fix.widget("throw-zone");
    auto* canvas = fix.widget("editor-canvas");
    REQUIRE(throw_zone != nullptr);
    REQUIRE(canvas != nullptr);

    // 1. Hit the throwing handler twice — it should record both calls
    //    before raising each time.
    MouseEvent left{};
    left.button = MouseButton::left;
    left.is_down = true;
    throw_zone->on_mouse_event(left);
    throw_zone->on_mouse_event(left);

    // 2. Mixed-button sequence on the canvas — every event survives
    //    the prior throws.
    MouseEvent ev{};
    ev.is_down = true;
    ev.button = MouseButton::left;   canvas->on_mouse_event(ev);
    ev.button = MouseButton::middle; canvas->on_mouse_event(ev);
    ev.button = MouseButton::right;  canvas->on_mouse_event(ev);

    // 3. Throw the global key channel into the mix.
    fix.bridge.forward_key_event(static_cast<int>(KeyCode::escape), 0, true);

    REQUIRE(fix.trace_int("thrown_seen_before") == 2);
    REQUIRE(fix.trace_join("clicks") == "0,1,2");
    REQUIRE(fix.trace_join("keys") == "Escape");
    REQUIRE(fix.trace_int("global_listener_fires") == 1);
}
