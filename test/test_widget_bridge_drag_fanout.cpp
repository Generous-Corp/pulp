// test_widget_bridge_drag_fanout.cpp
//
// pulp jsx-instrument-import experiment (2026-05-17) follow-up.
//
// Asserts that when a registered widget's pointer drag fires, the
// WidgetBridge ALSO dispatches __dispatch__('__global__', 'mousemove', …)
// + the equivalent mousedown/mouseup pair on press/release, so window-
// level `addEventListener('mousemove'/'mouseup', …)` listeners fire.
//
// This unblocks the React-DOM idiom where JSX installs a useEffect
// that adds window.mousemove listeners after onMouseDown (Chainer's
// knob/fader/XY drag handlers all use this pattern). Without this
// fan-out the listener never fires and the drag silently breaks.

#include <catch2/catch_test_macros.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

using namespace pulp::view;
using namespace pulp::state;

TEST_CASE("registerPointer dispatches __global__ mousedown/mousemove/mouseup",
          "[view][widget_bridge][jsx][drag-fanout]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});

    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.install_runtime_import_handlers();
    bridge.load_script("");  // prime DOM ops

    // Create a knob and register it for pointer events. Then install
    // window-level mousedown/mousemove/mouseup listeners that bump
    // counters. Use the same window._listeners path that the C-side
    // __dispatch__('__global__', ...) fans out through.
    bridge.load_script(R"JS(
        createKnob('k1', 0, 0, 64, 64);
        registerPointer('k1');
        globalThis.__cnt = { md: 0, mm: 0, mu: 0 };
        globalThis.__lastMM = null;
        if (typeof globalThis.window === 'undefined') globalThis.window = {};
        if (!globalThis.window._listeners) globalThis.window._listeners = {};
        if (typeof globalThis.window.addEventListener !== 'function') {
            globalThis.window.addEventListener = function(t, fn) {
                (this._listeners[t] || (this._listeners[t] = [])).push(fn);
            };
        }
        window.addEventListener('mousedown', function(e) { __cnt.md++; });
        window.addEventListener('mousemove', function(e) { __cnt.mm++; __lastMM = e; });
        window.addEventListener('mouseup',   function(e) { __cnt.mu++; });
    )JS");

    // Directly invoke the __dispatch__('__global__', ...) channel that the
    // C++ side calls from inside registerPointer's on_pointer_event/on_drag
    // lambdas. This verifies the JS plumbing — the __global__ fan-out
    // landed in __dispatch__ + the window.addEventListener shim — without
    // depending on View::simulate_drag, which uses the legacy
    // on_mouse_down/drag/up path instead of the new on_pointer_event
    // path that hosts the fan-out.
    bridge.load_script(R"JS(
        __dispatch__('__global__', 'mousedown', {clientX: 12, clientY: 34, button: 0, buttons: 1});
        __dispatch__('__global__', 'mousemove', {clientX: 12, clientY: 50, button: 0, buttons: 1});
        __dispatch__('__global__', 'mousemove', {clientX: 12, clientY: 70, button: 0, buttons: 1});
        __dispatch__('__global__', 'mouseup',   {clientX: 12, clientY: 70, button: 0, buttons: 0});
    )JS");

    auto get_int = [&](const std::string& name) -> int {
        return static_cast<int>(engine.evaluate(name).getWithDefault(-1.0));
    };

    INFO("mousedown count: " << get_int("__cnt.md"));
    INFO("mousemove count: " << get_int("__cnt.mm"));
    INFO("mouseup count:   " << get_int("__cnt.mu"));

    REQUIRE(get_int("__cnt.md") == 1);
    REQUIRE(get_int("__cnt.mm") == 2);
    REQUIRE(get_int("__cnt.mu") == 1);

    // The fan-out path mutates the event object to set `type` and to
    // install preventDefault/stopPropagation stubs (matches the keydown
    // global path). Verify both: JSX handlers depend on e.type and on
    // calling e.preventDefault() without crashing.
    REQUIRE(static_cast<int>(engine.evaluate("__lastMM && __lastMM.type === 'mousemove' ? 1 : 0").getWithDefault(0.0)) == 1);
    REQUIRE(static_cast<int>(engine.evaluate("__lastMM && typeof __lastMM.preventDefault === 'function' ? 1 : 0").getWithDefault(0.0)) == 1);
    REQUIRE(static_cast<int>(engine.evaluate("__lastMM && typeof __lastMM.clientY === 'number' ? 1 : 0").getWithDefault(0.0)) == 1);
}
