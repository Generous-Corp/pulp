// Canvas2D shim bridge-call budget and save/restore state semantics.
//
// A scripted editor pays roughly a fixed cost per JS->native canvas call, so
// the number of calls a frame makes is its cost. These tests pin the shim's
// two call-count reductions and the semantics they depend on:
//
//   * save()/restore() keep the shim's record of what the native canvas
//     holds, so unchanged state is not re-sent after every restore(). That is
//     only sound because CanvasWidget's replay reverts the Canvas2D drawing
//     state on restore() on every backend, which the pixel tests below prove
//     on Skia and CoreGraphics.
//   * a path of many disjoint subpaths crosses the bridge once.
//
// Counts are taken from JS by wrapping every `canvas*` bridge global: the
// native command stream deliberately cannot tell a batched call from the
// per-point calls it expands to.

#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/drawlist_format.hpp>
#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#endif

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <pulp/canvas/cg_canvas.hpp>
#endif

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace pulp::view;
using namespace pulp::state;

namespace {

struct ScriptedBridge {
    ScriptEngine engine;
    View root;
    StateStore store;
    std::unique_ptr<WidgetBridge> bridge;

    ScriptedBridge() {
        root.set_bounds({0, 0, 400, 300});
        bridge = std::make_unique<WidgetBridge>(engine, root, store);
    }

    void load(const std::string& js) { bridge->load_script(js); }

    CanvasWidget* canvas() {
        auto v = engine.evaluate("(globalThis.__test_canvas_el__ && "
                                 "globalThis.__test_canvas_el__._id) || ''");
        if (!v.isString()) return nullptr;
        std::string id = std::string(v.getString());
        if (id.empty()) return nullptr;
        return dynamic_cast<CanvasWidget*>(bridge->widget(id));
    }

    int64_t number(const std::string& expr) {
        return engine.evaluate(expr).getWithDefault<int64_t>(-1);
    }
    std::string string(const std::string& expr) {
        auto v = engine.evaluate("String(" + expr + ")");
        return v.isString() ? std::string(v.getString()) : std::string{};
    }
};

// Wraps every `canvas*` bridge global so `__calls.total` counts JS->native
// canvas crossings and `__calls.by[name]` counts them per function.
constexpr const char* kCountCalls = R"(
    globalThis.__calls = { total: 0, by: {} };
    Object.getOwnPropertyNames(globalThis).forEach(function(name) {
        if (!/^canvas[A-Z]/.test(name) || typeof globalThis[name] !== "function") return;
        var inner = globalThis[name];
        globalThis[name] = function() {
            __calls.total++;
            __calls.by[name] = (__calls.by[name] || 0) + 1;
            return inner.apply(null, arguments);
        };
    });
)";

constexpr const char* kNewCanvas = R"(
    var c = document.createElement('canvas');
    globalThis.__test_canvas_el__ = c;
    document.body.appendChild(c);
    c.width = 64; c.height = 32;
    var ctx = c.getContext('2d');
)";

// A representative scripted-editor frame: grid lines, tick marks, 64 bars,
// and a glow loop that scopes a transform per bar with save()/restore().
constexpr const char* kEditorFrame = R"(
    function drawFrame(ctx) {
        ctx.clearRect(0, 0, 640, 320);
        ctx.strokeStyle = 'rgba(255,255,255,0.08)';
        ctx.lineWidth = 1;
        ctx.beginPath();
        for (var g = 0; g <= 10; ++g) {
            ctx.moveTo(0, g * 32); ctx.lineTo(640, g * 32);
        }
        ctx.stroke();
        ctx.beginPath();
        for (var t = 0; t < 63; ++t) {
            ctx.moveTo(t * 10, 310); ctx.lineTo(t * 10, 316);
        }
        ctx.stroke();
        ctx.fillStyle = '#3aa0ff';
        for (var b = 0; b < 64; ++b) {
            var h = 40 + (b * 37) % 200;
            ctx.fillRect(b * 10, 300 - h, 8, h);
        }
        ctx.fillStyle = 'rgba(58,160,255,0.35)';
        ctx.shadowColor = '#3aa0ff';
        ctx.shadowBlur = 6;
        for (var k = 0; k < 64; ++k) {
            ctx.save();
            ctx.translate(k * 10 + 4, 300 - (40 + (k * 37) % 200));
            ctx.scale(1, 0.5);
            ctx.beginPath();
            ctx.arc(0, 0, 5, 0, Math.PI * 2);
            ctx.fill();
            ctx.restore();
        }
        ctx.shadowBlur = 0;
        ctx.shadowColor = 'rgba(0,0,0,0)';
        ctx.fillStyle = '#c0c8d0';
        ctx.font = '10px Inter';
        ctx.textAlign = 'center';
        for (var l = 0; l < 8; ++l) {
            ctx.save();
            ctx.fillText(String(l * 100), l * 80 + 40, 12);
            ctx.restore();
        }
    }
)";

int count_type(const CanvasWidget& cw, CanvasDrawCmd::Type type) {
    int n = 0;
    for (const auto& cmd : cw.commands())
        if (cmd.type == type) ++n;
    return n;
}

} // namespace

TEST_CASE("Canvas2D restore() does not re-send unchanged drawing state",
          "[view][canvas2d][call-budget]") {
    ScriptedBridge env;
    env.load(std::string(kNewCanvas) + R"(
        ctx.fillStyle = '#ff0000';
        ctx.strokeStyle = '#00ff00';
        ctx.lineWidth = 3;
        ctx.globalAlpha = 0.5;
        ctx.globalCompositeOperation = 'multiply';
        ctx.shadowColor = '#000000';
        ctx.shadowBlur = 2;
        ctx.filter = 'blur(1px)';
        ctx.font = 'bold 12px Inter';
        for (var i = 0; i < 16; ++i) {
            ctx.save();
            ctx.translate(i, 0);
            ctx.beginPath();
            ctx.arc(4, 4, 2, 0, Math.PI * 2);
            ctx.fill();
            ctx.stroke();
            ctx.fillText('x', 0, 10);
            ctx.restore();
        }
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);

    using T = CanvasDrawCmd::Type;
    REQUIRE(count_type(*cw, T::fill_path) == 16);
    REQUIRE(count_type(*cw, T::save) == 16);
    // Every setter crossed once, before the first draw, and never again:
    // nothing assigned inside the loop differs from the state at save().
    CHECK(count_type(*cw, T::set_fill_color) == 1);
    CHECK(count_type(*cw, T::set_stroke_color) == 1);
    CHECK(count_type(*cw, T::set_line_width) == 1);
    CHECK(count_type(*cw, T::set_global_alpha) == 1);
    CHECK(count_type(*cw, T::set_blend_mode) == 1);
    CHECK(count_type(*cw, T::set_shadow_color) == 1);
    CHECK(count_type(*cw, T::set_shadow_blur) == 1);
    CHECK(count_type(*cw, T::set_filter) == 1);
    CHECK(count_type(*cw, T::set_font_full) == 1);
}

TEST_CASE("Canvas2D restore() reverts the JS-visible drawing state",
          "[view][canvas2d][call-budget]") {
    ScriptedBridge env;
    env.load(std::string(kNewCanvas) + R"(
        ctx.fillStyle = '#ff0000';
        ctx.globalAlpha = 1;
        ctx.globalCompositeOperation = 'source-over';
        ctx.shadowColor = 'rgba(0, 0, 0, 0)';
        ctx.shadowOffsetY = 0;
        ctx.font = '12px Inter';
        ctx.lineWidth = 2;
        ctx.save();
        ctx.fillStyle = '#0000ff';
        ctx.globalAlpha = 0.25;
        ctx.globalCompositeOperation = 'destination-out';
        ctx.shadowColor = '#00ff00';
        ctx.shadowOffsetY = 16;
        ctx.font = 'italic 20px Inter';
        ctx.lineWidth = 9;
        ctx.restore();
        globalThis.__after = [ctx.fillStyle, ctx.globalAlpha, ctx.globalCompositeOperation,
                              ctx.shadowColor, ctx.shadowOffsetY, ctx.font,
                              ctx.lineWidth].join('|');
    )");
    REQUIRE(env.string("__after") ==
            "#ff0000|1|source-over|rgba(0, 0, 0, 0)|0|12px Inter|2");
}

// Native command replacement (a full-frame clearRect on a retained-frame
// canvas) discards what a saved snapshot recorded as sent, so the next draw
// after restore() must send its state again.
TEST_CASE("Canvas2D restore() after a command-stream replacement re-sends state",
          "[view][canvas2d][call-budget]") {
    ScriptedBridge env;
    env.load(std::string(kNewCanvas) + R"(
        ctx._pulpRetainedCanvasFrames = true;
        ctx.fillStyle = '#ff0000';
        ctx.fillRect(0, 0, 4, 4);
        ctx.save();
        ctx.clearRect(0, 0, 64, 32);
        ctx.restore();
        ctx.fillRect(8, 0, 4, 4);
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    CHECK(count_type(*cw, CanvasDrawCmd::Type::set_fill_color) == 1);
    CHECK(count_type(*cw, CanvasDrawCmd::Type::fill_rect) == 1);
}

// fill_text sets the fill colour it carries and stroke_rect sets its own line
// width. The shim's record of sent state must follow, or the next draw that
// asks for the previous value is skipped and paints with the implicit one.
TEST_CASE("Canvas2D draws that set state implicitly keep the sent record honest",
          "[view][canvas2d][call-budget]") {
    ScriptedBridge env;
    env.load(std::string(kNewCanvas) + R"(
        ctx.fillStyle = '#ff0000';
        ctx.fillRect(0, 0, 2, 2);
        var g = ctx.createLinearGradient(0, 0, 10, 0);
        g.addColorStop(0, '#00ff00');
        g.addColorStop(1, '#00ff00');
        ctx.fillStyle = g;
        ctx.fillText('g', 0, 10);        // native fill colour is now #00ff00
        ctx.fillStyle = '#ff0000';
        ctx.fillRect(4, 0, 2, 2);        // must re-send #ff0000

        ctx.lineWidth = 3;
        ctx.beginPath(); ctx.moveTo(0, 0); ctx.lineTo(9, 9); ctx.stroke();
        ctx.strokeRect(1, 1, 5, 5);      // native line width is now 1
        ctx.beginPath(); ctx.moveTo(0, 9); ctx.lineTo(9, 0); ctx.stroke();  // must re-send 3
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    cw->set_bounds({0, 0, static_cast<float>(64), static_cast<float>(32)});
    pulp::canvas::RecordingCanvas rc;
    cw->paint(rc);
    const std::string drawlist = pulp::canvas::format_commands(rc.commands());
    INFO(drawlist);

    std::istringstream lines(drawlist);
    std::string line, fill, width, fill_at_last_rect, width_at_last_stroke;
    while (std::getline(lines, line)) {
        if (line.rfind("set_fill_color", 0) == 0) fill = line;
        if (line.rfind("set_line_width", 0) == 0) width = line;
        if (line.rfind("fill_rect", 0) == 0) fill_at_last_rect = fill;
        if (line.rfind("stroke_current_path", 0) == 0) width_at_last_stroke = width;
    }
    CHECK(fill_at_last_rect.find("rgba(1.000 0.000 0.000 1.000)") != std::string::npos);
    CHECK(width_at_last_stroke.rfind("set_line_width 3.000 ", 0) == 0);
}

TEST_CASE("Canvas2D disjoint subpaths cross the bridge in one call",
          "[view][canvas2d][call-budget][path-batching]") {
    const std::string ticks = R"(
        ctx.beginPath();
        for (var t = 0; t < 63; ++t) { ctx.moveTo(t, 20); ctx.lineTo(t, 26); }
        ctx.rect(1, 1, 5, 5);
        ctx.lineTo(9, 9);
        ctx.moveTo(40, 4);
        ctx.stroke();
    )";

    ScriptedBridge batched;
    batched.load(std::string(kCountCalls) + kNewCanvas + ticks);
    auto* bcw = batched.canvas();
    REQUIRE(bcw != nullptr);
    INFO("canvasPathPolyline calls: " << batched.number("__calls.by.canvasPathPolyline|0"));
    CHECK(batched.number("__calls.by.canvasPathPolyline|0") == 1);
    CHECK(batched.number("(__calls.by.canvasMoveTo|0) + (__calls.by.canvasLineTo|0)") == 0);

    // The same path through the per-point entry points is the reference
    // stream; batching must not change a single recorded command.
    ScriptedBridge unbatched;
    unbatched.load(std::string("delete globalThis.canvasPathPolyline;\n") + kNewCanvas + ticks);
    auto* ucw = unbatched.canvas();
    REQUIRE(ucw != nullptr);
    REQUIRE(bcw->commands().size() == ucw->commands().size());
    int move_tos = 0;
    for (size_t i = 0; i < bcw->commands().size(); ++i) {
        const auto& a = bcw->commands()[i];
        const auto& b = ucw->commands()[i];
        INFO("command " << i);
        REQUIRE(a.type == b.type);
        REQUIRE(a.x == b.x);
        REQUIRE(a.y == b.y);
        if (a.type == CanvasDrawCmd::Type::move_to) ++move_tos;
    }
    // 63 ticks, the rect, and the trailing lone moveTo.
    CHECK(move_tos == 65);
}

TEST_CASE("Canvas2D path run past the bridge coordinate cap keeps every point",
          "[view][canvas2d][call-budget][path-batching]") {
    // The bridge rejects a polyline above 65536 coordinates as a whole, so
    // the shim must flush before a run crosses it rather than lose the path.
    ScriptedBridge env;
    env.load(std::string(kNewCanvas) + R"(
        ctx.beginPath();
        for (var i = 0; i < 40000; ++i) {
            if (i === 0) ctx.moveTo(i, i % 7); else ctx.lineTo(i, i % 7);
        }
        ctx.moveTo(1, 1); ctx.lineTo(2, 2);
        ctx.stroke();
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    CHECK(count_type(*cw, CanvasDrawCmd::Type::move_to) == 2);
    CHECK(count_type(*cw, CanvasDrawCmd::Type::line_to) == 40000);
}

TEST_CASE("canvasPathPolyline rejects malformed subpath starts whole",
          "[view][canvas2d][call-budget][path-batching]") {
    ScriptedBridge env;
    env.load(std::string(kNewCanvas) + R"(
        canvasPathPolyline(c._id, [0, 0, 1, 1, 2, 2], [2, 1]);   // not increasing
        canvasPathPolyline(c._id, [0, 0, 1, 1, 2, 2], [3]);      // out of range
        canvasPathPolyline(c._id, [0, 0, 1, 1, 2, 2], [0]);      // restates point 0
        canvasPathPolyline(c._id, [0, 0, 1, 1, 2, 2], [1.5]);    // not an index
        canvasPathPolyline(c._id, [0, 0, 1, 1, 2, 2], 'x');      // not an array
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    CHECK(count_type(*cw, CanvasDrawCmd::Type::move_to) == 0);
    CHECK(count_type(*cw, CanvasDrawCmd::Type::line_to) == 0);
}

TEST_CASE("Canvas2D path mirror stays exact for long paths without per-point arrays",
          "[view][canvas2d][call-budget][hit-test]") {
    ScriptedBridge env;
    env.load(std::string(kNewCanvas) + R"(
        ctx.beginPath();
        var n = 20000;
        for (var i = 0; i < n; ++i) {
            var a = (i / n) * Math.PI * 2;
            var x = 100 + 80 * Math.cos(a), y = 100 + 80 * Math.sin(a);
            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        }
        ctx.closePath();
        ctx.moveTo(300, 300); ctx.lineTo(320, 300); ctx.lineTo(320, 320); ctx.lineTo(300, 320);
        globalThis.__hits = [ctx.isPointInPath(100, 100), ctx.isPointInPath(100, 179),
                             ctx.isPointInPath(100, 181), ctx.isPointInPath(310, 310),
                             ctx.isPointInPath(330, 310), ctx.isPointInStroke(180, 100)].join(',');
        // One array per subpath holding plain numbers, not one per point.
        var subs = ctx._pathSubpaths;
        globalThis.__shape = subs.length + ':' + subs[0].length + ':' + typeof subs[0][1];
    )");
    CHECK(env.string("__hits") == "true,true,false,true,false,true");
    CHECK(env.string("__shape") == "2:40002:number");
}

TEST_CASE("Canvas2D representative editor frame call budget",
          "[view][canvas2d][call-budget]") {
    ScriptedBridge env;
    env.load(std::string(kCountCalls) + kNewCanvas + kEditorFrame + R"(
        c.width = 640; c.height = 320;
        ctx._pulpRetainedCanvasFrames = true;
        drawFrame(ctx);                 // first frame sends the initial state
        __calls.total = 0; __calls.by = {};
        drawFrame(ctx);
        var names = Object.keys(__calls.by).sort();
        globalThis.__breakdown = names.map(function(n) { return n + '=' + __calls.by[n]; }).join(' ');
    )");
    const auto total = env.number("__calls.total");
    INFO("steady-state canvas calls per frame: " << total);
    INFO("by function: " << env.string("__breakdown"));
    // 64 fillRects, 64 glow iterations of save/translate/scale/beginPath/arc/
    // fill/restore, 8 labels of save/fillText/restore, two batched strokes,
    // and the state changes the frame actually makes.
    CHECK(total <= 64 + 64 * 7 + 8 * 3 + 2 * 3 + 30);
    CHECK(env.number("__calls.by.canvasPathPolyline|0") == 2);
    CHECK(env.number("__calls.by.canvasSetGlobalAlpha|0") <= 1);
    CHECK(env.number("__calls.by.canvasSetFillColor|0") <= 3);
}

// ── Restore semantics rendered on real backends ─────────────────────────────
//
// State assigned inside save()/restore() must not leak into draws after it.
// Before the draw that follows restore() the shim sends nothing (its record
// says the canvas already holds the outer state), so these pixels are
// correct only if the replay itself reverted the backend.

namespace {

constexpr int kW = 64;
constexpr int kH = 32;

// Outer state is sent by a first draw, then overridden inside save/restore by
// a draw, then a draw after restore must use the outer state again.
constexpr const char* kRestoreScene = R"(
    ctx.fillStyle = '#ff0000';
    ctx.fillRect(60, 0, 4, 4);
    ctx.save();
    ctx.fillStyle = '#0000ff';
    ctx.globalAlpha = 0.25;
    ctx.globalCompositeOperation = 'destination-out';
    ctx.shadowColor = '#00ff00';
    ctx.shadowOffsetY = 16;
    ctx.fillRect(0, 0, 16, 8);
    ctx.restore();
    ctx.fillRect(32, 0, 16, 8);
    ctx.save();
    var g = ctx.createLinearGradient(0, 0, 64, 0);
    g.addColorStop(0, '#0000ff');
    g.addColorStop(1, '#0000ff');
    ctx.fillStyle = g;
    ctx.fillRect(0, 24, 8, 8);
    ctx.restore();
    ctx.fillRect(16, 24, 8, 8);
)";

struct PixelCounts {
    int opaque_red = 0;
    int any_green = 0;
    int any_blue = 0;
};

// rgba8, unpremultiplied enough for these opaque/absent checks.
PixelCounts count_pixels(const uint8_t* rgba, int w, int h, size_t row_bytes) {
    PixelCounts out;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = rgba + static_cast<size_t>(y) * row_bytes + static_cast<size_t>(x) * 4;
            if (p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255) ++out.opaque_red;
            if (p[1] > 0) ++out.any_green;
            if (p[2] > 0) ++out.any_blue;
        }
    }
    return out;
}

void check_restore_scene(const PixelCounts& px) {
    INFO("opaque red=" << px.opaque_red << " green=" << px.any_green
         << " blue=" << px.any_blue);
    // Every red rect opaque at full size: the ones after restore() inherited
    // neither globalAlpha, destination-out, nor the gradient fill. No shadow
    // anywhere; the first blue rect was erased by its own destination-out and
    // only the gradient rect is blue.
    CHECK(px.opaque_red == 4 * 4 + 16 * 8 + 8 * 8);
    CHECK(px.any_green == 0);
    CHECK(px.any_blue == 8 * 8);
}

CanvasWidget* load_scene(ScriptedBridge& env) {
    env.load(std::string(kNewCanvas) + kRestoreScene);
    auto* cw = env.canvas();
    if (cw) cw->set_bounds({0, 0, static_cast<float>(kW), static_cast<float>(kH)});
    return cw;
}

} // namespace

TEST_CASE("Canvas2D restore() reverts drawing state in the replayed drawlist",
          "[view][canvas2d][call-budget]") {
    ScriptedBridge env;
    auto* cw = load_scene(env);
    REQUIRE(cw != nullptr);
    pulp::canvas::RecordingCanvas rc;
    cw->paint(rc);
    const std::string drawlist = pulp::canvas::format_commands(rc.commands());
    INFO(drawlist);

    // The state in effect at the last fill_rect, read back from the drawlist
    // as a backend whose restore() keeps paint state (Skia) would hold it.
    std::istringstream lines(drawlist);
    std::string line, fill, blend, shadow_y, outer_blend;
    std::string fill_at_last, blend_at_last, shadow_at_last;
    int fill_rects = 0;
    while (std::getline(lines, line)) {
        if (line.rfind("set_fill_color", 0) == 0) fill = line;
        if (line.rfind("set_blend_mode", 0) == 0) {
            blend = line;
            if (outer_blend.empty()) outer_blend = line;
        }
        if (line.rfind("set_shadow_offset_y", 0) == 0) shadow_y = line;
        if (line.rfind("fill_rect", 0) == 0) {
            ++fill_rects;
            fill_at_last = fill;
            blend_at_last = blend;
            shadow_at_last = shadow_y;
        }
    }
    REQUIRE(fill_rects == 5);
    CHECK(fill_at_last.find("rgba(1.000 0.000 0.000 1.000)") != std::string::npos);
    // The composite op in effect is the one sent before save().
    REQUIRE_FALSE(outer_blend.empty());
    CHECK(blend_at_last == outer_blend);
    CHECK((shadow_at_last.empty() || shadow_at_last.rfind("set_shadow_offset_y 0.000 ", 0) == 0));
}

#ifdef PULP_HAS_SKIA
TEST_CASE("Canvas2D restore() reverts drawing state on Skia",
          "[view][canvas2d][call-budget][skia]") {
    ScriptedBridge env;
    auto* cw = load_scene(env);
    REQUIRE(cw != nullptr);
    SkImageInfo info = SkImageInfo::Make(kW, kH, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
    cw->paint(canvas);
    SkPixmap pm;
    REQUIRE(surface->peekPixels(&pm));
    check_restore_scene(count_pixels(static_cast<const uint8_t*>(pm.addr()),
                                     kW, kH, pm.rowBytes()));
}
#endif

#ifdef __APPLE__
TEST_CASE("Canvas2D restore() reverts drawing state on CoreGraphics",
          "[view][canvas2d][call-budget][cg]") {
    ScriptedBridge env;
    auto* cw = load_scene(env);
    REQUIRE(cw != nullptr);
    std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4u, 0u);
    auto cs = CGColorSpaceCreateDeviceRGB();
    const uint32_t bitmap_info = static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
                                 static_cast<uint32_t>(kCGBitmapByteOrder32Big);
    CGContextRef ctx = CGBitmapContextCreate(pixels.data(), kW, kH, 8, kW * 4u, cs, bitmap_info);
    CGColorSpaceRelease(cs);
    REQUIRE(ctx != nullptr);
    {
        pulp::canvas::CoreGraphicsCanvas canvas(ctx, static_cast<float>(kW),
                                                static_cast<float>(kH));
        cw->paint(canvas);
    }
    CGContextRelease(ctx);
    check_restore_scene(count_pixels(pixels.data(), kW, kH, static_cast<size_t>(kW) * 4u));
}
#endif
