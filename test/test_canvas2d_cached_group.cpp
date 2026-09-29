// Canvas2D cached groups: ctx.pulpCachedGroup(key, drawFn).
//
// A cached group records drawFn once and replays it by reference. These tests
// pin the contract that makes that safe to use for any static content:
//
//   * a replay renders exactly the pixels of calling drawFn directly, under
//     the transform in effect at the replay, including composite operations
//     inside the group and after it, on Skia and CoreGraphics;
//   * a replay is one bridge call and does not run drawFn;
//   * invalidation (explicit, resize, a new context) records again;
//   * nothing the group sets, saves or restores leaks out of it.

#include <catch2/catch_test_macros.hpp>
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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;
using namespace pulp::state;

namespace {

constexpr int kW = 64;
constexpr int kH = 32;

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
    void run(const std::string& js) { engine.evaluate(js); }

    CanvasWidget* canvas() {
        auto v = engine.evaluate("(globalThis.__test_canvas_el__ && "
                                 "globalThis.__test_canvas_el__._id) || ''");
        if (!v.isString()) return nullptr;
        std::string id = std::string(v.getString());
        if (id.empty()) return nullptr;
        auto* cw = dynamic_cast<CanvasWidget*>(bridge->widget(id));
        if (cw) cw->set_bounds({0, 0, static_cast<float>(kW), static_cast<float>(kH)});
        return cw;
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
    ctx._pulpRetainedCanvasFrames = true;
)";

// A frame over non-empty content: a background, then the content under test
// inside the caller's translate, then lighter and multiply draws on top of
// it, then a plain draw that must see none of the content's state.
// `group` itself sets composite operations, alpha, a transform and a path.
constexpr const char* kScene = R"(
    globalThis.__groupRuns = 0;
    function group(ctx) {
        __groupRuns++;
        ctx.translate(4, 2);
        ctx.fillStyle = '#30a0ff';
        ctx.globalAlpha = 0.75;
        ctx.fillRect(0, 0, 20, 12);
        ctx.globalCompositeOperation = 'lighter';
        ctx.fillStyle = '#806040';
        ctx.fillRect(10, 6, 20, 12);
        ctx.globalCompositeOperation = 'multiply';
        ctx.fillStyle = '#a0ff60';
        ctx.beginPath();
        ctx.arc(30, 14, 8, 0, Math.PI * 2);
        ctx.fill();
        ctx.strokeStyle = '#ffffff';
        ctx.lineWidth = 2;
        ctx.strokeRect(2, 2, 40, 20);
    }
    function frame(ctx, content, dx, dy) {
        ctx.clearRect(0, 0, 64, 32);
        ctx.globalCompositeOperation = 'source-over';
        ctx.globalAlpha = 1;
        ctx.fillStyle = '#204060';
        ctx.fillRect(0, 0, 64, 32);
        ctx.fillStyle = '#c08020';
        ctx.fillRect(8, 4, 24, 20);
        ctx.save();
        ctx.translate(dx, dy);
        content(ctx);
        ctx.restore();
        ctx.globalCompositeOperation = 'lighter';
        ctx.fillStyle = '#402020';
        ctx.fillRect(20, 10, 30, 16);
        ctx.globalCompositeOperation = 'multiply';
        ctx.fillStyle = '#8080ff';
        ctx.fillRect(0, 16, 40, 12);
        ctx.globalCompositeOperation = 'source-over';
        ctx.fillStyle = '#ff0000';
        ctx.fillRect(56, 0, 8, 8);
    }
    function cached(ctx) { ctx.pulpCachedGroup('scene', group); }
    function immediate(ctx) { ctx.save(); group(ctx); ctx.restore(); }
)";

using Pixels = std::vector<uint8_t>;

#ifdef PULP_HAS_SKIA
Pixels render_skia(CanvasWidget& cw) {
    SkImageInfo info = SkImageInfo::Make(kW, kH, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    // The parent surface is not empty: the canvas composites over it.
    surface->getCanvas()->clear(SkColorSetARGB(255, 90, 90, 90));
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
    cw.paint(canvas);
    SkPixmap pm;
    REQUIRE(surface->peekPixels(&pm));
    Pixels out(static_cast<size_t>(kW) * kH * 4u);
    for (int y = 0; y < kH; ++y)
        std::memcpy(out.data() + static_cast<size_t>(y) * kW * 4u, pm.addr(0, y),
                    static_cast<size_t>(kW) * 4u);
    return out;
}
#endif

#ifdef __APPLE__
Pixels render_cg(CanvasWidget& cw) {
    Pixels pixels(static_cast<size_t>(kW) * kH * 4u, 0u);
    auto cs = CGColorSpaceCreateDeviceRGB();
    const uint32_t bitmap_info = static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
                                 static_cast<uint32_t>(kCGBitmapByteOrder32Big);
    CGContextRef ctx = CGBitmapContextCreate(pixels.data(), kW, kH, 8, kW * 4u, cs, bitmap_info);
    CGColorSpaceRelease(cs);
    REQUIRE(ctx != nullptr);
    CGContextSetRGBFillColor(ctx, 90.0 / 255.0, 90.0 / 255.0, 90.0 / 255.0, 1.0);
    CGContextFillRect(ctx, CGRectMake(0, 0, kW, kH));
    {
        pulp::canvas::CoreGraphicsCanvas canvas(ctx, static_cast<float>(kW),
                                                static_cast<float>(kH));
        cw.paint(canvas);
    }
    CGContextRelease(ctx);
    return pixels;
}
#endif

int count_type(const CanvasWidget& cw, CanvasDrawCmd::Type type) {
    int n = 0;
    for (const auto& cmd : cw.commands())
        if (cmd.type == type) ++n;
    return n;
}

int differing_pixels(const Pixels& a, const Pixels& b) {
    if (a.size() != b.size()) return -1;
    int n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        if (std::memcmp(a.data() + i, b.data() + i, 4) != 0) ++n;
    return n;
}

// Renders the same scene three ways and hands each to `render`: drawn
// directly, on the frame that records the group, and on a later frame that
// only replays it. The later frame moves the content, so the replay must
// follow the transform in effect at the replay rather than at recording.
template <typename Render>
void check_replay_matches_direct_drawing(Render render) {
    ScriptedBridge direct;
    direct.load(std::string(kNewCanvas) + kScene + "frame(ctx, immediate, 6, 3);");
    auto* dcw = direct.canvas();
    REQUIRE(dcw != nullptr);
    const Pixels direct_first = render(*dcw);
    direct.run("frame(ctx, immediate, 9, 5);");
    const Pixels direct_moved = render(*dcw);
    REQUIRE(differing_pixels(direct_first, direct_moved) > 0);

    ScriptedBridge grouped;
    grouped.load(std::string(kNewCanvas) + kScene + "frame(ctx, cached, 6, 3);");
    auto* gcw = grouped.canvas();
    REQUIRE(gcw != nullptr);
    REQUIRE(gcw->group_count() == 1);
    CHECK(differing_pixels(render(*gcw), direct_first) == 0);

    grouped.run("frame(ctx, cached, 9, 5);");
    REQUIRE(grouped.number("__groupRuns") == 1);
    REQUIRE(count_type(*gcw, CanvasDrawCmd::Type::replay_group) == 1);
    CHECK(differing_pixels(render(*gcw), direct_moved) == 0);
}

} // namespace

TEST_CASE("Cached group replay costs one bridge call and skips drawFn",
          "[view][canvas2d][cached-group]") {
    ScriptedBridge env;
    env.load(std::string(kCountCalls) + kNewCanvas + kScene + R"(
        function measure(dx) {
            ctx.clearRect(0, 0, 64, 32);
            ctx.save();
            ctx.translate(dx, 0);
            __calls.total = 0; __calls.by = {};
            ctx.pulpCachedGroup('scene', group);
            globalThis.__groupCalls = __calls.total;
            globalThis.__breakdown = Object.keys(__calls.by).sort().map(function(n) {
                return n + '=' + __calls.by[n];
            }).join(' ');
            ctx.restore();
        }
        measure(0);
        globalThis.__recordCalls = __groupCalls;
        measure(3);
    )");
    INFO("recording frame calls: " << env.number("__recordCalls"));
    INFO("replay frame calls: " << env.string("__breakdown"));
    CHECK(env.number("__groupRuns") == 1);
    CHECK(env.number("__recordCalls") > 10);
    CHECK(env.number("__groupCalls") == 1);
    CHECK(env.number("__calls.by.canvasReplayGroup|0") == 1);
}

#ifdef PULP_HAS_SKIA
TEST_CASE("Cached group replay is pixel-identical to direct drawing on Skia",
          "[view][canvas2d][cached-group][skia]") {
    check_replay_matches_direct_drawing(render_skia);
}
#endif

#ifdef __APPLE__
TEST_CASE("Cached group replay is pixel-identical to direct drawing on CoreGraphics",
          "[view][canvas2d][cached-group][cg]") {
    check_replay_matches_direct_drawing(render_cg);
}
#endif

TEST_CASE("Cached group records again after invalidation",
          "[view][canvas2d][cached-group]") {
    ScriptedBridge env;
    env.load(std::string(kNewCanvas) + R"(
        globalThis.__runs = 0;
        globalThis.__color = '#ff0000';
        function content(ctx) { __runs++; ctx.fillStyle = __color; ctx.fillRect(0, 0, 4, 4); }
        function draw() { ctx.clearRect(0, 0, 64, 32); ctx.pulpCachedGroup('k', content); }
        draw(); draw();
    )");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    const auto fill_of_group = [&] {
        const auto* cmds = cw->group_commands("k");
        REQUIRE(cmds != nullptr);
        for (const auto& cmd : *cmds)
            if (cmd.type == CanvasDrawCmd::Type::set_fill_color)
                return static_cast<int>(std::lround(cmd.color.g * 255.0f));
        return -1;
    };
    CHECK(env.number("__runs") == 1);
    CHECK(fill_of_group() == 0);

    env.run("__color = '#00ff00'; draw();");
    CHECK(env.number("__runs") == 1);    // still valid: the key did not change
    CHECK(fill_of_group() == 0);

    env.run("ctx.pulpInvalidateGroup('k'); draw();");
    CHECK(env.number("__runs") == 2);
    CHECK(fill_of_group() == 255);

    env.run("c.width = 64; draw();");     // same size: not a resize
    CHECK(env.number("__runs") == 2);
    env.run("c.width = 80; draw();");
    CHECK(env.number("__runs") == 3);

    env.run("ctx.pulpInvalidateGroup(); draw();");
    CHECK(env.number("__runs") == 4);

    // A new context for the canvas is a new script drawing it.
    env.run("c._canvasContext2d = null; ctx = c.getContext('2d');"
            "ctx._pulpRetainedCanvasFrames = true; draw();");
    CHECK(env.number("__runs") == 5);
}

TEST_CASE("Cached group state never leaks whether recorded or replayed",
          "[view][canvas2d][cached-group]") {
    ScriptedBridge env;
    env.load(std::string(kNewCanvas) + R"(
        function leaky(ctx) {
            ctx.restore();                 // must not pop the caller's save
            ctx.fillStyle = '#0000ff';
            ctx.globalAlpha = 0.25;
            ctx.globalCompositeOperation = 'destination-out';
            ctx.lineWidth = 7;
            ctx.translate(100, 100);
            ctx.save();                    // left open
            ctx.fillRect(0, 0, 1, 1);
        }
        function draw() {
            ctx.clearRect(0, 0, 64, 32);
            ctx.fillStyle = '#ff0000';
            ctx.lineWidth = 1;
            ctx.save();
            ctx.translate(8, 0);
            ctx.pulpCachedGroup('leaky', leaky);
            ctx.fillRect(0, 0, 8, 8);      // lands at x 8..16 in opaque red
            ctx.restore();
            globalThis.__state = [ctx.fillStyle, ctx.globalAlpha,
                                  ctx.globalCompositeOperation, ctx.lineWidth,
                                  ctx.getTransform().e].join('|');
        }
        draw();
        globalThis.__recorded = __state;
        draw();
    )");
    CHECK(env.string("__recorded") == "#ff0000|1|source-over|1|0");
    CHECK(env.string("__state") == "#ff0000|1|source-over|1|0");
    auto* cw = env.canvas();
    REQUIRE(cw != nullptr);
    REQUIRE(count_type(*cw, CanvasDrawCmd::Type::replay_group) == 1);

    // The replayed sequence is balanced: every save the group opens, and the
    // implicit one around it, is closed inside the group.
    int depth = 0, min_depth = 0;
    for (const auto* cmd : cw->replay_sequence()) {
        if (cmd->type == CanvasDrawCmd::Type::save) ++depth;
        if (cmd->type == CanvasDrawCmd::Type::restore) --depth;
        min_depth = std::min(min_depth, depth);
    }
    CHECK(depth == 0);
    CHECK(min_depth == 0);

#ifdef PULP_HAS_SKIA
    const Pixels px = render_skia(*cw);
    for (int x = 8; x < 16; ++x) {
        const uint8_t* p = px.data() + static_cast<size_t>(4 * 4 * kW + 4 * x);
        INFO("x=" << x);
        CHECK(p[0] == 255);
        CHECK(p[1] == 0);
        CHECK(p[2] == 0);
        CHECK(p[3] == 255);
    }
#endif
}

TEST_CASE("Cached group edge cases", "[view][canvas2d][cached-group]") {
    SECTION("a drawFn that throws keeps no partial group") {
        ScriptedBridge env;
        env.load(std::string(kNewCanvas) + R"(
            globalThis.__caught = '';
            try {
                ctx.pulpCachedGroup('bad', function(ctx) {
                    ctx.fillRect(0, 0, 2, 2);
                    throw new Error('boom');
                });
            } catch (e) { __caught = e.message; }
            ctx.fillStyle = '#ff0000';
            ctx.fillRect(0, 0, 1, 1);
        )");
        CHECK(env.string("__caught") == "boom");
        auto* cw = env.canvas();
        REQUIRE(cw != nullptr);
        CHECK(cw->group_count() == 0);
        // Recording ended, so later draws reach the frame again.
        CHECK(count_type(*cw, CanvasDrawCmd::Type::fill_rect) == 1);
    }
    SECTION("a group inside a recording group draws into it directly") {
        ScriptedBridge env;
        env.load(std::string(kNewCanvas) + R"(
            ctx.pulpCachedGroup('outer', function(ctx) {
                ctx.fillRect(0, 0, 1, 1);
                ctx.pulpCachedGroup('inner', function(ctx) { ctx.fillRect(2, 0, 1, 1); });
            });
        )");
        auto* cw = env.canvas();
        REQUIRE(cw != nullptr);
        CHECK(cw->group_count() == 1);
        const auto* outer = cw->group_commands("outer");
        REQUIRE(outer != nullptr);
        int rects = 0;
        for (const auto& cmd : *outer)
            if (cmd.type == CanvasDrawCmd::Type::fill_rect) ++rects;
        CHECK(rects == 2);
    }
    SECTION("a full clear inside a group does not replace the frame") {
        ScriptedBridge env;
        env.load(std::string(kNewCanvas) + R"(
            ctx.fillRect(0, 0, 1, 1);
            ctx.pulpCachedGroup('clears', function(ctx) { ctx.clearRect(0, 0, 64, 32); });
        )");
        auto* cw = env.canvas();
        REQUIRE(cw != nullptr);
        CHECK(count_type(*cw, CanvasDrawCmd::Type::fill_rect) == 1);
        // The group reads the backdrop, so the canvas keeps its own layer.
        CHECK(cw->needs_backdrop_isolation());
    }
    SECTION("a restore recorded into a group cannot pop state saved outside it") {
        // The shim never sends such a restore while it knows the group's
        // floor; a group recorded through the bridge directly still must not.
        ScriptedBridge env;
        env.load(std::string(kNewCanvas) + R"(
            canvasSave(c._id);
            canvasTranslate(c._id, 8, 0);
            canvasBeginGroup(c._id, 'raw');
            canvasRestore(c._id);
            canvasRestore(c._id);
            canvasEndGroup(c._id);
            canvasFillRect(c._id, 0, 0, 8, 8);
            canvasRestore(c._id);
        )");
        auto* cw = env.canvas();
        REQUIRE(cw != nullptr);
        int depth = 0, min_depth = 0, depth_at_fill = -1;
        for (const auto* cmd : cw->replay_sequence()) {
            if (cmd->type == CanvasDrawCmd::Type::save) ++depth;
            if (cmd->type == CanvasDrawCmd::Type::restore) --depth;
            if (cmd->type == CanvasDrawCmd::Type::fill_rect) depth_at_fill = depth;
            min_depth = std::min(min_depth, depth);
        }
        CHECK(depth_at_fill == 1);   // still inside the outer save
        CHECK(min_depth == 0);
        CHECK(depth == 0);
    }
    SECTION("an abandoned recording does not swallow the next frame") {
        // A script interrupted inside drawFn never reaches canvasEndGroup.
        ScriptedBridge env;
        env.load(std::string(kNewCanvas) + R"(
            canvasBeginGroup(c._id, 'abandoned');
            canvasFillRect(c._id, 0, 0, 1, 1);
            ctx.clearRect(0, 0, 64, 32);
            ctx.fillRect(0, 0, 2, 2);
        )");
        auto* cw = env.canvas();
        REQUIRE(cw != nullptr);
        CHECK(cw->group_count() == 0);
        CHECK(count_type(*cw, CanvasDrawCmd::Type::fill_rect) == 1);
        env.run("canvasBeginGroup(c._id, 'abandoned');"
                "c._canvasContext2d = null; ctx = c.getContext('2d');"
                "ctx.fillRect(0, 0, 2, 2);");
        CHECK(count_type(*cw, CanvasDrawCmd::Type::fill_rect) == 2);
    }
    SECTION("text drawn in a group is visible to replay_sequence consumers") {
        ScriptedBridge env;
        env.load(std::string(kNewCanvas) + R"(
            ctx.pulpCachedGroup('label', function(ctx) { ctx.fillText('gain', 2, 12); });
            ctx.clearRect(0, 0, 64, 32);
            ctx.pulpCachedGroup('label', function(ctx) { ctx.fillText('gain', 2, 12); });
        )");
        auto* cw = env.canvas();
        REQUIRE(cw != nullptr);
        CHECK(count_type(*cw, CanvasDrawCmd::Type::fill_text) == 0);
        int texts = 0;
        for (const auto* cmd : cw->replay_sequence())
            if (cmd->type == CanvasDrawCmd::Type::fill_text && cmd->text == "gain") ++texts;
        CHECK(texts == 1);
    }
}
