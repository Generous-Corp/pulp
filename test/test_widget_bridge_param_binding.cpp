// Declarative native→widget param/meter bindings.
//
// bindWidgetToParam / bindMeter register a binding ONCE from JS; thereafter
// C++ pushes the atomic param-store value onto the widget every frame with no
// per-frame JS crossing (WidgetBridge::service_param_bindings, driven from
// service_frame_callbacks on the host FrameClock). These headless tests drive
// the store + the service pump directly and assert the widget tracks the
// source, that a transform is applied, that an active drag gesture wins over
// the binding, and that unbind stops the push.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/state/store.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

namespace {

// Populate a store with a normalized [0,1] "gain" param and a dB-ranged
// "level" param. StateStore is non-copyable, so callers own the instance.
void add_params(StateStore& store) {
    store.add_parameter({.id = 1, .name = "gain", .unit = "", .range = {.min = 0.0f, .max = 1.0f}});
    store.add_parameter({.id = 2, .name = "level", .unit = "dB", .range = {.min = -60.0f, .max = 0.0f}});
}

} // namespace

TEST_CASE("bindWidgetToParam pushes the store value to a knob each frame",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('gain-knob');
        bindWidgetToParam('gain-knob', 'gain');
    )");

    REQUIRE(bridge.param_binding_count() == 1);
    auto* knob = dynamic_cast<Knob*>(bridge.widget("gain-knob"));
    REQUIRE(knob != nullptr);

    store.set_normalized(1, 0.25f);
    bridge.service_param_bindings();
    REQUIRE_THAT(knob->value(), WithinAbs(0.25f, 1e-5f));

    // A later store change is tracked with no further JS involvement.
    store.set_normalized(1, 0.80f);
    bridge.service_param_bindings();
    REQUIRE_THAT(knob->value(), WithinAbs(0.80f, 1e-5f));
}

TEST_CASE("service_frame_callbacks drives bindings with no rAF registered",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    // No requestAnimationFrame anywhere — the whole point is that a metering /
    // param-following UI needs no per-frame JS callback.
    bridge.load_script(R"(
        createFader('vol-fader');
        bindWidgetToParam('vol-fader', 'gain');
    )");
    auto* fader = dynamic_cast<Fader*>(bridge.widget("vol-fader"));
    REQUIRE(fader != nullptr);

    store.set_normalized(1, 0.6f);
    bridge.service_frame_callbacks();  // the real host per-vsync pump
    REQUIRE_THAT(fader->value(), WithinAbs(0.6f, 1e-5f));
}

TEST_CASE("bindMeter drives a Meter from a param with no per-frame JS",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createMeter('out-meter');
        bindMeter('out-meter', 'gain');
    )");
    auto* meter = dynamic_cast<Meter*>(bridge.widget("out-meter"));
    REQUIRE(meter != nullptr);

    store.set_normalized(1, 0.5f);
    bridge.service_param_bindings();
    REQUIRE_THAT(meter->display_rms(), WithinAbs(0.5f, 1e-5f));
}

TEST_CASE("binding transform: scale/offset and dB mapping are applied",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('half-knob');
        bindWidgetToParam('half-knob', 'gain', { scale: 0.5 });
        createMeter('db-meter');
        bindMeter('db-meter', 'level', { db: true, dbMin: -60, dbMax: 0 });
    )");

    auto* knob = dynamic_cast<Knob*>(bridge.widget("half-knob"));
    auto* meter = dynamic_cast<Meter*>(bridge.widget("db-meter"));
    REQUIRE(knob != nullptr);
    REQUIRE(meter != nullptr);

    store.set_normalized(1, 1.0f);          // gain = 1.0 → knob = 1.0 * 0.5
    store.set_value(2, -30.0f);             // level = -30 dB → (−30+60)/60 = 0.5
    bridge.service_param_bindings();

    REQUIRE_THAT(knob->value(), WithinAbs(0.5f, 1e-5f));
    REQUIRE_THAT(meter->display_rms(), WithinAbs(0.5f, 1e-5f));
}

TEST_CASE("binding transform: clamp bounds the result",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('clamped');
        bindWidgetToParam('clamped', 'gain', { scale: 4.0, max: 0.75 });
    )");
    auto* knob = dynamic_cast<Knob*>(bridge.widget("clamped"));
    REQUIRE(knob != nullptr);

    store.set_normalized(1, 0.5f);          // 0.5 * 4 = 2.0, clamped to 0.75
    bridge.service_param_bindings();
    REQUIRE_THAT(knob->value(), WithinAbs(0.75f, 1e-5f));
}

TEST_CASE("precedence: an active drag gesture wins over the binding",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('gain-knob');
        bindWidgetToParam('gain-knob', 'gain');
    )");
    auto* knob = dynamic_cast<Knob*>(bridge.widget("gain-knob"));
    REQUIRE(knob != nullptr);
    knob->set_bounds({0, 0, 48, 48});

    store.set_normalized(1, 0.20f);
    bridge.service_param_bindings();
    REQUIRE_THAT(knob->value(), WithinAbs(0.20f, 1e-5f));

    // Begin a drag: the knob now reports an active gesture.
    knob->on_mouse_down({24, 24});
    REQUIRE(knob->is_gesture_active());
    const float during_drag = knob->value();

    // The store moves while the user drags — the binding must NOT overwrite the
    // widget mid-gesture.
    store.set_normalized(1, 0.90f);
    bridge.service_param_bindings();
    REQUIRE_THAT(knob->value(), WithinAbs(during_drag, 1e-5f));

    // End the drag: the binding re-asserts the current store value.
    knob->on_mouse_up({24, 24});
    REQUIRE_FALSE(knob->is_gesture_active());
    bridge.service_param_bindings();
    REQUIRE_THAT(knob->value(), WithinAbs(0.90f, 1e-5f));
}

TEST_CASE("precedence: binding re-asserts over a stray direct set on source change",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('gain-knob');
        bindWidgetToParam('gain-knob', 'gain');
    )");
    auto* knob = dynamic_cast<Knob*>(bridge.widget("gain-knob"));
    REQUIRE(knob != nullptr);

    store.set_normalized(1, 0.30f);
    bridge.service_param_bindings();
    REQUIRE_THAT(knob->value(), WithinAbs(0.30f, 1e-5f));

    // A stray direct set (not a gesture) is overridden when the source moves.
    knob->set_value(0.95f);
    store.set_normalized(1, 0.40f);
    bridge.service_param_bindings();
    REQUIRE_THAT(knob->value(), WithinAbs(0.40f, 1e-5f));
}

TEST_CASE("unbindWidget stops the native push; rebinding replaces the source",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('gain-knob');
        bindWidgetToParam('gain-knob', 'gain');
    )");
    auto* knob = dynamic_cast<Knob*>(bridge.widget("gain-knob"));
    REQUIRE(knob != nullptr);

    store.set_normalized(1, 0.50f);
    bridge.service_param_bindings();
    REQUIRE_THAT(knob->value(), WithinAbs(0.50f, 1e-5f));

    const auto removed = engine.evaluate("unbindWidget('gain-knob')").getWithDefault<int64_t>(-1);
    REQUIRE(removed == 1);
    REQUIRE(bridge.param_binding_count() == 0);

    // The knob no longer tracks the param after unbinding.
    store.set_normalized(1, 0.10f);
    bridge.service_param_bindings();
    REQUIRE_THAT(knob->value(), WithinAbs(0.50f, 1e-5f));

    // Re-binding a widget replaces (not stacks) the binding.
    engine.evaluate("bindWidgetToParam('gain-knob', 'gain')");
    engine.evaluate("bindWidgetToParam('gain-knob', 'gain')");
    REQUIRE(bridge.param_binding_count() == 1);
}

TEST_CASE("bindWidgetToParam maps a ranged RangeSlider across its full travel",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createRangeSlider('freq');");
    auto* slider = dynamic_cast<RangeSlider*>(bridge.widget("freq"));
    REQUIRE(slider != nullptr);
    slider->set_min(20.0f);
    slider->set_max(20000.0f);

    engine.evaluate("bindWidgetToParam('freq', 'gain')");
    store.set_normalized(1, 0.5f);          // 0.5 fraction → 20 + 0.5*(20000-20)
    bridge.service_param_bindings();
    REQUIRE_THAT(slider->value(), WithinAbs(10010.0f, 0.5f));

    store.set_normalized(1, 0.0f);
    bridge.service_param_bindings();
    REQUIRE_THAT(slider->value(), WithinAbs(20.0f, 0.5f));
}

TEST_CASE("a binding registered before its widget exists resolves later",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    // Bind first — the param exists, the widget does not yet.
    const bool ok = engine.evaluate("bindWidgetToParam('deferred', 'gain')")
                        .getWithDefault<bool>(false);
    REQUIRE(ok);
    REQUIRE(bridge.param_binding_count() == 1);

    store.set_normalized(1, 0.7f);
    bridge.service_param_bindings();     // inert: widget absent

    engine.evaluate("createKnob('deferred')");
    auto* knob = dynamic_cast<Knob*>(bridge.widget("deferred"));
    REQUIRE(knob != nullptr);
    bridge.service_param_bindings();
    REQUIRE_THAT(knob->value(), WithinAbs(0.7f, 1e-5f));
}

TEST_CASE("removeWidget drops the binding — a reused id is not resurrected",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('k1');
        bindWidgetToParam('k1', 'gain');
    )");
    store.set_normalized(1, 0.7f);
    bridge.service_param_bindings();
    REQUIRE(bridge.param_binding_count() == 1);

    engine.evaluate("removeWidget('k1')");
    REQUIRE(bridge.param_binding_count() == 0);

    // A new widget reusing the id must NOT inherit the old binding.
    engine.evaluate("createKnob('k1')");
    auto* knob = dynamic_cast<Knob*>(bridge.widget("k1"));
    REQUIRE(knob != nullptr);
    store.set_normalized(1, 0.9f);
    bridge.service_param_bindings();
    REQUIRE_THAT(knob->value(), WithinAbs(0.0f, 1e-5f));  // default, not resurrected
}

TEST_CASE("precedence: a value widget is re-asserted even when the source is static",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('gain-knob');
        bindWidgetToParam('gain-knob', 'gain');
    )");
    auto* knob = dynamic_cast<Knob*>(bridge.widget("gain-knob"));
    REQUIRE(knob != nullptr);

    store.set_normalized(1, 0.50f);
    bridge.service_param_bindings();
    REQUIRE_THAT(knob->value(), WithinAbs(0.50f, 1e-5f));

    // Stray direct set with NO source change is corrected on the next frame.
    knob->set_value(0.20f);
    bridge.service_param_bindings();
    REQUIRE_THAT(knob->value(), WithinAbs(0.50f, 1e-5f));
}

TEST_CASE("clear() drops bindings so a hot reload doesn't leak stale ones",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('gain-knob');
        bindWidgetToParam('gain-knob', 'gain');
    )");
    REQUIRE(bridge.param_binding_count() == 1);

    bridge.clear();
    REQUIRE(bridge.param_binding_count() == 0);
    // Servicing after teardown is inert (the widget is gone).
    store.set_normalized(1, 0.9f);
    bridge.service_param_bindings();
    REQUIRE(bridge.widget("gain-knob") == nullptr);
}

TEST_CASE("binding an unknown param is a no-op that returns false",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain-knob');");
    const bool ok = engine.evaluate("bindWidgetToParam('gain-knob', 'no-such-param')")
                        .getWithDefault<bool>(true);
    REQUIRE_FALSE(ok);
    REQUIRE(bridge.param_binding_count() == 0);
}

// ── Custom-drawn controls: the canvas owns its own value ────────────────────
//
// A scripted UI that paints its own knobs into a canvas does not use a Knob
// widget at all: it records draw commands and drives the param from its own
// pointer handlers. Nothing pinned that a canvas is reachable by a drag, so the
// whole idiom could break — a canvas losing its pointer wiring, or the
// simulator diverging from the host dispatch — with every test still green.

namespace {

// The generated shape: a canvas, a drag that maps vertical travel to [0,1], and
// setParam on every move. Deliberately verbatim in structure (local drag state
// captured on press, clientY delta over a fixed pixel span) so a regression in
// any hop — hit_test, registerPointer, on_drag, __dispatch__, setParam — fails
// here rather than in a DAW.
constexpr const char* kCanvasKnobScript = R"JS(
    var cvs = createCanvas('gain-canvas-knob', '');
    var st = { val: getParam('gain'), drag: false, y0: 0, v0: 0 };
    on(cvs, 'pointerdown', function (e) {
        st.drag = true; st.y0 = e.clientY; st.v0 = st.val;
    });
    on(cvs, 'pointermove', function (e) {
        if (!st.drag) return;
        st.val = Math.max(0, Math.min(1, st.v0 + (st.y0 - e.clientY) / 150));
        setParam('gain', st.val);
        canvasClear(cvs);
        canvasFillRect(cvs, 0, 84 - st.val * 84, 84, st.val * 84, '#4af');
    });
    on(cvs, 'pointerup', function () { st.drag = false; });
)JS";

} // namespace

TEST_CASE("a custom-drawn canvas knob drives its param through a drag",
          "[view][bridge][state-binding][pointer]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);
    store.set_normalized(1, 0.5f);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(kCanvasKnobScript);

    auto* canvas = dynamic_cast<CanvasWidget*>(bridge.widget("gain-canvas-knob"));
    REQUIRE(canvas != nullptr);
    // The host's layout pass places the canvas in a real editor; place it here.
    canvas->set_bounds({20, 20, 84, 84});
    // A canvas is hit-testable with no opt-in, and `on(...)` wired the pointer
    // channels. Both are preconditions for the drag below — assert them so a
    // failure names which half broke.
    REQUIRE(root.hit_test({62, 62}) == canvas);
    REQUIRE(static_cast<bool>(canvas->on_pointer_event));
    REQUIRE(static_cast<bool>(canvas->on_drag));

    // Drag upward by 50px inside the canvas: +50/150 = +0.3333 over the 0.5 the
    // press latched.
    root.simulate_drag({62, 90}, {62, 40}, 5);

    REQUIRE_THAT(store.get_normalized(1), WithinAbs(0.8333f, 1e-3f));
    // The handler repainted as it went, so the canvas carries its draw commands.
    REQUIRE(canvas->command_count() > 0);
    // Release ended the gesture: a later move with no button must not move the
    // param.
    canvas->on_drag({40, 0});
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(0.8333f, 1e-3f));
}

TEST_CASE("binding a canvas to a param returns false instead of silently no-op'ing",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createCanvas('gain-canvas', '');");
    // A canvas is not a value widget: the per-frame push has no way to write it,
    // so accepting the bind would hand the caller a truthy "bound" for a push
    // that can never happen.
    const bool value_ok = engine.evaluate("bindWidgetToParam('gain-canvas', 'gain')")
                              .getWithDefault<bool>(true);
    REQUIRE_FALSE(value_ok);
    const bool meter_ok = engine.evaluate("bindMeter('gain-canvas', 'gain')")
                              .getWithDefault<bool>(true);
    REQUIRE_FALSE(meter_ok);
    REQUIRE(bridge.param_binding_count() == 0);

    // A real value widget still binds — the rejection is type-scoped, not a
    // blanket tightening.
    bridge.load_script("createKnob('gain-knob');");
    REQUIRE(engine.evaluate("bindWidgetToParam('gain-knob', 'gain')")
                .getWithDefault<bool>(false));
}
