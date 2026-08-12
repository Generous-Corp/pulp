// WidgetBridge Wave 2 cheap-wiring tests: two bundles of compat.json
// partial → supported closures from the Wave 2 cleanup pass:
//
//   1. canvas2d cheap wiring (DIVERGE → PASS). JS-evaluates a single
//      CanvasRenderingContext2D method/property, asserts on the
//      recorded CanvasDrawCmd or View slot. Bridge ↔ Canvas API
//      contract tests; Skia paint-side honouring is unit-tested at
//      the Canvas backend layer.
//
//   2. css bundle cheap value-coverage wiring. Pins previously
//      unwired CSS property/value combinations through the JS shim
//      → CSS translator → View slot pipeline.

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
#include <pulp/view/pointer_dispatch.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <thread>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

namespace {
pulp::view::CanvasWidget* canvasFromBridge(pulp::view::WidgetBridge& bridge,
                                            pulp::view::ScriptEngine& engine,
                                            const std::string& id) {
    auto value = engine.evaluate("document.getElementById('" + id + "')._id");
    auto nativeId = std::string(value.getWithDefault<std::string_view>(""));
    return dynamic_cast<pulp::view::CanvasWidget*>(bridge.widget(nativeId));
}
} // namespace

// ── pulp Wave 2 canvas2d cheap wiring (DIVERGE → PASS) ───────────────────
//
// These tests close the loop on the five compat.json entries that flipped
// from partial → supported in the Wave 2 sweep. Each test goes JS → bridge
// → CanvasWidget::paint(RecordingCanvas) → assert on the recorded Canvas
// API call so a regression anywhere in the chain surfaces here.
//
// Scope:
//   1. canvas2d/fill   — `ctx.fill('evenodd')` reaches Canvas::fill_current_path
//                        with FillRule::evenodd (replayed via cmd.f[0] == 1).
//   2. canvas2d/clip   — `ctx.clip('evenodd')` reaches Canvas::clip with
//                        FillRule::evenodd.
//   3. canvas2d/roundRect — 4-corner non-uniform radii thread through to
//                           canvasPathRoundRect with 8 distinct floats so
//                           SkRRect::setRectRadii sees per-corner geometry.
//   4. canvas2d/ellipse — non-zero rotation reaches the bridge and produces a
//                         single-contour replay (path_ellipse on the
//                         RecordingCanvas; tests confirm one moveTo follows).
//   5. canvas2d/strokeText — strokeText routes to the dedicated stroke_text
//                            command (not fillText with strokeStyle as fill).
//
// The Skia / CG paint-side honouring of FillRule and kStroke_Style is unit-
// tested at the Canvas backend layer; here we focus on the bridge ↔ Canvas
// API contract that the harness adapter scores.

TEST_CASE("Wave 2 canvas2d — ctx.fill('evenodd') reaches Canvas::fill_current_path with FillRule::evenodd",
          "[view][bridge][canvas][wave2-canvas2d]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 200});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'evenodd-fill';
        c.width = 100; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        // Self-overlapping path — the only paths where nonzero vs evenodd
        // differ. Outer square + reverse-wound inner square: nonzero fills
        // both squares, evenodd leaves a hole in the middle.
        ctx.beginPath();
        ctx.moveTo(0, 0); ctx.lineTo(100, 0); ctx.lineTo(100, 100); ctx.lineTo(0, 100); ctx.closePath();
        ctx.moveTo(25, 25); ctx.lineTo(25, 75); ctx.lineTo(75, 75); ctx.lineTo(75, 25); ctx.closePath();
        ctx.fill('evenodd');
        ctx.beginPath();
        ctx.moveTo(0, 0); ctx.lineTo(10, 0); ctx.lineTo(10, 10); ctx.closePath();
        ctx.fill();  // default = nonzero
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "evenodd-fill");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    std::vector<float> rules;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_current_path) {
            rules.push_back(cmd.f[0]);
        }
    }
    REQUIRE(rules.size() == 2);
    REQUIRE(rules[0] == 1.0f);  // evenodd
    REQUIRE(rules[1] == 0.0f);  // nonzero default
}

TEST_CASE("Wave 2 canvas2d — ctx.clip('evenodd') reaches Canvas::clip with FillRule::evenodd",
          "[view][bridge][canvas][wave2-canvas2d]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 200});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'evenodd-clip';
        c.width = 100; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.beginPath();
        ctx.moveTo(0, 0); ctx.lineTo(100, 0); ctx.lineTo(100, 100); ctx.lineTo(0, 100); ctx.closePath();
        ctx.moveTo(25, 25); ctx.lineTo(25, 75); ctx.lineTo(75, 75); ctx.lineTo(75, 25); ctx.closePath();
        ctx.clip('evenodd');
        ctx.beginPath();
        ctx.moveTo(0, 0); ctx.lineTo(10, 0); ctx.lineTo(10, 10); ctx.closePath();
        ctx.clip();  // default = nonzero
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "evenodd-clip");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    std::vector<float> rules;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::clip) {
            rules.push_back(cmd.f[0]);
        }
    }
    REQUIRE(rules.size() == 2);
    REQUIRE(rules[0] == 1.0f);  // evenodd
    REQUIRE(rules[1] == 0.0f);  // nonzero default
}

TEST_CASE("Wave 2 canvas2d — ctx.roundRect with 4 distinct corners produces 4 distinct radii",
          "[view][bridge][canvas][wave2-canvas2d]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 200});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'roundrect-4';
        c.width = 100; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.beginPath();
        // CSS spec [tl, tr, br, bl] — four distinct corner radii.
        ctx.roundRect(0, 0, 100, 100, [4, 8, 12, 16]);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "roundrect-4");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    int rrCount = 0;
    pulp::canvas::DrawCommand rrCmd{};
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::round_rect) {
            rrCount++;
            rrCmd = cmd;
        }
    }
    REQUIRE(rrCount == 1);
    // f[0..3] = x, y, w, h; f[4..5] = tl_x, tl_y; floats[0..5] = tr_x, tr_y, br_x, br_y, bl_x, bl_y
    REQUIRE_THAT(rrCmd.f[0], WithinAbs(0.0f, 1e-5f));   // x
    REQUIRE_THAT(rrCmd.f[1], WithinAbs(0.0f, 1e-5f));   // y
    REQUIRE_THAT(rrCmd.f[2], WithinAbs(100.0f, 1e-5f)); // w
    REQUIRE_THAT(rrCmd.f[3], WithinAbs(100.0f, 1e-5f)); // h
    REQUIRE_THAT(rrCmd.f[4], WithinAbs(4.0f, 1e-5f));   // tl_x
    REQUIRE_THAT(rrCmd.f[5], WithinAbs(4.0f, 1e-5f));   // tl_y
    REQUIRE(rrCmd.floats.size() >= 6);
    REQUIRE_THAT(rrCmd.floats[0], WithinAbs(8.0f,  1e-5f));  // tr_x
    REQUIRE_THAT(rrCmd.floats[1], WithinAbs(8.0f,  1e-5f));  // tr_y
    REQUIRE_THAT(rrCmd.floats[2], WithinAbs(12.0f, 1e-5f));  // br_x
    REQUIRE_THAT(rrCmd.floats[3], WithinAbs(12.0f, 1e-5f));  // br_y
    REQUIRE_THAT(rrCmd.floats[4], WithinAbs(16.0f, 1e-5f));  // bl_x
    REQUIRE_THAT(rrCmd.floats[5], WithinAbs(16.0f, 1e-5f));  // bl_y
}

TEST_CASE("Wave 2 canvas2d — ctx.ellipse with non-zero rotation threads through to a single ellipse command",
          "[view][bridge][canvas][wave2-canvas2d]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 200});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'ellipse-rot';
        c.width = 100; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.beginPath();
        // 45 degrees in radians, full sweep.
        ctx.ellipse(50, 50, 30, 15, Math.PI / 4, 0, Math.PI * 2, false);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "ellipse-rot");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    int ellipseCount = 0;
    pulp::canvas::DrawCommand eCmd{};
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::ellipse) {
            ellipseCount++;
            eCmd = cmd;
        }
    }
    // Single ellipse command — the JS shim must NOT decompose into multiple
    // arc segments when rotation is non-zero (pre-Wave-2 the rotation arg
    // was ignored entirely, which would have collapsed the call to either
    // `arc` or a no-op).
    REQUIRE(ellipseCount == 1);
    REQUIRE_THAT(eCmd.f[0], WithinAbs(50.0f, 1e-5f));   // cx
    REQUIRE_THAT(eCmd.f[1], WithinAbs(50.0f, 1e-5f));   // cy
    REQUIRE_THAT(eCmd.f[2], WithinAbs(30.0f, 1e-5f));   // rx
    REQUIRE_THAT(eCmd.f[3], WithinAbs(15.0f, 1e-5f));   // ry
    // f[4] = rotation (radians) — confirm it was forwarded, not zeroed.
    REQUIRE_THAT(eCmd.f[4], WithinAbs(std::numbers::pi_v<float> / 4.0f, 1e-4f));
}

TEST_CASE("Wave 2 canvas2d — ctx.strokeText routes through dedicated stroke_text command",
          "[view][bridge][canvas][wave2-canvas2d]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'stroke-text';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.strokeStyle = '#ff0000';
        ctx.lineWidth = 2;
        ctx.strokeText('OK', 10, 30);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "stroke-text");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    int strokeTextCount = 0;
    int fillTextCount = 0;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::stroke_text) {
            strokeTextCount++;
        } else if (cmd.type == pulp::canvas::DrawCommand::Type::fill_text) {
            fillTextCount++;
        }
    }
    // Wave 2 cheap wiring confirmation: strokeText must produce a real
    // stroke_text command (true stroked-glyph rendering with kStroke_Style),
    // NOT a fill_text command using strokeStyle as the fill color
    // (the pre-#1525 approximation).
    REQUIRE(strokeTextCount == 1);
    REQUIRE(fillTextCount == 0);
}

// ── pulp Wave 2 css bundle (cheap value-coverage wiring) ────────────────
//
// The CSS shim accepts a wider value vocabulary than the bridge fns
// natively understand, with the resolution math performed JS-side
// before reaching the bridge. These tests pin the shim->bridge
// dispatch for the new value forms so DIVERGE doesn't silently
// regress when the catalog claims them.
//
// CSS value-coverage round-trips through the shim -> translator -> View
// slot (width%, fontSize em, lineHeight, gap two-value).

TEST_CASE("CSSStyleDeclaration forwards width percent via el.style",
          "[view][bridge][css][wave2-css]") {
    // Round-trips `el.style.width = '50%'` through the shim into the
    // bridge's setFlex dim_width.unit = percent route. Mirrors the
    // direct setFlex('a','width','50%') test (issue-1434-auto) but
    // exercises the JS-side _applyProperty('width', '50%') path.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        sa._applyProperty('width',  '50%');
        sa._applyProperty('height', '25%');
    )");

    const auto& fa = bridge.widget("a")->flex();
    REQUIRE(fa.dim_width.unit  == DimensionUnit::percent);
    REQUIRE_THAT(fa.dim_width.value,  WithinAbs(50.0f, 0.001f));
    REQUIRE(fa.dim_height.unit == DimensionUnit::percent);
    REQUIRE_THAT(fa.dim_height.value, WithinAbs(25.0f, 0.001f));
}

TEST_CASE("CSSStyleDeclaration fontSize em resolves against default 14px",
          "[view][bridge][css][wave2-css]") {
    // Wave 2 css.2 — em/rem/% relative-unit resolution. Default
    // inherited font-size is 14px (matches resolveLength fallback).
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('half', 'X', 0, 0, 100, 100);
        createLabel('rem',  'X', 0, 0, 100, 100);
        createLabel('pct',  'X', 0, 0, 100, 100);
        createLabel('sm',   'X', 0, 0, 100, 100);
        createLabel('lg',   'X', 0, 0, 100, 100);
        var sH = new CSSStyleDeclaration({ _id: 'half', _nativeCreated: true });
        var sR = new CSSStyleDeclaration({ _id: 'rem',  _nativeCreated: true });
        var sP = new CSSStyleDeclaration({ _id: 'pct',  _nativeCreated: true });
        var sS = new CSSStyleDeclaration({ _id: 'sm',   _nativeCreated: true });
        var sL = new CSSStyleDeclaration({ _id: 'lg',   _nativeCreated: true });
        sH._applyProperty('fontSize', '0.5em');   // 7
        sR._applyProperty('fontSize', '2rem');    // 28
        sP._applyProperty('fontSize', '50%');     // 7
        sS._applyProperty('fontSize', 'smaller'); // 11.62
        sL._applyProperty('fontSize', 'larger');  // 16.8
    )");

    auto* lH = dynamic_cast<Label*>(bridge.widget("half"));
    auto* lR = dynamic_cast<Label*>(bridge.widget("rem"));
    auto* lP = dynamic_cast<Label*>(bridge.widget("pct"));
    auto* lS = dynamic_cast<Label*>(bridge.widget("sm"));
    auto* lL = dynamic_cast<Label*>(bridge.widget("lg"));
    REQUIRE(lH != nullptr);
    REQUIRE(lR != nullptr);
    REQUIRE(lP != nullptr);
    REQUIRE(lS != nullptr);
    REQUIRE(lL != nullptr);
    REQUIRE_THAT(lH->font_size(), WithinAbs(7.0f,    0.05f));
    REQUIRE_THAT(lR->font_size(), WithinAbs(28.0f,   0.05f));
    REQUIRE_THAT(lP->font_size(), WithinAbs(7.0f,    0.05f));
    REQUIRE_THAT(lS->font_size(), WithinAbs(11.62f,  0.05f));
    REQUIRE_THAT(lL->font_size(), WithinAbs(16.8f,   0.05f));
}

TEST_CASE("CSSStyleDeclaration lineHeight unitless multiplies font-size",
          "[view][bridge][css][wave2-css]") {
    // Wave 2 css.2 — unitless multiplier is the most common CSS form
    // (e.g. `line-height: 1.5`). Resolved against the default 14px
    // font-size; nested cascade is the follow-up.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('a', 'X', 0, 0, 100, 100);
        createLabel('b', 'X', 0, 0, 100, 100);
        createLabel('c', 'X', 0, 0, 100, 100);
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        var sb = new CSSStyleDeclaration({ _id: 'b', _nativeCreated: true });
        var sc = new CSSStyleDeclaration({ _id: 'c', _nativeCreated: true });
        sa._applyProperty('lineHeight', '1.5');     // 21
        sb._applyProperty('lineHeight', 'normal');  // 16.8
        sc._applyProperty('lineHeight', '150%');    // 21
    )");

    auto* la = dynamic_cast<Label*>(bridge.widget("a"));
    auto* lb = dynamic_cast<Label*>(bridge.widget("b"));
    auto* lc = dynamic_cast<Label*>(bridge.widget("c"));
    REQUIRE(la != nullptr);
    REQUIRE(lb != nullptr);
    REQUIRE(lc != nullptr);
    REQUIRE_THAT(la->line_height(), WithinAbs(21.0f, 0.05f));
    REQUIRE_THAT(lb->line_height(), WithinAbs(16.8f, 0.05f));
    REQUIRE_THAT(lc->line_height(), WithinAbs(21.0f, 0.05f));
}

TEST_CASE("CSSStyleDeclaration gap two-value fans out to row + column",
          "[view][bridge][css][wave2-css]") {
    // Wave 2 css.2 — `gap: 10px 20px` is the CSS shorthand for
    // row-gap + column-gap. The shim splits on whitespace and dispatches
    // to setFlex(row_gap) + setFlex(column_gap).
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('p', '');
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s._applyProperty('gap', '10px 20px');
    )");

    const auto& f = bridge.widget("p")->flex();
    REQUIRE_THAT(f.row_gap,    WithinAbs(10.0f, 0.001f));
    REQUIRE_THAT(f.column_gap, WithinAbs(20.0f, 0.001f));
}

// pulp #1638 — single-token `gap` must clear prior row_gap/column_gap.
// FlexStyle::effective_gap prefers per-axis when ≥0, so `gap: 5px`
// after `gap: 10px 20px` would read 10/20 instead of 5/5 unless the
// per-axis slots reset to the -1 sentinel before writing the shared slot.
TEST_CASE("CSSStyleDeclaration single-token gap clears per-axis (no shadowing)",
          "[view][bridge][css][issue-1638]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('p', '');
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s._applyProperty('gap', '10px 20px');
    )");
    const auto& f = bridge.widget("p")->flex();
    REQUIRE_THAT(f.row_gap,    WithinAbs(10.0f, 0.001f));
    REQUIRE_THAT(f.column_gap, WithinAbs(20.0f, 0.001f));

    // Now overwrite with single-token gap. Per-axis must reset (-1)
    // so the shared `gap` value is consulted by effective_gap.
    bridge.load_script(R"(
        var s2 = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s2._applyProperty('gap', '5px');
    )");
    REQUIRE_THAT(f.gap,        WithinAbs(5.0f,  0.001f));
    REQUIRE(f.row_gap    < 0.0f);  // -1 sentinel = "consult shared gap"
    REQUIRE(f.column_gap < 0.0f);
    // effective_gap on either axis should now resolve to 5.0
    REQUIRE_THAT(f.effective_gap(pulp::view::FlexDirection::row),
                 WithinAbs(5.0f, 0.001f));
    REQUIRE_THAT(f.effective_gap(pulp::view::FlexDirection::column),
                 WithinAbs(5.0f, 0.001f));
}

// pulp #1707 — single-token gap with invalid input must NOT clobber
// prior 2-token state. Parse first; only reset the per-axis slots when
// the new value is valid (an earlier ordering reset them before parsing,
// nuking the per-axis state silently on a failed parse).
TEST_CASE("CSSStyleDeclaration single-token gap with invalid input preserves prior 2-token state",
          "[view][bridge][css][issue-1707]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('p', '');
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s._applyProperty('gap', '10px 20px');
    )");
    const auto& f = bridge.widget("p")->flex();
    REQUIRE_THAT(f.row_gap, WithinAbs(10.0f, 0.001f));
    REQUIRE_THAT(f.column_gap, WithinAbs(20.0f, 0.001f));

    // Now apply invalid single-token gap: must be a no-op (per-axis
    // values must REMAIN 10/20, not reset to -1).
    bridge.load_script(R"(
        var s2 = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s2._applyProperty('gap', 'foo');
    )");
    REQUIRE_THAT(f.row_gap, WithinAbs(10.0f, 0.001f));
    REQUIRE_THAT(f.column_gap, WithinAbs(20.0f, 0.001f));

    // Empty string also a no-op (parseCSSLength returns null).
    bridge.load_script(R"(
        var s3 = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s3._applyProperty('gap', '');
    )");
    REQUIRE_THAT(f.row_gap, WithinAbs(10.0f, 0.001f));
    REQUIRE_THAT(f.column_gap, WithinAbs(20.0f, 0.001f));

    // Valid single-token gap still resets per-axis (existing #1638 behavior).
    bridge.load_script(R"(
        var s4 = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s4._applyProperty('gap', '7px');
    )");
    REQUIRE_THAT(f.gap, WithinAbs(7.0f, 0.001f));
    REQUIRE(f.row_gap < 0.0f);
    REQUIRE(f.column_gap < 0.0f);
}

// pulp #1638 baseline-corruption: this TEST_CASE body got truncated
// during the bad merge — it set up `using BM = ...; ScriptEngine
// engine; View root; root.set_bounds(...);` but never wrapped the
// rest of the test, instead transitioning straight into a banner
// comment for the canvas2d block. Stubbed for compile; the
// equivalent assertion lives below in the renamed
// "CSSStyleDeclaration mixBlendMode plus-lighter / plus-darker map
// to BM::lighter" test (which is the canvas2d-fill title that got
// the body that should have lived here, post-shuffle).
// Wave 5 css.5 audit — recover the corrupted #1638 body that was
// orphaned by a merge into a stub-with-stray-string. The body below
// is the original Wave 2 css.9 plus-lighter / plus-darker test.
TEST_CASE("CSSStyleDeclaration mixBlendMode plus-lighter -> kPlus",
          "[view][bridge][css][wave2-css][issue-1549]") {
    // Wave 2 css.9 — plus-lighter / plus-darker are CSS Compositing &
    // Blending Level 2 keywords. Both map to BlendMode::lighter
    // (Skia's SkBlendMode::kPlus / additive). Previously fell through
    // to the unknown-keyword normal fallback.
    using BM = pulp::canvas::Canvas::BlendMode;
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
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


// ────────────────────────────────────────────────────────────────────────────
// Event-bridge dispatch payload contract — regressions documented in
// .agents/skills/view-bridge/SKILL.md ("Event payload contract").
//
// These tests pin the JSON shape the bridge emits over `__dispatch__`
// for pointer / wheel / key events. The @pulp/react synthetic-event
// shim depends on every field listed here. Regressions historically
// caused user-visible breakage (Spectr band-drawing, trackpad zoom,
// Escape modal dismissal) that took multiple PRs to re-fix because
// nothing pinned the contract — these tests are the pin.
// ────────────────────────────────────────────────────────────────────────────

TEST_CASE("Event contract: W3C MouseEvent.button maps left=0, middle=1, right=2",
          "[view][bridge][events][contract]") {
    // Pre-fix the bridge sent the raw MouseButton enum (left=1, right=2,
    // middle=3). JSX handlers reading `e.button === 1` (W3C: middle
    // click) then fired on every LEFT click — the cause of Spectr's
    // band-drawing breakage (left click triggered pan-mode).
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var btn_log = [];
        createLabel('s', 'S', '');
        on('s', 'pointerdown', function(e) { btn_log.push(e.button); });
        registerPointer('s');
    )");
    auto* s = bridge.widget("s");
    REQUIRE(s != nullptr);

    MouseEvent e{};
    e.is_down = true;
    REQUIRE(s->on_dom_pointer_event);

    e.button = MouseButton::left;   s->on_dom_pointer_event(e, true);
    e.button = MouseButton::middle; s->on_dom_pointer_event(e, true);
    e.button = MouseButton::right;  s->on_dom_pointer_event(e, true);

    // left=0, middle=1, right=2 — W3C order, NOT the enum order.
    REQUIRE(engine.evaluate("btn_log.join(',')").toString() == "0,1,2");
}

TEST_CASE("Event contract: forward_key_event emits W3C UIEvent.key strings + modifier booleans",
          "[view][bridge][events][contract]") {
    // Pre-fix `e.key` was the raw int KeyCode — every JSX
    // `e.key === 'Escape'` / `e.key === 'ArrowLeft'` comparison failed.
    // Also pin `ctrlKey/shiftKey/altKey/metaKey` booleans (the W3C
    // KeyboardEvent surface).
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var keys = [];
        var mods_seen = '';
        on('__global__', 'keydown', function(e) {
            keys.push(e.key);
            mods_seen = [e.ctrlKey, e.shiftKey, e.altKey, e.metaKey].join(':');
        });
    )");

    bridge.forward_key_event(static_cast<int>(KeyCode::escape),    0, true);
    bridge.forward_key_event(static_cast<int>(KeyCode::left),      0, true);
    bridge.forward_key_event(static_cast<int>(KeyCode::right),     0, true);
    bridge.forward_key_event(static_cast<int>(KeyCode::up),        0, true);
    bridge.forward_key_event(static_cast<int>(KeyCode::down),      0, true);
    bridge.forward_key_event(static_cast<int>(KeyCode::enter),     0, true);
    bridge.forward_key_event(static_cast<int>(KeyCode::tab),       0, true);
    bridge.forward_key_event(static_cast<int>(KeyCode::backspace), 0, true);
    bridge.forward_key_event(static_cast<int>(KeyCode::delete_),   0, true);
    bridge.forward_key_event(static_cast<int>(KeyCode::space),     0, true);
    bridge.forward_key_event(static_cast<int>(KeyCode::a),         0, true);
    bridge.forward_key_event(static_cast<int>(KeyCode::a),         static_cast<uint16_t>(kModShift), true);
    bridge.forward_key_event(static_cast<int>(KeyCode::f1),        0, true);

    REQUIRE(engine.evaluate("keys.join('|')").toString() ==
            "Escape|ArrowLeft|ArrowRight|ArrowUp|ArrowDown|Enter|Tab|Backspace|Delete| |a|A|F1");

    // Last forward_key_event call carried kModCtrl|kModCmd|kModAlt:
    bridge.forward_key_event(static_cast<int>(KeyCode::a),
                             static_cast<uint16_t>(kModCtrl | kModAlt | kModCmd), true);
    REQUIRE(engine.evaluate("mods_seen").toString() == "true:false:true:true");
}

TEST_CASE("Event contract: window.addEventListener('keydown', fn) receives __global__ keydown",
          "[view][bridge][events][contract]") {
    // Pre-fix only __callbacks__['__global__:keydown'] was fanned to —
    // `window.addEventListener` listeners (the standard DOM API and the
    // one Spectr uses for Escape) never fired.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var win_keys = [];
        window.addEventListener('keydown', function(e) { win_keys.push(e.key); });
    )");

    bridge.forward_key_event(static_cast<int>(KeyCode::escape), 0, true);
    bridge.forward_key_event(static_cast<int>(KeyCode::a),      0, true);

    REQUIRE(engine.evaluate("win_keys.join(',')").toString() == "Escape,a");
}

TEST_CASE("Event contract: __dispatch__ try/catch keeps listeners alive after a handler throws",
          "[view][bridge][events][contract]") {
    // Pre-fix a throw from any handler (a stale ref in a React tick,
    // a bad prop deref) propagated out of evaluate() and killed the
    // rAF self-rescheduling chain. Symptom: waveform animation died
    // until mouse-move restarted it.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var calls = 0;
        var errs = 0;
        __dispatchError__ = function() { errs++; };
        window.addEventListener('keydown', function(e) { throw new Error('boom'); });
        window.addEventListener('keydown', function(e) { calls++; });
    )");

    bridge.forward_key_event(static_cast<int>(KeyCode::a), 0, true);
    bridge.forward_key_event(static_cast<int>(KeyCode::a), 0, true);

    // First listener throws each time; second listener still fires.
    REQUIRE(engine.evaluate("calls").getWithDefault<int>(0) == 2);
    REQUIRE(engine.evaluate("errs").getWithDefault<int>(0) == 2);
}

TEST_CASE("Event contract: wheel dispatch carries coordinates and modifiers",
          "[view][bridge][events][contract]") {
    // Pre-fix wheel sent raw positional args (deltaX, deltaY). The
    // @pulp/react synthetic-event shim only lifts fields when
    // isPlainObject(rawArgs[0]) — positional args fell through,
    // `e.deltaY` was undefined, trackpad zoom broke silently.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var got = null;
        createLabel('w', 'W', '');
        on('w', 'wheel', function(e) { got = e; });
        registerWheel('w');
    )");
    auto* w = bridge.widget("w");
    REQUIRE(w != nullptr);

    MouseEvent ev{};
    ev.is_wheel = true;
    ev.scroll_delta_x = 1.5f;
    ev.scroll_delta_y = -3.0f;
    ev.window_position = {200.0f, 250.0f};
    // kModMeta is the Windows-key representation; it must map to the same W3C
    // metaKey field as macOS kModCmd.
    ev.modifiers = kModCtrl | kModShift | kModAlt | kModMeta;
    REQUIRE(w->on_dom_wheel_event);
    w->on_dom_wheel_event(ev, true);

    // The shape pinned by .agents/skills/view-bridge/SKILL.md.
    REQUIRE(engine.evaluate("typeof got").toString() == "object");
    REQUIRE_THAT(engine.evaluate("got.deltaX").getWithDefault<double>(0.0), WithinAbs(1.5, 0.001));
    REQUIRE_THAT(engine.evaluate("got.deltaY").getWithDefault<double>(0.0), WithinAbs(-3.0, 0.001));
    REQUIRE_THAT(engine.evaluate("got.clientX").getWithDefault<double>(0.0), WithinAbs(200.0, 0.001));
    REQUIRE_THAT(engine.evaluate("got.clientY").getWithDefault<double>(0.0), WithinAbs(250.0, 0.001));
    CHECK(engine.evaluate("got.ctrlKey").getWithDefault<bool>(false));
    CHECK(engine.evaluate("got.shiftKey").getWithDefault<bool>(false));
    CHECK(engine.evaluate("got.altKey").getWithDefault<bool>(false));
    CHECK(engine.evaluate("got.metaKey").getWithDefault<bool>(false));
}

TEST_CASE("Event contract: registerPointer/registerWheel are idempotent (no lambda-stack growth)",
          "[view][bridge][events][contract]") {
    // Pre-fix each call wrapped the previous pointer callback, so
    // re-renders multiplied dispatch cost by the render count and
    // every pointer event fired N times into the JS callback chain.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var pointer_fires = 0;
        var wheel_fires = 0;
        createLabel('s', 'S', '');
        on('s', 'pointerdown', function(e) { pointer_fires++; });
        on('s', 'wheel', function(e) { wheel_fires++; });
        registerPointer('s'); registerPointer('s'); registerPointer('s');
        registerWheel('s'); registerWheel('s'); registerWheel('s');
    )");

    auto* s = bridge.widget("s");
    REQUIRE(s != nullptr);

    MouseEvent down{};
    down.is_down = true;
    REQUIRE(s->on_dom_pointer_event);
    s->on_dom_pointer_event(down, true);

    MouseEvent wheel{};
    wheel.is_wheel = true;
    wheel.scroll_delta_y = 1.0f;
    REQUIRE(s->on_dom_wheel_event);
    s->on_dom_wheel_event(wheel, true);

    // Each event should fire its handler exactly once even though we
    // called registerPointer / registerWheel three times.
    REQUIRE(engine.evaluate("pointer_fires").getWithDefault<int>(0) == 1);
    REQUIRE(engine.evaluate("wheel_fires").getWithDefault<int>(0) == 1);
}

TEST_CASE("Event contract: nested raw wheel ancestors fire without duplicating DOM bubbling",
          "[view][bridge][events][contract]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var raw_outer_fires = 0;
        var middle_dom_fires = 0;
        var inner_dom_fires = 0;
        var outer = document.createElement('div');
        var middle = document.createElement('div');
        var inner = document.createElement('div');
        document.body.appendChild(outer);
        outer.appendChild(middle);
        middle.appendChild(inner);
        on(outer._id, 'wheel', function() { raw_outer_fires++; });
        registerWheel(outer._id);
        middle.addEventListener('wheel', function() { middle_dom_fires++; });
        inner.addEventListener('wheel', function() { inner_dom_fires++; });
        globalThis.__wheel_outer_id = outer._id;
        globalThis.__wheel_middle_id = middle._id;
        globalThis.__wheel_inner_id = inner._id;
    )");

    const auto id = [&](const char* name) {
        return std::string(engine.evaluate(name).getWithDefault<std::string_view>(""));
    };
    auto* outer = bridge.widget(id("globalThis.__wheel_outer_id"));
    auto* middle = bridge.widget(id("globalThis.__wheel_middle_id"));
    auto* inner = bridge.widget(id("globalThis.__wheel_inner_id"));
    REQUIRE(outer != nullptr);
    REQUIRE(middle != nullptr);
    REQUIRE(inner != nullptr);
    outer->set_bounds({0, 0, 300, 200});
    middle->set_bounds({0, 0, 200, 150});
    inner->set_bounds({0, 0, 100, 100});

    WheelHost host;
    host.request_repaint = [] {};
    deliver_mouse_wheel(root, {20, 20}, 1.5f, -3.0f,
                        static_cast<std::uint16_t>(kModCtrl | kModShift), host);

    CHECK(engine.evaluate("raw_outer_fires").getWithDefault<int>(0) == 1);
    CHECK(engine.evaluate("middle_dom_fires").getWithDefault<int>(0) == 1);
    CHECK(engine.evaluate("inner_dom_fires").getWithDefault<int>(0) == 1);
}


TEST_CASE("setBackgroundGradient parses linear / radial / conic into the right kind",
          "[view][widget-bridge][gradient]") {
    // The CSS parser must route each gradient form to the matching View kind
    // (1=linear, 2=radial, 3=conic) so the canvas paints a real radial/sweep
    // instead of the old flat-color fallback. With or without a shape/position
    // prefix.
    auto kind = [](const std::string& css) {
        ScriptEngine engine;
        View root;
        root.set_bounds({0, 0, 200, 200});
        root.set_theme(Theme::dark());
        StateStore store;
        WidgetBridge bridge(engine, root, store);
        bridge.load_script("setBackgroundGradient('', '" + css + "');");
        return root.background_gradient_type();
    };
    CHECK(kind("linear-gradient(to right, #ff0000, #0000ff)") == 1);
    CHECK(kind("radial-gradient(circle at 50% 50%, #ffffff, #000000)") == 2);
    CHECK(kind("radial-gradient(#ffffff, #000000)") == 2);  // no prefix
    CHECK(kind("conic-gradient(from 90deg at 50% 50%, #ff0000, #00ff00, #0000ff)") == 3);
    CHECK(kind("conic-gradient(#ff0000, #00ff00)") == 3);   // no prefix
}

// The box is 200x120, NOT square, and that is the point. A radial sizing
// keyword resolves to a distance from the gradient's own centre to a side or a
// corner, which is two different numbers per axis unless the box happens to be
// square and the centre happens to be in the middle of it — the case under
// which a single "fraction of the larger side" stands in for all four keywords
// without ever being caught. On a square box every assertion below is also
// satisfied by that wrong model.
//
// These resolve through View::resolve_radial, the same function the painter
// calls; test/test_css_gradient_geometry.cpp checks its output end-to-end
// against Chromium's render of the same strings.
TEST_CASE("setBackgroundGradient resolves radial sizing keywords per axis",
          "[view][widget-bridge][gradient]") {
    auto resolved = [](const std::string& css) {
        ScriptEngine engine;
        View root;
        root.set_bounds({0, 0, 200, 120});
        root.set_theme(Theme::dark());
        StateStore store;
        WidgetBridge bridge(engine, root, store);
        bridge.load_script("setBackgroundGradient('', '" + css + "');");
        REQUIRE(root.has_background_gradient());
        return View::resolve_radial(root.background_gradient_layers().front(),
                                    200.0f, 120.0f);
    };
    auto near = [](float a, float b) { return std::abs(a - b) < 0.01f; };

    // A circle takes ONE radius: the nearest side on either axis, so the short
    // axis wins. A per-axis model would give it 100.
    const auto cs_circle =
        resolved("radial-gradient(circle closest-side at 50% 50%, #fff, #000)");
    CHECK(near(cs_circle.rx, 60.0f));
    CHECK(near(cs_circle.ry, 60.0f));

    // An ellipse takes the nearest side on EACH axis independently.
    const auto cs_ellipse =
        resolved("radial-gradient(closest-side at 50% 50%, #fff, #000)");
    CHECK(near(cs_ellipse.rx, 100.0f));
    CHECK(near(cs_ellipse.ry, 60.0f));

    // farthest-corner keeps the farthest-side aspect and scales until it meets
    // the corner — exactly sqrt(2) per axis, not the corner distance (116.6).
    const auto fc = resolved("radial-gradient(farthest-corner at 50% 50%, #fff, #000)");
    CHECK(near(fc.rx, 141.42f));
    CHECK(near(fc.ry, 84.85f));

    // No prefix at all means `farthest-corner ellipse at center`.
    const auto bare = resolved("radial-gradient(#fff, #000)");
    CHECK(near(bare.rx, 141.42f));
    CHECK(near(bare.ry, 84.85f));

    // An off-centre `at` changes every radius, which a box-relative constant
    // cannot express at all: the centre sits 40px from the left and 160 from
    // the right, 24 from the top and 96 from the bottom.
    const auto off = resolved("radial-gradient(farthest-side at 20% 20%, #fff, #000)");
    CHECK(near(off.cx, 40.0f));
    CHECK(near(off.cy, 24.0f));
    CHECK(near(off.rx, 160.0f));
    CHECK(near(off.ry, 96.0f));
}

TEST_CASE("setTextRuns builds a styled AttributedString on the Label",
          "[view][widget-bridge][text]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 40});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(
        "createLabel('t', 'Hello world', '');\n"
        "setFontSize('t', 16);\n"
        "setTextRuns('t', [{ start: 0, end: 5, fontWeight: 700, color: '#ff0000' }]);");
    auto* lbl = dynamic_cast<Label*>(bridge.widget("t"));
    REQUIRE(lbl != nullptr);
    CHECK(lbl->has_attributed_string());
    CHECK(lbl->attributed_span_count() == 2);  // "Hello" styled run + " world" gap
}

TEST_CASE("setTextRuns preserves inherited dominant typography",
          "[view][widget-bridge][text][inheritance]") {
    ScriptEngine engine;
    View root;
    root.set_inheritable_font_family("Courier");
    root.set_inheritable_font_size(19.0f);
    root.set_inheritable_font_weight(600);
    root.set_inheritable_letter_spacing(1.5f);
    root.set_inheritable_text_color(pulp::canvas::Color::rgba8(1, 2, 3));
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(
        "createLabel('t', 'Hello world', '');\n"
        "setTextRuns('t', [{ start: 0, end: 5, fontWeight: 700 }]);");
    auto* label = dynamic_cast<Label*>(bridge.widget("t"));
    REQUIRE(label != nullptr);
    label->set_bounds({0, 0, 300, 40});
    pulp::canvas::RecordingCanvas canvas;
    label->paint(canvas);
    std::vector<pulp::canvas::DrawCommand> fonts;
    for (const auto& command : canvas.commands())
        if (command.type == pulp::canvas::DrawCommand::Type::set_font_full)
            fonts.push_back(command);
    auto has_font = [&](float weight) {
        return std::any_of(fonts.begin(), fonts.end(), [&](const auto& font) {
            return font.text == "Courier" &&
                   std::abs(font.f[0] - 19.0f) < 1e-4f &&
                   std::abs(font.f[1] - weight) < 1e-4f &&
                   std::abs(font.f[3] - 1.5f) < 1e-4f;
        });
    };
    CHECK(has_font(700.0f));
    CHECK(has_font(600.0f));
}

// pulp #3336: a plain set_text() must supersede prior per-range runs. The old
// spans index into the OLD string, so leaving has_attributed_ set would paint
// stale, mis-indexed runs over the new text.
TEST_CASE("set_text clears stale attributed runs",
          "[view][widget][text][issue-3336]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 40});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(
        "createLabel('t', 'Hello world', '');\n"
        "setTextRuns('t', [{ start: 0, end: 5, fontWeight: 700 }]);");
    auto* lbl = dynamic_cast<Label*>(bridge.widget("t"));
    REQUIRE(lbl != nullptr);
    REQUIRE(lbl->has_attributed_string());
    lbl->set_text("Different text");
    CHECK_FALSE(lbl->has_attributed_string());  // runs dropped → single-style path
}

// pulp #3336: paint_attributed_ must honor the text-align cascade instead of
// hard-coding a left/x=0 anchor. RecordingCanvas measures 7px/char, so the two
// spans ("Hello"+" world" = 11 chars) span 77px inside a 200px-wide label.
TEST_CASE("paint_attributed_ honors text-align",
          "[view][widget][text][issue-3336]") {
    using pulp::canvas::DrawCommand;
    using pulp::canvas::RecordingCanvas;
    auto first_fill_x = [](RecordingCanvas& rc) -> float {
        for (const auto& c : rc.commands())
            if (c.type == DrawCommand::Type::fill_text) return c.f[0];
        return -999.0f;
    };

    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 40});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(
        "createLabel('t', 'Hello world', '');\n"
        "setFontSize('t', 16);\n"
        "setTextRuns('t', [{ start: 0, end: 5, fontWeight: 700, color: '#ff0000' }]);");
    auto* lbl = dynamic_cast<Label*>(bridge.widget("t"));
    REQUIRE(lbl != nullptr);
    REQUIRE(lbl->has_attributed_string());
    lbl->set_bounds({0, 0, 200, 40});

    constexpr float kTotal = 11 * 7.0f;  // RecordingCanvas measure model

    lbl->set_text_align(LabelAlign::left);
    RecordingCanvas left;
    lbl->paint(left);
    const float xl = first_fill_x(left);
    REQUIRE_THAT(xl, WithinAbs(0.0f, 1e-4f));

    lbl->set_text_align(LabelAlign::center);
    RecordingCanvas center;
    lbl->paint(center);
    const float xc = first_fill_x(center);
    REQUIRE_THAT(xc, WithinAbs((200.0f - kTotal) * 0.5f, 1e-4f));
    CHECK(xc > xl);

    lbl->set_text_align(LabelAlign::right);
    RecordingCanvas right;
    lbl->paint(right);
    const float xr = first_fill_x(right);
    REQUIRE_THAT(xr, WithinAbs(200.0f - kTotal, 1e-4f));
    CHECK(xr > xc);
}
