// test_widget_bridge_svg.cpp — extracted from test_widget_bridge.cpp
// in the 2026-05 Phase 5 (P5-1 follow-up) refactor.
//
// SVG widget JS-bridge integration, three coherent compat surfaces:
//
//   1. pulp #965 — SvgPathWidget JS bridge integration.
//   2. pulp #1416 — SvgRectWidget + SvgLineWidget JS bridge integration.
//   3. Compound-path SVG icons (Spectr PEAK / AVG / BOTH / OFF) —
//      regression-pins the parser's enumeration of every M and L across
//      disjoint subpaths, separate from the main #965 cluster.

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
#include <pulp/view/svg_path_widget.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <thread>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

// Local copy of canvasFromBridge (file-static in parent + sibling TUs).
namespace {
pulp::view::CanvasWidget* canvasFromBridge(pulp::view::WidgetBridge& bridge,
                                            pulp::view::ScriptEngine& engine,
                                            const std::string& id) {
    auto value = engine.evaluate("document.getElementById('" + id + "')._id");
    auto nativeId = std::string(value.getWithDefault<std::string_view>(""));
    return dynamic_cast<pulp::view::CanvasWidget*>(bridge.widget(nativeId));
}
} // namespace

// ── pulp #965 — SvgPathWidget JS bridge integration ──────────────────────────

#include <pulp/view/svg_path_widget.hpp>

TEST_CASE("WidgetBridge createSvgPath produces an SvgPathWidget the bridge can address",
          "[view][bridge][issue-965]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgPath('icon', '')");
    bridge.load_script("setSvgPath('icon', 'M 0 0 L 10 0 L 10 10 Z')");
    bridge.load_script("setSvgViewBox('icon', 10, 10)");
    bridge.load_script("setSvgFill('icon', '#ff0000')");
    bridge.load_script("setSvgStroke('icon', '#000000')");
    bridge.load_script("setSvgStrokeWidth('icon', 2.0)");

    auto* w = dynamic_cast<SvgPathWidget*>(bridge.widget("icon"));
    REQUIRE(w != nullptr);
    REQUIRE(w->path_data() == "M 0 0 L 10 0 L 10 10 Z");
    REQUIRE(w->segments().size() == 4);
    REQUIRE(w->viewbox_width() == 10.0f);
    REQUIRE(w->viewbox_height() == 10.0f);
    REQUIRE(w->has_fill());
    REQUIRE(w->has_stroke());
    REQUIRE(w->stroke_width() == 2.0f);
    REQUIRE(w->fill_color().r8() == 255);
    REQUIRE(w->fill_color().g8() == 0);
    REQUIRE(w->fill_color().b8() == 0);
}

TEST_CASE("WidgetBridge setSvgFill 'none' disables fill",
          "[view][bridge][issue-965]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgPath('a', '')");
    bridge.load_script("setSvgPath('a', 'M0 0L1 1')");
    bridge.load_script("setSvgFill('a', 'none')");
    bridge.load_script("setSvgStroke('a', '#222222')");
    bridge.load_script("setSvgStrokeWidth('a', 1.5)");

    auto* w = dynamic_cast<SvgPathWidget*>(bridge.widget("a"));
    REQUIRE(w != nullptr);
    REQUIRE_FALSE(w->has_fill());
    REQUIRE(w->has_stroke());
}

// pulp #968 — canvasRect / canvasStrokeRect must honour the active fill /
// stroke style when no color arg is passed. Validates the JS bridge path:
//   1. five-arg canvasRect → fillStyle (color or gradient) wins
//   2. six-arg canvasRect with explicit color → explicit color wins
//   3. linear gradient set, then five-arg canvasRect → gradient wins
//   4. five-arg canvasStrokeRect → strokeStyle wins
TEST_CASE("WidgetBridge canvasRect with no color uses active fillStyle (issue-968)",
          "[view][bridge][canvas][issue-968]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'fill-style-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);

        // Active fill = magenta. Then a five-arg canvasRect (no color).
        canvasSetFillColor(c._id, '#ff00ff');
        canvasRect(c._id, 10, 10, 50, 50);

        // Explicit color (six-arg) — overrides active fill.
        canvasSetFillColor(c._id, '#ff00ff');
        canvasRect(c._id, 70, 10, 50, 50, '#00ffff');
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "fill-style-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    using DrawType = pulp::canvas::DrawCommand::Type;
    pulp::canvas::Color active_fill{};
    pulp::canvas::Color first_rect_fill{};
    pulp::canvas::Color second_rect_fill{};
    bool saw_first = false, saw_second = false;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == DrawType::set_fill_color) {
            active_fill = cmd.color;
            continue;
        }
        if (cmd.type == DrawType::fill_rect) {
            const bool is_first  = (cmd.f[0] == 10.0f && cmd.f[1] == 10.0f &&
                                    cmd.f[2] == 50.0f && cmd.f[3] == 50.0f);
            const bool is_second = (cmd.f[0] == 70.0f && cmd.f[1] == 10.0f &&
                                    cmd.f[2] == 50.0f && cmd.f[3] == 50.0f);
            if (is_first  && !saw_first)  { saw_first  = true; first_rect_fill  = active_fill; }
            if (is_second && !saw_second) { saw_second = true; second_rect_fill = active_fill; }
        }
    }
    REQUIRE(saw_first);
    REQUIRE(saw_second);
    // First rect (no color arg) → magenta (the active fill at the time).
    const bool first_is_magenta = (first_rect_fill.r8() == 255 &&
                                   first_rect_fill.g8() == 0 &&
                                   first_rect_fill.b8() == 255 &&
                                   first_rect_fill.a8() == 255);
    REQUIRE(first_is_magenta);
    // Second rect (explicit color arg) → cyan, overriding the active fill.
    const bool second_is_cyan = (second_rect_fill.r8() == 0 &&
                                 second_rect_fill.g8() == 255 &&
                                 second_rect_fill.b8() == 255 &&
                                 second_rect_fill.a8() == 255);
    REQUIRE(second_is_cyan);
}

TEST_CASE("WidgetBridge canvasRect with no color preserves active linear gradient (issue-968)",
          "[view][bridge][canvas][issue-968]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Linear gradient red→blue along x. RecordingCanvas's default
    // set_fill_gradient_linear records a set_fill_color of the first
    // stop (red) — we use that as the proxy for "gradient is active".
    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'grad-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);

        canvasSetLinearGradient(c._id, 0, 0, 100, 0, '#ff0000', 0, '#0000ff', 1);
        canvasRect(c._id, 10, 10, 50, 50);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "grad-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    using DrawType = pulp::canvas::DrawCommand::Type;
    pulp::canvas::Color active_fill{};
    bool saw_rect = false;
    pulp::canvas::Color rect_fill{};
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == DrawType::set_fill_color) {
            active_fill = cmd.color;
            continue;
        }
        if (cmd.type == DrawType::fill_rect) {
            const bool matches = (cmd.f[0] == 10.0f && cmd.f[1] == 10.0f &&
                                  cmd.f[2] == 50.0f && cmd.f[3] == 50.0f);
            if (matches) {
                saw_rect = true;
                rect_fill = active_fill;
            }
        }
    }
    REQUIRE(saw_rect);
    // The gradient's first stop (red) must still be the active fill —
    // i.e. no white set_fill_color from a baked-in cmd.color was emitted
    // between the gradient set and the fill_rect.
    const bool is_red = (rect_fill.r8() == 255 && rect_fill.g8() == 0 &&
                         rect_fill.b8() == 0 && rect_fill.a8() == 255);
    REQUIRE(is_red);
}

TEST_CASE("WidgetBridge canvasStrokeRect with no color uses active strokeStyle (issue-968)",
          "[view][bridge][canvas][issue-968]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'stroke-style-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);

        canvasSetStrokeColor(c._id, '#00ff00');
        canvasStrokeRect(c._id, 5, 5, 40, 40);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "stroke-style-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    using DrawType = pulp::canvas::DrawCommand::Type;
    pulp::canvas::Color active_stroke{};
    bool saw_rect = false;
    pulp::canvas::Color stroke_at_draw{};
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == DrawType::set_stroke_color) {
            active_stroke = cmd.color;
            continue;
        }
        if (cmd.type == DrawType::stroke_rect) {
            const bool matches = (cmd.f[0] == 5.0f && cmd.f[1] == 5.0f &&
                                  cmd.f[2] == 40.0f && cmd.f[3] == 40.0f);
            if (matches) {
                saw_rect = true;
                stroke_at_draw = active_stroke;
            }
        }
    }
    REQUIRE(saw_rect);
    const bool is_green = (stroke_at_draw.r8() == 0 && stroke_at_draw.g8() == 255 &&
                           stroke_at_draw.b8() == 0 && stroke_at_draw.a8() == 255);
    REQUIRE(is_green);
}


// ── pulp #1416 — SvgRectWidget + SvgLineWidget JS bridge integration ─────────
//
// Mirrors the #965 SvgPath bridge tests. Closes Spectr [G] preset
// manager band-shape thumbnails: MiniPreview renders <svg><rect> per
// band + <line> separators, which dom-adapter routes to <View> with
// SVG attribute props. Without these bridge handlers the geometry is
// dropped on the floor and the tiles render blank.

#include <pulp/view/widgets/svg_line.hpp>
#include <pulp/view/widgets/svg_rect.hpp>

TEST_CASE("WidgetBridge createSvgRect produces a SvgRectWidget the bridge can address",
          "[view][bridge][issue-1416]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgRect('bar', '')");
    bridge.load_script("setSvgRect('bar', 10, 20, 50, 30)");
    bridge.load_script("setSvgFill('bar', '#ff0000')");
    bridge.load_script("setSvgStroke('bar', '#000000')");
    bridge.load_script("setSvgStrokeWidth('bar', 2.0)");

    auto* w = dynamic_cast<SvgRectWidget*>(bridge.widget("bar"));
    REQUIRE(w != nullptr);
    REQUIRE(w->rect_x() == 10.0f);
    REQUIRE(w->rect_y() == 20.0f);
    REQUIRE(w->rect_width() == 50.0f);
    REQUIRE(w->rect_height() == 30.0f);
    REQUIRE(w->has_fill());
    REQUIRE(w->has_stroke());
    REQUIRE(w->stroke_width() == 2.0f);
    REQUIRE(w->fill_color().r8() == 255);
    REQUIRE(w->fill_color().g8() == 0);
    REQUIRE(w->fill_color().b8() == 0);
}

TEST_CASE("WidgetBridge setSvgFill 'none' disables fill on SvgRectWidget",
          "[view][bridge][issue-1416]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgRect('a', '')");
    bridge.load_script("setSvgRect('a', 0, 0, 10, 10)");
    bridge.load_script("setSvgFill('a', 'none')");
    bridge.load_script("setSvgStroke('a', '#222222')");
    bridge.load_script("setSvgStrokeWidth('a', 1.5)");

    auto* w = dynamic_cast<SvgRectWidget*>(bridge.widget("a"));
    REQUIRE(w != nullptr);
    REQUIRE_FALSE(w->has_fill());
    REQUIRE(w->has_stroke());
    REQUIRE(w->stroke_width() == 1.5f);
}

TEST_CASE("WidgetBridge createSvgRect + paint produces fill_rect at expected geometry",
          "[view][bridge][issue-1416]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgRect('bar', '')");
    bridge.load_script("setSvgRect('bar', 5, 6, 40, 8)");
    bridge.load_script("setSvgFill('bar', '#00ff00')");

    auto* w = dynamic_cast<SvgRectWidget*>(bridge.widget("bar"));
    REQUIRE(w != nullptr);

    pulp::canvas::RecordingCanvas rc;
    w->paint(rc);

    bool saw_fill = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rect) {
            REQUIRE(cmd.f[0] == 5.0f);
            REQUIRE(cmd.f[1] == 6.0f);
            REQUIRE(cmd.f[2] == 40.0f);
            REQUIRE(cmd.f[3] == 8.0f);
            saw_fill = true;
        }
    }
    REQUIRE(saw_fill);
}

TEST_CASE("WidgetBridge createSvgLine produces a SvgLineWidget the bridge can address",
          "[view][bridge][issue-1416]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgLine('sep', '')");
    bridge.load_script("setSvgLine('sep', 0, 10, 100, 10)");
    bridge.load_script("setSvgStroke('sep', '#0000ff')");
    bridge.load_script("setSvgStrokeWidth('sep', 1.5)");

    auto* w = dynamic_cast<SvgLineWidget*>(bridge.widget("sep"));
    REQUIRE(w != nullptr);
    REQUIRE(w->x1() == 0.0f);
    REQUIRE(w->y1() == 10.0f);
    REQUIRE(w->x2() == 100.0f);
    REQUIRE(w->y2() == 10.0f);
    REQUIRE(w->has_stroke());
    REQUIRE(w->stroke_width() == 1.5f);
    REQUIRE(w->stroke_color().r8() == 0);
    REQUIRE(w->stroke_color().g8() == 0);
    REQUIRE(w->stroke_color().b8() == 255);
}

TEST_CASE("WidgetBridge setSvgStroke 'none' disables stroke on SvgLineWidget",
          "[view][bridge][issue-1416]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgLine('l', '')");
    bridge.load_script("setSvgLine('l', 0, 0, 10, 10)");
    bridge.load_script("setSvgStroke('l', 'none')");

    auto* w = dynamic_cast<SvgLineWidget*>(bridge.widget("l"));
    REQUIRE(w != nullptr);
    REQUIRE_FALSE(w->has_stroke());
}

TEST_CASE("WidgetBridge createSvgLine + paint emits stroke_line at endpoints",
          "[view][bridge][issue-1416]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createSvgLine('sep', '')");
    bridge.load_script("setSvgLine('sep', 1, 2, 11, 12)");
    bridge.load_script("setSvgStroke('sep', '#ff00ff')");
    bridge.load_script("setSvgStrokeWidth('sep', 2.5)");

    auto* w = dynamic_cast<SvgLineWidget*>(bridge.widget("sep"));
    REQUIRE(w != nullptr);

    pulp::canvas::RecordingCanvas rc;
    w->paint(rc);

    bool saw_line = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::stroke_line) {
            REQUIRE(cmd.f[0] == 1.0f);
            REQUIRE(cmd.f[1] == 2.0f);
            REQUIRE(cmd.f[2] == 11.0f);
            REQUIRE(cmd.f[3] == 12.0f);
            saw_line = true;
        }
    }
    REQUIRE(saw_line);
}

TEST_CASE("WidgetBridge SvgRect uses parent for hierarchy attachment",
          "[view][bridge][issue-1416]") {
    // The createSvgRect bridge handler accepts a parent_id so JSX can
    // mount band thumbnails inside their MiniPreview row.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createCol('preview', '')");
    bridge.load_script("createSvgRect('band1', 'preview')");
    bridge.load_script("createSvgRect('band2', 'preview')");
    bridge.load_script("createSvgLine('axis', 'preview')");

    REQUIRE(bridge.widget("band1") != nullptr);
    REQUIRE(bridge.widget("band2") != nullptr);
    REQUIRE(bridge.widget("axis") != nullptr);
    auto* preview = bridge.widget("preview");
    REQUIRE(preview != nullptr);
    REQUIRE(preview->child_count() == 3);
}

// pulp #1410 — setWhiteSpace must (a) flip the generic
// `View::white_space_nowrap()` flag for ANY widget (not just Label) so
// non-Label text-bearing surfaces can react, and (b) keep
// `Label::set_multi_line` in lock-step so existing callers / the #1407
// ellipsis path keep working when only one of the flags is set.
TEST_CASE("WidgetBridge setWhiteSpace flips View flag and Label multi_line for both modes",
          "[view][bridge][css][issue-1410]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('mylabel', 'long preset name', '');
        createPanel('mypanel', '');
        setWhiteSpace('mylabel', 'nowrap');
        setWhiteSpace('mypanel', 'nowrap');
    )");

    auto* label = dynamic_cast<Label*>(bridge.widget("mylabel"));
    auto* panel = bridge.widget("mypanel");
    REQUIRE(label != nullptr);
    REQUIRE(panel != nullptr);

    // Generic flag is set on BOTH the Label and the non-Label Panel —
    // before #1410 only the Label dynamic_cast branch handled it.
    REQUIRE(label->white_space_nowrap());
    REQUIRE(panel->white_space_nowrap());
    // Label's multi_line side-effect stays in lock-step.
    REQUIRE_FALSE(label->multi_line());

    // Toggle back to normal.
    bridge.load_script(R"(
        setWhiteSpace('mylabel', 'normal');
        setWhiteSpace('mypanel', 'normal');
    )");
    REQUIRE_FALSE(label->white_space_nowrap());
    REQUIRE_FALSE(panel->white_space_nowrap());
    REQUIRE(label->multi_line());
}

// pulp #1737 — full CSS white-space enum. Beyond normal/nowrap (#1410
// covered), the bridge now routes pre / pre-wrap / pre-line /
// break-spaces to View::WhiteSpaceMode and toggles Label.multi_line
// per spec semantics:
//   pre          → no wrap (treat like nowrap for Label)
//   pre-wrap     → wrap
//   pre-line     → wrap
//   break-spaces → wrap
// The legacy white_space_nowrap() bool is true for { nowrap, pre }
// so existing consumers (text shaper) keep working.
TEST_CASE("WidgetBridge setWhiteSpace routes all 6 CSS keywords to WhiteSpaceMode",
          "[view][bridge][css][issue-1737]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('lbl', 'multi line text', '');
    )");
    auto* label = dynamic_cast<Label*>(bridge.widget("lbl"));
    REQUIRE(label != nullptr);

    using M = View::WhiteSpaceMode;
    struct Case {
        const char* keyword;
        M expected_mode;
        bool expected_multi_line;
        bool expected_nowrap_bool;
    } cases[] = {
        // pulp #1737 (Codex P1 followup on #1786): `pre` keeps
        // multi_line=true so newlines render. The spec's "no-soft-
        // wrap" semantic for `pre` is a Label-side follow-up;
        // newline preservation is the spec-critical part.
        {"normal",       M::normal,       true,  false},
        {"nowrap",       M::nowrap,       false, true },
        {"pre",          M::pre,          true,  true },
        {"pre-wrap",     M::pre_wrap,     true,  false},
        {"pre-line",     M::pre_line,     true,  false},
        {"break-spaces", M::break_spaces, true,  false},
    };
    for (const auto& c : cases) {
        std::string js = std::string("setWhiteSpace('lbl', '") + c.keyword + "')";
        bridge.load_script(js);
        INFO("white-space keyword: " << c.keyword);
        REQUIRE(label->white_space_mode() == c.expected_mode);
        REQUIRE(label->multi_line() == c.expected_multi_line);
        REQUIRE(label->white_space_nowrap() == c.expected_nowrap_bool);
    }

    // Unknown keyword falls back to normal per CSS forward-compat.
    bridge.load_script("setWhiteSpace('lbl', 'mystery-future-keyword')");
    REQUIRE(label->white_space_mode() == M::normal);
    REQUIRE(label->multi_line());

    // pulp #1737 (Codex P2 followup on #1786): the legacy
    // set_white_space_nowrap() setter MUST keep the WhiteSpaceMode
    // enum in sync. Pre-fix, the legacy setter only touched the bool
    // and left the enum stale (still `normal` after a `set_white_space_nowrap(true)`
    // call), violating the stated backward-compat contract.
    label->set_white_space_nowrap(true);
    REQUIRE(label->white_space_mode() == M::nowrap);
    REQUIRE(label->white_space_nowrap());
    label->set_white_space_nowrap(false);
    REQUIRE(label->white_space_mode() == M::normal);
    REQUIRE_FALSE(label->white_space_nowrap());
}

// pulp #1410 — CSS translator side. style.whiteSpace = 'nowrap' must
// route through CSSStyleDeclaration._applyProperty to setWhiteSpace,
// which then sets the View flag.
TEST_CASE("CSSStyleDeclaration translates whiteSpace to setWhiteSpace bridge call",
          "[view][bridge][css][issue-1410]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        globalThis.__wsCalls = [];
        var __native_setWhiteSpace = setWhiteSpace;
        setWhiteSpace = function(id, mode) {
            globalThis.__wsCalls.push(id + '|' + mode);
            return __native_setWhiteSpace(id, mode);
        };
        createLabel('mylabel', 'long preset name', '');
        var stub_el = { _id: 'mylabel', _nativeCreated: true };
        var sd = new CSSStyleDeclaration(stub_el);
        sd._applyProperty('whiteSpace', 'nowrap');
    )");

    auto count = engine.evaluate("globalThis.__wsCalls.length")
                       .getWithDefault<double>(-1);
    REQUIRE(count == 1);
    auto recorded = engine.evaluate("globalThis.__wsCalls[0]")
                          .getWithDefault<std::string>("");
    REQUIRE(recorded == "mylabel|nowrap");

    auto* label = dynamic_cast<Label*>(bridge.widget("mylabel"));
    REQUIRE(label != nullptr);
    REQUIRE(label->white_space_nowrap());
}

// pulp #1423 — `width: '100%'` and `height: '100%'` propagate through the
// CSS translator and bridge to Yoga's percent API. Spectr uses the
// `width:'100%'` form at spectr-editor-extracted.js:2377 and :3414.
TEST_CASE("CSS width/height percent strings propagate to Yoga via setFlex",
          "[view][bridge][css][issue-1423]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        var stub = { _id: 'child', _nativeCreated: true };
        var sd = new CSSStyleDeclaration(stub);
        sd._applyProperty('width', '100%');
        sd._applyProperty('height', '50%');
    )");

    auto* child = bridge.widget("child");
    REQUIRE(child != nullptr);

    // FlexStyle.dim_width / dim_height now carry the percent unit, so
    // yoga_layout.cpp will emit YGNodeStyleSetWidthPercent/HeightPercent.
    const auto& f = child->flex();
    REQUIRE(f.dim_width.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_width.value, WithinAbs(100.0f, 0.001f));
    REQUIRE(f.dim_height.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_height.value, WithinAbs(50.0f, 0.001f));

    // After layout against the 400x200 root, the child should be laid
    // out as 400 wide (100% of parent) and 100 tall (50% of parent).
    root.layout_children();
    REQUIRE_THAT(child->bounds().width, WithinAbs(400.0f, 0.5f));
    REQUIRE_THAT(child->bounds().height, WithinAbs(100.0f, 0.5f));
}

// pulp #1423 — px values still work after the percent-aware refactor.
// Regression guard: the old code path stored only `preferred_width`;
// the new path also stores into `dim_width.unit = px`. Layout must keep
// using the px size when no percent was specified.
TEST_CASE("CSS width/height px paths unchanged by percent support",
          "[view][bridge][css][issue-1423]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        var stub = { _id: 'child', _nativeCreated: true };
        var sd = new CSSStyleDeclaration(stub);
        sd._applyProperty('width', '120px');
        sd._applyProperty('height', '80px');
    )");

    auto* child = bridge.widget("child");
    REQUIRE(child != nullptr);
    const auto& f = child->flex();
    REQUIRE(f.dim_width.unit == DimensionUnit::px);
    REQUIRE_THAT(f.preferred_width, WithinAbs(120.0f, 0.001f));
    REQUIRE_THAT(f.preferred_height, WithinAbs(80.0f, 0.001f));

    root.layout_children();
    REQUIRE_THAT(child->bounds().width, WithinAbs(120.0f, 0.5f));
    REQUIRE_THAT(child->bounds().height, WithinAbs(80.0f, 0.5f));
}

// pulp #1434 batch 6 — `top: '50%'`, `right`, `bottom`, `left` percent
// strings propagate through the CSS translator and bridge to Yoga's
// `YGNodeStyleSetPositionPercent`. Mirrors the issue-1423 width/height
// percent path; the four View positional fields previously dropped the
// `%` suffix at the bridge boundary.
TEST_CASE("CSS top/right/bottom/left percent strings propagate to Yoga",
          "[view][bridge][css][issue-1434]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        var stub = { _id: 'child', _nativeCreated: true };
        var sd = new CSSStyleDeclaration(stub);
        sd._applyProperty('position', 'absolute');
        sd._applyProperty('top', '50%');
        sd._applyProperty('left', '25%');
        sd._applyProperty('right', '10%');
        sd._applyProperty('bottom', '0%');
    )");

    auto* child = bridge.widget("child");
    REQUIRE(child != nullptr);

    // View::top_unit_ / etc. now carry the percent unit, so
    // yoga_layout.cpp will emit YGNodeStyleSetPositionPercent.
    REQUIRE(child->has_top());
    REQUIRE(child->top_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->top(), WithinAbs(50.0f, 0.001f));

    REQUIRE(child->has_left());
    REQUIRE(child->left_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->left(), WithinAbs(25.0f, 0.001f));

    REQUIRE(child->has_right());
    REQUIRE(child->right_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->right(), WithinAbs(10.0f, 0.001f));

    REQUIRE(child->has_bottom());
    REQUIRE(child->bottom_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->bottom(), WithinAbs(0.0f, 0.001f));
}

// pulp #1434 batch 6 — px positional values still work after the
// percent-aware refactor. Regression guard: the existing single-arg
// View::set_top setter must keep top_unit_ at px so layout_children
// uses YGNodeStyleSetPosition (not Percent).
TEST_CASE("CSS top/right/bottom/left px paths unchanged by percent support",
          "[view][bridge][css][issue-1434]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        var stub = { _id: 'child', _nativeCreated: true };
        var sd = new CSSStyleDeclaration(stub);
        sd._applyProperty('position', 'absolute');
        sd._applyProperty('top', '12px');
        sd._applyProperty('left', '34px');
        sd._applyProperty('right', '56px');
        sd._applyProperty('bottom', '78px');
    )");

    auto* child = bridge.widget("child");
    REQUIRE(child != nullptr);
    REQUIRE(child->top_unit() == DimensionUnit::px);
    REQUIRE_THAT(child->top(), WithinAbs(12.0f, 0.001f));
    REQUIRE(child->left_unit() == DimensionUnit::px);
    REQUIRE_THAT(child->left(), WithinAbs(34.0f, 0.001f));
    REQUIRE(child->right_unit() == DimensionUnit::px);
    REQUIRE_THAT(child->right(), WithinAbs(56.0f, 0.001f));
    REQUIRE(child->bottom_unit() == DimensionUnit::px);
    REQUIRE_THAT(child->bottom(), WithinAbs(78.0f, 0.001f));
}

// pulp #1434 batch 6 — direct bridge entry-point coverage. The CSS
// translator path is exercised by the test above; this case calls the
// bridge's setTop/setRight/setBottom/setLeft directly so the @pulp/react
// JSX path (which forwards `'NN%'` strings without going through the
// CSS translator) is also covered.
TEST_CASE("setTop/setRight/setBottom/setLeft accept percent strings directly",
          "[view][bridge][issue-1434]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        setTop('child', '50%');
        setRight('child', '25%');
        setBottom('child', '10%');
        setLeft('child', '0%');
    )");

    auto* child = bridge.widget("child");
    REQUIRE(child != nullptr);
    REQUIRE(child->top_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->top(), WithinAbs(50.0f, 0.001f));
    REQUIRE(child->right_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->right(), WithinAbs(25.0f, 0.001f));
    REQUIRE(child->bottom_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->bottom(), WithinAbs(10.0f, 0.001f));
    REQUIRE(child->left_unit() == DimensionUnit::percent);
    REQUIRE_THAT(child->left(), WithinAbs(0.0f, 0.001f));
}



// ────────────────────────────────────────────────────────────────────────────
// Compound-path SVG icons (Spectr PEAK / AVG / BOTH / OFF analyzer icons)
//
// Spectr's analyzer dropdown uses paths like:
//   "M 3 20 L 3 14 M 7 20 L 7 10 M 11 20 L 11 6 ..."
// which is 10 disjoint subpaths each a vertical/horizontal line. User
// reports these icons render BLANK while single-subpath icons (e.g. the
// SCULPT M-Q-T-T-T wave) render correctly. The parser is supposed to
// emit one move_to + one line_to per pair → 20 segments total.
//
// This test pins:
//  (a) the parser correctly enumerates every M and L
//  (b) sibling <path> elements inside one <svg> each get their own widget
// so we can tell parse-time vs paint-time when the icon goes blank.
// ────────────────────────────────────────────────────────────────────────────
TEST_CASE("WidgetBridge SvgPath compound multi-subpath parses every segment",
          "[view][bridge][issue-965][compound-path]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Spectr PEAK analyzer icon — 10 subpaths (5 bars + 5 tick marks).
    const std::string peak_d =
        "M 3 20 L 3 14 M 7 20 L 7 10 M 11 20 L 11 6 "
        "M 15 20 L 15 12 M 19 20 L 19 8 M 1 14 L 5 14 "
        "M 5 10 L 9 10 M 9 6 L 13 6 M 13 12 L 17 12 M 17 8 L 21 8";

    bridge.load_script(std::string("createSvgPath('peak', '')"));
    bridge.load_script("setSvgPath('peak', '" + peak_d + "')");
    // Spectr JSX `<path fill="none" stroke="currentColor" strokeWidth="1.3">`
    // dispatches setSvgFill('none') before stroke setup:
    bridge.load_script("setSvgFill('peak', 'none')");
    bridge.load_script("setSvgStroke('peak', '#ffffff')");
    bridge.load_script("setSvgStrokeWidth('peak', 1.3)");

    auto* w = dynamic_cast<SvgPathWidget*>(bridge.widget("peak"));
    REQUIRE(w != nullptr);
    // 10 (M) + 10 (L) = 20 segments — parser MUST emit every M and L.
    REQUIRE(w->segments().size() == 20);
    REQUIRE(w->has_stroke());
    REQUIRE_FALSE(w->has_fill());

    // Count move_to and line_to ops to make sure neither is being
    // silently merged or dropped.
    int moves = 0, lines = 0;
    for (const auto& s : w->segments()) {
        if (s.op == SvgPathSegment::Op::move_to) ++moves;
        else if (s.op == SvgPathSegment::Op::line_to) ++lines;
    }
    REQUIRE(moves == 10);
    REQUIRE(lines == 10);
}

