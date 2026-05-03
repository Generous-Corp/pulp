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
