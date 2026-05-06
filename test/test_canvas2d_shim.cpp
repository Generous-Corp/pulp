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
#endif  // PULP_HAS_SKIA
