// test_widget_bridge_canvas2d_bridge_fns.cpp — extracted from
// test_widget_bridge.cpp in the 2026-05 Phase 5 (P5-1 follow-up) refactor.
//
// Canvas2D bridge-fn cluster — three closely related bridge entry-points
// that thread JS-side Canvas2D semantics through the WidgetBridge into
// the native canvas pipeline:
//
//   * pulp #1434 — canvasSetFontFull bridge fn. Threads CSS-font
//     shorthand (font-style, font-variant, font-weight, font-size,
//     line-height, font-family) into the canvas pipeline.
//   * pulp #1522 — Canvas2D fillRule arg. fillRule="evenodd"|"nonzero"
//     threads through the dedicated canvasFill / canvasClip bridge fns.
//   * pulp #1520 — canvasSetDirection / canvasSetFilter bridge fns.
//     Routes canvas direction and filter-chain shim through the bridge.

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

// ── pulp #1434 — canvasSetFontFull bridge fn ─────────────────────────────
//
// The Canvas2D shim's full CSS font shorthand parser dispatches through
// `canvasSetFontFull(id, family, size, weight, slant, letterSpacing)`.
// Cover the bridge fn directly to lock in the recorded
// CanvasDrawCmd::set_font_full payload field-for-field, independent of
// the JS-side parse layer covered in test_canvas2d_shim.cpp.
TEST_CASE("WidgetBridge canvasSetFontFull records weight/slant verbatim",
          "[view][bridge][canvas][issue-1434]") {
    // Drive the bridge fn directly (bypassing the JS parser) and assert
    // the recorded CanvasDrawCmd carries the full payload.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'font-full-canvas';
        c.width = 100; c.height = 50;
        document.body.appendChild(c);
        // Bypass the JS parser — call the bridge fn directly with each
        // payload field so the recorded CanvasDrawCmd round-trips
        // verbatim.
        canvasSetFontFull(c._id, 'Inter', 18.0, 700, 1, 0.5);
    )");

    auto* canvas = canvasFromBridge(bridge, engine, "font-full-canvas");
    REQUIRE(canvas != nullptr);
    REQUIRE(canvas->command_count() == 1);

    const auto& cmd = canvas->commands().front();
    REQUIRE(cmd.type == pulp::view::CanvasDrawCmd::Type::set_font_full);
    REQUIRE(cmd.text == "Inter");
    REQUIRE_THAT(cmd.extra, WithinAbs(18.0f, 1e-5f));   // size
    REQUIRE_THAT(cmd.x,     WithinAbs(700.0f, 1e-5f));  // weight
    REQUIRE_THAT(cmd.y,     WithinAbs(1.0f, 1e-5f));    // slant=italic
    REQUIRE_THAT(cmd.x2,    WithinAbs(0.5f, 1e-5f));    // letter_spacing
}

TEST_CASE("WidgetBridge canvasSetFontFull replays through Canvas::set_font_full",
          "[view][bridge][canvas][issue-1434]") {
    // Drive a CanvasWidget paint onto a RecordingCanvas and assert the
    // backend received both the legacy set_font (back-compat) AND the
    // rich set_font_full carrying weight/slant. RecordingCanvas's
    // set_font_full override emits both per the existing #927 contract.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'font-full-replay';
        c.width = 100; c.height = 50;
        document.body.appendChild(c);
        canvasSetFontFull(c._id, 'Helvetica', 14.0, 300, 0, 0);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "font-full-replay");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    using DrawType = pulp::canvas::DrawCommand::Type;
    const pulp::canvas::DrawCommand* full = nullptr;
    for (const auto& c : rec.commands()) {
        if (c.type == DrawType::set_font_full) { full = &c; break; }
    }
    REQUIRE(full != nullptr);
    REQUIRE(full->text == "Helvetica");
    REQUIRE_THAT(full->f[0], WithinAbs(14.0f, 1e-5f));   // size
    REQUIRE_THAT(full->f[1], WithinAbs(300.0f, 1e-5f));  // weight
    REQUIRE_THAT(full->f[2], WithinAbs(0.0f, 1e-5f));    // slant=upright
}

// pulp #1434 (sub-agent #12 follow-up) — align_content multi-line
// flex cross-axis distribution. Yoga supports it natively via
// YGNodeStyleSetAlignContent; the gap was a missing FlexStyle field
// + setter wiring. Round-trip every value the bridge accepts so a
// regression in either the parser, the FlexStyle field, or the
// space-* sibling enum gets caught here rather than silently
// reverting Yoga to the default FlexStart.
TEST_CASE("setFlex align_content accepts start / end / center / stretch / space-* aliases",
          "[view][bridge][css][issue-1434-aligncontent]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  setFlex('a','align_content','start');
        createPanel('b','');  setFlex('b','align_content','flex-start');
        createPanel('c','');  setFlex('c','align_content','end');
        createPanel('d','');  setFlex('d','align_content','flex-end');
        createPanel('e','');  setFlex('e','align_content','center');
        createPanel('f','');  setFlex('f','align_content','stretch');
        createPanel('g','');  setFlex('g','align_content','space-between');
        createPanel('h','');  setFlex('h','align_content','space-around');
        createPanel('i','');  setFlex('i','align_content','space-evenly');
    )");

    using AcSpace = FlexStyle::AlignContentSpace;
    auto ac = [&](const std::string& id) { return bridge.widget(id)->flex().align_content; };
    auto sp = [&](const std::string& id) { return bridge.widget(id)->flex().align_content_space; };

    REQUIRE(ac("a") == FlexAlign::start);    REQUIRE(sp("a") == AcSpace::none);
    REQUIRE(ac("b") == FlexAlign::start);    REQUIRE(sp("b") == AcSpace::none);
    REQUIRE(ac("c") == FlexAlign::end);      REQUIRE(sp("c") == AcSpace::none);
    REQUIRE(ac("d") == FlexAlign::end);      REQUIRE(sp("d") == AcSpace::none);
    REQUIRE(ac("e") == FlexAlign::center);   REQUIRE(sp("e") == AcSpace::none);
    REQUIRE(ac("f") == FlexAlign::stretch);  REQUIRE(sp("f") == AcSpace::none);
    REQUIRE(sp("g") == AcSpace::space_between);
    REQUIRE(sp("h") == AcSpace::space_around);
    REQUIRE(sp("i") == AcSpace::space_evenly);
}

// pulp #1434 (sub-agent #12 follow-up) — width: 'auto' routes through
// the bridge's setFlex string path to FlexStyle.dim_width.unit =
// DimensionUnit::auto_. yoga_layout.cpp dispatches on that to
// YGNodeStyleSetWidthAuto. The percent path remains intact, and
// numeric values still flow through the px branch.
TEST_CASE("setFlex width accepts 'auto' keyword and routes to dim_width.auto_",
          "[view][bridge][css][issue-1434-auto]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  setFlex('a','width','auto');
        createPanel('b','');  setFlex('b','width', 120);
        createPanel('c','');  setFlex('c','width', '50%');
    )");

    const auto& fa = bridge.widget("a")->flex();
    REQUIRE(fa.dim_width.unit == DimensionUnit::auto_);
    REQUIRE(fa.preferred_width == 0.0f);

    const auto& fb = bridge.widget("b")->flex();
    REQUIRE(fb.dim_width.unit == DimensionUnit::px);
    REQUIRE_THAT(fb.preferred_width, WithinAbs(120.0f, 0.001f));

    const auto& fc = bridge.widget("c")->flex();
    REQUIRE(fc.dim_width.unit == DimensionUnit::percent);
    REQUIRE_THAT(fc.dim_width.value, WithinAbs(50.0f, 0.001f));
}

TEST_CASE("setFlex height accepts 'auto' keyword and routes to dim_height.auto_",
          "[view][bridge][css][issue-1434-auto]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  setFlex('a','height','auto');
        createPanel('b','');  setFlex('b','height', 80);
        createPanel('c','');  setFlex('c','height', '25%');
    )");

    const auto& fa = bridge.widget("a")->flex();
    REQUIRE(fa.dim_height.unit == DimensionUnit::auto_);
    REQUIRE(fa.preferred_height == 0.0f);

    const auto& fb = bridge.widget("b")->flex();
    REQUIRE(fb.dim_height.unit == DimensionUnit::px);
    REQUIRE_THAT(fb.preferred_height, WithinAbs(80.0f, 0.001f));

    const auto& fc = bridge.widget("c")->flex();
    REQUIRE(fc.dim_height.unit == DimensionUnit::percent);
    REQUIRE_THAT(fc.dim_height.value, WithinAbs(25.0f, 0.001f));
}

// pulp #1434 (sub-agent #12 follow-up) — verify the CSS shim path
// also forwards 'auto' for width/height. The DOM-lite el.style
// adapter must produce the same FlexStyle.dim_*.unit = auto_ result
// as the direct setFlex(id, 'width', 'auto') path.
TEST_CASE("CSSStyleDeclaration forwards width/height auto to bridge",
          "[view][bridge][css][issue-1434-auto]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        sa._applyProperty('width', 'auto');
        sa._applyProperty('height', 'auto');
    )");

    const auto& fa = bridge.widget("a")->flex();
    REQUIRE(fa.dim_width.unit  == DimensionUnit::auto_);
    REQUIRE(fa.dim_height.unit == DimensionUnit::auto_);
}

// ── pulp #1522 — Canvas2D fillRule arg threads through bridge fns ───────
//
// `canvasFillPath` and `canvasClip` accept an optional fillRule int
// (0 = nonzero/winding, 1 = evenodd). The bridge stores it on
// CanvasDrawCmd::int_val; the widget-level canvas2d shim tests in
// test_canvas2d_shim.cpp drive ctx.fill('evenodd')/ctx.clip('evenodd')
// end-to-end. This bridge-level test exercises the fns directly so a
// regression in the int_val plumbing surfaces here independent of the
// JS shim parser.
TEST_CASE("WidgetBridge canvasFillPath / canvasClip thread fillRule int_val",
          "[view][bridge][canvas][issue-1522]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'fillrule-canvas';
        c.width = 100; c.height = 100;
        document.body.appendChild(c);
        // Drive the bridge fns directly so we exercise int_val plumbing
        // without going through the JS shim arg parser.
        canvasBeginPath(c._id);
        canvasMoveTo(c._id, 0, 0);
        canvasLineTo(c._id, 10, 0);
        canvasLineTo(c._id, 10, 10);
        canvasLineTo(c._id, 0, 10);
        canvasClosePath(c._id);
        canvasFillPath(c._id, 1);     // evenodd
        canvasBeginPath(c._id);
        canvasMoveTo(c._id, 0, 0);
        canvasLineTo(c._id, 10, 0);
        canvasLineTo(c._id, 10, 10);
        canvasClosePath(c._id);
        canvasFillPath(c._id);        // default (nonzero)
        canvasBeginPath(c._id);
        canvasMoveTo(c._id, 0, 0);
        canvasLineTo(c._id, 10, 0);
        canvasLineTo(c._id, 10, 10);
        canvasClosePath(c._id);
        canvasClip(c._id, 1);         // evenodd
        canvasBeginPath(c._id);
        canvasMoveTo(c._id, 0, 0);
        canvasLineTo(c._id, 10, 0);
        canvasLineTo(c._id, 10, 10);
        canvasClosePath(c._id);
        canvasClip(c._id);            // default (nonzero)
    )");

    auto* canvas = canvasFromBridge(bridge, engine, "fillrule-canvas");
    REQUIRE(canvas != nullptr);

    using T = pulp::view::CanvasDrawCmd::Type;
    std::vector<int> fill_int_vals;
    std::vector<int> clip_int_vals;
    for (const auto& cmd : canvas->commands()) {
        if (cmd.type == T::fill_path) fill_int_vals.push_back(cmd.int_val);
        if (cmd.type == T::clip)      clip_int_vals.push_back(cmd.int_val);
    }
    REQUIRE(fill_int_vals.size() == 2);
    REQUIRE(fill_int_vals[0] == 1);   // explicit evenodd
    REQUIRE(fill_int_vals[1] == 0);   // default nonzero
    REQUIRE(clip_int_vals.size() == 2);
    REQUIRE(clip_int_vals[0] == 1);   // explicit evenodd
    REQUIRE(clip_int_vals[1] == 0);   // default nonzero
}

// ── pulp #1520 — canvasSetDirection / canvasSetFilter bridge fns ────────
//
// These two register_function entries are the only direct surface
// between the Canvas2D ctx.direction / ctx.filter setters and the
// underlying canvas state. The shim's own coverage lives in
// test_canvas2d_shim.cpp; this test asserts the bridge fn → canvas
// command record path with no JS-side caching in the way.

TEST_CASE("WidgetBridge canvasSetDirection records direction enum on the canvas command stream",
          "[view][bridge][canvas][issue-1520]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 100});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'dir-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        // Drive the bridge fn directly so the cache in the JS shim
        // can't suppress the call.
        canvasSetDirection(c._id, 1);  // rtl
        canvasSetDirection(c._id, 2);  // inherit
        canvasSetDirection(c._id, 0);  // ltr
        canvasSetDirection(c._id, 99); // invalid → coerced to ltr
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "dir-canvas");
    REQUIRE(canvas != nullptr);
    using T = pulp::view::CanvasDrawCmd::Type;
    std::vector<int> values;
    for (const auto& cmd : canvas->commands()) {
        if (cmd.type == T::set_direction) values.push_back(cmd.int_val);
    }
    REQUIRE(values.size() == 4);
    REQUIRE(values[0] == 1);
    REQUIRE(values[1] == 2);
    REQUIRE(values[2] == 0);
    REQUIRE(values[3] == 0); // out-of-range coerced to ltr
}

TEST_CASE("WidgetBridge canvasSetFilter records the raw CSS filter string",
          "[view][bridge][canvas][issue-1520]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 100});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'filter-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        // Bypass the JS-side _syncFilterState cache; drive the bridge
        // fn directly so each call records.
        canvasSetFilter(c._id, 'blur(5px)');
        canvasSetFilter(c._id, 'sepia(80%) hue-rotate(45deg)');
        canvasSetFilter(c._id, 'none');
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "filter-canvas");
    REQUIRE(canvas != nullptr);
    using T = pulp::view::CanvasDrawCmd::Type;
    std::vector<std::string> sources;
    for (const auto& cmd : canvas->commands()) {
        if (cmd.type == T::set_filter) sources.push_back(cmd.text);
    }
    REQUIRE(sources.size() == 3);
    REQUIRE(sources[0] == "blur(5px)");
    REQUIRE(sources[1] == "sepia(80%) hue-rotate(45deg)");
    REQUIRE(sources[2] == "none");
}

TEST_CASE("WidgetBridge canvasSetFilter chain replays through to the recording canvas",
          "[view][bridge][canvas][issue-1520]") {
    // End-to-end: bridge fn → CanvasWidget command → RecordingCanvas
    // capture. Asserts the dispatch table in canvas_widget.cpp wires
    // set_filter through to Canvas::set_filter() and that the
    // RecordingCanvas captures the same string.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 100});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'filter-replay-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        canvasSetFilter(c._id, 'blur(3px) sepia(50%)');
        canvasSetDirection(c._id, 1);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "filter-replay-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    using DT = pulp::canvas::DrawCommand::Type;
    bool saw_filter = false, saw_direction = false;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == DT::set_filter) {
            saw_filter = true;
            REQUIRE(cmd.text == "blur(3px) sepia(50%)");
        }
        if (cmd.type == DT::set_direction) {
            saw_direction = true;
            // RTL = enum value 1 (TextDirection::rtl).
            REQUIRE(cmd.f[0] == static_cast<float>(
                pulp::canvas::Canvas::TextDirection::rtl));
        }
    }
    REQUIRE(saw_filter);
    REQUIRE(saw_direction);
}

