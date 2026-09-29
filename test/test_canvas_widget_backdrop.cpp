// CanvasWidget per-canvas layer: opened only when the stream reads its backdrop.
//
// Canvas2D gives every canvas its own backing store. CanvasWidget realises
// that with a full-bounds offscreen layer, which costs an allocation, clear and
// composite per canvas per paint. Only commands whose result depends on the
// pixels under them - clearRect, putImageData, and composite operations other
// than source-over - can tell the difference, so the layer is opened only for
// streams that contain one. These tests pin both halves:
//
//   * the decision, read from the replayed drawlist;
//   * that skipping the layer changes no pixel for a source-over stream, and
//     that lighter / multiply / screen / clearRect over a non-empty parent
//     still composite against this canvas's own (transparent) pixels, on
//     Skia and CoreGraphics.

#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/drawlist_format.hpp>
#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/view/canvas_widget.hpp>

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
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace pulp::view;
using pulp::canvas::Canvas;
using pulp::canvas::Color;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;

namespace {

using T = CanvasDrawCmd::Type;
constexpr int kW = 48;
constexpr int kH = 24;

CanvasDrawCmd fill_rect(float x, float y, float w, float h, Color c) {
    CanvasDrawCmd cmd;
    cmd.type = T::fill_rect;
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    cmd.color = c;
    return cmd;
}

CanvasDrawCmd blend(Canvas::BlendMode mode) {
    CanvasDrawCmd cmd;
    cmd.type = T::set_blend_mode;
    cmd.int_val = static_cast<int>(mode);
    return cmd;
}

CanvasDrawCmd global_alpha(float a) {
    CanvasDrawCmd cmd;
    cmd.type = T::set_global_alpha;
    cmd.extra = a;
    return cmd;
}

CanvasDrawCmd clear_rect(float x, float y, float w, float h) {
    CanvasDrawCmd cmd;
    cmd.type = T::clear_rect;
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    return cmd;
}

bool paints_into_layer(CanvasWidget& cw) {
    RecordingCanvas rc;
    cw.paint(rc);
    for (const auto& c : rc.commands())
        if (c.type == DrawCommand::Type::save_layer) return true;
    return false;
}

// A source-over stream with translucency, overlap and drawing past the
// widget's bounds, which the layer (or its replacement clip) must cut off.
void add_source_over_scene(CanvasWidget& cw) {
    cw.add_command(fill_rect(2, 2, 20, 12, Color::rgba8(40, 160, 255, 255)));
    cw.add_command(global_alpha(0.5f));
    cw.add_command(fill_rect(10, 6, 30, 14, Color::rgba8(255, 80, 20, 255)));
    cw.add_command(global_alpha(1.0f));
    cw.add_command(fill_rect(30, -8, 40, 12, Color::rgba8(20, 220, 90, 180)));
    cw.add_command(blend(Canvas::BlendMode::source_over));
    cw.add_command(fill_rect(-6, 16, 20, 20, Color::rgba8(250, 250, 250, 120)));
}

} // namespace

TEST_CASE("CanvasWidget opens its layer only for backdrop-reading streams",
          "[canvas_widget][backdrop-isolation]") {
    SECTION("source-over only: clipped save, no layer") {
        CanvasWidget cw;
        cw.set_bounds({0, 0, kW, kH});
        add_source_over_scene(cw);
        REQUIRE_FALSE(cw.needs_backdrop_isolation());
        RecordingCanvas rc;
        cw.paint(rc);
        const std::string drawlist = pulp::canvas::format_commands(rc.commands());
        INFO(drawlist);
        CHECK(drawlist.find("save_layer") == std::string::npos);
        CHECK(drawlist.rfind("save ", 0) == 0);
        CHECK(drawlist.find("clip_rect 0.000 0.000 48.000 24.000") != std::string::npos);
        CHECK(rc.save_count() == 0);
    }
    SECTION("each backdrop-reading command opens the layer") {
        const std::vector<CanvasDrawCmd> readers = {
            clear_rect(0, 0, 4, 4),
            blend(Canvas::BlendMode::lighter),
            blend(Canvas::BlendMode::multiply),
            blend(Canvas::BlendMode::screen),
            blend(Canvas::BlendMode::destination_out),
            blend(Canvas::BlendMode::copy),
            [] { CanvasDrawCmd c; c.type = T::put_image_data; return c; }(),
        };
        for (const auto& reader : readers) {
            CanvasWidget cw;
            cw.set_bounds({0, 0, kW, kH});
            add_source_over_scene(cw);
            cw.add_command(reader);
            INFO("command type " << static_cast<int>(reader.type) << " mode " << reader.int_val);
            CHECK(cw.needs_backdrop_isolation());
            CHECK(paints_into_layer(cw));
        }
    }
    SECTION("replacing the stream re-evaluates the decision") {
        CanvasWidget cw;
        cw.set_bounds({0, 0, kW, kH});
        cw.add_command(clear_rect(0, 0, kW, kH));
        REQUIRE(paints_into_layer(cw));
        cw.clear_commands();
        add_source_over_scene(cw);
        CHECK_FALSE(paints_into_layer(cw));
    }
    SECTION("a shader effect keeps the layer") {
        CanvasWidget cw;
        cw.set_bounds({0, 0, kW, kH});
        add_source_over_scene(cw);
        cw.set_shader_effect("grain", 0.5f);
        CHECK(paints_into_layer(cw));
    }
}

// ── Pixels ──────────────────────────────────────────────────────────────────

namespace {

using Pixels = std::vector<uint8_t>;
// Paints `paint` over a parent pre-filled with opaque `bg`; returns RGBA8.
using Backend = std::function<Pixels(Color bg, const std::function<void(Canvas&)>& paint)>;

struct NamedBackend {
    const char* name;
    Backend render;
};

std::vector<NamedBackend> backends() {
    std::vector<NamedBackend> out;
#ifdef PULP_HAS_SKIA
    out.push_back({"skia", [](Color bg, const std::function<void(Canvas&)>& paint) {
        SkImageInfo info = SkImageInfo::Make(kW, kH, kRGBA_8888_SkColorType,
                                             kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
        auto surface = SkSurfaces::Raster(info);
        REQUIRE(surface != nullptr);
        surface->getCanvas()->clear(SkColor4f{bg.r, bg.g, bg.b, bg.a});
        pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
        paint(canvas);
        SkPixmap pm;
        REQUIRE(surface->peekPixels(&pm));
        Pixels px(static_cast<size_t>(kW) * kH * 4u);
        for (int y = 0; y < kH; ++y) {
            const auto* row = static_cast<const uint8_t*>(pm.addr(0, y));
            std::copy(row, row + kW * 4, px.begin() + static_cast<long>(y) * kW * 4);
        }
        return px;
    }});
#endif
#ifdef __APPLE__
    out.push_back({"coregraphics", [](Color bg, const std::function<void(Canvas&)>& paint) {
        Pixels px(static_cast<size_t>(kW) * kH * 4u, 0u);
        auto cs = CGColorSpaceCreateDeviceRGB();
        const uint32_t info = static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
                              static_cast<uint32_t>(kCGBitmapByteOrder32Big);
        CGContextRef ctx = CGBitmapContextCreate(px.data(), kW, kH, 8, kW * 4u, cs, info);
        CGColorSpaceRelease(cs);
        REQUIRE(ctx != nullptr);
        CGContextSetRGBFillColor(ctx, bg.r, bg.g, bg.b, bg.a);
        CGContextFillRect(ctx, CGRectMake(0, 0, kW, kH));
        {
            pulp::canvas::CoreGraphicsCanvas canvas(ctx, static_cast<float>(kW),
                                                    static_cast<float>(kH));
            paint(canvas);
        }
        CGContextRelease(ctx);
        return px;
    }});
#endif
    return out;
}

Pixels render(const NamedBackend& backend, Color bg, CanvasWidget& cw) {
    return backend.render(bg, [&](Canvas& c) { cw.paint(c); });
}

// Pixel at (x, y), independent of whether the backend's rows run up or down:
// the scenes below are horizontally symmetric bands, sampled mid-height.
const uint8_t* at(const Pixels& px, int x, int y) {
    return px.data() + (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
}

bool near(const uint8_t* p, int r, int g, int b, int a, int tol = 2) {
    return std::abs(p[0] - r) <= tol && std::abs(p[1] - g) <= tol &&
           std::abs(p[2] - b) <= tol && std::abs(p[3] - a) <= tol;
}

} // namespace

TEST_CASE("CanvasWidget without a layer paints the same pixels as with one",
          "[canvas_widget][backdrop-isolation]") {
    for (const auto& backend : backends()) {
        INFO("backend " << backend.name);
        CanvasWidget direct;
        direct.set_bounds({0, 0, kW, kH});
        add_source_over_scene(direct);
        REQUIRE_FALSE(paints_into_layer(direct));

        // An empty clearRect reads the backdrop in principle, so it forces
        // the layer, and in practice changes no pixel.
        CanvasWidget layered;
        layered.set_bounds({0, 0, kW, kH});
        add_source_over_scene(layered);
        layered.add_command(clear_rect(0, 0, 0, 0));
        REQUIRE(paints_into_layer(layered));

        const Color bg = Color::rgba8(30, 34, 48, 255);
        const Pixels a = render(backend, bg, direct);
        const Pixels b = render(backend, bg, layered);
        REQUIRE(a.size() == b.size());
        int worst = 0, changed = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            const int d = std::abs(int(a[i]) - int(b[i]));
            worst = std::max(worst, d);
            if (d) ++changed;
        }
        INFO("max channel delta " << worst << ", channels changed " << changed);
        // Source-over is associative; any difference is 8-bit rounding.
        CHECK(worst <= 1);
        // The scene is visible at all (guards a blank render passing).
        CHECK_FALSE(near(at(a, 12, 8), 30, 34, 48, 255, 0));
    }
}

TEST_CASE("CanvasWidget backdrop-reading commands see only the canvas's own pixels",
          "[canvas_widget][backdrop-isolation]") {
    const Color red = Color::rgba8(255, 0, 0, 255);
    struct Case {
        const char* name;
        Canvas::BlendMode mode;
        Color source;
        int r, g, b;  // expected: the source over a transparent backdrop
    };
    const Case cases[] = {
        {"lighter", Canvas::BlendMode::lighter, Color::rgba8(0, 255, 0, 255), 0, 255, 0},
        {"multiply", Canvas::BlendMode::multiply, Color::rgba8(0, 0, 255, 255), 0, 0, 255},
        {"screen", Canvas::BlendMode::screen, Color::rgba8(0, 0, 255, 255), 0, 0, 255},
    };
    for (const auto& backend : backends()) {
        for (const auto& c : cases) {
            INFO("backend " << backend.name << " mode " << c.name);
            CanvasWidget cw;
            cw.set_bounds({0, 0, kW, kH});
            cw.add_command(blend(c.mode));
            cw.add_command(fill_rect(0, 0, kW, kH, c.source));
            const Pixels px = render(backend, red, cw);
            const uint8_t* p = at(px, kW / 2, kH / 2);
            INFO("rgba " << int(p[0]) << "," << int(p[1]) << "," << int(p[2]) << "," << int(p[3]));
            CHECK(near(p, c.r, c.g, c.b, 255));
        }

        INFO("backend " << backend.name << " clearRect");
        CanvasWidget cw;
        cw.set_bounds({0, 0, kW, kH});
        cw.add_command(fill_rect(0, 0, kW, kH, Color::rgba8(0, 0, 255, 255)));
        cw.add_command(clear_rect(kW / 4.0f, 0, kW / 2.0f, kH));
        const Pixels px = render(backend, red, cw);
        // The clear punched this canvas, so the parent's red shows through;
        // it did not punch the parent to transparent.
        CHECK(near(at(px, kW / 2, kH / 2), 255, 0, 0, 255));
        CHECK(near(at(px, 2, kH / 2), 0, 0, 255, 255));
    }
}

#ifdef __APPLE__
// CoreGraphicsCanvas implements save_layer() as a transparency layer. The
// layer must end at the restore() that matches it, not at the first restore()
// of an inner save, and restore_to_count() must close it when it pops it.
TEST_CASE("CoreGraphicsCanvas ends a transparency layer at its own restore",
          "[canvas][cg][backdrop-isolation]") {
    const auto blue = Color::rgba8(0, 0, 255, 255);
    auto draw = [&](bool balanced) {
        Pixels px(static_cast<size_t>(kW) * kH * 4u, 0u);
        auto cs = CGColorSpaceCreateDeviceRGB();
        const uint32_t info = static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
                              static_cast<uint32_t>(kCGBitmapByteOrder32Big);
        CGContextRef ctx = CGBitmapContextCreate(px.data(), kW, kH, 8, kW * 4u, cs, info);
        CGColorSpaceRelease(cs);
        REQUIRE(ctx != nullptr);
        {
            pulp::canvas::CoreGraphicsCanvas canvas(ctx, static_cast<float>(kW),
                                                    static_cast<float>(kH));
            const int depth = canvas.save_count();
            canvas.save_layer(0, 0, kW, kH, 0.5f, 0.0f);
            CHECK(canvas.save_count() == depth + 1);
            canvas.set_fill_color(blue);
            canvas.fill_rect(0, 0, kW / 2.0f, kH);
            canvas.save();
            canvas.restore();
            canvas.fill_rect(kW / 2.0f, 0, kW / 2.0f, kH);
            if (balanced) canvas.restore();
            else canvas.restore_to_count(depth);
            CHECK(canvas.save_count() == depth);
        }
        CGContextRelease(ctx);
        return px;
    };
    for (bool balanced : {true, false}) {
        INFO(std::string(balanced ? "balanced restore" : "restore_to_count"));
        const Pixels px = draw(balanced);
        // Both halves were drawn inside the half-opacity layer.
        const uint8_t* left = at(px, 4, kH / 2);
        const uint8_t* right = at(px, kW - 4, kH / 2);
        INFO("left a=" << int(left[3]) << " right a=" << int(right[3]));
        CHECK(std::abs(int(left[3]) - 128) <= 2);
        CHECK(std::abs(int(right[3]) - 128) <= 2);
    }
}
#endif
