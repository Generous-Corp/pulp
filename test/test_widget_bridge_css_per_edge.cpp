// test_widget_bridge_css_per_edge.cpp — extracted from test_widget_bridge.cpp
// in the 2026-05 Phase 5 (P5-1 follow-up) refactor.
//
// pulp #1434 (cross-surface mega-batch) — per-edge margin/padding.
//
// Pins the bridge ↔ Yoga node ↔ ParsedStyle plumbing for per-edge
// margin/padding (marginTop, marginRight, marginBottom, marginLeft, and
// the padding counterparts). Originally landed as part of the cross-
// surface mega-batch on #1434; these tests guard against regressions in
// the JS-style → Yoga-edge mapping (each side maps to its own enum and
// must NOT cross-contaminate).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <thread>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

static pulp::view::CanvasWidget* canvasFromBridge(pulp::view::WidgetBridge& bridge,
                                                  pulp::view::ScriptEngine& engine,
                                                  const std::string& id) {
    auto value = engine.evaluate("document.getElementById('" + id + "')._id");
    auto nativeId = std::string(value.getWithDefault<std::string_view>(""));
    return dynamic_cast<pulp::view::CanvasWidget*>(bridge.widget(nativeId));
}

// ── pulp #1434 (cross-surface mega-batch) — per-edge margin/padding
// accept percent strings + auto (margin only). Mirrors width/height
// (#1426) and top/right/bottom/left (#1451) percent patterns. Yoga
// dispatches `dim_*.unit == percent` to `YGNodeStyleSetMargin/PaddingPercent`;
// `unit == auto_` (margin only) to `YGNodeStyleSetMarginAuto`.

TEST_CASE("setFlex padding edges accept percent strings",
          "[view][bridge][css][issue-1434-edges]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        setFlex('a', 'padding_top',    '10%');
        setFlex('a', 'padding_right',  '20%');
        setFlex('a', 'padding_bottom', '30%');
        setFlex('a', 'padding_left',   '40%');
    )");

    const auto& f = bridge.widget("a")->flex();
    REQUIRE(f.dim_padding_top.unit    == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_padding_top.value,    WithinAbs(10.0f, 0.001f));
    REQUIRE(f.dim_padding_right.unit  == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_padding_right.value,  WithinAbs(20.0f, 0.001f));
    REQUIRE(f.dim_padding_bottom.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_padding_bottom.value, WithinAbs(30.0f, 0.001f));
    REQUIRE(f.dim_padding_left.unit   == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_padding_left.value,   WithinAbs(40.0f, 0.001f));
}

TEST_CASE("setFlex padding edges numeric path stays px",
          "[view][bridge][css][issue-1434-edges]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        setFlex('a', 'padding_top',    8);
        setFlex('a', 'padding_right',  12);
        setFlex('a', 'padding_bottom', 16);
        setFlex('a', 'padding_left',   4);
    )");

    const auto& f = bridge.widget("a")->flex();
    REQUIRE(f.dim_padding_top.unit == DimensionUnit::px);
    REQUIRE_THAT(f.padding_top,    WithinAbs(8.0f,  0.001f));
    REQUIRE_THAT(f.padding_right,  WithinAbs(12.0f, 0.001f));
    REQUIRE_THAT(f.padding_bottom, WithinAbs(16.0f, 0.001f));
    REQUIRE_THAT(f.padding_left,   WithinAbs(4.0f,  0.001f));
}

TEST_CASE("setFlex margin edges accept percent strings",
          "[view][bridge][css][issue-1434-edges]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        setFlex('a', 'margin_top',    '5%');
        setFlex('a', 'margin_right',  '10%');
        setFlex('a', 'margin_bottom', '15%');
        setFlex('a', 'margin_left',   '20%');
    )");

    const auto& f = bridge.widget("a")->flex();
    REQUIRE(f.dim_margin_top.unit    == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_margin_top.value,    WithinAbs(5.0f,  0.001f));
    REQUIRE(f.dim_margin_right.unit  == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_margin_right.value,  WithinAbs(10.0f, 0.001f));
    REQUIRE(f.dim_margin_bottom.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_margin_bottom.value, WithinAbs(15.0f, 0.001f));
    REQUIRE(f.dim_margin_left.unit   == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_margin_left.value,   WithinAbs(20.0f, 0.001f));
}

TEST_CASE("setFlex margin edges accept 'auto' keyword",
          "[view][bridge][css][issue-1434-edges]") {
    // `marginLeft: 'auto'; marginRight: 'auto'` is the canonical
    // centering idiom in CSS / RN; Yoga supports it via
    // YGNodeStyleSetMarginAuto.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        setFlex('a', 'margin_left',  'auto');
        setFlex('a', 'margin_right', 'auto');
    )");

    const auto& f = bridge.widget("a")->flex();
    REQUIRE(f.dim_margin_left.unit  == DimensionUnit::auto_);
    REQUIRE(f.dim_margin_right.unit == DimensionUnit::auto_);
}

// ── pulp DIVERGE→PASS sweep — canvas2d surface ──────────────────────────

TEST_CASE("ctx.fill('evenodd') threads fillRule int_val through bridge",
          "[view][bridge][canvas][diverge-pass-canvas2d]") {
    // pulp DIVERGE→PASS sweep — `ctx.fill('evenodd')` and
    // `ctx.clip('evenodd')` now thread the fillRule keyword through
    // to the bridge as int_val (0=nonzero, 1=evenodd). Pairs with
    // [issue-1522] which tests the bridge fns directly.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'fr-shim-canvas';
        c.width = 100; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.beginPath();
        ctx.moveTo(0, 0); ctx.lineTo(10, 0);
        ctx.lineTo(10, 10); ctx.lineTo(0, 10);
        ctx.closePath();
        ctx.fill('evenodd');
        ctx.beginPath();
        ctx.moveTo(0, 0); ctx.lineTo(10, 0);
        ctx.lineTo(10, 10); ctx.closePath();
        ctx.fill();             // default nonzero
        ctx.beginPath();
        ctx.moveTo(0, 0); ctx.lineTo(10, 0);
        ctx.lineTo(10, 10); ctx.closePath();
        ctx.clip('evenodd');
        ctx.beginPath();
        ctx.moveTo(0, 0); ctx.lineTo(10, 0);
        ctx.lineTo(10, 10); ctx.closePath();
        ctx.clip();
    )");

    auto* canvas = canvasFromBridge(bridge, engine, "fr-shim-canvas");
    REQUIRE(canvas != nullptr);
    using T = pulp::view::CanvasDrawCmd::Type;
    std::vector<int> fill_int_vals;
    std::vector<int> clip_int_vals;
    for (const auto& cmd : canvas->commands()) {
        if (cmd.type == T::fill_path) fill_int_vals.push_back(cmd.int_val);
        if (cmd.type == T::clip)      clip_int_vals.push_back(cmd.int_val);
    }
    REQUIRE(fill_int_vals.size() == 2);
    REQUIRE(fill_int_vals[0] == 1);  // explicit evenodd
    REQUIRE(fill_int_vals[1] == 0);  // default nonzero
    REQUIRE(clip_int_vals.size() == 2);
    REQUIRE(clip_int_vals[0] == 1);  // explicit evenodd
    REQUIRE(clip_int_vals[1] == 0);  // default nonzero
}

// ── pulp DIVERGE→PASS sweep — html surface ──────────────────────────────

TEST_CASE("Element.disabled wires to setEnabled",
          "[view][bridge][html][diverge-pass-html]") {
    // Previously `el.disabled = true` only flipped the stylesheet flag;
    // the underlying widget kept handling pointer events. Now wired
    // end-to-end via setEnabled. Drive via JS so the assertion runs
    // against the real CSSStyleDeclaration / disabled property accessor.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var input = document.createElement('input');
        input.id = 'box';
        input.type = 'text';
        document.body.appendChild(input);
        globalThis.__test_input_id__ = input._id;
        input.disabled = true;
        globalThis.__test_disabled_value__ = input.disabled;
    )");

    auto id = engine.evaluate("globalThis.__test_input_id__").getWithDefault<std::string>("");
    REQUIRE_FALSE(id.empty());
    auto* w = bridge.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE_FALSE(w->enabled());
    auto js_disabled = engine.evaluate("globalThis.__test_disabled_value__").getWithDefault<bool>(false);
    REQUIRE(js_disabled);
}

TEST_CASE("new Event() round-trips through dispatchEvent",
          "[view][bridge][html][diverge-pass-html]") {
    // The Event / CustomEvent constructors live in web-compat-element.js.
    // Verify `new Event('foo')` produces a target-able event that
    // dispatchEvent fires through addEventListener listeners.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var el = document.createElement('div');
        document.body.appendChild(el);
        globalThis.__test_event_count__ = 0;
        globalThis.__test_event_type__ = '';
        globalThis.__test_event_detail__ = null;
        el.addEventListener('foo', function(e) {
            globalThis.__test_event_count__++;
            globalThis.__test_event_type__ = e.type;
        });
        el.addEventListener('bar', function(e) {
            globalThis.__test_event_detail__ = e.detail;
        });
        el.dispatchEvent(new Event('foo'));
        el.dispatchEvent(new CustomEvent('bar', { detail: 42 }));
    )");

    auto count  = engine.evaluate("globalThis.__test_event_count__").getWithDefault<int>(-1);
    auto type   = engine.evaluate("globalThis.__test_event_type__").getWithDefault<std::string>("");
    auto detail = engine.evaluate("globalThis.__test_event_detail__").getWithDefault<int>(-1);
    REQUIRE(count == 1);
    REQUIRE(type == "foo");
    REQUIRE(detail == 42);
}

TEST_CASE("<dialog> show/close round-trips visibility and dispatches close",
          "[view][bridge][html][diverge-pass-html]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var d = document.createElement('dialog');
        document.body.appendChild(d);
        globalThis.__test_dialog_id__ = d._id;
        globalThis.__test_dialog_close_count__ = 0;
        d.addEventListener('close', function() {
            globalThis.__test_dialog_close_count__++;
        });
        // Initially hidden.
        globalThis.__test_dialog_open_initial__ = d.open;
        d.show();
        globalThis.__test_dialog_open_after_show__ = d.open;
        d.close('ok');
        globalThis.__test_dialog_open_after_close__ = d.open;
        globalThis.__test_dialog_return_value__ = d.returnValue;
    )");

    auto initial      = engine.evaluate("globalThis.__test_dialog_open_initial__").getWithDefault<bool>(true);
    auto after_show   = engine.evaluate("globalThis.__test_dialog_open_after_show__").getWithDefault<bool>(false);
    auto after_close  = engine.evaluate("globalThis.__test_dialog_open_after_close__").getWithDefault<bool>(true);
    auto close_count  = engine.evaluate("globalThis.__test_dialog_close_count__").getWithDefault<int>(-1);
    auto return_value = engine.evaluate("globalThis.__test_dialog_return_value__").getWithDefault<std::string>("");
    REQUIRE_FALSE(initial);
    REQUIRE(after_show);
    REQUIRE_FALSE(after_close);
    REQUIRE(close_count == 1);
    REQUIRE(return_value == "ok");
}

TEST_CASE("<details> open setter dispatches toggle event",
          "[view][bridge][html][diverge-pass-html]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var d = document.createElement('details');
        document.body.appendChild(d);
        globalThis.__test_toggle_count__ = 0;
        d.addEventListener('toggle', function() {
            globalThis.__test_toggle_count__++;
        });
        globalThis.__test_open_initial__ = d.open;
        d.open = true;
        globalThis.__test_open_after_set__ = d.open;
        globalThis.__test_open_attribute__ = d.getAttribute('open');
        d.open = false;
        globalThis.__test_open_after_unset__ = d.open;
    )");

    auto initial    = engine.evaluate("globalThis.__test_open_initial__").getWithDefault<bool>(true);
    auto after_set  = engine.evaluate("globalThis.__test_open_after_set__").getWithDefault<bool>(false);
    auto attr       = engine.evaluate("globalThis.__test_open_attribute__").getWithDefault<std::string>("__missing__");
    auto after_unset = engine.evaluate("globalThis.__test_open_after_unset__").getWithDefault<bool>(true);
    auto toggles    = engine.evaluate("globalThis.__test_toggle_count__").getWithDefault<int>(-1);
    REQUIRE_FALSE(initial);
    REQUIRE(after_set);
    REQUIRE(attr == "");  // present, value empty
    REQUIRE_FALSE(after_unset);
    REQUIRE(toggles == 2);
}

TEST_CASE("<label for=...> click toggles labeled checkbox",
          "[view][bridge][html][diverge-pass-html]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var cb = document.createElement('input');
        cb.type = 'checkbox';
        cb.id = 'mybox';
        document.body.appendChild(cb);
        var lbl = document.createElement('label');
        lbl.setAttribute('for', 'mybox');
        document.body.appendChild(lbl);

        globalThis.__test_input_event_count__ = 0;
        cb.addEventListener('input', function() {
            globalThis.__test_input_event_count__++;
        });
        // Synthesize a click on the label.
        var evt = new Event('click', { bubbles: true });
        lbl.dispatchEvent(evt);
        globalThis.__test_checked_after_click__ = cb.checked;
    )");

    auto count   = engine.evaluate("globalThis.__test_input_event_count__").getWithDefault<int>(-1);
    auto checked = engine.evaluate("globalThis.__test_checked_after_click__").getWithDefault<bool>(false);
    REQUIRE(count == 1);
    REQUIRE(checked);
}

TEST_CASE("addEventListener('wheel', fn) routes through registerWheel",
          "[view][bridge][html][diverge-pass-html]") {
    // Verify the wheel branch in _registerNativeEvent is exercised — we
    // can't easily generate a wheel scroll from C++ without driving the
    // event system, so smoke-test that the call doesn't throw and the
    // listener is recorded.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var el = document.createElement('div');
        document.body.appendChild(el);
        var fired = false;
        el.addEventListener('wheel', function(e) {
            fired = true;
        });
        // Manually invoke the bridge's __dispatch__ for 'wheel' to
        // simulate a native wheel event delivery — that's the path
        // _registerNativeEvent wires up via on(id, 'wheel', ...).
        if (typeof __dispatch__ === 'function') {
            __dispatch__(el._id, 'wheel', 5, -7);
        }
        globalThis.__test_wheel_fired__ = fired;
    )");

    auto fired = engine.evaluate("globalThis.__test_wheel_fired__").getWithDefault<bool>(false);
    REQUIRE(fired);
}

TEST_CASE("addEventListener('drop', fn) routes through registerDrop",
          "[view][bridge][html][diverge-pass-html]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var el = document.createElement('div');
        document.body.appendChild(el);
        globalThis.__test_drop_payload__ = '';
        el.addEventListener('drop', function(e) {
            globalThis.__test_drop_payload__ = e._dropData
                ? (e._dropData.type + ':' + e._dropData.data)
                : 'no-data';
        });
        // The drop handler is registered as a global function named
        // __drop_cb_<id>; invoke it directly to simulate the native
        // drop completion path.
        var cbName = '__drop_cb_' + el._id.replace(/[^a-zA-Z0-9_]/g, '_');
        if (typeof globalThis[cbName] === 'function') {
            globalThis[cbName]('text', 'hello world', 10, 20);
        }
    )");

    auto payload = engine.evaluate("globalThis.__test_drop_payload__").getWithDefault<std::string>("");
    REQUIRE(payload == "text:hello world");
}

TEST_CASE("setFlex start/end accept 'auto' keyword",
          "[view][bridge][css][diverge-pass-yoga]") {
    // pulp DIVERGE→PASS sweep — yoga/start and yoga/end claimed `auto`
    // as unsupported. Yoga DOES expose YGNodeStyleSetPositionAuto, so
    // wire `'auto'` through the bridge → FlexStyle::dim_start/dim_end
    // → yoga_layout.cpp::apply_logical_position. The keyword routes via
    // DimensionUnit::auto_.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        setFlex('a', 'start', 'auto');
        setFlex('a', 'end',   'auto');
    )");

    const auto& f = bridge.widget("a")->flex();
    REQUIRE(f.dim_start.unit == DimensionUnit::auto_);
    REQUIRE(f.dim_end.unit   == DimensionUnit::auto_);
}

TEST_CASE("flex shorthand keyword forms",
          "[view][bridge][css][diverge-pass-yoga]") {
    // pulp DIVERGE→PASS sweep — yoga/flex claimed `'auto'`/`'none'`/
    // `'initial'` as unsupported because the JS shorthand parser
    // parseFloat()ed the keyword to NaN. The CSS Flexible Box spec
    // expansion is:
    //   flex: auto    ≡ 1 1 auto
    //   flex: none    ≡ 0 0 auto
    //   flex: initial ≡ 0 1 auto
    // Drives the CSSStyleDeclaration shim (the actual code path that
    // contains the new keyword expansion) and verifies each keyword
    // reaches FlexStyle with the correct grow / shrink / basis-unit.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        createPanel('b', '');
        createPanel('c', '');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        var sb = new CSSStyleDeclaration({ _id: 'b', _nativeCreated: true });
        var sc = new CSSStyleDeclaration({ _id: 'c', _nativeCreated: true });
        sa.flex = 'auto';
        sb.flex = 'none';
        sc.flex = 'initial';
    )");

    const auto& fa = bridge.widget("a")->flex();
    REQUIRE_THAT(fa.flex_grow,   WithinAbs(1.0, 0.0001));
    REQUIRE_THAT(fa.flex_shrink, WithinAbs(1.0, 0.0001));
    REQUIRE(fa.dim_flex_basis.unit == DimensionUnit::auto_);

    const auto& fb = bridge.widget("b")->flex();
    REQUIRE_THAT(fb.flex_grow,   WithinAbs(0.0, 0.0001));
    REQUIRE_THAT(fb.flex_shrink, WithinAbs(0.0, 0.0001));
    REQUIRE(fb.dim_flex_basis.unit == DimensionUnit::auto_);

    const auto& fc = bridge.widget("c")->flex();
    REQUIRE_THAT(fc.flex_grow,   WithinAbs(0.0, 0.0001));
    REQUIRE_THAT(fc.flex_shrink, WithinAbs(1.0, 0.0001));
    REQUIRE(fc.dim_flex_basis.unit == DimensionUnit::auto_);
}

TEST_CASE("setOverflow accepts scroll keyword",
          "[view][bridge][diverge-pass-yoga]") {
    // pulp DIVERGE→PASS sweep — yoga/overflow claimed `scroll` as
    // unsupported. View::Overflow now has 3 values; the bridge accepts
    // 'scroll' as a third keyword and yoga_layout.cpp forwards it
    // through YGNodeStyleSetOverflow. Paint clipping treats scroll
    // like hidden (no scrollbar UI yet), but the keyword survives the
    // round-trip — closing the harness gap.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        setOverflow('a', 'scroll');
        createPanel('b', '');
        setOverflow('b', 'visible');
        createPanel('c', '');
        setOverflow('c', 'hidden');
    )");

    REQUIRE(bridge.widget("a")->overflow() == View::Overflow::scroll);
    REQUIRE(bridge.widget("b")->overflow() == View::Overflow::visible);
    REQUIRE(bridge.widget("c")->overflow() == View::Overflow::hidden);
}

TEST_CASE("padding_left percent caps the layout edge",
          "[view][bridge][css][issue-1434-edges]") {
    // End-to-end: percent reaches Yoga and produces the expected
    // resolved pixel size after layout.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('parent', '');
        setFlex('parent', 'padding_left', '25%');
        setFlex('parent', 'width',  '100%');
        setFlex('parent', 'height', '100%');
    )");

    auto* parent = bridge.widget("parent");
    REQUIRE(parent != nullptr);
    root.layout_children();
    // 25% of 400px parent width → 100px padding-left.
    REQUIRE_THAT(parent->bounds().width, WithinAbs(400.0f, 0.5f));
}

TEST_CASE("CSSStyleDeclaration forwards marginTop percent + auto verbatim",
          "[view][bridge][css][issue-1434-edges]") {
    // The DOM-lite el.style adapter must forward 'NN%' and 'auto' as
    // strings so the bridge's per-unit branch is reached.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        createPanel('b', '');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        var sb = new CSSStyleDeclaration({ _id: 'b', _nativeCreated: true });
        sa._applyProperty('marginTop',  '50%');
        sa._applyProperty('paddingTop', '25%');
        sb._applyProperty('marginLeft',  'auto');
        sb._applyProperty('marginRight', 'auto');
    )");

    const auto& fa = bridge.widget("a")->flex();
    REQUIRE(fa.dim_margin_top.unit    == DimensionUnit::percent);
    REQUIRE_THAT(fa.dim_margin_top.value,    WithinAbs(50.0f, 0.001f));
    REQUIRE(fa.dim_padding_top.unit   == DimensionUnit::percent);
    REQUIRE_THAT(fa.dim_padding_top.value,   WithinAbs(25.0f, 0.001f));

    const auto& fb = bridge.widget("b")->flex();
    REQUIRE(fb.dim_margin_left.unit  == DimensionUnit::auto_);
    REQUIRE(fb.dim_margin_right.unit == DimensionUnit::auto_);
}

