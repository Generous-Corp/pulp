// test_canvas2d_shim_late.cpp — extracted from test_canvas2d_shim.cpp
// in the 2026-05 Phase 5 (P5-2 follow-up) refactor.
//
// Post-Wave-2 Canvas2D shim coverage. Each cluster pins runtime path
// for entries that landed under specific issues:
//
//   * #1525 — fillText / strokeText maxWidth + glyph cluster handling
//   * #1521 — arc-as-path cluster (DIVERGE → PASS)
//   * #1520 — ctx.direction / ctx.filter
//   * #1526 — catalog hygiene round-trip for already-supported properties
//   * Wave 4 c2d cleanup — lineDashOffset re-flushes on assignment
//   * #1527 — getTransform / resetTransform + isPointInPath / isPointInStroke

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#endif

#include <string>

using namespace pulp::view;
using namespace pulp::state;

// Local copies of run_in_bridge + ScriptedBridge — file-static in the
// parent test_canvas2d_shim.cpp. Duplicated here per the extracted-TU
// pattern. Updating one without the other is a documented gotcha; this
// is the same trade-off the canvas2d-wave2 + svg + other widget-bridge
// splits already accept.
namespace {

std::string run_in_bridge(const std::string& js) {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("globalThis.__test_result__ = (function(){\n"
                       + js +
                       "\n})();");
    auto val = engine.evaluate("String(globalThis.__test_result__)");
    if (val.isString()) return std::string(val.getString());
    return std::string{};
}

struct ScriptedBridge {
    ScriptEngine engine;
    View root;
    StateStore store;
    std::unique_ptr<WidgetBridge> bridge;

    ScriptedBridge() {
        root.set_bounds({0, 0, 400, 300});
        bridge = std::make_unique<WidgetBridge>(engine, root, store);
    }

    void load(const std::string& js) {
        bridge->load_script(js);
    }

    CanvasWidget* canvas() {
        auto v = engine.evaluate("(globalThis.__test_canvas_el__ && "
                                 "globalThis.__test_canvas_el__._id) || ''");
        if (!v.isString()) return nullptr;
        std::string id = std::string(v.getString());
        if (id.empty()) return nullptr;
        return dynamic_cast<CanvasWidget*>(bridge->widget(id));
    }
};

} // namespace

// ── pulp #1525 — fillText / strokeText maxWidth + glyph cluster handling ──
//
// Promotion target: 2 DIVERGE → PASS for canvas2d/fillText and
// canvas2d/strokeText. Pre-#1525 the JS shim accepted `maxWidth` as a
// 4th arg but discarded it (`void maxWidth;`) and `strokeText` re-routed
// through `fillText` with the strokeStyle as the fill colour — visually
// approximate but spec-incompatible (no real outlined glyphs, no
// horizontal squeeze).
//
// These tests cover the full JS → bridge → CanvasDrawCmd surface so the
// catalog can flip from `partial` to `supported`.

TEST_CASE("Canvas2D fillText threads maxWidth through to bridge",
          "[view][canvas2d][issue-1525]") {
    // Spec: `ctx.fillText(text, x, y, maxWidth)` records a fill_text cmd
    // carrying maxWidth on the bridge-side payload (cmd.w). The 3-arg
    // form (no maxWidth) records cmd.w == 0 — the "no constraint"
    // sentinel that the paint loop uses to fall through to the legacy
    // unconstrained path.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 200; c.height = 50;
        var ctx = c.getContext('2d');
        ctx.font = '14px Inter';
        ctx.fillText('hello world', 10, 20, 50);
        ctx.fillText('no limit',    10, 40);
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);

    using T = CanvasDrawCmd::Type;
    int with_max = 0, without_max = 0;
    float observed_max_width = -1.0f;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type != T::fill_text) continue;
        if (cmd.text == "hello world") {
            with_max++;
            observed_max_width = cmd.w;
        } else if (cmd.text == "no limit") {
            without_max++;
            REQUIRE(cmd.w == 0.0f);
        }
    }
    REQUIRE(with_max == 1);
    REQUIRE(without_max == 1);
    REQUIRE(observed_max_width == Catch::Approx(50.0f));
}

TEST_CASE("Canvas2D fillText coerces non-finite maxWidth to no-constraint sentinel",
          "[view][canvas2d][issue-1525]") {
    // Spec: NaN / Infinity / negative / zero / null / undefined /
    // non-numeric string / false all collapse to "no constraint" — the
    // bridge sees cmd.w == 0 and the paint loop falls through to the
    // legacy unconstrained fill_text. Keep this path free of newly-created
    // JS NaN values: QuickJS trips UBSan while boxing them in sanitizer CI.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 200; c.height = 80;
        var ctx = c.getContext('2d');
        ctx.font = '14px Inter';
        ctx.fillText('a', 0, 10, NaN);
        ctx.fillText('b', 0, 20, Infinity);
        ctx.fillText('c', 0, 30, -10);
        ctx.fillText('d', 0, 40, 0);
        ctx.fillText('e', 0, 50, null);
        ctx.fillText('f', 0, 60, undefined);
        ctx.fillText('g', 0, 70, 'not-a-number');
        ctx.fillText('h', 0, 80, false);
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);

    using T = CanvasDrawCmd::Type;
    int seen = 0;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type != T::fill_text) continue;
        if (cmd.text.size() == 1 && cmd.text[0] >= 'a' && cmd.text[0] <= 'h') {
            INFO("text=" << cmd.text << " observed cmd.w=" << cmd.w);
            REQUIRE(cmd.w == 0.0f);
            seen++;
        }
    }
    REQUIRE(seen == 8);
}

TEST_CASE("Canvas2D strokeText routes through canvasStrokeText with maxWidth",
          "[view][canvas2d][issue-1525]") {
    // Pre-#1525: strokeText re-routed through canvasFillText with
    // strokeStyle as the fill colour, recording a fill_text cmd. Post-
    // #1525: strokeText records a dedicated stroke_text cmd carrying
    // the strokeStyle in cmd.color and the optional maxWidth in cmd.w
    // — the paint loop dispatches to Canvas::stroke_text for true
    // outlined-glyph rendering (Skia / CG override).
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 200; c.height = 50;
        var ctx = c.getContext('2d');
        ctx.font = '14px Inter';
        ctx.strokeStyle = '#ff0000';
        ctx.lineWidth = 2;
        ctx.strokeText('outline', 10, 20, 80);
        ctx.strokeText('plain',   10, 40);
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);

    using T = CanvasDrawCmd::Type;
    int stroke_count = 0, fill_text_count = 0;
    float seen_max_width = -1.0f;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == T::stroke_text) {
            stroke_count++;
            if (cmd.text == "outline") seen_max_width = cmd.w;
            if (cmd.text == "plain") REQUIRE(cmd.w == 0.0f);
        } else if (cmd.type == T::fill_text) {
            fill_text_count++;
        }
    }
    REQUIRE(stroke_count == 2);
    // strokeText must NOT have leaked into fill_text — that's the
    // pre-#1525 approximation we're explicitly replacing.
    REQUIRE(fill_text_count == 0);
    REQUIRE(seen_max_width == Catch::Approx(80.0f));
}

TEST_CASE("Canvas2D ctx.measureText is unaffected by a subsequent maxWidth fillText",
          "[view][canvas2d][issue-1525]") {
    // measureText reports the natural advance of the current text +
    // font; the maxWidth-squeeze on fillText is rendering-only and
    // must not retroactively alter the metrics returned to JS.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.font = '14px Inter';
        var natural = ctx.measureText('squeeze me').width;
        ctx.fillText('squeeze me', 0, 20, 5);  // tiny maxWidth
        var after = ctx.measureText('squeeze me').width;
        // Round to integer so font-fallback fuzz between hosts doesn't
        // make this brittle — the contract is "before == after", not a
        // specific advance value.
        return Math.round(natural * 100) === Math.round(after * 100) ? 'stable' : 'drifted';
    )");
    REQUIRE(result == "stable");
}

#ifdef PULP_HAS_SKIA
TEST_CASE("Canvas2D fillText with maxWidth horizontally squeezes raster output",
          "[view][canvas2d][issue-1525][skia][!mayfail]") {
    // End-to-end: render the same text once with a maxWidth narrower than
    // the natural advance. Assert the constrained raster has its right edge
    // at (x + maxWidth), not at (x + natural advance).
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 200; c.height = 40;
        var ctx = c.getContext('2d');
        ctx.font = '20px Inter';
        ctx.fillStyle = '#ffffff';
        ctx.fillText('Hello there friends', 5, 25, 60);
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);

    SkImageInfo info = SkImageInfo::Make(200, 40,
                                         kBGRA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface);
    surface->getCanvas()->clear(SK_ColorBLACK);
    pulp::canvas::SkiaCanvas skia(surface->getCanvas());
    cw->set_bounds({0, 0, 200, 40});
    cw->paint(skia);

    SkPixmap pm;
    REQUIRE(surface->peekPixels(&pm));
    auto sample = [&](int x, int y) {
        return *static_cast<const uint32_t*>(pm.addr(x, y));
    };
    auto is_lit = [&](int x, int y) {
        uint32_t px = sample(x, y);
        // BGRA: any non-trivial luminance counts as "text drawn here".
        uint8_t b = (px >>  0) & 0xff;
        uint8_t g = (px >>  8) & 0xff;
        uint8_t r = (px >> 16) & 0xff;
        return (r + g + b) > 60;
    };
    // Find the rightmost lit column. With maxWidth=60 starting at x=5,
    // the squeezed run must end no further than x ~= 65 + a small
    // anti-alias tolerance. Without the squeeze the natural advance of
    // "Hello there friends" at 20px would extend well past x=120.
    int rightmost = 0;
    for (int x = 199; x >= 0; --x) {
        bool lit = false;
        for (int y = 0; y < 40; ++y) {
            if (is_lit(x, y)) { lit = true; break; }
        }
        if (lit) { rightmost = x; break; }
    }
    INFO("rightmost lit column: " << rightmost);
    // Allow a 6px AA fringe on the right edge of the last glyph.
    REQUIRE(rightmost >= 5);
    REQUIRE(rightmost <= 5 + 60 + 6);
}

// Codex P2 (PR #1555): SkiaCanvas::stroke_text built its stroke paint
// via make_stroke_paint() but never called apply_stroke_state(), so
// ctx.lineJoin / ctx.lineCap / ctx.miterLimit / ctx.strokeStyle pattern
// shaders were silently dropped on strokeText only — every other stroke
// primitive honoured them. This test renders a heavy-stroke glyph twice
// against identical surfaces, once with the default (miter) line join
// and once with LineJoin::round, and asserts the rasters differ. With
// the apply_stroke_state call missing, both paths would resolve to the
// same default-join SkPaint and produce identical pixels.
TEST_CASE("SkiaCanvas::stroke_text honors sticky line_join state",
          "[canvas][skia][issue-1525-fix]") {
    using pulp::canvas::SkiaCanvas;
    using pulp::canvas::Color;
    using pulp::canvas::LineJoin;

    auto render = [](LineJoin join) {
        SkImageInfo info = SkImageInfo::Make(120, 60,
                                             kBGRA_8888_SkColorType,
                                             kPremul_SkAlphaType,
                                             SkColorSpace::MakeSRGB());
        auto surface = SkSurfaces::Raster(info);
        REQUIRE(surface);
        surface->getCanvas()->clear(SK_ColorBLACK);
        SkiaCanvas canvas(surface->getCanvas());
        canvas.set_font("Inter", 36.0f);
        canvas.set_stroke_color(Color::rgba(1.0f, 1.0f, 1.0f, 1.0f));
        // Heavy stroke so corner-shape differences (round vs miter) are
        // pixel-visible at the glyph junctions.
        canvas.set_line_width(6.0f);
        canvas.set_line_join(join);
        canvas.stroke_text("M", 10.0f, 45.0f);
        return surface;
    };

    auto surface_miter = render(LineJoin::miter);
    auto surface_round = render(LineJoin::round);

    SkPixmap pm_miter, pm_round;
    REQUIRE(surface_miter->peekPixels(&pm_miter));
    REQUIRE(surface_round->peekPixels(&pm_round));
    REQUIRE(pm_miter.width()  == pm_round.width());
    REQUIRE(pm_miter.height() == pm_round.height());

    // Count pixels that differ. If apply_stroke_state never reaches the
    // text-stroke paint, both passes draw with the default miter join
    // and the buffers are byte-identical (diff_count == 0). With the
    // fix, the round-join junction shape produces a non-trivial pixel
    // delta around the corners of 'M'.
    int diff_count = 0;
    for (int y = 0; y < pm_miter.height(); ++y) {
        for (int x = 0; x < pm_miter.width(); ++x) {
            uint32_t a = *static_cast<const uint32_t*>(pm_miter.addr(x, y));
            uint32_t b = *static_cast<const uint32_t*>(pm_round.addr(x, y));
            if (a != b) ++diff_count;
        }
    }
    INFO("differing pixels between miter and round join rasters: "
         << diff_count);
    // Threshold is a sanity floor — miter vs round on a 36px 'M' stroked
    // at width=6 produces hundreds of differing AA pixels at the four
    // corner junctions. Anything above ~10 proves apply_stroke_state
    // propagated the setStrokeJoin call into the text-stroke paint.
    REQUIRE(diff_count > 10);
}
#endif  // PULP_HAS_SKIA

// ── pulp #1521 — arc-as-path cluster (DIVERGE → PASS) ────────────────────
//
// The JS shim now routes ctx.arc / arcTo / ellipse / roundRect through the
// new canvasPathArc / canvasPathArcTo / canvasPathEllipse /
// canvasPathRoundRect bridge fns. Before this PR the shim emitted
// canvasMoveTo + canvasCubicTo (arc / ellipse) or canvasLineTo (arcTo /
// roundRect) — N approximation segments per arc. After this PR each call
// emits exactly one path_arc / path_arc_to / path_ellipse /
// path_round_rect command, and the cubic_to / line_to fallbacks no
// longer fire from the arc family.
TEST_CASE("Canvas2D arc shim emits path_arc, not bezier approximation",
          "[view][canvas2d][issue-1521]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.beginPath();
        ctx.arc(50, 50, 30, 0, Math.PI * 2);
        ctx.stroke();
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    int saw_arc = 0, saw_cubic = 0, saw_move = 0;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::path_arc) ++saw_arc;
        if (cmd.type == CanvasDrawCmd::Type::cubic_to) ++saw_cubic;
        if (cmd.type == CanvasDrawCmd::Type::move_to) ++saw_move;
    }
    INFO("path_arc=" << saw_arc << " cubic_to=" << saw_cubic
         << " move_to=" << saw_move);
    REQUIRE(saw_arc == 1);
    REQUIRE(saw_cubic == 0); // old shim emitted N cubic_to per arc
    REQUIRE(saw_move == 0);  // old shim emitted a move_to per arc
}

TEST_CASE("Canvas2D arcTo shim emits path_arc_to with radius preserved",
          "[view][canvas2d][issue-1521]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.beginPath();
        ctx.moveTo(0, 0);
        ctx.arcTo(50, 0, 50, 50, 12);
        ctx.stroke();
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool saw_arc_to = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::path_arc_to) {
            saw_arc_to = true;
            REQUIRE(cmd.x == Catch::Approx(50.0f));
            REQUIRE(cmd.y == Catch::Approx(0.0f));
            REQUIRE(cmd.x2 == Catch::Approx(50.0f));
            REQUIRE(cmd.y2 == Catch::Approx(50.0f));
            REQUIRE(cmd.extra == Catch::Approx(12.0f));
        }
    }
    REQUIRE(saw_arc_to);
}

TEST_CASE("Canvas2D ellipse shim emits path_ellipse with rotation",
          "[view][canvas2d][issue-1521]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.beginPath();
        ctx.ellipse(50, 50, 40, 20, Math.PI / 4, 0, Math.PI * 2);
        ctx.fill();
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool saw_ellipse = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::path_ellipse) {
            saw_ellipse = true;
            REQUIRE(cmd.w == Catch::Approx(40.0f));   // rx
            REQUIRE(cmd.h == Catch::Approx(20.0f));   // ry
            REQUIRE(cmd.extra == Catch::Approx(0.7853982f).margin(1e-5)); // rotation
        }
    }
    REQUIRE(saw_ellipse);
}

TEST_CASE("Canvas2D roundRect shim emits path_round_rect with 4 distinct radii",
          "[view][canvas2d][issue-1521]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.beginPath();
        ctx.roundRect(10, 20, 80, 40, [2, 4, 6, 8]);
        ctx.fill();
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool saw_rr = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::path_round_rect) {
            saw_rr = true;
            REQUIRE(cmd.x == Catch::Approx(10.0f));
            REQUIRE(cmd.y == Catch::Approx(20.0f));
            REQUIRE(cmd.w == Catch::Approx(80.0f));
            REQUIRE(cmd.h == Catch::Approx(40.0f));
            REQUIRE(cmd.gradient_positions.size() == 8u);
            REQUIRE(cmd.gradient_positions[0] == Catch::Approx(2.0f)); // tl_x
            REQUIRE(cmd.gradient_positions[2] == Catch::Approx(4.0f)); // tr_x
            REQUIRE(cmd.gradient_positions[4] == Catch::Approx(6.0f)); // br_x
            REQUIRE(cmd.gradient_positions[6] == Catch::Approx(8.0f)); // bl_x
        }
    }
    REQUIRE(saw_rr);
}

TEST_CASE("Canvas2D roundRect shim accepts {x,y} elliptical corner",
          "[view][canvas2d][issue-1521]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 200; c.height = 40;
        var ctx = c.getContext('2d');
        ctx.beginPath();
        ctx.roundRect(0, 0, 50, 50, [{x: 4, y: 8}]);
        ctx.fill();
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool saw_rr = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::path_round_rect) {
            saw_rr = true;
            REQUIRE(cmd.gradient_positions.size() == 8u);
            // [{x:4,y:8}] expands to all four corners with rx=4, ry=8.
            REQUIRE(cmd.gradient_positions[0] == Catch::Approx(4.0f)); // tl_x
            REQUIRE(cmd.gradient_positions[1] == Catch::Approx(8.0f)); // tl_y
            REQUIRE(cmd.gradient_positions[6] == Catch::Approx(4.0f)); // bl_x
            REQUIRE(cmd.gradient_positions[7] == Catch::Approx(8.0f)); // bl_y
        }
    }
    REQUIRE(saw_rr);
}

// ── pulp #1520 — Canvas2D ctx.direction / ctx.filter ────────────────────
//
// canvas2d/direction and canvas2d/filter were the last two NOT-IMPL
// entries in compat.json's canvas2d catalog. This block flips them to
// `partial` by routing the JS shim through the bridge and into Skia's
// SkShaper / SkImageFilter chain. The tests below exercise:
//   * round-trip getter/setter on the JS shim
//   * defensive coercion of unknown direction strings
//   * sticky flush of direction setter before fillText
//   * round-trip getter/setter on filter (raw string)
//   * sticky flush of filter setter before fill / drawImage
//   * cache invalidation across save/restore
// SkImageFilter chain rasterisation parity is intentionally not asserted
// here — the parser is exercised by the [issue-1520] subset; full visual
// parity with Chrome is a follow-up shared with #1503's element-side
// CSS filter parser.

TEST_CASE("Canvas2D shim exposes direction property as round-trip field",
          "[view][canvas2d][issue-1520]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        c.width = 32; c.height = 32;
        var ctx = c.getContext('2d');
        // Spec default: "ltr".
        var initial = ctx.direction;
        ctx.direction = 'rtl';
        var rtl = ctx.direction;
        ctx.direction = 'inherit';
        var inh = ctx.direction;
        return [initial, rtl, inh].join('|');
    )");
    REQUIRE(result == "ltr|rtl|inherit");
}

TEST_CASE("Canvas2D shim coerces unknown direction strings on the bridge flush",
          "[view][canvas2d][issue-1520]") {
    // Per spec, assigning an unknown string is a no-op (the previous
    // valid value persists). Our shim mirrors the assigned string in
    // the JS getter (matching the spec's "store the IDL value" rule)
    // but coerces to "ltr" on the bridge flush — there's no enum value
    // for "garbage" downstream. The test asserts the recorded command
    // stream sees a valid (ltr) enum.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 32; c.height = 32;
        var ctx = c.getContext('2d');
        ctx.direction = 'sideways';
        ctx.fillText('x', 0, 0);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool saw_direction = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::set_direction) {
            saw_direction = true;
            // Coerced to ltr enum (0).
            REQUIRE(cmd.int_val == 0);
        }
    }
    REQUIRE(saw_direction);
}

TEST_CASE("Canvas2D direction flushes via canvasSetDirection bridge fn before fillText",
          "[view][canvas2d][issue-1520]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.direction = 'rtl';
        ctx.font = '14px Inter';
        ctx.fillStyle = '#ffffff';
        ctx.fillText('Hello', 50, 50);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    using T = pulp::view::CanvasDrawCmd::Type;
    int dir_idx = -1, text_idx = -1;
    for (size_t i = 0; i < cw->commands().size(); ++i) {
        if (cw->commands()[i].type == T::set_direction) dir_idx = (int)i;
        if (cw->commands()[i].type == T::fill_text)     text_idx = (int)i;
    }
    REQUIRE(dir_idx >= 0);
    REQUIRE(text_idx >= 0);
    // Sticky-state contract: setter must precede the consuming draw.
    REQUIRE(dir_idx < text_idx);
    // RTL = enum value 1.
    REQUIRE(cw->commands()[dir_idx].int_val == 1);
}

TEST_CASE("Canvas2D shim exposes filter property as round-trip field",
          "[view][canvas2d][issue-1520]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        c.width = 32; c.height = 32;
        var ctx = c.getContext('2d');
        // Spec default: "none".
        var initial = ctx.filter;
        ctx.filter = 'blur(5px) sepia(80%)';
        var assigned = ctx.filter;
        // Reset.
        ctx.filter = 'none';
        var reset = ctx.filter;
        return [initial, assigned, reset].join('||');
    )");
    REQUIRE(result == "none||blur(5px) sepia(80%)||none");
}

TEST_CASE("Canvas2D filter flushes via canvasSetFilter bridge fn before fillRect",
          "[view][canvas2d][issue-1520]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.filter = 'blur(4px) grayscale(50%)';
        ctx.fillStyle = '#ff0000';
        ctx.fillRect(0, 0, 50, 50);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    using T = pulp::view::CanvasDrawCmd::Type;
    int filter_idx = -1, rect_idx = -1;
    for (size_t i = 0; i < cw->commands().size(); ++i) {
        if (cw->commands()[i].type == T::set_filter)  filter_idx = (int)i;
        if (cw->commands()[i].type == T::fill_rect)   rect_idx   = (int)i;
    }
    REQUIRE(filter_idx >= 0);
    REQUIRE(rect_idx >= 0);
    REQUIRE(filter_idx < rect_idx);
    // Raw CSS string round-trips through the bridge unchanged.
    REQUIRE(cw->commands()[filter_idx].text == "blur(4px) grayscale(50%)");
}

TEST_CASE("Canvas2D filter flushes only on change (sticky-state cache)",
          "[view][canvas2d][issue-1520]") {
    // Cache contract: assigning the same filter twice is a no-op on
    // the bridge side — the second fillRect must not produce a second
    // set_filter command. Mirrors the same caching shape shadowColor
    // / miterLimit / imageSmoothing already use.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 64; c.height = 64;
        var ctx = c.getContext('2d');
        ctx.filter = 'blur(2px)';
        ctx.fillRect(0, 0, 10, 10);
        ctx.filter = 'blur(2px)';   // identical assignment — no flush
        ctx.fillRect(20, 0, 10, 10);
        ctx.filter = 'sepia(100%)'; // changed — must flush
        ctx.fillRect(40, 0, 10, 10);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    int filter_count = 0;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == pulp::view::CanvasDrawCmd::Type::set_filter) ++filter_count;
    }
    // First assignment + the change to sepia = 2 flushes total.
    REQUIRE(filter_count == 2);
}

TEST_CASE("Canvas2D filter flushes before drawImage so sticky chain wraps the bitmap",
          "[view][canvas2d][issue-1520]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.filter = 'invert(100%)';
        ctx.drawImage('does-not-exist.png', 0, 0, 50, 50);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    using T = pulp::view::CanvasDrawCmd::Type;
    int filter_idx = -1, draw_idx = -1;
    for (size_t i = 0; i < cw->commands().size(); ++i) {
        if (cw->commands()[i].type == T::set_filter) filter_idx = (int)i;
        if (cw->commands()[i].type == T::draw_image) draw_idx   = (int)i;
    }
    REQUIRE(filter_idx >= 0);
    REQUIRE(draw_idx >= 0);
    REQUIRE(filter_idx < draw_idx);
    REQUIRE(cw->commands()[filter_idx].text == "invert(100%)");
}

TEST_CASE("Canvas2D direction + filter cache invalidates on save/restore",
          "[view][canvas2d][issue-1520]") {
    // The JS-side shim invalidates its sticky-flush cache across
    // ctx.save() / ctx.restore() because the C++ canvas pops the
    // GState back to the saved snapshot, including any direction /
    // filter state. Without invalidation the next draw after restore
    // would skip the flush thinking the value matched cache.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 64; c.height = 64;
        var ctx = c.getContext('2d');
        ctx.filter = 'blur(2px)';
        ctx.direction = 'rtl';
        ctx.fillText('a', 0, 10);
        ctx.save();
        // Inside save scope: change values, draw, restore.
        ctx.filter = 'sepia(100%)';
        ctx.direction = 'ltr';
        ctx.fillText('b', 0, 20);
        ctx.restore();
        // Re-assign to the OUTER values. Cache is invalidated by
        // restore(), so the bridge MUST emit setters again.
        ctx.filter = 'blur(2px)';
        ctx.direction = 'rtl';
        ctx.fillText('c', 0, 30);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    int filter_count = 0, dir_count = 0;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == pulp::view::CanvasDrawCmd::Type::set_filter)    ++filter_count;
        if (cmd.type == pulp::view::CanvasDrawCmd::Type::set_direction) ++dir_count;
    }
    // 3 distinct flushes per setter: outer pre-save, inner change,
    // and the post-restore re-assignment.
    REQUIRE(filter_count == 3);
    REQUIRE(dir_count    == 3);
}

// ── pulp #1526: catalog hygiene round-trip for the already-supported
// canvas2d surface ───────────────────────────────────────────────────────
//
// Ten entries — globalAlpha, lineCap, lineJoin, lineDashOffset,
// textAlign, textBaseline, globalCompositeOperation, quadraticCurveTo,
// bezierCurveTo, arc — were cataloged in PR #1366 / wired in PR #1348 /
// fanned out across #1480 (line cap/join paint plumbing) and the pre-
// existing FilterBank repro suite. Their bridge-side coverage is split
// across the issue-964 cases above, but no single test exercises the
// 10-as-a-set as the catalog claims. This test pins each one's full
// round-trip through the JS shim → bridge → CanvasWidget command stream
// so a regression in any of them surfaces directly under [issue-1526].
TEST_CASE("Canvas2D shim flushes the 10-entry catalog set to the bridge",
          "[view][canvas2d][issue-1526]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 64; c.height = 64;
        var ctx = c.getContext('2d');

        // (1) globalAlpha — pushed via canvasSetGlobalAlpha on the next
        // draw. (2) globalCompositeOperation — pushed via
        // canvasGlobalCompositeOperation OR canvasSetBlendMode.
        ctx.globalAlpha = 0.5;
        ctx.globalCompositeOperation = 'multiply';

        // (3) lineCap, (4) lineJoin — pushed via canvasSetLineCap /
        // canvasSetLineJoin on the next stroke (idempotent).
        ctx.lineCap  = 'round';
        ctx.lineJoin = 'bevel';

        // (5) textAlign, (6) textBaseline — pushed via
        // canvasSetTextAlign / canvasSetTextBaseline before fillText.
        ctx.textAlign    = 'center';
        ctx.textBaseline = 'middle';

        // (7) lineDashOffset — passed positionally on every setLineDash
        // call; mutating in isolation is documented partial.
        ctx.lineDashOffset = 4;
        ctx.setLineDash([6, 3]);

        // Path methods — needed before stroke / fill flushes.
        ctx.beginPath();
        ctx.moveTo(0, 0);
        // (8) quadraticCurveTo — wraps canvasQuadTo (cmd quad_to).
        ctx.quadraticCurveTo(10, 20, 30, 0);
        // (9) bezierCurveTo — wraps canvasCubicTo (cmd cubic_to).
        ctx.bezierCurveTo(35, 5, 45, 5, 50, 10);
        // (10) arc — shim emits a native path_arc command.
        ctx.arc(40, 40, 8, 0, 6.28);

        // Trigger the stroke flush so lineCap/lineJoin and globalAlpha
        // / globalCompositeOperation reach the bridge.
        ctx.strokeStyle = '#ffffff';
        ctx.stroke();
        // Trigger the fillText flush so textAlign/textBaseline reach
        // the bridge.
        ctx.fillStyle = '#ff0000';
        ctx.fillText('x', 5, 50);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    REQUIRE(cw->command_count() > 0);

    using T = pulp::view::CanvasDrawCmd::Type;
    bool saw_global_alpha = false, saw_blend = false;
    bool saw_line_cap = false, saw_line_join = false;
    bool saw_text_align = false, saw_text_baseline = false;
    bool saw_set_line_dash = false;
    bool saw_quad = false, saw_cubic = false, saw_path_arc = false;
    bool saw_move = false;
    int  cubic_count = 0;
    int  set_line_dash_phase_x4 = 0;  // count cmds where extra==4 (offset)
    int  text_align_enum = -1, text_baseline_enum = -1;
    int  line_cap_enum = -1, line_join_enum = -1;
    int  blend_enum = -1;

    for (const auto& cmd : cw->commands()) {
        switch (cmd.type) {
        case T::set_global_alpha:    saw_global_alpha = true; break;
        case T::set_blend_mode:      saw_blend = true; blend_enum = cmd.int_val; break;
        case T::set_line_cap:        saw_line_cap = true; line_cap_enum = cmd.int_val; break;
        case T::set_line_join:       saw_line_join = true; line_join_enum = cmd.int_val; break;
        case T::set_text_align:      saw_text_align = true; text_align_enum = cmd.int_val; break;
        case T::set_text_baseline:   saw_text_baseline = true; text_baseline_enum = cmd.int_val; break;
        case T::set_line_dash:
            saw_set_line_dash = true;
            if (cmd.extra == 4.0f) ++set_line_dash_phase_x4;
            break;
        case T::quad_to:             saw_quad = true; break;
        case T::cubic_to:            saw_cubic = true; ++cubic_count; break;
        case T::path_arc:            saw_path_arc = true; break;
        case T::move_to:             saw_move = true; break;
        default: break;
        }
    }

    INFO("global_alpha=" << saw_global_alpha
         << " blend=" << saw_blend
         << " line_cap=" << saw_line_cap
         << " line_join=" << saw_line_join
         << " text_align=" << saw_text_align
         << " text_baseline=" << saw_text_baseline
         << " set_line_dash=" << saw_set_line_dash
         << " quad=" << saw_quad
         << " cubic=" << saw_cubic
         << " path_arc=" << saw_path_arc
         << " move=" << saw_move
         << " cubic_count=" << cubic_count);

    REQUIRE(saw_global_alpha);
    REQUIRE(saw_blend);
    REQUIRE(saw_line_cap);
    REQUIRE(saw_line_join);
    REQUIRE(saw_text_align);
    REQUIRE(saw_text_baseline);
    REQUIRE(saw_set_line_dash);
    REQUIRE(set_line_dash_phase_x4 >= 1);  // lineDashOffset=4 carried through
    REQUIRE(saw_quad);                      // quadraticCurveTo
    REQUIRE(saw_cubic);                     // bezierCurveTo
    REQUIRE(cubic_count >= 1);              // bezierCurveTo (1)
    REQUIRE(saw_path_arc);                  // arc
    REQUIRE(saw_move);                      // explicit moveTo before path methods

    // Enum payloads — 'center'/'middle'/'round'/'bevel'/'multiply' must
    // round-trip through the bridge as the documented enum values, not
    // just be present. Asserting `>= 0` would silently pass if the bridge
    // regressed to default 0; pin the exact mapping the bridge declares
    // in widget_bridge.cpp:
    //   textAlign:    'left'=0, 'center'=1, 'right'=2
    //   textBaseline: 'top'=0,  'middle'=1, 'bottom'=2
    //   lineCap:      'butt'=0, 'round'=1,  'square'=2
    //   lineJoin:     'miter'=0,'round'=1,  'bevel'=2
    //   blendMode:    'source-over'=0, 'multiply'=1, ...
    REQUIRE(text_align_enum    == 1); // 'center'
    REQUIRE(text_baseline_enum == 1); // 'middle'
    REQUIRE(line_cap_enum      == 1); // 'round'
    REQUIRE(line_join_enum     == 2); // 'bevel'
    REQUIRE(blend_enum         == 1); // 'multiply'
}

// ── pulp #1526: getter round-trip for the 10-entry catalog set ───────────
//
// Spec: the JS getter returns the most-recently-assigned value (or the
// canonical default before any assignment). Verifying the getter round-
// trips ensures ctx.X reads back what was written without dipping back
// into the bridge — the shim must store the assigned value locally.
TEST_CASE("Canvas2D shim getter round-trip for the 10-entry catalog set",
          "[view][canvas2d][issue-1526]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        // Defaults per HTML5 spec.
        var defaults = [
            ctx.globalAlpha,
            ctx.globalCompositeOperation,
            ctx.lineCap,
            ctx.lineJoin,
            ctx.lineDashOffset,
            ctx.textAlign,
            ctx.textBaseline
        ].join('|');
        ctx.globalAlpha = 0.25;
        ctx.globalCompositeOperation = 'screen';
        ctx.lineCap = 'square';
        ctx.lineJoin = 'round';
        ctx.lineDashOffset = 7;
        ctx.textAlign = 'right';
        ctx.textBaseline = 'bottom';
        var assigned = [
            ctx.globalAlpha,
            ctx.globalCompositeOperation,
            ctx.lineCap,
            ctx.lineJoin,
            ctx.lineDashOffset,
            ctx.textAlign,
            ctx.textBaseline
        ].join('|');
        // Methods exist as functions.
        var have_methods = (
            typeof ctx.quadraticCurveTo === 'function' &&
            typeof ctx.bezierCurveTo === 'function' &&
            typeof ctx.arc === 'function'
        ) ? 'methods-ok' : 'methods-missing';
        return defaults + ' || ' + assigned + ' || ' + have_methods;
    )");
    REQUIRE(result ==
        "1|source-over|butt|miter|0|left|top"
        " || "
        "0.25|screen|square|round|7|right|bottom"
        " || "
        "methods-ok");
}

// ── Wave 4 c2d cleanup — lineDashOffset re-flushes on assignment ─────────
//
// HTML5 spec: ctx.lineDashOffset is a sticky phase property; assigning to
// it must shift the dash phase on subsequent strokes without requiring a
// redundant setLineDash call. Pre-Wave-4 the field was tracked locally
// but only sent on the next setLineDash, so phase mutations between
// draws were silently dropped.
//
// Wave 4 converted lineDashOffset to an Object.defineProperty getter/
// setter pair: the setter re-pushes the active dash pattern via
// canvasSetLineDash with the new phase. Verify that:
//   1. The default (lineDashOffset=0) reads back as 0.
//   2. Assigning a new value with a pattern in place re-pushes
//      canvasSetLineDash with the new phase.
//   3. Non-finite values (NaN, Infinity) are ignored per spec.
TEST_CASE("Canvas2D lineDashOffset re-flushes dash pattern on assignment",
          "[view][canvas2d][issue-1526][wave4]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.setLineDash([6, 4]);            // phase 0 (default)
        ctx.lineDashOffset = 3;             // re-push w/ phase 3
        ctx.lineDashOffset = NaN;           // ignored — phase stays 3
        ctx.lineDashOffset = Infinity;      // ignored
        ctx.lineDashOffset = -2;            // negative is finite — accepted
        globalThis.__final_offset__ = ctx.lineDashOffset;
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);

    using T = pulp::view::CanvasDrawCmd::Type;
    int set_line_dash_count = 0;
    float last_phase = 0.0f;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == T::set_line_dash) {
            ++set_line_dash_count;
            last_phase = cmd.extra;
        }
    }
    // Expect at least 3 set_line_dash records:
    //   (1) ctx.setLineDash([6,4]) phase=0
    //   (2) ctx.lineDashOffset = 3 (re-flush) phase=3
    //   (3) ctx.lineDashOffset = -2 (re-flush) phase=-2
    // NaN / Infinity assignments must NOT push, so the count must be
    // exactly 3 (not 5).
    REQUIRE(set_line_dash_count == 3);
    REQUIRE(last_phase == Catch::Approx(-2.0f));
}

// Round-trip read: lineDashOffset getter returns the most-recently
// assigned finite value (HTML5 spec).
TEST_CASE("Canvas2D lineDashOffset getter round-trips assigned value",
          "[view][canvas2d][issue-1526][wave4]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var defaults = ctx.lineDashOffset;
        ctx.lineDashOffset = 3;
        var assigned1 = ctx.lineDashOffset;
        ctx.lineDashOffset = NaN;        // ignored
        var assigned2 = ctx.lineDashOffset;
        ctx.lineDashOffset = -2.5;
        var assigned3 = ctx.lineDashOffset;
        return defaults + '|' + assigned1 + '|' + assigned2 + '|' + assigned3;
    )");
    REQUIRE(result == "0|3|3|-2.5");
}

// ── pulp #1527 — getTransform / resetTransform ───────────────────────────
//
// HTML5 spec: ctx.getTransform() returns a DOMMatrix-shaped object whose
// `a, b, c, d, e, f` mirror the current 2D affine transform. The shim
// keeps a JS-side mirror updated by translate / scale / rotate /
// setTransform / transform / save / restore so the read can answer
// synchronously without a bridge round-trip.
TEST_CASE("Canvas2D getTransform returns identity by default",
          "[view][canvas2d][issue-1527]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var m = ctx.getTransform();
        return [m.a, m.b, m.c, m.d, m.e, m.f, m.is2D, m.isIdentity].join(',');
    )");
    REQUIRE(result == "1,0,0,1,0,0,true,true");
}

TEST_CASE("Canvas2D getTransform reflects translate / scale / rotate",
          "[view][canvas2d][issue-1527]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.translate(10, 20);
        ctx.scale(2, 3);
        var m = ctx.getTransform();
        return [m.a, m.b, m.c, m.d, m.e, m.f].join(',');
    )");
    // After translate(10,20) then scale(2,3) on identity: a=2,b=0,c=0,d=3,e=10,f=20.
    REQUIRE(result == "2,0,0,3,10,20");
}

TEST_CASE("Canvas2D setTransform replaces current transform",
          "[view][canvas2d][issue-1527]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.translate(99, 99);
        ctx.setTransform(1, 0, 0, 1, 5, 7);
        var m = ctx.getTransform();
        return [m.a, m.b, m.c, m.d, m.e, m.f, m.isIdentity].join(',');
    )");
    REQUIRE(result == "1,0,0,1,5,7,false");
}

TEST_CASE("Canvas2D resetTransform returns matrix to identity",
          "[view][canvas2d][issue-1527]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.translate(50, 60);
        ctx.scale(3, 4);
        ctx.resetTransform();
        var m = ctx.getTransform();
        return [m.a, m.b, m.c, m.d, m.e, m.f, m.isIdentity].join(',');
    )");
    REQUIRE(result == "1,0,0,1,0,0,true");
}

// pulp #1348 / #1666 — `ctx.transform(a,b,c,d,e,f)` is concat-on-right,
// not replace. Previously only the pure-translation sub-case forwarded
// to the bridge (canvasTranslate fast path); arbitrary scale/rotate/
// skew concats updated the JS-side mirror but never reached the bridge.
// The fix forwards the FULL composed matrix via canvasSetTransform.
TEST_CASE("Canvas2D transform() concats on right and forwards to bridge",
          "[view][canvas2d][issue-1348][codex-p1]") {
    // Scale * translate: result must be the JS-side composed matrix,
    // and getTransform() must reflect that (post-fix the bridge state
    // matches; pre-fix only the JS mirror was correct, but
    // getTransform() reads from the JS mirror anyway, so the failure
    // mode was a *paint-time* divergence, not a getTransform read).
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        // Establish a baseline transform: translate(10, 20)
        ctx.translate(10, 20);
        // Now concat a 2x scale via transform()
        ctx.transform(2, 0, 0, 2, 0, 0);
        var m = ctx.getTransform();
        return [m.a, m.b, m.c, m.d, m.e, m.f].join(',');
    )");
    // After translate(10,20): M = [[1,0,10],[0,1,20]]
    // After transform(2,0,0,2,0,0): M' = M * S
    //   na = 1*2 + 0*0 = 2, nb = 0*2 + 1*0 = 0
    //   nc = 1*0 + 0*2 = 0, nd = 0*0 + 1*2 = 2
    //   ne = 1*0 + 0*0 + 10 = 10, nf = 0*0 + 1*0 + 20 = 20
    REQUIRE(result == "2,0,0,2,10,20");
}

// Sequential transform() concats — exercises the strict-concat path
// across multiple non-translation operations.
TEST_CASE("Canvas2D transform() composes with sequential rotate + scale",
          "[view][canvas2d][issue-1348][codex-p1]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        // Identity → 3x scale → 3x scale (cumulative 9x)
        ctx.transform(3, 0, 0, 3, 0, 0);
        ctx.transform(3, 0, 0, 3, 0, 0);
        var m = ctx.getTransform();
        return [m.a, m.d].join(',');
    )");
    REQUIRE(result == "9,9");
}

TEST_CASE("Canvas2D save/restore restores prior transform",
          "[view][canvas2d][issue-1527]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.translate(10, 20);
        ctx.save();
        ctx.translate(100, 200);
        var inner = ctx.getTransform();
        ctx.restore();
        var outer = ctx.getTransform();
        return [inner.e, inner.f, outer.e, outer.f].join(',');
    )");
    REQUIRE(result == "110,220,10,20");
}

TEST_CASE("Canvas2D getTransform returns independent copy",
          "[view][canvas2d][issue-1527]") {
    // Spec: mutating the returned DOMMatrix must not affect the live ctx
    // transform. Confirm by mutating m.a and reading the next getTransform.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.translate(5, 7);
        var m = ctx.getTransform();
        m.a = 999; m.e = 999;
        var m2 = ctx.getTransform();
        return [m2.a, m2.e].join(',');
    )");
    REQUIRE(result == "1,5");
}

TEST_CASE("Canvas2D getTransform DOMMatrix-shaped fields",
          "[view][canvas2d][issue-1527]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.setTransform(2, 0, 0, 3, 4, 5);
        var m = ctx.getTransform();
        // m11/m12/m21/m22/m41/m42 are the DOMMatrix aliases for a/b/c/d/e/f.
        return [m.m11, m.m12, m.m21, m.m22, m.m41, m.m42, m.is2D].join(',');
    )");
    REQUIRE(result == "2,0,0,3,4,5,true");
}

// ── pulp #1527 — isPointInPath / isPointInStroke ─────────────────────────
TEST_CASE("Canvas2D isPointInPath: point inside rect path",
          "[view][canvas2d][issue-1527]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.beginPath();
        ctx.rect(10, 10, 100, 50);
        return ctx.isPointInPath(50, 30);
    )");
    REQUIRE(result == "true");
}

TEST_CASE("Canvas2D isPointInPath: point outside rect path",
          "[view][canvas2d][issue-1527]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.beginPath();
        ctx.rect(10, 10, 100, 50);
        return ctx.isPointInPath(5, 5);
    )");
    REQUIRE(result == "false");
}

TEST_CASE("Canvas2D isPointInPath: works with moveTo + lineTo polygon",
          "[view][canvas2d][issue-1527]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        // Triangle (0,0) -> (100,0) -> (50,100).
        ctx.beginPath();
        ctx.moveTo(0, 0);
        ctx.lineTo(100, 0);
        ctx.lineTo(50, 100);
        ctx.closePath();
        return [ctx.isPointInPath(50, 30),    // central, low y — inside
                ctx.isPointInPath(10, 80),    // near left base — outside (sloped)
                ctx.isPointInPath(-5, 50)].join(',');
    )");
    // Triangle vertices (0,0)-(100,0)-(50,100). At y=80 the left edge
    // crosses at x=40, so x=10 is outside. Origin-side x=-5 is outside.
    REQUIRE(result == "true,false,false");
}

TEST_CASE("Canvas2D beginPath resets path mirror",
          "[view][canvas2d][issue-1527]") {
    // After beginPath, prior geometry must not contribute to hit tests.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.beginPath();
        ctx.rect(0, 0, 100, 100);
        var hit1 = ctx.isPointInPath(50, 50);
        ctx.beginPath();
        ctx.rect(200, 200, 50, 50);
        var hit2 = ctx.isPointInPath(50, 50);
        return [hit1, hit2].join(',');
    )");
    REQUIRE(result == "true,false");
}

TEST_CASE("Canvas2D isPointInPath rejects non-finite coordinates",
          "[view][canvas2d][issue-1527]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.beginPath();
        ctx.rect(0, 0, 100, 100);
        return [ctx.isPointInPath(NaN, 50),
                ctx.isPointInPath(50, Infinity)].join(',');
    )");
    REQUIRE(result == "false,false");
}

TEST_CASE("Canvas2D isPointInStroke: point on stroke edge",
          "[view][canvas2d][issue-1527]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.lineWidth = 4;  // half-width = 2px
        ctx.beginPath();
        ctx.moveTo(0, 50);
        ctx.lineTo(100, 50);
        return [ctx.isPointInStroke(50, 50),     // on the line
                ctx.isPointInStroke(50, 51),     // 1px below — inside half-width
                ctx.isPointInStroke(50, 60)].join(','); // 10px below — outside
    )");
    REQUIRE(result == "true,true,false");
}

TEST_CASE("Canvas2D isPointInStroke respects lineWidth changes",
          "[view][canvas2d][issue-1527]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.beginPath();
        ctx.moveTo(0, 50);
        ctx.lineTo(100, 50);
        ctx.lineWidth = 2;  // half-width = 1px
        var thin = ctx.isPointInStroke(50, 53);
        ctx.lineWidth = 20; // half-width = 10px
        var thick = ctx.isPointInStroke(50, 53);
        return [thin, thick].join(',');
    )");
    REQUIRE(result == "false,true");
}

TEST_CASE("Canvas2D isPointInPath / isPointInStroke survive save/restore",
          "[view][canvas2d][issue-1527]") {
    // The path mirror is part of the save/restore snapshot, so a save()
    // of an empty path followed by appending geometry, then restore(),
    // must roll back the path so isPointInPath returns false.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.beginPath();          // empty path
        ctx.save();
        ctx.rect(0, 0, 100, 100);
        var inside = ctx.isPointInPath(50, 50);
        ctx.restore();
        var afterRestore = ctx.isPointInPath(50, 50);
        return [inside, afterRestore].join(',');
    )");
    REQUIRE(result == "true,false");
}


// pulp #1527 — DOMMatrix mutator methods on _PulpCanvasMatrix.
// Snapshot semantics preserved per HTML5 spec: mutating the returned
// matrix does NOT affect the live ctx. This proves the 5 mutator
// methods (multiplySelf, scaleSelf, rotateSelf, translateSelf, inverse).
TEST_CASE("Canvas2D getTransform DOMMatrix mutator chains + snapshot semantics",
          "[view][canvas2d][issue-1527][dommatrix-mutators]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe'; document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.setTransform(2, 0, 0, 3, 4, 5);  // [[2,0,4],[0,3,5]]
        var m = ctx.getTransform();
        // multiplySelf with translation matrix [[1,0,10],[0,1,20]]
        var tr = ctx.getTransform();
        tr.a = 1; tr.b = 0; tr.c = 0; tr.d = 1; tr.e = 10; tr.f = 20;
        m.multiplySelf(tr);
        // After: a=2, b=0, c=0, d=3, e=2*10+4=24, f=3*20+5=65
        // Live ctx unaffected (snapshot semantics)
        var live = ctx.getTransform();
        return [m.a, m.b, m.c, m.d, m.e, m.f,
                live.a, live.e, live.f].join(',');
    )");
    REQUIRE(result == "2,0,0,3,24,65,2,4,5");
}

TEST_CASE("Canvas2D getTransform DOMMatrix scaleSelf + isIdentity recompute",
          "[view][canvas2d][issue-1527][dommatrix-mutators]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe'; document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var m = ctx.getTransform();
        var was_identity = m.isIdentity;
        var same = (m.scaleSelf(2, 3) === m);
        return [was_identity, m.a, m.d, m.isIdentity, same].join(',');
    )");
    REQUIRE(result == "true,2,3,false,true");
}

TEST_CASE("Canvas2D getTransform DOMMatrix rotateSelf takes degrees [issue-1730]",
          "[view][canvas2d][issue-1527][issue-1730][dommatrix-mutators]") {
    // Codex P1 on #1730: rotateSelf() input is DEGREES per the
    // DOMMatrix spec (https://drafts.fxtf.org/geometry/#dom-dommatrix-rotateself).
    // Previously this test passed Math.PI/2 and expected a 90deg
    // rotation — that documented the BUG. Now we pass 90 (degrees).
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe'; document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var m = ctx.getTransform();
        m.rotateSelf(90);
        // identity rotated 90deg: a≈0, b≈1, c≈-1, d≈0
        return [Math.abs(m.a) < 1e-10,
                Math.abs(m.b - 1) < 1e-10,
                Math.abs(m.c + 1) < 1e-10,
                Math.abs(m.d) < 1e-10].join(',');
    )");
    REQUIRE(result == "true,true,true,true");
}

TEST_CASE("Canvas2D DOMMatrix rotateSelf(180) flips signs [issue-1730]",
          "[view][canvas2d][issue-1730][dommatrix-mutators]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe'; document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var m = ctx.getTransform();
        m.rotateSelf(180);
        // identity rotated 180deg: a=-1, b=0, c=0, d=-1
        return [Math.abs(m.a + 1) < 1e-10,
                Math.abs(m.b) < 1e-10,
                Math.abs(m.c) < 1e-10,
                Math.abs(m.d + 1) < 1e-10].join(',');
    )");
    REQUIRE(result == "true,true,true,true");
}

TEST_CASE("Canvas2D DOMMatrix rotateSelf() omitted arg defaults to 0 [issue-1730]",
          "[view][canvas2d][issue-1730][dommatrix-mutators]") {
    // Codex P1 on #1730: omitted angle defaults to 0 — must not be NaN.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe'; document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var m = ctx.getTransform();
        m.rotateSelf();
        return [m.a, m.b, m.c, m.d].join(',');
    )");
    REQUIRE(result == "1,0,0,1");
}

TEST_CASE("Canvas2D DOMMatrix scaleSelf() omitted args default to identity [issue-1730]",
          "[view][canvas2d][issue-1730][dommatrix-mutators]") {
    // Codex P2 on #1730: spec says scaleX defaults to 1 when omitted,
    // scaleY defaults to scaleX when omitted. Was producing NaN before.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe'; document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.setTransform(2, 0, 0, 3, 0, 0);
        var m = ctx.getTransform();
        // Bare scaleSelf() — should be a no-op.
        m.scaleSelf();
        var noop_ok = (m.a === 2 && m.d === 3);
        // scaleSelf(4) — applies 4x to BOTH axes.
        m.scaleSelf(4);
        var single_arg_ok = (m.a === 8 && m.d === 12);
        return [noop_ok, single_arg_ok].join(',');
    )");
    REQUIRE(result == "true,true");
}

TEST_CASE("Canvas2D getTransform DOMMatrix translateSelf affine compose",
          "[view][canvas2d][issue-1527][dommatrix-mutators]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe'; document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.setTransform(2, 0, 0, 1, 0, 0);
        var m = ctx.getTransform();
        m.translateSelf(5, 3);
        // e = 0 + 2*5 + 0*3 = 10; f = 0 + 0*5 + 1*3 = 3
        return [m.a, m.d, m.e, m.f].join(',');
    )");
    REQUIRE(result == "2,1,10,3");
}

TEST_CASE("Canvas2D DOMMatrix singular inverse: all 16 components are NaN [issue-1730]",
          "[view][canvas2d][issue-1730][dommatrix-mutators]") {
    // Codex P2 follow-up on #1754: spec says ALL 16 matrix components
    // become NaN for a non-invertible inverse. Constructor only NaN'd
    // the 2D aliases; m13/m14/m23/m24/m31..m34/m43/m44 stayed at
    // constructor-default identity. toFloat32Array/toFloat64Array
    // would return mixed finite/NaN, violating the contract.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe'; document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.setTransform(1, 1, 1, 1, 0, 0);  // det=0
        var ninv = ctx.getTransform().inverse();
        var arr = ninv.toFloat32Array();
        var allNaN = true;
        for (var i = 0; i < arr.length; ++i) {
            if (!isNaN(arr[i])) { allNaN = false; break; }
        }
        return [allNaN, arr.length].join(',');
    )");
    REQUIRE(result == "true,16");
}

TEST_CASE("Canvas2D DOMMatrix toJSON honors actual is2D [issue-1730]",
          "[view][canvas2d][issue-1730][dommatrix-mutators]") {
    // Codex P2 follow-up on #1754: toJSON used to hardcode is2D=true;
    // a singular-inverse result has is2D=false but JSON serialization
    // would lose the inversion-failure indicator.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe'; document.body.appendChild(c);
        var ctx = c.getContext('2d');
        // Identity → is2D=true should serialize true.
        var j_ok = ctx.getTransform().toJSON();
        var ok1 = (j_ok.is2D === true);
        // Singular → is2D=false should serialize false.
        ctx.setTransform(1, 1, 1, 1, 0, 0);
        var j_bad = ctx.getTransform().inverse().toJSON();
        var ok2 = (j_bad.is2D === false);
        return [ok1, ok2].join(',');
    )");
    REQUIRE(result == "true,true");
}

TEST_CASE("Canvas2D DOMMatrix inverse round-trips identity; singular yields NaN matrix [issue-1730]",
          "[view][canvas2d][issue-1527][issue-1730][dommatrix-mutators]") {
    // Codex P1 on #1730: per spec
    // (https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-inverse),
    // a non-invertible matrix produces a matrix with NaN components and
    // is2D=false. It does NOT throw. The previous test asserted "throws
    // TypeError" — that was documenting the BUG.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe'; document.body.appendChild(c);
        var ctx = c.getContext('2d');
        // Inverse of identity is identity.
        var inv = ctx.getTransform().inverse();
        var ok1 = (inv.a === 1 && inv.b === 0 && inv.c === 0 &&
                   inv.d === 1 && inv.e === 0 && inv.f === 0);
        // Singular: does NOT throw, returns NaN matrix with is2D=false.
        ctx.setTransform(1, 1, 1, 1, 0, 0);  // det = 1*1 - 1*1 = 0
        var threw = false;
        var ninv;
        try {
            ninv = ctx.getTransform().inverse();
        } catch (e) {
            threw = true;
        }
        var ok2 = !threw && isNaN(ninv.a) && isNaN(ninv.b) && isNaN(ninv.c) &&
                  isNaN(ninv.d) && isNaN(ninv.e) && isNaN(ninv.f) &&
                  ninv.is2D === false;
        return [ok1, ok2].join(',');
    )");
    REQUIRE(result == "true,true");
}

TEST_CASE("Canvas2D getTransform DOMMatrix method chaining returns this",
          "[view][canvas2d][issue-1527][dommatrix-mutators]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe'; document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var m = ctx.getTransform();
        var same = (m.scaleSelf(2).rotateSelf(0).translateSelf(0, 0) === m);
        return same ? '1' : '0';
    )");
    REQUIRE(result == "1");
}
