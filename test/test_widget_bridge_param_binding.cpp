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
#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <cstddef>
#include <memory>
#include <thread>
#include <chrono>
#include <pulp/view/value_channel_set.hpp>
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

TEST_CASE("bound Stepper host refresh is silent while user edits still notify",
          "[view][bridge][state-binding][stepper][gesture]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    add_params(store);
    int begins = 0;
    int ends = 0;
    store.set_gesture_callbacks(
        [&](ParamID) { ++begins; },
        [&](ParamID) { ++ends; });
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        globalThis.stepperEvents = 0;
        createStepper('voices', '');
        setMin('voices', 0);
        setMax('voices', 10);
        setStep('voices', 1);
        on('voices', 'change', function() { ++globalThis.stepperEvents; });
        bindWidgetToParam('voices', 'gain');
    )");
    auto* stepper = dynamic_cast<Stepper*>(bridge.widget("voices"));
    REQUIRE(stepper != nullptr);

    store.set_normalized(1, 0.7f);
    bridge.service_param_bindings();
    CHECK_THAT(stepper->value(), WithinAbs(7.0, 1e-9));
    CHECK(engine.evaluate("globalThis.stepperEvents")
              .getWithDefault<int32_t>(-1) == 0);
    CHECK(begins == 0);
    CHECK(ends == 0);
    CHECK(store.open_gesture_count() == 0);

    stepper->set_value(8.0);
    CHECK(engine.evaluate("globalThis.stepperEvents")
              .getWithDefault<int32_t>(-1) == 1);
    CHECK(begins == 1);
    CHECK(ends == 1);
    CHECK(store.open_gesture_count() == 0);
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

TEST_CASE("declarative bindings reject widgets the native push cannot write",
          "[view][bridge][state-binding]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createCanvas('custom-face');");
    const bool value_ok =
        engine.evaluate("bindWidgetToParam('custom-face', 'gain')")
            .getWithDefault<bool>(true);
    const bool meter_ok =
        engine.evaluate("bindMeter('custom-face', 'gain')")
            .getWithDefault<bool>(true);

    REQUIRE_FALSE(value_ok);
    REQUIRE_FALSE(meter_ok);
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
    REQUIRE(static_cast<bool>(canvas->on_dom_pointer_event));
    REQUIRE(static_cast<bool>(canvas->on_dom_pointer_move_event));

    // Drag upward by 50px inside the canvas: +50/150 = +0.3333 over the 0.5 the
    // press latched.
    root.simulate_drag({62, 90}, {62, 40}, 5);

    REQUIRE_THAT(store.get_normalized(1), WithinAbs(0.8333f, 1e-3f));
    // The handler repainted as it went, so the canvas carries its draw commands.
    REQUIRE(canvas->command_count() > 0);
    // Release ended the gesture: a later move with no button must not move the
    // param.
    MouseEvent late_move{};
    late_move.position = {40, 0};
    late_move.window_position = {60, 20};
    late_move.phase = MousePhase::hover;
    canvas->on_dom_pointer_move_event(late_move, true);
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(0.8333f, 1e-3f));
}

// ── Binding-attempt diagnostics ──────────────────────────────────────────────
//
// bindWidgetToParam / bindMeter return a bool that no generated UI script
// checks, so every failure below used to render a control that looks correct
// and does nothing. The bridge records why each attempt did or did not bind.

namespace {

using Outcome = BindingOutcome;

// Outcome of the single attempt a script made, for tests that make exactly one.
Outcome only_outcome(const WidgetBridge& bridge) {
    REQUIRE(bridge.binding_attempts().size() == 1);
    return bridge.binding_attempts().front().outcome;
}

} // namespace

TEST_CASE("a successful binding is recorded as bound",
          "[view][bridge][state-binding][diagnostics]") {
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

    // POSITIVE CONTROL. Every failure case below is only meaningful if the
    // recorder can report success — a recorder that reported "broken" for
    // everything would pass them all.
    REQUIRE(only_outcome(bridge) == Outcome::ok);
    CHECK(is_bound(Outcome::ok));
    CHECK(bridge.binding_attempts().front().widget_id == "gain-knob");
    CHECK(bridge.binding_attempts().front().param_name == "gain");
    CHECK(bridge.param_binding_count() == 1);
}

TEST_CASE("binding to an undeclared param is recorded, not silently dropped",
          "[view][bridge][state-binding][diagnostics]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    // 'gian' is a typo for 'gain'. Today the knob renders and does nothing.
    bridge.load_script(R"(
        createKnob('gain-knob');
        bindWidgetToParam('gain-knob', 'gian');
    )");

    CHECK(only_outcome(bridge) == Outcome::unknown_param);
    CHECK_FALSE(is_bound(Outcome::unknown_param));
    CHECK(bridge.param_binding_count() == 0);
}

TEST_CASE("binding a target the widget cannot accept is recorded",
          "[view][bridge][state-binding][diagnostics]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    // bindMeter needs a Meter; a Knob has no level surface for the frame
    // service to write, so the push would silently no-op.
    bridge.load_script(R"(
        createKnob('gain-knob');
        bindMeter('gain-knob', 'gain');
    )");

    CHECK(only_outcome(bridge) == Outcome::incompatible_widget);
    CHECK(bridge.param_binding_count() == 0);
}

TEST_CASE("empty widget id and empty param name are distinguished",
          "[view][bridge][state-binding][diagnostics]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('gain-knob');
        bindWidgetToParam('', 'gain');
        bindWidgetToParam('gain-knob', '');
    )");

    REQUIRE(bridge.binding_attempts().size() == 2);
    CHECK(bridge.binding_attempts()[0].outcome == Outcome::empty_widget_id);
    CHECK(bridge.binding_attempts()[1].outcome == Outcome::empty_param_name);
    CHECK(bridge.param_binding_count() == 0);
}

TEST_CASE("binding before the widget exists is a success, not a failure",
          "[view][bridge][state-binding][diagnostics]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    // Scripts may deliberately bind before creating the view; a diagnostic that
    // called this broken would cry wolf on correct code.
    bridge.load_script("bindWidgetToParam('later', 'gain');");

    CHECK(only_outcome(bridge) == Outcome::deferred_widget_missing);
    CHECK(is_bound(Outcome::deferred_widget_missing));
    CHECK(bridge.param_binding_count() == 1);
}

TEST_CASE("rebinding a widget records that it displaced the prior binding",
          "[view][bridge][state-binding][diagnostics]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    // A widget has one source; the second bind wins silently today.
    bridge.load_script(R"(
        createKnob('k');
        bindWidgetToParam('k', 'gain');
        bindWidgetToParam('k', 'level');
    )");

    REQUIRE(bridge.binding_attempts().size() == 2);
    CHECK(bridge.binding_attempts()[0].outcome == Outcome::ok);
    CHECK(bridge.binding_attempts()[1].outcome == Outcome::replaced_prior_binding);
    CHECK(bridge.param_binding_count() == 1);
}

TEST_CASE("unbound_params names declared params the UI cannot reach",
          "[view][bridge][state-binding][diagnostics]") {
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

    // 'gain' is reachable; 'level' is declared and has no control.
    const auto unbound = bridge.unbound_params();
    REQUIRE(unbound.size() == 1);
    CHECK(unbound.front() == "level");
}

TEST_CASE("unbound_params ignores host-owned parameters",
          "[view][bridge][state-binding][diagnostics]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    store.add_parameter({.id = 1, .name = "gain", .unit = "", .range = {.min = 0.0f, .max = 1.0f}});
    store.add_parameter({.id = 2, .name = "Bypass", .unit = "",
                         .range = {.min = 0.0f, .max = 1.0f},
                         .designation = ParamDesignation::Bypass});
    store.add_parameter({.id = 3, .name = "Panic", .unit = "",
                         .range = {.min = 0.0f, .max = 1.0f},
                         .is_trigger = true});
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createKnob('gain-knob');
        bindWidgetToParam('gain-knob', 'gain');
    )");

    // The host surfaces bypass and triggers itself, so a plugin that omits them
    // from its own UI is not incomplete.
    CHECK(bridge.unbound_params().empty());
}

TEST_CASE("a fully bound UI reports nothing unreachable",
          "[view][bridge][state-binding][diagnostics]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);
    WidgetBridge bridge(engine, root, store);

    // NEGATIVE CONTROL for unbound_params: a correct UI must come back clean,
    // or the check is noise a developer learns to ignore.
    bridge.load_script(R"(
        createKnob('gain-knob');
        createFader('level-fader', 'vertical');
        bindWidgetToParam('gain-knob', 'gain');
        bindWidgetToParam('level-fader', 'level');
    )");

    for (const auto& attempt : bridge.binding_attempts()) {
        INFO(attempt.widget_id << " -> " << attempt.param_name << ": "
                               << describe(attempt.outcome));
        CHECK(is_bound(attempt.outcome));
    }
    CHECK(bridge.unbound_params().empty());
}

// ── Parameter metadata reaches JavaScript ────────────────────────────────────
//
// Before these, a scripted UI got a normalized float and a name. Everything a
// control actually needs — the real range, the unit, the curve, the default —
// had to be retyped in JS, where it silently drifts from define_parameters().
// These read the same pulp::state::param_json payload the inspector uses.

namespace {

// Evaluate `expr` in the bridge's engine and return it as JSON text, so a test
// asserts on what a UI script would actually observe.
std::string eval_json(ScriptEngine& engine, const std::string& expr) {
    return engine.evaluate("JSON.stringify(" + expr + ")").toString();
}

void add_rich_params(StateStore& store) {
    ParamInfo cutoff{};
    cutoff.id = 10;
    cutoff.name = "Cutoff";
    cutoff.unit = "Hz";
    cutoff.range = {.min = 20.0f, .max = 20000.0f, .default_value = 1000.0f};
    store.add_parameter(cutoff);

    ParamInfo mode{};
    mode.id = 11;
    mode.name = "Mode";
    mode.kind = ParamKind::Enum;
    mode.value_labels = {"Low", "Band", "High"};
    mode.range = {.min = 0.0f, .max = 2.0f, .default_value = 0.0f, .step = 1.0f};
    store.add_parameter(mode);
}

} // namespace

TEST_CASE("getParamMetadata gives a script the real range, unit and default",
          "[view][bridge][state-binding][param-metadata]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("");

    const auto json = eval_json(engine, "getParamMetadata('Cutoff')");
    INFO("payload: " << json);
    CHECK(json.find("\"min\":20") != std::string::npos);
    CHECK(json.find("\"max\":20000") != std::string::npos);
    CHECK(json.find("\"default\":1000") != std::string::npos);
    CHECK(json.find("\"unit\":\"Hz\"") != std::string::npos);

    // An enum carries the author's labels, so a script can build a real picker
    // instead of a numeric slider.
    const auto mode = eval_json(engine, "getParamMetadata('Mode')");
    INFO("payload: " << mode);
    CHECK(mode.find("\"kind\":\"enum\"") != std::string::npos);
    CHECK(mode.find("\"Band\"") != std::string::npos);

    // An unknown name is undefined, not an empty object — a script must be able
    // to tell "no such parameter" from "a parameter with nothing in it".
    CHECK(eval_json(engine, "getParamMetadata('nope') === undefined") == "true");
}

TEST_CASE("formatParamValue matches what the host displays",
          "[view][bridge][state-binding][param-metadata]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("");

    CHECK(engine.evaluate("formatParamValue('Cutoff', 440)").toString() == "440 Hz");
    // The normalized flag denormalizes first, so a widget holding 0..1 can ask
    // for display text without doing range maths in JS.
    CHECK(engine.evaluate("formatParamValue('Cutoff', 0, true)").toString() == "20 Hz");
    // An enum reads as its label, not its index.
    CHECK(engine.evaluate("formatParamValue('Mode', 1)").toString() == "Band");
    CHECK(eval_json(engine, "formatParamValue('nope', 1) === undefined") == "true");
}

TEST_CASE("parseParamValue reports failure instead of yielding a silent zero",
          "[view][bridge][state-binding][param-metadata]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("");

    CHECK(eval_json(engine, "parseParamValue('Cutoff','440').ok") == "true");
    CHECK(eval_json(engine, "parseParamValue('Cutoff','440').value") == "440");
    CHECK(eval_json(engine, "parseParamValue('Cutoff','440 Hz').value") == "440");

    // THE point of the ok flag: a click-to-type field must not store 0 because
    // the user typed nonsense.
    CHECK(eval_json(engine, "parseParamValue('Cutoff','banana').ok") == "false");
    CHECK(eval_json(engine, "parseParamValue('Cutoff','').ok") == "false");

    // A label round-trips for an enum.
    CHECK(eval_json(engine, "parseParamValue('Mode','High').ok") == "true");
    CHECK(eval_json(engine, "parseParamValue('Mode','High').value") == "2");

    CHECK(eval_json(engine, "parseParamValue('nope','1') === undefined") == "true");
}

TEST_CASE("format and parse round-trip through the bridge",
          "[view][bridge][state-binding][param-metadata]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("");

    // The pair only earns its place if a UI can render a value, let the user
    // edit the text, and read the same number back.
    const auto ok = eval_json(engine,
        "(function(){"
        "  var vals=[20,440,1000,20000];"
        "  for (var i=0;i<vals.length;i++){"
        "    var t=formatParamValue('Cutoff', vals[i]);"
        "    var r=parseParamValue('Cutoff', t);"
        "    if(!r.ok || Math.abs(r.value-vals[i]) > Math.max(1, vals[i]*0.01)) return false;"
        "  }"
        "  return true;"
        "})()");
    CHECK(ok == "true");
}

// ── onParamChanged / offParamChanged ──────────────────────────────────
//
// Push notification of param movement, delivered async on the frame tick.
// The contract these pin: subscribing is not itself a change, delivery is
// coalesced to one callback per subscription per frame, it is origin-blind
// (a host write and this UI's own setParam look identical), and a handler
// that writes its own param terminates instead of recursing.

namespace {

// Drive one host frame. service_frame_callbacks is what the FrameClock calls,
// so a test that pumps this is exercising the real delivery path rather than
// reaching into service_param_subscriptions directly.
void tick(WidgetBridge& bridge) { bridge.service_frame_callbacks(); }

// Subscribe and record every payload into a global array the test reads back.
constexpr const char* kRecorder = R"(
    var seen = [];
    var sub = onParamChanged('Cutoff', function (p) { seen.push(p); });
)";

} // namespace

TEST_CASE("onParamChanged returns a handle, and 0 for an unknown param",
          "[view][bridge][state-binding][param-change]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("");

    CHECK(eval_json(engine, "onParamChanged('Cutoff', function () {}) > 0") == "true");
    CHECK(eval_json(engine, "onParamChanged('nope', function () {})") == "0");
    CHECK(eval_json(engine, "onParamChanged('', function () {})") == "0");
    // Only the real param produced a subscription.
    CHECK(bridge.param_subscription_count() == 1);
}

TEST_CASE("subscribing does not itself fire a change",
          "[view][bridge][state-binding][param-change]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(kRecorder);

    // Several frames with no store write must stay silent — otherwise every
    // subscriber would have to filter a synthetic change that never happened.
    tick(bridge);
    tick(bridge);
    CHECK(eval_json(engine, "seen.length") == "0");
}

TEST_CASE("a store write is delivered on the next frame with the full payload",
          "[view][bridge][state-binding][param-change]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(kRecorder);

    store.set_value(10, 5000.0f);
    // Nothing is delivered until the frame tick — that is what keeps JS off
    // the writer's thread.
    CHECK(eval_json(engine, "seen.length") == "0");

    tick(bridge);
    CHECK(eval_json(engine, "seen.length") == "1");

    const auto payload = eval_json(engine, "seen[0]");
    INFO("payload: " << payload);
    CHECK(payload.find("\"name\":\"Cutoff\"") != std::string::npos);
    CHECK(payload.find("\"value\":5000") != std::string::npos);
    // normalized and modulated ride along so JS never re-derives skew math.
    CHECK(payload.find("\"normalized\":") != std::string::npos);
    CHECK(payload.find("\"modulated\":") != std::string::npos);
}

TEST_CASE("many writes between frames coalesce into one callback",
          "[view][bridge][state-binding][param-change]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(kRecorder);

    // An automation sweep: 100 writes inside one frame. A push-per-write
    // design would cross into JS 100 times.
    for (int i = 1; i <= 100; ++i) store.set_value(10, 1000.0f + static_cast<float>(i));
    tick(bridge);

    CHECK(eval_json(engine, "seen.length") == "1");
    // The one callback reports the value getParam would have returned.
    CHECK(eval_json(engine, "seen[0].value") == "1100");

    // A frame with no further write stays silent.
    tick(bridge);
    CHECK(eval_json(engine, "seen.length") == "1");
}

TEST_CASE("delivery is origin-blind: a JS setParam reports like a host write",
          "[view][bridge][state-binding][param-change]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(kRecorder);

    // The UI writes its own param. A design that suppressed self-writes would
    // silently break a UI whose source of truth is the store.
    engine.evaluate("setParam('Cutoff', 0.5)");
    tick(bridge);
    CHECK(eval_json(engine, "seen.length") == "1");

    // ...and a native/host-side write is reported the same way.
    store.set_value(10, 9000.0f);
    tick(bridge);
    CHECK(eval_json(engine, "seen.length") == "2");
}

TEST_CASE("offParamChanged stops delivery and reports whether it removed one",
          "[view][bridge][state-binding][param-change]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(kRecorder);

    store.set_value(10, 5000.0f);
    tick(bridge);
    REQUIRE(eval_json(engine, "seen.length") == "1");

    CHECK(eval_json(engine, "offParamChanged(sub)") == "true");
    CHECK(bridge.param_subscription_count() == 0);

    store.set_value(10, 6000.0f);
    tick(bridge);
    CHECK(eval_json(engine, "seen.length") == "1");  // no new delivery

    // Unsubscribing twice is not an error — a view tearing down should be able
    // to call this unconditionally.
    CHECK(eval_json(engine, "offParamChanged(sub)") == "false");
    CHECK(eval_json(engine, "offParamChanged(0)") == "false");
}

TEST_CASE("subscription ids are never reused, so a stale off cannot cancel a live one",
          "[view][bridge][state-binding][param-change]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        var seen = [];
        var first = onParamChanged('Cutoff', function () {});
        offParamChanged(first);
        var second = onParamChanged('Cutoff', function (p) { seen.push(p); });
    )");

    // If ids were slot indices, `second` would equal `first` and the stale
    // handle below would cancel a live subscription.
    CHECK(eval_json(engine, "second !== first") == "true");
    CHECK(eval_json(engine, "offParamChanged(first)") == "false");
    REQUIRE(bridge.param_subscription_count() == 1);

    store.set_value(10, 5000.0f);
    tick(bridge);
    CHECK(eval_json(engine, "seen.length") == "1");
}

TEST_CASE("a handler that writes its own param terminates instead of recursing",
          "[view][bridge][state-binding][param-change]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    // Clamping a param from its own change handler is a reasonable thing to
    // write. The value is snapshotted once per frame, so the handler's own
    // write surfaces as an ordinary change on the next frame — the loop
    // settles instead of recursing.
    bridge.load_script(R"(
        var calls = 0;
        onParamChanged('Cutoff', function (p) {
            calls++;
            if (p.value > 8000) setParam('Cutoff', 0.5);
        });
    )");

    store.set_value(10, 12000.0f);
    tick(bridge);
    CHECK(eval_json(engine, "calls") == "1");

    // Frame 2 observes the handler's own corrective write; frame 3 is quiet.
    tick(bridge);
    CHECK(eval_json(engine, "calls") == "2");
    tick(bridge);
    CHECK(eval_json(engine, "calls") == "2");
}

TEST_CASE("modulation movement is reported even when the base value is static",
          "[view][bridge][state-binding][param-change]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(kRecorder);

    // A CLAP host can move the modulated value while the base is untouched.
    // A UI drawing the modulated position has to see that, so `modulated` is
    // part of the change signal, not just part of the payload.
    store.set_mod_offset(10, 0.25f);
    tick(bridge);
    CHECK(eval_json(engine, "seen.length") == "1");

    tick(bridge);
    CHECK(eval_json(engine, "seen.length") == "1");  // static again
}

TEST_CASE("a throwing handler does not stop delivery to other subscriptions",
          "[view][bridge][state-binding][param-change]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        var good = 0;
        onParamChanged('Cutoff', function () { throw new Error('boom'); });
        onParamChanged('Cutoff', function () { good++; });
    )");
    REQUIRE(bridge.param_subscription_count() == 2);

    store.set_value(10, 5000.0f);
    tick(bridge);
    // __dispatch__ contains the exception, so one bad handler cannot silence
    // its neighbour or unwind the frame loop.
    CHECK(eval_json(engine, "good") == "1");

    store.set_value(10, 6000.0f);
    tick(bridge);
    CHECK(eval_json(engine, "good") == "2");
}

TEST_CASE("a handler may unsubscribe itself mid-dispatch",
          "[view][bridge][state-binding][param-change]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    // "Fire once, then stop" — ordinary to write, and it mutates the very
    // container the servicing loop is walking.
    bridge.load_script(R"(
        var calls = 0;
        var once = onParamChanged('Cutoff', function () {
            calls++;
            offParamChanged(once);
        });
        var other = 0;
        onParamChanged('Cutoff', function () { other++; });
    )");
    REQUIRE(bridge.param_subscription_count() == 2);

    store.set_value(10, 5000.0f);
    tick(bridge);
    CHECK(eval_json(engine, "calls") == "1");
    // The neighbour still gets this frame's change: cancelling one subscription
    // must not truncate the pass.
    CHECK(eval_json(engine, "other") == "1");
    CHECK(bridge.param_subscription_count() == 1);

    store.set_value(10, 6000.0f);
    tick(bridge);
    CHECK(eval_json(engine, "calls") == "1");   // stayed unsubscribed
    CHECK(eval_json(engine, "other") == "2");
}

TEST_CASE("a handler may subscribe from inside a dispatch",
          "[view][bridge][state-binding][param-change]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    // Growing the vector mid-pass can reallocate it, so this is the case that
    // catches a servicing loop holding a reference across the dispatch.
    bridge.load_script(R"(
        var late = 0;
        var added = false;
        onParamChanged('Cutoff', function () {
            if (added) return;
            added = true;
            for (var i = 0; i < 8; i++) onParamChanged('Cutoff', function () { late++; });
        });
    )");

    store.set_value(10, 5000.0f);
    tick(bridge);
    // The new subscriptions exist but are seeded at the current value, so they
    // do not retroactively fire for the change that created them.
    CHECK(bridge.param_subscription_count() == 9);
    CHECK(eval_json(engine, "late") == "0");

    store.set_value(10, 6000.0f);
    tick(bridge);
    CHECK(eval_json(engine, "late") == "8");
}

TEST_CASE("a NaN param value does not dispatch every frame",
          "[view][bridge][state-binding][param-change]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_rich_params(store);
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(kRecorder);

    // NaN never compares equal to itself, so an unguarded change test would
    // report a change on every single frame for as long as the value stays NaN.
    store.set_value(10, std::numeric_limits<float>::quiet_NaN());
    tick(bridge);
    tick(bridge);
    tick(bridge);
    CHECK(eval_json(engine, "seen.length") == "0");

    // Recovering to a real value reports normally.
    store.set_value(10, 5000.0f);
    tick(bridge);
    CHECK(eval_json(engine, "seen.length") == "1");
}

// ── {fromParam: true} — metadata-derived transforms ───────────────────
//
// Opt-in. The two gaps it closes: a RangeSlider maps the store's normalized
// value linearly onto its own range, which is wrong whenever the param is
// skewed; and a dB meter has to hand-copy ParamRange.min/max that the param
// already declares.

namespace {

// A skewed 20 Hz..20 kHz range whose normalized midpoint sits at 1 kHz — the
// classic filter-cutoff shape, and the case where normalized and real-linear
// disagree most visibly.
void add_skewed_cutoff(StateStore& store) {
    ParamInfo p{};
    p.id = 20;
    p.name = "Cutoff";
    p.unit = "Hz";
    p.range = ParamRange::with_center(20.0f, 20000.0f, 1000.0f);
    p.range.default_value = 1000.0f;
    store.add_parameter(p);
}

} // namespace

TEST_CASE("fromParam makes a RangeSlider skew-correct in real units",
          "[view][bridge][state-binding][from-param]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_skewed_cutoff(store);
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createRangeSlider('plain');
        createRangeSlider('derived');
        bindWidgetToParam('plain', 'Cutoff');
        bindWidgetToParam('derived', 'Cutoff', { fromParam: true });
    )");
    auto* plain = dynamic_cast<RangeSlider*>(bridge.widget("plain"));
    auto* derived = dynamic_cast<RangeSlider*>(bridge.widget("derived"));
    REQUIRE(plain != nullptr);
    REQUIRE(derived != nullptr);
    plain->set_min(20.0f);
    plain->set_max(20000.0f);
    derived->set_min(20.0f);
    derived->set_max(20000.0f);

    store.set_value(20, 1000.0f);
    bridge.service_param_bindings();

    // The derived slider reads back the real value it is bound to.
    REQUIRE_THAT(derived->value(), WithinAbs(1000.0f, 1.0f));
    // The plain binding puts a 1 kHz cutoff near the middle of the travel,
    // because the normalized value is curved — this is the bug being fixed,
    // pinned so a future change to the default path is visible.
    REQUIRE_THAT(plain->value(), WithinAbs(20.0f + 0.5f * (20000.0f - 20.0f), 200.0f));
    CHECK(plain->value() > derived->value() * 5.0f);
}

TEST_CASE("fromParam derives a meter's range instead of hand-copied dbMin/dbMax",
          "[view][bridge][state-binding][from-param]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    store.add_parameter({.id = 21, .name = "Level", .unit = "dB",
                         .range = {.min = -60.0f, .max = 0.0f}});
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createMeter('hand');
        createMeter('derived');
        bindMeter('hand', 'Level', { db: true, dbMin: -60, dbMax: 0 });
        bindMeter('derived', 'Level', { fromParam: true });
    )");
    auto* hand = dynamic_cast<Meter*>(bridge.widget("hand"));
    auto* derived = dynamic_cast<Meter*>(bridge.widget("derived"));
    REQUIRE(hand != nullptr);
    REQUIRE(derived != nullptr);

    store.set_value(21, -30.0f);
    bridge.service_param_bindings();
    // Derivation reproduces the hand-written transform exactly — that
    // equivalence is the whole point, minus the duplicated constants.
    REQUIRE_THAT(derived->display_rms(), WithinAbs(hand->display_rms(), 1e-5f));
    REQUIRE_THAT(derived->display_rms(), WithinAbs(0.5f, 1e-3f));
}

TEST_CASE("the bare two-arg bind is untouched by the fromParam work",
          "[view][bridge][state-binding][from-param]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_skewed_cutoff(store);
    WidgetBridge bridge(engine, root, store);

    // A knob is a normalized surface: its position should track the host's
    // automation curve, so the bare call must keep pushing get_normalized and
    // fromParam must deliberately NOT re-map it.
    bridge.load_script(R"(
        createKnob('bare');
        createKnob('asked');
        bindWidgetToParam('bare', 'Cutoff');
        bindWidgetToParam('asked', 'Cutoff', { fromParam: true });
    )");
    auto* bare = dynamic_cast<Knob*>(bridge.widget("bare"));
    auto* asked = dynamic_cast<Knob*>(bridge.widget("asked"));
    REQUIRE(bare != nullptr);
    REQUIRE(asked != nullptr);

    store.set_value(20, 1000.0f);
    bridge.service_param_bindings();
    // with_center puts 1 kHz at the normalized midpoint.
    REQUIRE_THAT(bare->value(), WithinAbs(0.5f, 1e-3f));
    REQUIRE_THAT(asked->value(), WithinAbs(bare->value(), 1e-6f));
}

TEST_CASE("an explicit db range still wins over fromParam",
          "[view][bridge][state-binding][from-param]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    store.add_parameter({.id = 21, .name = "Level", .unit = "dB",
                         .range = {.min = -60.0f, .max = 0.0f}});
    WidgetBridge bridge(engine, root, store);

    // Passing a full explicit db transform alongside fromParam is the author
    // overriding the derivation on purpose; silently replacing what they wrote
    // would be the wrong call. (dbMin/dbMax without `db: true` is inert in this
    // API — the mapping is off — so a real override names all three.)
    bridge.load_script(R"(
        createMeter('m');
        bindMeter('m', 'Level', { fromParam: true, db: true, dbMin: -30, dbMax: 0 });
    )");
    auto* m = dynamic_cast<Meter*>(bridge.widget("m"));
    REQUIRE(m != nullptr);

    store.set_value(21, -15.0f);
    bridge.service_param_bindings();
    // Halfway up the EXPLICIT -30..0 window, not the param's -60..0.
    REQUIRE_THAT(m->display_rms(), WithinAbs(0.5f, 1e-3f));
}

TEST_CASE("fromParam still derives when the widget is created after the bind",
          "[view][bridge][state-binding][from-param]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_skewed_cutoff(store);
    WidgetBridge bridge(engine, root, store);

    // Binding before the view exists is explicitly supported, so derivation
    // cannot happen at bind time — it has to wait for the widget.
    bridge.load_script(R"(
        bindWidgetToParam('later', 'Cutoff', { fromParam: true });
        createRangeSlider('later');
    )");
    auto* later = dynamic_cast<RangeSlider*>(bridge.widget("later"));
    REQUIRE(later != nullptr);
    later->set_min(20.0f);
    later->set_max(20000.0f);

    store.set_value(20, 1000.0f);
    bridge.service_param_bindings();
    REQUIRE_THAT(later->value(), WithinAbs(1000.0f, 1.0f));
}

// ── value: channel bindings ───────────────────────────────────────────
//
// `bindMeter(id, "value:<name>")` binds a widget to a value the processor
// PUBLISHES rather than a parameter. Before this, a meter could only show a
// level if the processor wrote it into a parameter — so gain reduction and
// envelope displays had to be hand-rolled per plugin.

namespace {

MeterFrame mono_frame(float rms) {
    MeterFrame f{};
    f.channels = 1;
    f.rms[0] = rms;
    f.peak[0] = rms;
    return f;
}

class SameAddressValueChannelSet {
public:
    SameAddressValueChannelSet() { reconstruct(); }
    ~SameAddressValueChannelSet() { std::destroy_at(current_); }

    ValueChannelSet* get() const noexcept { return current_; }
    ValueChannelSet* replace() {
        std::destroy_at(current_);
        return reconstruct();
    }

private:
    ValueChannelSet* reconstruct() {
        current_ = std::construct_at(
            reinterpret_cast<ValueChannelSet*>(storage_));
        return current_;
    }

    alignas(ValueChannelSet) std::byte storage_[sizeof(ValueChannelSet)]{};
    ValueChannelSet* current_ = nullptr;
};

} // namespace

TEST_CASE("a meter binds to a published value channel, not a parameter",
          "[view][bridge][state-binding][value-channel]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);

    ValueChannelSet channels;
    auto* gr = channels.declare_meter("gr_db");
    REQUIRE(gr != nullptr);

    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channels(&channels);
    bridge.load_script(R"(
        createMeter('gr');
        bindMeter('gr', 'value:gr_db');
    )");
    auto* meter = dynamic_cast<Meter*>(bridge.widget("gr"));
    REQUIRE(meter != nullptr);

    // The publish side is what an audio thread would do.
    gr->publish(mono_frame(0.25f));
    bridge.service_param_bindings();
    REQUIRE_THAT(meter->display_rms(), WithinAbs(0.25f, 1e-5f));

    gr->publish(mono_frame(0.75f));
    bridge.service_param_bindings();
    REQUIRE_THAT(meter->display_rms(), WithinAbs(0.75f, 1e-5f));
}

TEST_CASE("value bindings lease and re-resolve the replacement channel set",
          "[view][bridge][state-binding][value-channel][hot-swap][lifetime]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);

    auto retired = std::make_unique<ValueChannelSet>();
    auto* retired_meter = retired->declare_meter("level");
    auto* retired_events = retired->declare_events("ticks");
    REQUIRE(retired_meter != nullptr);
    REQUIRE(retired_events != nullptr);

    auto replacement = std::make_unique<ValueChannelSet>();
    auto* replacement_meter = replacement->declare_meter("level");
    auto* replacement_events = replacement->declare_events("ticks");
    REQUIRE(replacement_meter != nullptr);
    REQUIRE(replacement_events != nullptr);
    const ValueEvent retired_event{.frame_index = 1, .value = 0.25f};
    retired_events->publish(&retired_event, 1);

    ValueChannelSet* active = retired.get();
    int lease_depth = 0;
    int access_calls = 0;
    bool nested_lease = false;

    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channel_access(
        [&](const ValueChannelVisitor& visitor) {
            ++access_calls;
            nested_lease = nested_lease || lease_depth != 0;
            ++lease_depth;
            visitor(active);
            --lease_depth;
        });
    bridge.load_script(R"(
        createMeter('live');
        createMeter('live2');
        bindMeter('live', 'value:level');
        bindMeter('live2', 'value:level');
        globalThis.eventCalls = 0;
        bindEvents('value:ticks', function() {
            ++eventCalls;
            // This performs another leased channel visit. It must run after the
            // event frame's source lease has been released.
            listValueChannels();
        });
    )");
    auto* meter = dynamic_cast<Meter*>(bridge.widget("live"));
    auto* meter2 = dynamic_cast<Meter*>(bridge.widget("live2"));
    REQUIRE(meter != nullptr);
    REQUIRE(meter2 != nullptr);

    retired_meter->publish(mono_frame(0.2f));
    access_calls = 0;
    bridge.service_param_bindings();
    CHECK(access_calls == 1);
    REQUIRE_THAT(meter->display_rms(), WithinAbs(0.2f, 1e-5f));
    REQUIRE_THAT(meter2->display_rms(), WithinAbs(0.2f, 1e-5f));

    // Mutation proof: leave the retired generation alive and publish a
    // different value to it. A cached source pointer reads 0.3; a name resolved
    // under the current lease reads the replacement's 0.8.
    active = replacement.get();
    retired_meter->publish(mono_frame(0.3f));
    replacement_meter->publish(mono_frame(0.8f));
    // Both generations now report publication 1. Generation identity, not just
    // the per-source counter, must make the replacement event observable.
    const ValueEvent replacement_event{.frame_index = 7, .value = 1.0f};
    replacement_events->publish(&replacement_event, 1);
    bridge.service_frame_callbacks();
    CHECK_THAT(meter->display_rms(), WithinAbs(0.8f, 1e-5f));
    CHECK(eval_json(engine, "eventCalls") == "1");
    CHECK_FALSE(nested_lease);

    // Destruction proof: the old set and all its sources are now gone while
    // the original bridge and bindings remain. Servicing must touch only the
    // replacement generation.
    retired.reset();
    replacement_meter->publish(mono_frame(0.6f));
    bridge.service_frame_callbacks();
    CHECK_THAT(meter->display_rms(), WithinAbs(0.6f, 1e-5f));
}

TEST_CASE("a temporarily unavailable value-channel lease preserves its neutral",
          "[view][bridge][state-binding][value-channel][hot-swap][lifetime]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);

    ValueChannelSet channels;
    auto* level = channels.declare_meter("level", {}, 0.35f);
    REQUIRE(level != nullptr);
    ValueChannelSet* active = &channels;

    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channel_access(
        [&](const ValueChannelVisitor& visitor) { visitor(active); });
    bridge.load_script(R"(
        createMeter('live');
        bindMeter('live', 'value:level');
    )");
    auto* meter = dynamic_cast<Meter*>(bridge.widget("live"));
    REQUIRE(meter != nullptr);

    level->publish(mono_frame(0.25f));
    bridge.service_param_bindings();
    REQUIRE_THAT(meter->display_rms(), WithinAbs(0.25f, 1e-5f));

    active = nullptr;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bridge.service_param_bindings();
    CHECK_THAT(meter->display_rms(), WithinAbs(0.35f, 1e-5f));
}

TEST_CASE("value generation identity survives same-address replacement ABA",
          "[view][bridge][state-binding][value-channel][hot-swap][aba]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);

    SameAddressValueChannelSet storage;
    ValueChannelSet* active = storage.get();
    auto* first_meter = active->declare_meter("level");
    auto* first_events = active->declare_events("ticks");
    REQUIRE(first_meter != nullptr);
    REQUIRE(first_events != nullptr);
    first_meter->publish(mono_frame(0.2f));       // publish sequence 1
    const ValueEvent first_event{.frame_index = 1, .value = 0.25f};
    first_events->publish(&first_event, 1);       // publication 1

    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channel_access(
        [&](const ValueChannelVisitor& visitor) { visitor(active); });
    bridge.load_script(R"(
        createMeter('live');
        bindMeter('live', 'value:level');
        globalThis.abaEventCalls = 0;
        bindEvents('value:ticks', function() { ++abaEventCalls; });
    )");
    auto* meter = dynamic_cast<Meter*>(bridge.widget("live"));
    REQUIRE(meter != nullptr);
    bridge.service_frame_callbacks();
    REQUIRE_THAT(meter->display_rms(), WithinAbs(0.2f, 1e-5f));
    CHECK(eval_json(engine, "abaEventCalls") == "0");

    const auto first_identity = active->generation_identity();
    const auto* first_address = active;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    active = storage.replace();
    REQUIRE(active == first_address);
    REQUIRE(active->generation_identity() != first_identity);
    auto* second_meter = active->declare_meter("level");
    auto* second_events = active->declare_events("ticks");
    REQUIRE(second_meter != nullptr);
    REQUIRE(second_events != nullptr);
    second_meter->publish(mono_frame(0.8f));      // publish sequence 1 again
    const ValueEvent second_event{.frame_index = 7, .value = 1.0f};
    second_events->publish(&second_event, 1);     // publication 1 again

    bridge.service_frame_callbacks();
    CHECK_THAT(meter->display_rms(), WithinAbs(0.8f, 1e-5f));
    CHECK(eval_json(engine, "abaEventCalls") == "1");
}

TEST_CASE("an undeclared value channel fails loudly rather than silently",
          "[view][bridge][state-binding][value-channel]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);

    ValueChannelSet channels;
    channels.declare_meter("gr_db");

    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channels(&channels);
    bridge.load_script(R"(
        createMeter('m');
        bindMeter('m', 'value:nope');
    )");

    REQUIRE(bridge.param_binding_count() == 0);
    const auto& attempts = bridge.binding_attempts();
    REQUIRE(attempts.size() == 1);
    CHECK(attempts[0].outcome == BindingOutcome::unknown_value_channel);
    CHECK_FALSE(is_bound(attempts[0].outcome));
}

TEST_CASE("value: and parameter names are separate namespaces",
          "[view][bridge][state-binding][value-channel]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);  // declares a param called "gain"

    ValueChannelSet channels;  // declares NO channel called "gain"

    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channels(&channels);
    bridge.load_script(R"(
        createMeter('a');
        createMeter('b');
        bindMeter('a', 'value:gain');
        bindMeter('b', 'gain');
    )");

    // `value:gain` must NOT fall back to the same-named parameter — resolving
    // across namespaces would bind a meter to the wrong source and look fine.
    const auto& attempts = bridge.binding_attempts();
    REQUIRE(attempts.size() == 2);
    CHECK(attempts[0].outcome == BindingOutcome::unknown_value_channel);
    // The bare name still resolves as a parameter, unchanged.
    CHECK(is_bound(attempts[1].outcome));
    CHECK(bridge.param_binding_count() == 1);
}

TEST_CASE("value: binds fail cleanly when the processor declares no channels",
          "[view][bridge][state-binding][value-channel]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);

    // No set attached at all — the default for every processor that does not
    // override value_channels(). This must not crash or bind.
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createMeter('m');
        bindMeter('m', 'value:anything');
    )");

    CHECK(bridge.param_binding_count() == 0);
    const auto& attempts = bridge.binding_attempts();
    REQUIRE(attempts.size() == 1);
    CHECK(attempts[0].outcome == BindingOutcome::unknown_value_channel);
}

// ── bindScope + staleness ─────────────────────────────────────────────

TEST_CASE("bindScope pushes a vector channel into a SpectrumView",
          "[view][bridge][state-binding][value-channel]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);

    ValueChannelSet channels;
    auto* env = channels.declare_vector("env");
    REQUIRE(env != nullptr);

    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channels(&channels);
    bridge.load_script(R"(
        createSpectrum('scope');
        bindScope('scope', 'value:env');
    )");
    REQUIRE(bridge.param_binding_count() == 1);

    const float block[4] = {-6.0f, -12.0f, -18.0f, -24.0f};
    env->publish(block, 4);
    bridge.service_param_bindings();
    // The block reached the view: binding a scope is a whole-block push, not a
    // scalar one, so this is the path that would silently do nothing if the
    // scope target fell through to the value/meter branch.
    CHECK(bridge.binding_attempts().back().outcome == BindingOutcome::ok);
}

TEST_CASE("a scope cannot bind to a meter channel, or to a parameter",
          "[view][bridge][state-binding][value-channel]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);  // has a param named "gain"

    ValueChannelSet channels;
    channels.declare_meter("level");  // a METER channel, not a vector

    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channels(&channels);
    bridge.load_script(R"(
        createSpectrum('a');
        createSpectrum('b');
        bindScope('a', 'value:level');
        bindScope('b', 'gain');
    )");

    const auto& attempts = bridge.binding_attempts();
    REQUIRE(attempts.size() == 2);
    // Wrong SHAPE is a miss, not a coercion — rendering a meter as a spectrum
    // would draw a plausible picture of the wrong thing.
    CHECK(attempts[0].outcome == BindingOutcome::unknown_value_channel);
    // And a scope has no parameter equivalent at all.
    CHECK(attempts[1].outcome == BindingOutcome::unknown_value_channel);
    CHECK(bridge.param_binding_count() == 0);
}

TEST_CASE("a meter holds a static-but-live reading, and decays once publishing stops",
          "[view][bridge][state-binding][value-channel][staleness]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);

    ValueChannelSet channels;
    // neutral = 0: a gain-reduction meter rests at "no reduction".
    auto* gr = channels.declare_meter("gr", "dB", 0.0f);
    REQUIRE(gr != nullptr);

    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channels(&channels);
    bridge.load_script(R"(
        createMeter('gr');
        bindMeter('gr', 'value:gr');
    )");
    auto* meter = dynamic_cast<Meter*>(bridge.widget("gr"));
    REQUIRE(meter != nullptr);

    // A compressor holding steady reduction publishes the SAME value every
    // block. Value-equality staleness would wrongly decay this; the publish
    // counter keeps it alive.
    for (int i = 0; i < 5; ++i) {
        gr->publish(mono_frame(0.6f));
        bridge.service_param_bindings();
    }
    REQUIRE_THAT(meter->display_rms(), WithinAbs(0.6f, 1e-5f));

    // Now the writer stops. Still fresh immediately after.
    bridge.service_param_bindings();
    CHECK_THAT(meter->display_rms(), WithinAbs(0.6f, 1e-5f));

    // ...and decays to neutral once the stale window has elapsed.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bridge.service_param_bindings();
    CHECK_THAT(meter->display_rms(), WithinAbs(0.0f, 1e-5f));

    // A fresh publish revives it.
    gr->publish(mono_frame(0.4f));
    bridge.service_param_bindings();
    CHECK_THAT(meter->display_rms(), WithinAbs(0.4f, 1e-5f));
}
// ── listValueChannels ─────────────────────────────────────────────────
//
// Discovery. Without it a UI hard-codes channel names, and those names stop
// resolving silently when the processor is edited — the same drift
// getParamMetadata removed for parameters.

TEST_CASE("listValueChannels reports what a UI can bind",
          "[view][bridge][state-binding][value-channel]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);

    ValueChannelSet channels;
    channels.declare_meter("gr", "dB", 0.0f);
    channels.declare_vector("env");
    channels.declare_events("onsets");

    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channels(&channels);
    bridge.load_script("");

    const auto json = eval_json(engine, "listValueChannels()");
    INFO("payload: " << json);
    CHECK(json.find("\"name\":\"gr\"") != std::string::npos);
    CHECK(json.find("\"unit\":\"dB\"") != std::string::npos);
    // `shape` is what tells a caller which binder applies.
    CHECK(json.find("\"shape\":\"meter\"") != std::string::npos);
    CHECK(json.find("\"shape\":\"vector\"") != std::string::npos);
    CHECK(json.find("\"shape\":\"events\"") != std::string::npos);
    CHECK(json.find("\"neutral\":0") != std::string::npos);
    CHECK(eval_json(engine, "listValueChannels().length") == "3");
}

TEST_CASE("listValueChannels is an empty array when nothing is declared",
          "[view][bridge][state-binding][value-channel]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);

    // No set attached at all — the default for every processor that does not
    // override value_channels(). A caller must get [] rather than undefined or
    // a throw, so `for (const c of listValueChannels())` is always safe.
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("");
    CHECK(eval_json(engine, "listValueChannels().length") == "0");
    CHECK(eval_json(engine, "Array.isArray(listValueChannels())") == "true");
}

TEST_CASE("bindEvents delivers frame offsets and zero-valued occurrences",
          "[view][bridge][state-binding][value-channel]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    add_params(store);

    ValueChannelSet channels;
    auto* onsets = channels.declare_events("onsets", "velocity");
    REQUIRE(onsets != nullptr);

    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channels(&channels);
    bridge.load_script(R"(
        globalThis.eventBlocks = [];
        globalThis.eventBinding = bindEvents('value:onsets', function(events) {
            eventBlocks.push(events);
        });
    )");
    REQUIRE(bridge.event_binding_count() == 1);
    REQUIRE(eval_json(engine, "eventBinding > 0") == "true");

    const ValueEvent occurrences[] = {
        {.frame_index = 0, .value = 0.0f},
        {.frame_index = 127, .value = 0.75f},
    };
    onsets->publish(occurrences, 2);
    bridge.service_frame_callbacks();

    CHECK(eval_json(engine, "eventBlocks.length") == "1");
    CHECK(eval_json(engine, "eventBlocks[0].length") == "2");
    CHECK(eval_json(engine, "eventBlocks[0][0].frameIndex") == "0");
    CHECK(eval_json(engine, "eventBlocks[0][0].value") == "0");
    CHECK(eval_json(engine, "eventBlocks[0][1].frameIndex") == "127");
    CHECK(eval_json(engine, "eventBlocks[0][1].value") == "0.75");

    // No new publish means no synthetic event block on later UI frames.
    bridge.service_frame_callbacks();
    CHECK(eval_json(engine, "eventBlocks.length") == "1");

    REQUIRE(eval_json(engine, "unbindEvents(eventBinding)") == "true");
    CHECK(bridge.event_binding_count() == 0);
    onsets->publish(occurrences, 1);
    bridge.service_frame_callbacks();
    CHECK(eval_json(engine, "eventBlocks.length") == "1");
}

TEST_CASE("bindEvents rejects absent and non-event channels",
          "[view][bridge][state-binding][value-channel]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    add_params(store);

    ValueChannelSet channels;
    channels.declare_vector("scope");
    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channels(&channels);
    bridge.load_script(R"(
        globalThis.notEvents = bindEvents('value:scope', function() {});
        globalThis.absent = bindEvents('value:absent', function() {});
        globalThis.noPrefix = bindEvents('scope', function() {});
    )");

    CHECK(eval_json(engine, "notEvents") == "0");
    CHECK(eval_json(engine, "absent") == "0");
    CHECK(eval_json(engine, "noPrefix") == "0");
    CHECK(bridge.event_binding_count() == 0);
}

TEST_CASE("bindEvents neither replays old blocks nor invalidates reentrant unbind",
          "[view][bridge][state-binding][value-channel]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    add_params(store);

    ValueChannelSet channels;
    auto* onsets = channels.declare_events("onsets");
    REQUIRE(onsets != nullptr);
    const ValueEvent occurrence{.frame_index = 4, .value = 1.0f};
    onsets->publish(&occurrence, 1);

    WidgetBridge bridge(engine, root, store);
    bridge.set_value_channels(&channels);
    bridge.load_script(R"(
        globalThis.calls = 0;
        globalThis.binding = bindEvents('value:onsets', function() {
            ++calls;
            unbindEvents(binding);
        });
    )");

    // Binding starts at the current publication rather than synthesizing a
    // callback for a block that preceded the subscription.
    bridge.service_frame_callbacks();
    CHECK(eval_json(engine, "calls") == "0");

    onsets->publish(&occurrence, 1);
    bridge.service_frame_callbacks();
    CHECK(eval_json(engine, "calls") == "1");
    CHECK(bridge.event_binding_count() == 0);

    onsets->publish(&occurrence, 1);
    bridge.service_frame_callbacks();
    CHECK(eval_json(engine, "calls") == "1");
}
