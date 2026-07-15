// WidgetBridge recovered Canvas2D/CSS compatibility tests.
//
// The canonical Wave 2 Canvas2D cheap-wiring tests live in
// `test_widget_bridge_wave2_cheap.cpp`: fill/clip evenodd,
// roundRect radii, ellipse rotation, and strokeText dedicated-command
// routing. This split keeps recovered CSS cases and later Canvas2D
// bridge regressions that were separated from the original merge cleanup.
//
// Each test goes JS → bridge → CanvasWidget::paint(RecordingCanvas) →
// asserts on the recorded Canvas API call so a regression anywhere in
// the chain surfaces here. Skia / CG paint-side honouring of FillRule
// and kStroke_Style is unit-tested at the Canvas backend layer; here we
// focus on the bridge ↔ Canvas API contract.

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
#include <thread>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

// Local copy of canvasFromBridge — the parent test_widget_bridge.cpp +
// test_widget_bridge_canvas2d.cpp both define this helper as `static`.
// Duplicate here to keep the split self-contained until a shared
// test/test_widget_bridge_helpers.hpp lands.
namespace {
pulp::view::CanvasWidget* canvasFromBridge(pulp::view::WidgetBridge& bridge,
                                            pulp::view::ScriptEngine& engine,
                                            const std::string& id) {
    auto value = engine.evaluate("document.getElementById('" + id + "')._id");
    auto nativeId = std::string(value.getWithDefault<std::string_view>(""));
    return dynamic_cast<pulp::view::CanvasWidget*>(bridge.widget(nativeId));
}
} // namespace

// ── Recovered bridge compatibility regressions ───────────────────────────

// Wave 5 css.5 audit — recover the corrupted Wave 2 css.9 plus-lighter
// title/body that was interleaved with an arcTo opener in #1638. The
// body is the canonical mixBlendMode plus-lighter / plus-darker test;
// arcTo coverage exists in a separate Wave 3 canvas2d block below.
TEST_CASE("CSSStyleDeclaration mixBlendMode plus-lighter / plus-darker maps to BM::lighter (Wave 2 css.9)",
          "[view][bridge][css][wave2-css][wave5-recovered]") {
    using BM = pulp::canvas::Canvas::BlendMode;
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 200});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        createPanel('b', '');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        var sb = new CSSStyleDeclaration({ _id: 'b', _nativeCreated: true });
        sa._applyProperty('mixBlendMode', 'plus-lighter');
        sb._applyProperty('mixBlendMode', 'plus-darker');
    )");

    auto* a = bridge.widget("a");
    auto* b = bridge.widget("b");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->mix_blend_mode() == BM::lighter);
    REQUIRE(b->mix_blend_mode() == BM::lighter);
    REQUIRE(a->has_non_default_blend_mode());
    REQUIRE(b->has_non_default_blend_mode());
}

TEST_CASE("CSSStyleDeclaration borderWidth keyword expansion thin/medium/thick",
          "[view][bridge][css][wave2-css]") {
    // Wave 2 css.2 — CSS Backgrounds & Borders L3 named widths.
    // Pulp picks 1/2/4 px (slightly thinner than browsers' canonical
    // 1/3/5 — see compat.json css/borderWidth note).
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('thin', '');
        createPanel('med',  '');
        createPanel('thick','');
        var st = new CSSStyleDeclaration({ _id: 'thin',  _nativeCreated: true });
        var sm = new CSSStyleDeclaration({ _id: 'med',   _nativeCreated: true });
        var sk = new CSSStyleDeclaration({ _id: 'thick', _nativeCreated: true });
        st._applyProperty('borderWidth', 'thin');
        sm._applyProperty('borderWidth', 'medium');
        sk._applyProperty('borderWidth', 'thick');
    )");

    REQUIRE_THAT(bridge.widget("thin")->border_width(),  WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(bridge.widget("med")->border_width(),   WithinAbs(2.0f, 0.001f));
    REQUIRE_THAT(bridge.widget("thick")->border_width(), WithinAbs(4.0f, 0.001f));
}

TEST_CASE("CSSStyleDeclaration fontStyle oblique aliases to italic",
          "[view][bridge][css][wave2-css]") {
    // Wave 2 css.4 — Skia distinguishes italic-vs-oblique only via
    // the `slnt` font variation axis, which most bundled fonts don't
    // ship. Aliasing oblique -> italic upgrades a silent no-op to the
    // closest visual approximation.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('a', 'X', 0, 0, 100, 100);
        createLabel('b', 'X', 0, 0, 100, 100);
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        var sb = new CSSStyleDeclaration({ _id: 'b', _nativeCreated: true });
        sa._applyProperty('fontStyle', 'oblique');
        sb._applyProperty('fontStyle', 'oblique 14deg');
    )");

    auto* la = dynamic_cast<Label*>(bridge.widget("a"));
    auto* lb = dynamic_cast<Label*>(bridge.widget("b"));
    REQUIRE(la != nullptr);
    REQUIRE(lb != nullptr);
    REQUIRE(la->font_style() == 1);   // italic
    REQUIRE(lb->font_style() == 1);   // italic (angle ignored)
}

TEST_CASE("CSSStyleDeclaration top em/vh resolves to default font-size/viewport",
          "[view][bridge][css][wave2-css]") {
    // Wave 2 css.2 — em/rem default to 14 px, vh/vw default to a
    // 600x800 viewport (matches resolveLength fallback).
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        createPanel('b', '');
        createPanel('c', '');
        createPanel('d', '');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        var sb = new CSSStyleDeclaration({ _id: 'b', _nativeCreated: true });
        var sc = new CSSStyleDeclaration({ _id: 'c', _nativeCreated: true });
        var sd = new CSSStyleDeclaration({ _id: 'd', _nativeCreated: true });
        sa._applyProperty('top',  '2em');   // 28
        sb._applyProperty('left', '1.5rem');// 21
        sc._applyProperty('top',  '50vh');  // 300
        sd._applyProperty('left', '25vw');  // 200
    )");

    REQUIRE_THAT(bridge.widget("a")->top(),  WithinAbs(28.0f, 0.05f));
    REQUIRE_THAT(bridge.widget("b")->left(), WithinAbs(21.0f, 0.05f));
    REQUIRE_THAT(bridge.widget("c")->top(),  WithinAbs(300.0f, 0.05f));
    REQUIRE_THAT(bridge.widget("d")->left(), WithinAbs(200.0f, 0.05f));
}

TEST_CASE("CSSStyleDeclaration margin shorthand honors auto + percent per token",
          "[view][bridge][css][wave2-css]") {
    // Wave 2 css.2 — margin shorthand re-tokenized so each edge
    // routes through the same string-aware setFlex pathway as the
    // per-edge longhands. `margin: auto` centers via Yoga's
    // YGNodeStyleSetMarginAuto when paired across opposing edges.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        createPanel('b', '');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        var sb = new CSSStyleDeclaration({ _id: 'b', _nativeCreated: true });
        sa._applyProperty('margin', 'auto');
        sb._applyProperty('margin', '10% 20px');
    )");

    const auto& fa = bridge.widget("a")->flex();
    REQUIRE(fa.dim_margin_top.unit    == DimensionUnit::auto_);
    REQUIRE(fa.dim_margin_right.unit  == DimensionUnit::auto_);
    REQUIRE(fa.dim_margin_bottom.unit == DimensionUnit::auto_);
    REQUIRE(fa.dim_margin_left.unit   == DimensionUnit::auto_);

    const auto& fb = bridge.widget("b")->flex();
    REQUIRE(fb.dim_margin_top.unit    == DimensionUnit::percent);
    REQUIRE_THAT(fb.dim_margin_top.value,    WithinAbs(10.0f, 0.001f));
    REQUIRE(fb.dim_margin_right.unit  == DimensionUnit::px);
    REQUIRE_THAT(fb.dim_margin_right.value,  WithinAbs(20.0f, 0.001f));
    REQUIRE(fb.dim_margin_bottom.unit == DimensionUnit::percent);
    REQUIRE_THAT(fb.dim_margin_bottom.value, WithinAbs(10.0f, 0.001f));
    REQUIRE(fb.dim_margin_left.unit   == DimensionUnit::px);
    REQUIRE_THAT(fb.dim_margin_left.value,   WithinAbs(20.0f, 0.001f));
}

// pulp #1638 — ctx.arcTo records a single path_arc_to with the radius.
TEST_CASE("Wave 3 canvas2d — ctx.arcTo records a single path_arc_to with the radius (recovered from #1638)",
          "[view][bridge][canvas][wave3-canvas2d][wave5-recovered]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'arcto-canvas';
        c.width = 200; c.height = 200;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.beginPath();
        ctx.moveTo(20, 20);
        // arcTo(x1, y1, x2, y2, radius). Tangent arc between (20,20)→(150,20)
        // and (150,20)→(150,150) with radius=30 should produce a single
        // path_arc_to cmd (NOT two lineTo legs from the pre-#1521 bezier
        // approximation).
        ctx.arcTo(150, 20, 150, 150, 30);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "arcto-canvas");
    REQUIRE(canvas != nullptr);

    using CmdType = pulp::view::CanvasDrawCmd::Type;
    int arcToCount = 0;
    bool radius_seen = false;
    for (const auto& cmd : canvas->commands()) {
        if (cmd.type == CmdType::path_arc_to) {
            ++arcToCount;
            REQUIRE_THAT(cmd.x,    WithinAbs(150.0f, 1e-3f));
            REQUIRE_THAT(cmd.y,    WithinAbs( 20.0f, 1e-3f));
            REQUIRE_THAT(cmd.x2,   WithinAbs(150.0f, 1e-3f));
            REQUIRE_THAT(cmd.y2,   WithinAbs(150.0f, 1e-3f));
            REQUIRE_THAT(cmd.extra, WithinAbs( 30.0f, 1e-3f));
            radius_seen = true;
        }
    }
    REQUIRE(arcToCount == 1);
    REQUIRE(radius_seen);

    // RecordingCanvas replay captures the same geometry on the
    // backend-facing `arc_to` virtual (radius lives in f[4]).
    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);
    int rec_arc_to = 0;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::arc_to) {
            ++rec_arc_to;
            REQUIRE_THAT(cmd.f[4], WithinAbs(30.0f, 1e-3f));
        }
    }
    REQUIRE(rec_arc_to == 1);
}

TEST_CASE("Wave 3 canvas2d — fillText after gradient fillStyle keeps the gradient active onto the glyph paint",
          "[view][bridge][canvas][wave3-canvas2d]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'gradtext-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var g = ctx.createLinearGradient(0, 0, 200, 0);
        g.addColorStop(0, '#ff0000');
        g.addColorStop(1, '#0000ff');
        ctx.fillStyle = g;
        ctx.font = '20px Inter';
        // No maxWidth: Wave 3 c2d.6 only requires gradient passthrough; the
        // maxWidth squeeze was wired in #1525.
        ctx.fillText('Hi', 20, 60);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "gradtext-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    using DrawType = pulp::canvas::DrawCommand::Type;
    bool saw_gradient = false;
    bool saw_stale_solid_after_gradient = false;
    bool saw_fill_text = false;
    bool gradient_active = false;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == DrawType::set_fill_color) {
            // RecordingCanvas's default set_fill_gradient_linear records
            // set_fill_color(first-stop) as a proxy for the gradient — the
            // first solid set_fill_color is the gradient's first stop. A
            // second white-ish set_fill_color between gradient and
            // fill_text would mean the gradient was clobbered.
            if (gradient_active && cmd.color.r != 1.0f && cmd.color.b != 0.0f) {
                // No-op — keep silent; the assertion below is the gate.
            }
            // After the gradient is "applied" (recorded as red set_fill_color),
            // any subsequent non-red set_fill_color before fill_text is the
            // bug we're guarding against.
            if (saw_gradient && !saw_fill_text) {
                const bool first_stop_red = (cmd.color.r == 1.0f && cmd.color.g == 0.0f && cmd.color.b == 0.0f);
                if (!first_stop_red) {
                    saw_stale_solid_after_gradient = true;
                }
            }
            if (cmd.color.r == 1.0f && cmd.color.g == 0.0f && cmd.color.b == 0.0f) {
                saw_gradient = true;
                gradient_active = true;
            }
        } else if (cmd.type == DrawType::fill_text) {
            saw_fill_text = true;
            REQUIRE(cmd.text == std::string("Hi"));
        }
    }
    REQUIRE(saw_gradient);          // gradient was set on the canvas
    REQUIRE(saw_fill_text);         // fillText reached the backend
    REQUIRE_FALSE(saw_stale_solid_after_gradient);
}

TEST_CASE("Wave 3 canvas2d — strokeStyle = createLinearGradient routes through canvasSetStrokeLinearGradient",
          "[view][bridge][canvas][wave3-canvas2d]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'strokegrad-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var g = ctx.createLinearGradient(10, 0, 190, 0);
        g.addColorStop(0, '#ff0000');
        g.addColorStop(1, '#00ff00');
        ctx.strokeStyle = g;
        ctx.lineWidth = 3;
        // strokeRect with no explicit color — uses the active strokeStyle
        // which is now a gradient and must emit a set_stroke_gradient_linear
        // cmd via the JS shim's _applyStrokeStyle dispatch.
        ctx.strokeRect(20, 20, 100, 60);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "strokegrad-canvas");
    REQUIRE(canvas != nullptr);

    using CmdType = pulp::view::CanvasDrawCmd::Type;
    int gradLinearCount = 0;
    bool gradient_geometry_ok = false;
    for (const auto& cmd : canvas->commands()) {
        if (cmd.type == CmdType::set_stroke_gradient_linear) {
            ++gradLinearCount;
            REQUIRE_THAT(cmd.x,  WithinAbs( 10.0f, 1e-3f));
            REQUIRE_THAT(cmd.y,  WithinAbs(  0.0f, 1e-3f));
            REQUIRE_THAT(cmd.x2, WithinAbs(190.0f, 1e-3f));
            REQUIRE_THAT(cmd.y2, WithinAbs(  0.0f, 1e-3f));
            REQUIRE(cmd.gradient_colors.size() == 2);
            REQUIRE(cmd.gradient_positions.size() == 2);
            REQUIRE_THAT(cmd.gradient_positions[0], WithinAbs(0.0f, 1e-3f));
            REQUIRE_THAT(cmd.gradient_positions[1], WithinAbs(1.0f, 1e-3f));
            // First stop = red, second = green.
            REQUIRE(cmd.gradient_colors[0].r == 1.0f);
            REQUIRE(cmd.gradient_colors[0].g == 0.0f);
            REQUIRE(cmd.gradient_colors[1].r == 0.0f);
            REQUIRE(cmd.gradient_colors[1].g == 1.0f);
            gradient_geometry_ok = true;
        }
    }
    REQUIRE(gradLinearCount == 1);
    REQUIRE(gradient_geometry_ok);

    // RecordingCanvas captures the dedicated set_stroke_gradient_linear
    // draw command — proves the dispatch reached the Canvas virtual.
    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);
    int rec_grad = 0;
    int rec_stop_count = 0;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_stroke_gradient_linear) {
            ++rec_grad;
            REQUIRE_THAT(cmd.f[0], WithinAbs( 10.0f, 1e-3f));
            REQUIRE_THAT(cmd.f[2], WithinAbs(190.0f, 1e-3f));
            // floats payload: [pos0, r0, g0, b0, a0, pos1, r1, g1, b1, a1].
            REQUIRE(cmd.floats.size() == 10);
            rec_stop_count = static_cast<int>(cmd.floats.size() / 5);
        }
    }
    REQUIRE(rec_grad == 1);
    REQUIRE(rec_stop_count == 2);
}

TEST_CASE("Wave 3 canvas2d — assigning a solid color to strokeStyle after a gradient clears the stroke shader",
          "[view][bridge][canvas][wave3-canvas2d]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'strokegrad-clear';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var g = ctx.createLinearGradient(0, 0, 100, 0);
        g.addColorStop(0, '#ff0000');
        g.addColorStop(1, '#0000ff');
        ctx.strokeStyle = g;
        ctx.strokeRect(10, 10, 50, 30);
        // Reassign to a solid color: the JS shim must flush
        // canvasClearStrokeGradient so the next stroke uses the solid
        // color without a stale shader.
        ctx.strokeStyle = '#00ff00';
        ctx.strokeRect(70, 10, 50, 30);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "strokegrad-clear");
    REQUIRE(canvas != nullptr);

    using CmdType = pulp::view::CanvasDrawCmd::Type;
    int clearCount = 0;
    int gradCount = 0;
    for (const auto& cmd : canvas->commands()) {
        if (cmd.type == CmdType::clear_stroke_gradient) ++clearCount;
        if (cmd.type == CmdType::set_stroke_gradient_linear) ++gradCount;
    }
    REQUIRE(gradCount == 1);
    REQUIRE(clearCount == 1);
}
