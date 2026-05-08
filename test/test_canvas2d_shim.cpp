// test_canvas2d_shim.cpp
//
// pulp #964 — Canvas2D JavaScript shim coverage tests.
//
// Spectr's FilterBank renders to a `<canvas>` via the standard
// CanvasRenderingContext2D API: ctx.save() / ctx.translate() /
// ctx.setTransform() / ctx.createLinearGradient() / ctx.fillStyle =
// gradient / ctx.fillRect() / ctx.beginPath() / ctx.lineTo() /
// ctx.stroke() / ctx.fillText() / ctx.restore(). Until this fix, the
// pulp web-compat layer only exposed a small subset of those methods —
// the very first call to e.g. ctx.save() threw "TypeError: ctx.save is
// not a function" in QuickJS / JSC, the React render boundary swallowed
// the exception, and FilterBank's frame draw aborted before painting
// anything visible. Earlier commands (clearRect, lineTo) showed up in
// the bridge dispatch log because they were called BEFORE the throw,
// but no visible content reached the Skia surface.
//
// These tests exercise the full JS → bridge command path for every
// shim method FilterBank uses, and the [issue-964][skia] case
// rasterizes the FilterBank sequence onto a Skia raster surface and
// asserts the resulting pixels match the expected colour. The Skia
// case fails on origin/main (no shim → render aborts at ctx.save) and
// passes after this fix.

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

namespace {

// Drive a JS snippet against a freshly constructed bridge so all web-compat
// preludes (including web-compat-canvas.js) are evaluated in production
// order. Returns the snippet's expression value coerced to a string via
// String(), or empty string if the engine returned a non-string.
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

// Same as run_in_bridge but exposes the live CanvasWidget so the test can
// inspect the recorded command stream after the JS runs. The DOM-side
// `<canvas>` Element has a generated `_id` (`__el_N__`) distinct from any
// user-set HTML `id` attribute; the bridge's widget lookup is keyed on
// `_id`. The JS test snippets stash the live canvas element on
// `globalThis.__test_canvas_el__` so this helper can read its `_id` back.
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

    // Look up the canvas widget by reading the JS element's `_id` —
    // call AFTER load(). The JS snippet must assign the canvas element
    // it created to `globalThis.__test_canvas_el__`.
    CanvasWidget* canvas() {
        auto v = engine.evaluate("(globalThis.__test_canvas_el__ && "
                                 "globalThis.__test_canvas_el__._id) || ''");
        if (!v.isString()) return nullptr;
        std::string id = std::string(v.getString());
        if (id.empty()) return nullptr;
        return dynamic_cast<CanvasWidget*>(bridge->widget(id));
    }
};

}  // namespace

// ── Existence of state-management methods ────────────────────────────────
//
// Without these the very first call to ctx.save() / ctx.translate() /
// ctx.setTransform() throws TypeError, and the entire frame render
// silently aborts inside React's render boundary. Treat each as a
// hard contract — if a future refactor drops one, the FilterBank-style
// repro will regress.
TEST_CASE("Canvas2D shim exposes save/restore/transform/state methods",
          "[view][canvas2d][issue-964]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        var methods = [
            'save', 'restore',
            'setTransform', 'transform', 'resetTransform',
            'translate', 'scale', 'rotate',
            'arc', 'arcTo', 'rect', 'roundRect', 'ellipse',
            'bezierCurveTo', 'quadraticCurveTo',
            'fillText', 'strokeText',
            'clip',
            'createLinearGradient', 'createRadialGradient',
            'createConicGradient', 'createPattern'
        ];
        var missing = [];
        for (var i = 0; i < methods.length; ++i) {
            if (typeof ctx[methods[i]] !== 'function') missing.push(methods[i]);
        }
        return missing.length === 0 ? 'ok' : ('missing: ' + missing.join(','));
    )");
    REQUIRE(result == "ok");
}

// ── State setters don't throw and update the live ctx ────────────────────
//
// FilterBank reads back textAlign / textBaseline in inner subroutines, so
// the shim must store the assigned value (Canvas2D spec — getter returns
// the most-recently-set value). Setters that go straight to the bridge
// and never store locally would round-trip incorrectly.
TEST_CASE("Canvas2D shim setters round-trip state",
          "[view][canvas2d][issue-964]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.lineCap = 'round';
        ctx.lineJoin = 'bevel';
        ctx.globalAlpha = 0.5;
        ctx.globalCompositeOperation = 'multiply';
        ctx.font = '20px Helvetica';
        return [ctx.textAlign, ctx.textBaseline, ctx.lineCap, ctx.lineJoin,
                ctx.globalAlpha, ctx.globalCompositeOperation, ctx.font].join('|');
    )");
    REQUIRE(result == "center|middle|round|bevel|0.5|multiply|20px Helvetica");
}

// ── createLinearGradient returns a CanvasGradient ────────────────────────
//
// FilterBank does:
//   var bg = ctx.createLinearGradient(0, 0, 0, h);
//   bg.addColorStop(0, 'rgba(8,12,18,0.0)');
//   ctx.fillStyle = bg;
//   ctx.fillRect(0, 0, w, h);
//
// Pre-fix, createLinearGradient was undefined → bg = undefined →
// addColorStop throws → render aborts. Post-fix we return a
// CanvasGradient with addColorStop and the right kind tag.
TEST_CASE("Canvas2D createLinearGradient returns CanvasGradient with addColorStop",
          "[view][canvas2d][issue-964]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var g = ctx.createLinearGradient(0, 0, 100, 0);
        g.addColorStop(0, '#ff0000');
        g.addColorStop(1, '#0000ff');
        return [typeof g, g._kind, g._stops.length,
                g._stops[0].color, g._stops[1].color].join('|');
    )");
    REQUIRE(result == "object|linear|2|#ff0000|#0000ff");
}

// ── createRadialGradient returns a CanvasGradient ────────────────────────
TEST_CASE("Canvas2D createRadialGradient returns CanvasGradient",
          "[view][canvas2d][issue-964]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        c.id = 'probe';
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var g = ctx.createRadialGradient(50, 50, 0, 50, 50, 60);
        g.addColorStop(0, 'rgba(255,255,255,0.6)');
        g.addColorStop(1, 'rgba(0,0,0,0)');
        return [g._kind, g._stops.length, g._params.r1].join('|');
    )");
    REQUIRE(result == "radial|2|60");
}

// pulp #1524 — createRadialGradient with distinct inner+outer circles
// flushes via the new two-circle bridge fn (canvasSetRadialGradientTwoCircles)
// and the resulting CanvasDrawCmd carries BOTH circles. Pre-fix, the JS shim
// stored only the outer circle and dropped (x0, y0, r0) silently.
TEST_CASE("Canvas2D createRadialGradient flushes two circles via the new bridge fn",
          "[view][canvas2d][issue-1524]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        var g = ctx.createRadialGradient(20, 30, 5, 80, 70, 50);
        g.addColorStop(0, '#ff0000');
        g.addColorStop(1, '#0000ff');
        ctx.fillStyle = g;
        ctx.fillRect(0, 0, 100, 100);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool saw_two_circles = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::set_fill_gradient_radial_two_circles) {
            saw_two_circles = true;
            // Inner circle (x0=20, y0=30, r0=5) packed as (x, y, extra).
            REQUIRE(cmd.x      == Catch::Approx(20.0f));
            REQUIRE(cmd.y      == Catch::Approx(30.0f));
            REQUIRE(cmd.extra  == Catch::Approx(5.0f));
            // Outer circle (x1=80, y1=70, r1=50) packed as (x2, y2, w).
            REQUIRE(cmd.x2     == Catch::Approx(80.0f));
            REQUIRE(cmd.y2     == Catch::Approx(70.0f));
            REQUIRE(cmd.w      == Catch::Approx(50.0f));
            REQUIRE(cmd.gradient_colors.size() == 2);
            REQUIRE(cmd.gradient_positions.size() == 2);
        }
    }
    REQUIRE(saw_two_circles);
}

// ── FilterBank-style command stream records via the shim ─────────────────
//
// JS calls ctx.save(); ctx.translate(); ctx.fillStyle = grad; ctx.fillRect();
// ctx.beginPath(); ctx.lineTo(); ctx.stroke(); ctx.restore(); — assert that
// the corresponding canvas* commands queue on the CanvasWidget. Pre-fix,
// only the prefix up to the missing method recorded; post-fix the full
// stream lands on commands_.
TEST_CASE("Canvas2D shim records full FilterBank-style command sequence",
          "[view][canvas2d][issue-964]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.save();
        ctx.translate(10, 20);
        ctx.setTransform(2, 0, 0, 2, 0, 0);
        var grad = ctx.createLinearGradient(0, 0, 0, 100);
        grad.addColorStop(0, '#ff0000');
        grad.addColorStop(1, '#0000ff');
        ctx.fillStyle = grad;
        ctx.fillRect(0, 0, 100, 100);
        ctx.beginPath();
        ctx.moveTo(0, 0);
        ctx.lineTo(50, 50);
        ctx.bezierCurveTo(60, 60, 70, 70, 80, 80);
        ctx.quadraticCurveTo(85, 85, 90, 90);
        ctx.arc(50, 50, 10, 0, 6.28);
        ctx.rect(0, 0, 10, 10);
        ctx.closePath();
        ctx.strokeStyle = '#ffff00';
        ctx.stroke();
        ctx.fillText('hello', 5, 5);
        ctx.restore();
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    REQUIRE(cw->command_count() > 0);

    using T = CanvasDrawCmd::Type;
    bool saw_save = false, saw_restore = false;
    bool saw_translate = false, saw_set_transform = false;
    bool saw_grad = false;
    bool saw_fill_rect = false;
    bool saw_begin = false, saw_move = false, saw_line = false;
    bool saw_cubic = false, saw_quad = false;
    bool saw_close = false, saw_stroke_path = false;
    bool saw_fill_text = false;
    for (const auto& cmd : cw->commands()) {
        switch (cmd.type) {
        case T::save:                       saw_save = true; break;
        case T::restore:                    saw_restore = true; break;
        case T::translate:                  saw_translate = true; break;
        case T::set_transform:              saw_set_transform = true; break;
        case T::set_fill_gradient_linear:   saw_grad = true; break;
        case T::fill_rect:                  saw_fill_rect = true; break;
        case T::begin_path:                 saw_begin = true; break;
        case T::move_to:                    saw_move = true; break;
        case T::line_to:                    saw_line = true; break;
        case T::cubic_to:                   saw_cubic = true; break;
        case T::quad_to:                    saw_quad = true; break;
        case T::close_path:                 saw_close = true; break;
        case T::stroke_path:                saw_stroke_path = true; break;
        case T::fill_text:                  saw_fill_text = true; break;
        default: break;
        }
    }
    INFO("save=" << saw_save << " restore=" << saw_restore
         << " translate=" << saw_translate
         << " set_transform=" << saw_set_transform
         << " grad=" << saw_grad
         << " fill_rect=" << saw_fill_rect
         << " begin=" << saw_begin << " move=" << saw_move
         << " line=" << saw_line << " cubic=" << saw_cubic
         << " quad=" << saw_quad << " close=" << saw_close
         << " stroke_path=" << saw_stroke_path
         << " fill_text=" << saw_fill_text);
    REQUIRE(saw_save);
    REQUIRE(saw_restore);
    REQUIRE(saw_translate);
    REQUIRE(saw_set_transform);
    REQUIRE(saw_grad);
    REQUIRE(saw_fill_rect);
    REQUIRE(saw_begin);
    REQUIRE(saw_move);
    REQUIRE(saw_line);
    REQUIRE(saw_cubic);
    REQUIRE(saw_quad);
    REQUIRE(saw_close);
    REQUIRE(saw_stroke_path);
    REQUIRE(saw_fill_text);
}

// ── Solid colour fillStyle still works after a gradient ──────────────────
//
// Canvas2D spec: assigning a string to fillStyle replaces the previous
// style outright (including any active gradient). The shim must call
// canvasClearGradient on the bridge before falling back to set_fill_color
// so the next fillRect doesn't pick up the stale gradient.
TEST_CASE("Canvas2D fillStyle = string clears prior gradient",
          "[view][canvas2d][issue-964]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 10; c.height = 10;
        var ctx = c.getContext('2d');
        var g = ctx.createLinearGradient(0, 0, 10, 0);
        g.addColorStop(0, '#ff0000');
        ctx.fillStyle = g;
        ctx.fillRect(0, 0, 10, 10);
        ctx.fillStyle = '#00ff00';
        ctx.fillRect(0, 0, 10, 10);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);

    // Walk the recorded JS command stream directly: must see
    // clear_fill_gradient between the two fill_rects so the second fill
    // paints solid green rather than re-using the gradient.
    using T = CanvasDrawCmd::Type;
    int seen_grad = 0, seen_clear_grad = 0, seen_fill_rect = 0;
    bool clear_came_after_grad = false, fill_came_after_clear = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == T::set_fill_gradient_linear) ++seen_grad;
        if (cmd.type == T::clear_fill_gradient) {
            ++seen_clear_grad;
            if (seen_grad > 0) clear_came_after_grad = true;
        }
        if (cmd.type == T::fill_rect) {
            ++seen_fill_rect;
            if (seen_clear_grad > 0) fill_came_after_clear = true;
        }
    }
    INFO("grad=" << seen_grad << " clear_grad=" << seen_clear_grad
         << " fills=" << seen_fill_rect);
    REQUIRE(seen_grad >= 1);
    REQUIRE(seen_clear_grad >= 1);
    REQUIRE(seen_fill_rect >= 2);
    REQUIRE(clear_came_after_grad);
    REQUIRE(fill_came_after_clear);
}

#ifdef PULP_HAS_SKIA

namespace {
struct Pixel { uint8_t r, g, b, a; };
Pixel sample_pixel(SkSurface* surface, int x, int y) {
    SkPixmap pix;
    REQUIRE(surface->peekPixels(&pix));
    auto* row = static_cast<const uint8_t*>(pix.addr(0, y));
    return {row[4 * x + 0], row[4 * x + 1], row[4 * x + 2], row[4 * x + 3]};
}
}  // namespace

// ── End-to-end FilterBank repro: JS → bridge → CanvasWidget → SkiaCanvas ──
//
// Pre-fix this test fails because ctx.save() throws inside the JS
// snippet, the surrounding (function(){ ... })() returns undefined,
// the CanvasWidget receives at most the clearRect that ran before the
// throw, and the centre pixel stays the parent's dark navy. Post-fix,
// the full sequence records and the centre pixel is the gradient
// fill colour.
TEST_CASE("Canvas2D shim end-to-end: FilterBank gradient draws onto Skia surface",
          "[view][canvas2d][skia][issue-964]") {
    SkImageInfo info = SkImageInfo::Make(64, 64, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    sk_canvas->clear(SkColorSetARGB(255, 8, 12, 24));   // parent navy

    // Run the JS render path. The JS sequence mirrors what FilterBank's
    // renderAll() does on every frame: save, transform setup, gradient
    // creation, fill, restore. The bridge widgets queue commands onto
    // the `<canvas id="fb">` CanvasWidget.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 64; c.height = 64;
        var ctx = c.getContext('2d');
        ctx.save();
        ctx.setTransform(1, 0, 0, 1, 0, 0);
        ctx.globalAlpha = 1;
        ctx.globalCompositeOperation = 'source-over';
        ctx.clearRect(0, 0, 64, 64);
        var grad = ctx.createLinearGradient(0, 0, 0, 64);
        grad.addColorStop(0, '#ff0000');
        grad.addColorStop(1, '#ff0000');
        ctx.fillStyle = grad;
        ctx.fillRect(0, 0, 64, 64);
        ctx.restore();
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    REQUIRE(cw->command_count() > 0);

    // Position the canvas widget over the surface and paint via Skia.
    cw->set_bounds({0, 0, 64, 64});
    pulp::canvas::SkiaCanvas canvas(sk_canvas);
    cw->paint(canvas);

    // Centre pixel: must be opaque red (the gradient — both stops are red,
    // so colour-interpolation is irrelevant). Pre-fix this samples the
    // navy parent because the JS render aborts before any fillRect runs.
    auto px = sample_pixel(surface.get(), 32, 32);
    INFO("Centre rgba=(" << int(px.r) << "," << int(px.g) << ","
         << int(px.b) << "," << int(px.a) << ")");
    REQUIRE(px.a == 255);
    REQUIRE(px.r == 255);
    REQUIRE(px.g == 0);
    REQUIRE(px.b == 0);
}

// ── Skia round-trip: solid fillStyle colours render as expected ──────────
//
// Sanity check that the most basic FilterBank-equivalent path —
// ctx.fillStyle = '#00aaff'; ctx.fillRect(...) — actually paints when
// the surrounding ctx.save() / ctx.restore() are in play. Catches
// regressions where save/restore disturb the bridge's active fill
// state.
TEST_CASE("Canvas2D save/restore brackets do not erase fillStyle pixels",
          "[view][canvas2d][skia][issue-964]") {
    SkImageInfo info = SkImageInfo::Make(32, 32, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);

    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 32; c.height = 32;
        var ctx = c.getContext('2d');
        ctx.save();
        ctx.fillStyle = '#00aaff';
        ctx.fillRect(0, 0, 32, 32);
        ctx.restore();
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    cw->set_bounds({0, 0, 32, 32});
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
    cw->paint(canvas);

    auto px = sample_pixel(surface.get(), 16, 16);
    INFO("Centre rgba=(" << int(px.r) << "," << int(px.g) << ","
         << int(px.b) << "," << int(px.a) << ")");
    REQUIRE(px.a == 255);
    REQUIRE(px.r == 0x00);
    REQUIRE(px.g == 0xaa);
    REQUIRE(px.b == 0xff);
}

#endif  // PULP_HAS_SKIA

// ── pulp #1434 batch 7: Canvas2D shadow* sticky state ────────────────────────
//
// These tests cover the JS-side shim behaviour and the bridge route; the
// SkiaCanvas-level pixel verification lives in test_canvas_widget.cpp.
TEST_CASE("Canvas2D shim exposes shadow* properties as round-trip fields",
          "[view][canvas2d][issue-1434-batch-7]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        // Defaults per Canvas2D spec.
        var defaults = [ctx.shadowColor, ctx.shadowBlur,
                        ctx.shadowOffsetX, ctx.shadowOffsetY].join('|');
        ctx.shadowColor = '#ff0000';
        ctx.shadowBlur = 12;
        ctx.shadowOffsetX = 4;
        ctx.shadowOffsetY = -3;
        var assigned = [ctx.shadowColor, ctx.shadowBlur,
                        ctx.shadowOffsetX, ctx.shadowOffsetY].join('|');
        return defaults + ' || ' + assigned;
    )");
    // Default shadowColor in the spec is "rgba(0, 0, 0, 0)"; defaults
    // for the numeric fields are 0. Assigned values must round-trip.
    REQUIRE(result == "rgba(0, 0, 0, 0)|0|0|0 || #ff0000|12|4|-3");
}

TEST_CASE("Canvas2D shim flushes shadow state to the bridge before fillRect",
          "[view][canvas2d][issue-1434-batch-7]") {
    // Drive the full chain: ctx.shadow* assignments -> _syncShadowState
    // -> canvasSetShadow* bridge calls -> CanvasWidget commands. The
    // CanvasWidget's recorded command stream is the assertion target.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 64; c.height = 64;
        var ctx = c.getContext('2d');
        ctx.shadowColor = 'rgba(255, 0, 0, 0.5)';
        ctx.shadowBlur = 10;
        ctx.shadowOffsetX = 5;
        ctx.shadowOffsetY = 7;
        ctx.fillStyle = '#000000';
        ctx.fillRect(10, 10, 20, 20);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    using T = pulp::view::CanvasDrawCmd::Type;
    int color_idx = -1, blur_idx = -1, ox_idx = -1, oy_idx = -1, rect_idx = -1;
    for (size_t i = 0; i < cw->commands().size(); ++i) {
        switch (cw->commands()[i].type) {
            case T::set_shadow_color:    color_idx = (int)i; break;
            case T::set_shadow_blur:     blur_idx  = (int)i; break;
            case T::set_shadow_offset_x: ox_idx    = (int)i; break;
            case T::set_shadow_offset_y: oy_idx    = (int)i; break;
            case T::fill_rect:           rect_idx  = (int)i; break;
            default: break;
        }
    }
    REQUIRE(color_idx >= 0);
    REQUIRE(blur_idx  >= 0);
    REQUIRE(ox_idx    >= 0);
    REQUIRE(oy_idx    >= 0);
    REQUIRE(rect_idx  >= 0);
    // Shadow setters must precede the rect — that's the whole point of
    // the sticky-state model.
    REQUIRE(color_idx < rect_idx);
    REQUIRE(blur_idx  < rect_idx);
    REQUIRE(ox_idx    < rect_idx);
    REQUIRE(oy_idx    < rect_idx);
    // Round-trip the numeric payloads exactly.
    REQUIRE(cw->commands()[blur_idx].extra == 10.0f);
    REQUIRE(cw->commands()[ox_idx].extra   == 5.0f);
    REQUIRE(cw->commands()[oy_idx].extra   == 7.0f);
}

TEST_CASE("Canvas2D shim ignores invalid shadow* assignments",
          "[view][canvas2d][issue-1434-batch-7]") {
    // Per HTML5 spec: assigning a non-finite number to shadowBlur /
    // shadowOffsetX / shadowOffsetY is silently ignored — the previous
    // valid value persists. Our shim implements this by gating the
    // bridge-flush on isFinite() while still letting the JS-side getter
    // mirror the assignment.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        c.width = 32; c.height = 32;
        var ctx = c.getContext('2d');
        ctx.shadowBlur = 4;
        ctx.shadowOffsetX = 2;
        ctx.shadowOffsetY = -3;
        // Assign NaN — must not propagate, but Canvas2D getter still
        // returns the most-recently-assigned raw value (per spec, the
        // setter coerces to a number; we mirror by storing the raw).
        ctx.fillRect(0, 0, 1, 1);  // flush #1 with valid values
        ctx.shadowBlur = NaN;
        ctx.shadowOffsetX = Infinity;
        ctx.shadowOffsetY = -Infinity;
        ctx.fillRect(0, 0, 1, 1);  // flush #2 — NaN/Inf must not leak through
        return 'ok';
    )");
    REQUIRE(result == "ok");
    // No crash + no exception thrown is the assertion. The drawing
    // happened (we'd see undefined / TypeError otherwise).
}

// ── pulp #1434 bridge-thin gap-fill ─────────────────────────────────────
//
// 4 entries flipped from NOT-IMPL → DIVERGE on the canvas2d catalog:
//   * createConicGradient — Skia routes through SkGradientShader::MakeSweep
//   * miterLimit — SkPaint::setStrokeMiter / CGContextSetMiterLimit
//   * imageSmoothingEnabled — SkSamplingOptions / CGContextSetInterpolationQuality
//   * imageSmoothingQuality — same
//
// createPattern (the 4th NOT-IMPL the triage flagged as "include if scope
// allows") is deferred to a follow-up — image-resource handling needs
// real plumbing, not just a bridge fn. Catalog stays NOT-IMPL with a
// note pointing at this PR's deferred scope.

TEST_CASE("Canvas2D createConicGradient returns CanvasGradient with conic kind",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var g = ctx.createConicGradient(1.5707, 50, 50);  // 90° start
        g.addColorStop(0, '#ff0000');
        g.addColorStop(0.5, '#00ff00');
        g.addColorStop(1, '#0000ff');
        return [g._kind, g._stops.length, g._params.cx, g._params.cy,
                Math.round(g._params.startAngle * 1000),
                g._stops[1].color].join('|');
    )");
    REQUIRE(result == "conic|3|50|50|1571|#00ff00");
}

TEST_CASE("Canvas2D conic gradient flushes via canvasSetConicGradient bridge fn",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        var g = ctx.createConicGradient(0, 50, 50);
        g.addColorStop(0, '#ff0000');
        g.addColorStop(1, '#0000ff');
        ctx.fillStyle = g;
        ctx.fillRect(0, 0, 100, 100);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool saw_conic = false;
    bool saw_fill_rect_after_conic = false;
    bool seen_conic = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::set_fill_gradient_conic) {
            saw_conic = true;
            seen_conic = true;
            // Centre + start angle round-trip exactly.
            REQUIRE(cmd.x == 50.0f);
            REQUIRE(cmd.y == 50.0f);
            REQUIRE(cmd.extra == 0.0f);
            // Two stops in (color, position) pairs.
            REQUIRE(cmd.gradient_colors.size() == 2);
            REQUIRE(cmd.gradient_positions.size() == 2);
            REQUIRE(cmd.gradient_positions[0] == 0.0f);
            REQUIRE(cmd.gradient_positions[1] == 1.0f);
        }
        if (seen_conic && cmd.type == CanvasDrawCmd::Type::fill_rect) {
            saw_fill_rect_after_conic = true;
        }
    }
    INFO("set_fill_gradient_conic recorded: " << saw_conic);
    REQUIRE(saw_conic);
    REQUIRE(saw_fill_rect_after_conic);
}

TEST_CASE("Canvas2D miterLimit flushes via canvasSetMiterLimit bridge fn",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        // Default miterLimit is 10 — assigning the same value is a
        // no-op in the bridge cache. Pick a non-default value so we
        // can observe the flush. Spec: assigning happens on the next
        // stroke (via _syncLineState).
        ctx.miterLimit = 4.5;
        ctx.beginPath();
        ctx.moveTo(0, 0);
        ctx.lineTo(50, 0);
        ctx.stroke();
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool saw_miter = false;
    bool saw_stroke_after_miter = false;
    bool seen_miter = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::set_miter_limit) {
            saw_miter = true;
            seen_miter = true;
            REQUIRE(cmd.extra == Catch::Approx(4.5f));
        }
        if (seen_miter && cmd.type == CanvasDrawCmd::Type::stroke_path) {
            saw_stroke_after_miter = true;
        }
    }
    INFO("set_miter_limit recorded: " << saw_miter);
    REQUIRE(saw_miter);
    REQUIRE(saw_stroke_after_miter);
}

TEST_CASE("Canvas2D imageSmoothing flushes via canvasSetImageSmoothing bridge fn",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        // Spec defaults: enabled=true, quality="low". Push non-defaults
        // so the cache invalidates and the bridge fn fires before the
        // next drawImage.
        ctx.imageSmoothingEnabled = false;
        ctx.imageSmoothingQuality = 'high';
        ctx.drawImage('does-not-exist.png', 0, 0, 50, 50);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool saw_smoothing = false;
    bool saw_draw_image_after = false;
    bool seen_smoothing = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::set_image_smoothing) {
            saw_smoothing = true;
            seen_smoothing = true;
            REQUIRE(cmd.int_val == 0);              // enabled = false
            REQUIRE(cmd.extra   == 2.0f);           // quality = high (2)
        }
        if (seen_smoothing && cmd.type == CanvasDrawCmd::Type::draw_image) {
            saw_draw_image_after = true;
        }
    }
    INFO("set_image_smoothing recorded: " << saw_smoothing);
    REQUIRE(saw_smoothing);
    REQUIRE(saw_draw_image_after);
}

TEST_CASE("Canvas2D imageSmoothing quality coerces unknown values to 'low'",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    // The spec restricts quality to "low" | "medium" | "high"; any other
    // assignment must be ignored (we coerce to "low" so the bridge call
    // gets a stable value).
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.imageSmoothingEnabled = true;
        ctx.imageSmoothingQuality = 'ultra-bogus';
        ctx.drawImage('x.png', 0, 0, 1, 1);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool saw = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::set_image_smoothing) {
            saw = true;
            REQUIRE(cmd.int_val == 1);          // enabled
            REQUIRE(cmd.extra   == 0.0f);       // coerced to low
        }
    }
    REQUIRE(saw);
}

TEST_CASE("Canvas2D miterLimit ignores non-positive / non-finite assignments",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    // Spec: assigning ≤ 0, NaN, or +/- Infinity to ctx.miterLimit is
    // silently ignored — the previous valid value persists. The shim
    // uses isFinite + > 0 as the gate.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.miterLimit = 6;
        ctx.beginPath(); ctx.moveTo(0,0); ctx.lineTo(10,0); ctx.stroke();
        ctx.miterLimit = NaN;
        ctx.miterLimit = 0;
        ctx.miterLimit = -3;
        ctx.miterLimit = Infinity;
        ctx.beginPath(); ctx.moveTo(0,0); ctx.lineTo(20,0); ctx.stroke();
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    int miter_cmd_count = 0;
    float last_miter = 0.0f;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::set_miter_limit) {
            ++miter_cmd_count;
            last_miter = cmd.extra;
        }
    }
    // Exactly one bridge flush — the second stroke's _syncLineState
    // sees no change (NaN / 0 / -3 / Inf all filtered out) and skips.
    REQUIRE(miter_cmd_count == 1);
    REQUIRE(last_miter == Catch::Approx(6.0f));
}

// ── Skia raster sanity test (gated, runs only with PULP_HAS_SKIA) ─────
#ifdef PULP_HAS_SKIA
TEST_CASE("Canvas2D conic gradient renders distinct colours via Skia sweep",
          "[view][canvas2d][issue-1434][bridge-thin][skia]") {
    // Smoke test: render a conic gradient and confirm the pixels at
    // (right of centre) and (below centre) differ. With the conic
    // bridge wired, Skia's MakeSweep distributes red→green→blue around
    // the centre — so the right and bottom samples should not match.
    // Pre-fix createConicGradient returned a degenerate linear, which
    // Skia draws as a flat first-stop colour — those samples would
    // match exactly.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 64; c.height = 64;
        var ctx = c.getContext('2d');
        var g = ctx.createConicGradient(0, 32, 32);
        g.addColorStop(0,   '#ff0000');
        g.addColorStop(0.33,'#00ff00');
        g.addColorStop(0.66,'#0000ff');
        g.addColorStop(1,   '#ff0000');
        ctx.fillStyle = g;
        ctx.fillRect(0, 0, 64, 64);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);

    // Rasterize via SkSurface. CanvasWidget::paint(canvas) replays
    // commands_ onto the supplied SkiaCanvas.
    auto info = SkImageInfo::Make(64, 64, kRGBA_8888_SkColorType,
                                  kPremul_SkAlphaType,
                                  SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface);
    pulp::canvas::SkiaCanvas skia_canvas(surface->getCanvas());
    cw->set_bounds({0, 0, 64, 64});
    cw->paint(skia_canvas);

    SkPixmap pm;
    REQUIRE(surface->peekPixels(&pm));
    auto sample = [&](int x, int y) {
        return *static_cast<const uint32_t*>(pm.addr(x, y));
    };
    // Two angularly-distinct points around the centre. With a real
    // conic sweep these MUST be different colours; with a flat-first-stop
    // fallback they would match.
    uint32_t right = sample(60, 32);
    uint32_t below = sample(32, 60);
    INFO("right = 0x" << std::hex << right << " below = 0x" << below);
    REQUIRE(right != below);
}
#endif  // PULP_HAS_SKIA

// ── pulp #1434 — full CSS `font` shorthand parser ──────────────────────────
//
// Pre-fix: the shim only parsed `'<size>px <family>'`, so any Figma
// copy-CSS value of the shape `'italic small-caps bold 14px/1.4 "Inter",
// sans-serif'` collapsed to size + family, dropping every other token.
// Post-fix: `_parseFontShorthand` walks the CSS Fonts Module Level 4
// grammar and dispatches `canvasSetFontFull(id, family, size, weight,
// slant, letterSpacing)` so Skia's `set_font_full` honours weight + slant.
//
// These tests cover both the JS-level parse round-trip and the bridge
// command stream, asserting on the recorded `set_font_full` cmd's fields.

TEST_CASE("Canvas2D _parseFontShorthand: legacy '<size>px <family>'",
          "[view][canvas2d][issue-1434]") {
    // Baseline — the existing form must keep working with all defaults.
    auto result = run_in_bridge(R"(
        var p = CanvasRenderingContext2D._parseFontShorthand('14px Inter');
        return [p.family, p.size, p.weight, p.slant,
                p.variant, String(p.lineHeight)].join('|');
    )");
    REQUIRE(result == "Inter|14|400|0|normal|null");
}

TEST_CASE("Canvas2D _parseFontShorthand: weight + size + family",
          "[view][canvas2d][issue-1434]") {
    auto result = run_in_bridge(R"(
        var p = CanvasRenderingContext2D._parseFontShorthand('bold 14px Inter');
        return [p.family, p.size, p.weight, p.slant].join('|');
    )");
    REQUIRE(result == "Inter|14|700|0");
}

TEST_CASE("Canvas2D _parseFontShorthand: style + size + family",
          "[view][canvas2d][issue-1434]") {
    auto result = run_in_bridge(R"(
        var p = CanvasRenderingContext2D._parseFontShorthand('italic 14px Inter');
        return [p.family, p.size, p.weight, p.slant].join('|');
    )");
    REQUIRE(result == "Inter|14|400|1");
}

TEST_CASE("Canvas2D _parseFontShorthand: style + weight + size + family",
          "[view][canvas2d][issue-1434]") {
    auto result = run_in_bridge(R"(
        var p = CanvasRenderingContext2D._parseFontShorthand('italic bold 14px Inter');
        return [p.family, p.size, p.weight, p.slant].join('|');
    )");
    REQUIRE(result == "Inter|14|700|1");
}

TEST_CASE("Canvas2D _parseFontShorthand: full Figma-style shorthand",
          "[view][canvas2d][issue-1434]") {
    // The canonical Figma copy-CSS value: every token category present.
    auto result = run_in_bridge(R"(
        var p = CanvasRenderingContext2D._parseFontShorthand(
            'italic small-caps bold 14px/1.4 "Inter", sans-serif');
        return [p.family, p.size, p.weight, p.slant,
                p.variant, p.lineHeight].join('|');
    )");
    // Multi-family list passes through verbatim (with the leading quote
    // intact — the shim only unwraps quotes on single-family strings).
    REQUIRE(result == "\"Inter\", sans-serif|14|700|1|small-caps|1.4");
}

TEST_CASE("Canvas2D _parseFontShorthand: 'normal' defaults round-trip",
          "[view][canvas2d][issue-1434]") {
    auto result = run_in_bridge(R"(
        var p = CanvasRenderingContext2D._parseFontShorthand('normal 16px sans-serif');
        return [p.family, p.size, p.weight, p.slant, p.variant].join('|');
    )");
    REQUIRE(result == "sans-serif|16|400|0|normal");
}

TEST_CASE("Canvas2D _parseFontShorthand: numeric weight",
          "[view][canvas2d][issue-1434]") {
    auto result = run_in_bridge(R"(
        var p1 = CanvasRenderingContext2D._parseFontShorthand('100 16px Inter');
        var p2 = CanvasRenderingContext2D._parseFontShorthand('500 16px Inter');
        var p3 = CanvasRenderingContext2D._parseFontShorthand('900 16px Inter');
        return [p1.weight, p2.weight, p3.weight].join('|');
    )");
    REQUIRE(result == "100|500|900");
}

TEST_CASE("Canvas2D _parseFontShorthand: weight keywords",
          "[view][canvas2d][issue-1434]") {
    auto result = run_in_bridge(R"(
        var bo = CanvasRenderingContext2D._parseFontShorthand('bolder 16px X');
        var li = CanvasRenderingContext2D._parseFontShorthand('lighter 16px X');
        var no = CanvasRenderingContext2D._parseFontShorthand('normal 16px X');
        return [bo.weight, li.weight, no.weight].join('|');
    )");
    REQUIRE(result == "700|300|400");
}

TEST_CASE("Canvas2D _parseFontShorthand: oblique maps to slant=1",
          "[view][canvas2d][issue-1434]") {
    auto result = run_in_bridge(R"(
        var p = CanvasRenderingContext2D._parseFontShorthand('oblique 14px Inter');
        return [p.slant, p.variant].join('|');
    )");
    REQUIRE(result == "1|normal");
}

TEST_CASE("Canvas2D _parseFontShorthand: line-height variants",
          "[view][canvas2d][issue-1434]") {
    // Number, length, %, and 'normal' — all must parse without
    // throwing. Per Canvas2D spec, the line-height value is parsed
    // but ignored at render time.
    auto result = run_in_bridge(R"(
        var a = CanvasRenderingContext2D._parseFontShorthand('14px/1.5 Inter');
        var b = CanvasRenderingContext2D._parseFontShorthand('14px/24px Inter');
        var c = CanvasRenderingContext2D._parseFontShorthand('14px/normal Inter');
        return [a.lineHeight, b.lineHeight, String(c.lineHeight)].join('|');
    )");
    REQUIRE(result == "1.5|24|null");
}

TEST_CASE("Canvas2D _parseFontShorthand: single-family quote stripping",
          "[view][canvas2d][issue-1434]") {
    auto result = run_in_bridge(R"(
        var dq = CanvasRenderingContext2D._parseFontShorthand('14px "Inter"');
        var sq = CanvasRenderingContext2D._parseFontShorthand("14px 'Inter'");
        // Multi-family list keeps quotes intact.
        var ml = CanvasRenderingContext2D._parseFontShorthand('14px "Inter", sans-serif');
        return [dq.family, sq.family, ml.family].join('|');
    )");
    REQUIRE(result == "Inter|Inter|\"Inter\", sans-serif");
}

TEST_CASE("Canvas2D _parseFontShorthand: stretch keywords are dropped",
          "[view][canvas2d][issue-1434]") {
    // 'condensed' is a stretch keyword. It's parsed and silently
    // dropped (no canvas-API surface for stretch). The parser must
    // NOT mistake it for a family or fall over.
    auto result = run_in_bridge(R"(
        var p = CanvasRenderingContext2D._parseFontShorthand('bold condensed 14px Inter');
        return [p.family, p.size, p.weight].join('|');
    )");
    REQUIRE(result == "Inter|14|700");
}

TEST_CASE("Canvas2D _parseFontShorthand: empty / family-only fallback",
          "[view][canvas2d][issue-1434]") {
    auto result = run_in_bridge(R"(
        // No '<size>px' token — treat as family-only, keep default 14.
        var p = CanvasRenderingContext2D._parseFontShorthand('Helvetica');
        return [p.family, p.size, p.weight, p.slant].join('|');
    )");
    REQUIRE(result == "Helvetica|14|400|0");
}

TEST_CASE("Canvas2D ctx.font setter dispatches canvasSetFontFull with weight + slant",
          "[view][canvas2d][issue-1434]") {
    // End-to-end JS → bridge: assigning the full shorthand to ctx.font
    // and triggering a draw must record a `set_font_full` cmd carrying
    // the parsed weight + slant on the CanvasWidget.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.font = 'italic bold 18px Inter';
        ctx.fillText('hi', 5, 20);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);

    // Find the most recent set_font_full cmd in the recorded stream.
    using T = CanvasDrawCmd::Type;
    bool saw_full = false;
    std::string family;
    float size = 0, weight = 0, slant = 0;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == T::set_font_full) {
            saw_full = true;
            family = cmd.text;
            size   = cmd.extra;
            weight = cmd.x;
            slant  = cmd.y;
        }
    }
    INFO("family=" << family << " size=" << size
         << " weight=" << weight << " slant=" << slant);
    REQUIRE(saw_full);
    REQUIRE(family == "Inter");
    REQUIRE(size == Catch::Approx(18.0f));
    REQUIRE(weight == Catch::Approx(700.0f));
    REQUIRE(slant == Catch::Approx(1.0f));
}

TEST_CASE("Canvas2D ctx.font setter: legacy 'Npx Family' still routes",
          "[view][canvas2d][issue-1434]") {
    // Pre-PR pipeline: the shim only parsed '<size>px <family>' and
    // dispatched canvasSetFont. Post-PR with no leading tokens we still
    // expect canvasSetFontFull (preferred when registered) to record
    // weight=400, slant=0 — i.e. spec-correct defaults.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.font = '12px Helvetica';
        ctx.fillText('legacy', 0, 12);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);

    using T = CanvasDrawCmd::Type;
    bool saw_full = false;
    std::string family;
    float size = 0, weight = 0, slant = 0;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == T::set_font_full) {
            saw_full = true;
            family = cmd.text; size = cmd.extra;
            weight = cmd.x; slant = cmd.y;
        }
    }
    REQUIRE(saw_full);
    REQUIRE(family == "Helvetica");
    REQUIRE(size == Catch::Approx(12.0f));
    REQUIRE(weight == Catch::Approx(400.0f));
    REQUIRE(slant == Catch::Approx(0.0f));
}

TEST_CASE("Canvas2D ctx.font getter round-trips assigned shorthand verbatim",
          "[view][canvas2d][issue-1434]") {
    // Spec: the Canvas2D `font` IDL attribute must return the most
    // recently-assigned string. The shim stores the raw assignment on
    // `this.font`, so multi-token shorthand round-trips intact even
    // though only size + family + weight + slant make it to the bridge.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.font = 'italic small-caps bold 14px/1.4 "Inter", sans-serif';
        return ctx.font;
    )");
    REQUIRE(result == "italic small-caps bold 14px/1.4 \"Inter\", sans-serif");
}

TEST_CASE("Canvas2D measureText reads the parsed shorthand size + family",
          "[view][canvas2d][issue-1434]") {
    // Pre-PR `measureText` ran its own ad-hoc regex that only matched
    // '<size>px <family>' — `'italic 18px Inter'` would parse the size
    // (18) but `familyMatch` matched the substring after `px ` so it
    // worked accidentally. With the shared parser the family field is
    // canonicalised; assert measureText doesn't throw and returns a
    // numeric width.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.font = 'italic bold 18px Inter';
        var m = ctx.measureText('hello');
        return [typeof m, typeof m.width, m.width >= 0].join('|');
    )");
    REQUIRE(result == "object|number|true");
}

// ─── Codex audit fixes (PR #1495) ─────────────────────────────────────────────

TEST_CASE("Canvas2D _parseFontShorthand: pt unit converts to px (1pt = 4/3 px)",
          "[view][canvas2d][issue-1434]") {
    // Codex P2 audit (PR #1495 comment 3192815904): `12pt Inter` must NOT
    // be treated as `12px Inter`. CSS specifies 1pt = 1/72in and the canvas
    // shim resolves at the conventional 96dpi root, so 1pt = 4/3 px.
    // 12pt → 16px exactly.
    auto result = run_in_bridge(R"(
        var p = CanvasRenderingContext2D._parseFontShorthand('12pt Inter');
        return [p.family, p.size].join('|');
    )");
    REQUIRE(result == "Inter|16");
}

TEST_CASE("Canvas2D _parseFontShorthand: em unit converts to px (1em = 16px)",
          "[view][canvas2d][issue-1434]") {
    // Codex P2 audit (PR #1495 comment 3192815904): `1.2em Inter` was
    // parsed as `1.2px Inter`, producing severely undersized text and
    // wrong measureText widths. Canvas2D has no DOM cascade, so em
    // resolves against a fixed 16px root — same default browsers + every
    // headless Canvas2D shim use. 1.2em → 19.2px.
    auto result = run_in_bridge(R"(
        var p = CanvasRenderingContext2D._parseFontShorthand('1.2em Inter');
        return [p.family, p.size].join('|');
    )");
    REQUIRE(result == "Inter|19.2");
}

TEST_CASE("Canvas2D _parseFontShorthand: rem unit converts to px (1rem = 16px)",
          "[view][canvas2d][issue-1434]") {
    // Codex P2 audit (PR #1495 comment 3192815904): `1rem Inter` was
    // parsed as `1px Inter`. rem resolves against the document root,
    // which canvas2d doesn't have — fall back to the conventional 16px
    // root font size. 1rem → 16px; 2rem → 32px.
    auto result = run_in_bridge(R"(
        var a = CanvasRenderingContext2D._parseFontShorthand('1rem Inter');
        var b = CanvasRenderingContext2D._parseFontShorthand('2rem Inter');
        return [a.size, b.size].join('|');
    )");
    REQUIRE(result == "16|32");
}

TEST_CASE("Canvas2D _parseFontShorthand: pt + bold + family round-trip",
          "[view][canvas2d][issue-1434]") {
    // Combined leading tokens + non-px size: `bold 18pt Helvetica` should
    // resolve to weight=700, size=24 (18 * 4/3), family=Helvetica.
    auto result = run_in_bridge(R"(
        var p = CanvasRenderingContext2D._parseFontShorthand('bold 18pt Helvetica');
        return [p.family, p.size, p.weight].join('|');
    )");
    REQUIRE(result == "Helvetica|24|700");
}

TEST_CASE("Canvas2D fill_text replay preserves rich set_font_full state",
          "[view][canvas2d][issue-1434]") {
    // Codex P1 audit (PR #1495 comment 3192815903): the legacy fill_text
    // replay path in CanvasWidget::paint() called canvas.set_font(family,
    // size) immediately before drawing, which reset weight/slant to
    // normal/upright (SkiaCanvas::set_font(), canvas.cpp:567-575). That
    // clobbered the rich state captured by the immediately-prior
    // set_font_full cmd, so `ctx.font = "italic bold 18px Inter"`
    // followed by `ctx.fillText(...)` rendered as plain Regular upright
    // text. Fix: drop the canvas.set_font() call in fill_text replay —
    // the JS shim's _syncTextState already pushes a set_font_full
    // (or legacy set_font) cmd ahead of every fill_text.
    //
    // Verify by replaying through a RecordingCanvas: the recorded cmd
    // sequence between set_font_full and fill_text must NOT contain a
    // legacy set_font call that would have reset the rich state.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.font = 'italic bold 18px Inter';
        ctx.fillText('hi', 5, 20);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    cw->set_bounds({0, 0, 100, 100});

    pulp::canvas::RecordingCanvas rc;
    cw->paint(rc);

    // Walk the replayed stream. Find the set_font_full and the
    // immediately-following fill_text. Between those two, there must
    // NOT be a legacy set_font cmd (RecordingCanvas captures one for
    // each Canvas::set_font call but explicitly doesn't emit a
    // legacy set_font during set_font_full's capture path inside
    // fill_text).
    using DT = pulp::canvas::DrawCommand::Type;
    int idx_full = -1, idx_fill = -1, idx_legacy_after_full = -1;
    for (size_t i = 0; i < rc.commands().size(); ++i) {
        auto t = rc.commands()[i].type;
        if (t == DT::set_font_full && idx_full < 0)
            idx_full = static_cast<int>(i);
        else if (t == DT::set_font && idx_full >= 0 && idx_fill < 0)
            idx_legacy_after_full = static_cast<int>(i);
        else if (t == DT::fill_text && idx_full >= 0 && idx_fill < 0)
            idx_fill = static_cast<int>(i);
    }
    INFO("idx_full=" << idx_full
         << " idx_legacy_after_full=" << idx_legacy_after_full
         << " idx_fill=" << idx_fill
         << " total=" << rc.commands().size());
    REQUIRE(idx_full >= 0);
    REQUIRE(idx_fill > idx_full);

    // The set_font_full cmd carried weight=700, slant=1.
    const auto& ff = rc.commands()[idx_full];
    REQUIRE(ff.text == "Inter");
    REQUIRE(ff.f[0] == Catch::Approx(18.0f));
    REQUIRE(ff.f[1] == Catch::Approx(700.0f));  // weight
    REQUIRE(ff.f[2] == Catch::Approx(1.0f));    // slant

    // CRITICAL: between idx_full and idx_fill there must be NO legacy
    // set_font (RecordingCanvas captures a side-channel set_font as
    // part of its set_font_full implementation, which appears at
    // index < idx_full because both are pushed by the SAME
    // set_font_full call; the bug we are guarding against is a
    // SEPARATE canvas.set_font(family, size) call inside the
    // fill_text replay branch).
    bool found_clobber = false;
    for (int i = idx_full + 1; i < idx_fill; ++i) {
        if (rc.commands()[i].type == DT::set_font) {
            found_clobber = true;
            break;
        }
    }
    INFO("If found_clobber=true the fill_text replay clobbers set_font_full's rich state.");
    REQUIRE_FALSE(found_clobber);
}

TEST_CASE("Canvas2D ctx.font with em produces correctly-scaled set_font_full size",
          "[view][canvas2d][issue-1434]") {
    // Codex P2 end-to-end: `ctx.font = "1.5em Inter"` should record a
    // set_font_full with size = 1.5 * 16 = 24, NOT size=1.5.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.font = '1.5em Inter';
        ctx.fillText('hi', 5, 20);
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);

    using T = CanvasDrawCmd::Type;
    bool saw = false;
    float size = -1.0f;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == T::set_font_full) { saw = true; size = cmd.extra; }
    }
    REQUIRE(saw);
    REQUIRE(size == Catch::Approx(24.0f));
}

// ── pulp #1434 bridge-thin gap-fill — createPattern (sub-agent #24) ─────
//
// `ctx.createPattern(image, repetition)` returns a CanvasPattern handle
// the shim assigns to fillStyle / strokeStyle. The shim then flushes
// via canvasSetFillPattern / canvasSetStrokePattern. Skia routes through
// SkShader::MakeImage with SkTileMode per axis (real tiled fill); CG
// degrades to the active solid colour (no native pattern shader without
// a CGPattern callback dance).

TEST_CASE("Canvas2D createPattern returns CanvasPattern with pattern kind",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var p = ctx.createPattern('texture.png', 'repeat');
        return [typeof p, p._kind, p._src, p._tileX, p._tileY].join('|');
    )");
    REQUIRE(result == "object|pattern|texture.png|repeat|repeat");
}

TEST_CASE("Canvas2D createPattern supports all four spec repetition values",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    // Spec values: "repeat", "repeat-x", "repeat-y", "no-repeat".
    // Shim maps them to (tile_x, tile_y) pairs the bridge consumes.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var p1 = ctx.createPattern('a.png', 'repeat');
        var p2 = ctx.createPattern('a.png', 'repeat-x');
        var p3 = ctx.createPattern('a.png', 'repeat-y');
        var p4 = ctx.createPattern('a.png', 'no-repeat');
        return [
            p1._tileX, p1._tileY,
            p2._tileX, p2._tileY,
            p3._tileX, p3._tileY,
            p4._tileX, p4._tileY
        ].join('|');
    )");
    REQUIRE(result == "repeat|repeat|repeat|no-repeat|no-repeat|repeat|no-repeat|no-repeat");
}

TEST_CASE("Canvas2D createPattern with null/empty repetition defaults to repeat",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    // Per spec, null / undefined / empty string defaults to "repeat".
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var p1 = ctx.createPattern('a.png', null);
        var p2 = ctx.createPattern('a.png', undefined);
        var p3 = ctx.createPattern('a.png', '');
        return [p1._tileX, p2._tileX, p3._tileX].join('|');
    )");
    REQUIRE(result == "repeat|repeat|repeat");
}

TEST_CASE("Canvas2D createPattern returns null for empty image source",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    // Per spec, returning null is permissible when source is unavailable.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var p = ctx.createPattern('', 'repeat');
        return String(p === null);
    )");
    REQUIRE(result == "true");
}

TEST_CASE("Canvas2D createPattern accepts image-like objects with .src",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    // drawImage normalises image arguments by reading .src / ._src;
    // createPattern should mirror that so plugins can pass real Image
    // instances or shim DOM image objects interchangeably.
    auto result = run_in_bridge(R"(
        var c = document.createElement('canvas');
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var imgLike = { src: 'foo.png' };
        var p = ctx.createPattern(imgLike, 'repeat-x');
        return [p._src, p._tileX, p._tileY].join('|');
    )");
    REQUIRE(result == "foo.png|repeat|no-repeat");
}

TEST_CASE("Canvas2D pattern flushes via canvasSetFillPattern bridge fn",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    // ctx.fillStyle = pattern -> canvasSetFillPattern -> set_fill_pattern
    // command on the CanvasWidget. The fill_rect that follows MUST land
    // after the pattern setter so the active fill paint stays current.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        var p = ctx.createPattern('texture.png', 'repeat');
        ctx.fillStyle = p;
        ctx.fillRect(0, 0, 100, 100);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool saw_pattern = false;
    bool saw_fill_rect_after_pattern = false;
    bool seen_pattern = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::set_fill_pattern) {
            saw_pattern = true;
            seen_pattern = true;
            REQUIRE(cmd.text == "texture.png");
            // tile_x = repeat (bit 0 = 0), tile_y = repeat (bit 1 = 0)
            REQUIRE(cmd.int_val == 0);
        }
        if (seen_pattern && cmd.type == CanvasDrawCmd::Type::fill_rect) {
            saw_fill_rect_after_pattern = true;
        }
    }
    INFO("set_fill_pattern recorded: " << saw_pattern);
    REQUIRE(saw_pattern);
    REQUIRE(saw_fill_rect_after_pattern);
}

TEST_CASE("Canvas2D pattern repeat-x packs tile modes correctly",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    // repeat-x => (repeat, no-repeat) => int_val bit0=0 (x:repeat), bit1=1 (y:no-repeat) => 0b10 = 2
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.fillStyle = ctx.createPattern('a.png', 'repeat-x');
        ctx.fillRect(0, 0, 50, 50);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool found = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::set_fill_pattern) {
            found = true;
            REQUIRE(cmd.int_val == 0b10);  // bit0 = repeat (0), bit1 = no_repeat (1)
        }
    }
    REQUIRE(found);
}

TEST_CASE("Canvas2D pattern repeat-y packs tile modes correctly",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    // repeat-y => (no-repeat, repeat) => int_val bit0=1, bit1=0 => 0b01 = 1
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.fillStyle = ctx.createPattern('a.png', 'repeat-y');
        ctx.fillRect(0, 0, 50, 50);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool found = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::set_fill_pattern) {
            found = true;
            REQUIRE(cmd.int_val == 0b01);  // bit0 = no_repeat, bit1 = repeat
        }
    }
    REQUIRE(found);
}

TEST_CASE("Canvas2D pattern no-repeat packs tile modes correctly",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    // no-repeat => (no-repeat, no-repeat) => int_val 0b11 = 3
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.fillStyle = ctx.createPattern('a.png', 'no-repeat');
        ctx.fillRect(0, 0, 50, 50);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool found = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::set_fill_pattern) {
            found = true;
            REQUIRE(cmd.int_val == 0b11);
        }
    }
    REQUIRE(found);
}

TEST_CASE("Canvas2D pattern strokeStyle flushes via canvasSetStrokePattern",
          "[view][canvas2d][issue-1434][bridge-thin]") {
    // ctx.strokeStyle = pattern -> canvasSetStrokePattern -> set_stroke_pattern
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 100; c.height = 100;
        var ctx = c.getContext('2d');
        ctx.strokeStyle = ctx.createPattern('stroke.png', 'repeat');
        ctx.beginPath();
        ctx.moveTo(0, 0);
        ctx.lineTo(50, 50);
        ctx.stroke();
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    bool saw_stroke_pattern = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::set_stroke_pattern) {
            saw_stroke_pattern = true;
            REQUIRE(cmd.text == "stroke.png");
        }
    }
    REQUIRE(saw_stroke_pattern);
}

// ── Skia raster sanity test (gated, runs only with PULP_HAS_SKIA) ─────
#ifdef PULP_HAS_SKIA
TEST_CASE("Canvas2D pattern set_fill_pattern reaches Skia without throwing",
          "[view][canvas2d][issue-1434][bridge-thin][skia]") {
    // Smoke test mirroring the conic raster guard (#1446 pattern). With
    // the pattern bridge wired, an unresolvable image source must NOT
    // crash — SkiaCanvas::set_fill_pattern fails the decode and clears
    // the gradient shader so the fill falls back to the previous solid
    // colour. The fillRect MUST still rasterize successfully (no crash,
    // no exception) and produce SOME painted pixels for the previous
    // solid-fill setting.
    //
    // We deliberately use a non-existent image path because there's no
    // production image-resource pipeline to depend on in the test
    // harness; the assertion is that the unresolved decode doesn't
    // poison the rest of the paint pass.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 32; c.height = 32;
        var ctx = c.getContext('2d');
        ctx.fillStyle = '#ff0000';
        ctx.fillRect(0, 0, 32, 32);
        // Now set a pattern with a missing source. Skia clears the
        // gradient shader, fillRect falls back to the previously-set
        // solid red colour, raster output stays painted.
        var p = ctx.createPattern('non-existent-file.png', 'repeat');
        ctx.fillStyle = p;
        ctx.fillRect(0, 0, 32, 32);
    )");

    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);

    // Confirm the pattern setter reached the command stream.
    bool saw_pattern = false;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type == CanvasDrawCmd::Type::set_fill_pattern) {
            saw_pattern = true;
            REQUIRE(cmd.text == "non-existent-file.png");
        }
    }
    REQUIRE(saw_pattern);

    // Rasterize via SkSurface — assertion is "no crash, surface paints".
    auto info = SkImageInfo::Make(32, 32, kRGBA_8888_SkColorType,
                                  kPremul_SkAlphaType,
                                  SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface);
    pulp::canvas::SkiaCanvas skia_canvas(surface->getCanvas());
    cw->set_bounds({0, 0, 32, 32});
    cw->paint(skia_canvas);
    SkPixmap pm;
    REQUIRE(surface->peekPixels(&pm));
    // Surface must have non-zero alpha somewhere — the first fillRect
    // (red) painted, even though the second pattern fill degraded.
    auto sample = [&](int x, int y) {
        return *static_cast<const uint32_t*>(pm.addr(x, y));
    };
    bool any_painted = false;
    for (int y = 0; y < 32 && !any_painted; ++y) {
        for (int x = 0; x < 32; ++x) {
            uint32_t px = sample(x, y);
            if ((px >> 24) != 0) { any_painted = true; break; }
        }
    }
    INFO("any pixel painted: " << any_painted);
    REQUIRE(any_painted);
}
#endif  // PULP_HAS_SKIA (closing the gradient/pattern test block above)

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
    // Spec: NaN / Infinity / negative / zero / null / undefined all
    // collapse to "no constraint" — the bridge sees cmd.w == 0 and the
    // paint loop falls through to the legacy unconstrained fill_text.
    ScriptedBridge env;
    env.load(R"(
        var c = document.createElement('canvas');
        globalThis.__test_canvas_el__ = c;
        document.body.appendChild(c);
        c.width = 200; c.height = 60;
        var ctx = c.getContext('2d');
        ctx.font = '14px Inter';
        ctx.fillText('a', 0, 10, NaN);
        ctx.fillText('b', 0, 20, Infinity);
        ctx.fillText('c', 0, 30, -10);
        ctx.fillText('d', 0, 40, 0);
        ctx.fillText('e', 0, 50, null);
        ctx.fillText('f', 0, 60, undefined);
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);

    using T = CanvasDrawCmd::Type;
    int seen = 0;
    for (const auto& cmd : cw->commands()) {
        if (cmd.type != T::fill_text) continue;
        if (cmd.text.size() == 1 && cmd.text[0] >= 'a' && cmd.text[0] <= 'f') {
            INFO("text=" << cmd.text << " observed cmd.w=" << cmd.w);
            REQUIRE(cmd.w == 0.0f);
            seen++;
        }
    }
    REQUIRE(seen == 6);
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
          "[view][canvas2d][issue-1525][skia]") {
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
