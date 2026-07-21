// test_canvas_shader_effects.cpp — headless Skia render proof for the
// curated named GPU post-effects (crt / grain / vignette / noise / brushed /
// bloom) exposed to the scripted-UI canvas via
// CanvasWidget::set_shader_effect -> Canvas::save_layer_with_shader_effect.
//
// Strategy: render a CanvasWidget into a Skia raster surface with a known
// uniform (or bright-spot) fill, once with NO effect (baseline) and once with
// each named effect, then assert the effect measurably changes pixels. An
// unknown effect name must be a no-op (pixels identical to baseline) and the
// build must stay valid — that is the safety contract Forge relies on.
//
// Skia-gated: these tests only compile/run under PULP_HAS_SKIA. A CPU-only
// (non-Skia) build takes the graceful no-effect fallback, which is covered by
// the plain CanvasWidget tests, so there is nothing extra to assert there.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/canvas_widget.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <memory>
#include <string>

// Backend-agnostic (available in the no-Skia coverage build too).
using pulp::view::CanvasWidget;
using pulp::view::CanvasDrawCmd;

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include "canvas_pixel_probe.hpp"

#include <algorithm>
#include <string>
#include <vector>

using pulp::canvas::SkiaCanvas;
using pulp::canvas_test::Pixel;
using pulp::canvas_test::sample_pixel;

namespace {

constexpr int kW = 96;
constexpr int kH = 96;

sk_sp<SkSurface> make_surface() {
    SkImageInfo info = SkImageInfo::Make(kW, kH, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    return SkSurfaces::Raster(info);
}

// Render a widget that fills its whole bounds with an opaque gray, applying
// `effect` (empty => none). Surface is pre-cleared to opaque black so the
// composited result reads back straight (premul == straight at a=255).
sk_sp<SkSurface> render_uniform(const std::string& effect, float intensity,
                                uint8_t gray) {
    auto surface = make_surface();
    REQUIRE(surface != nullptr);
    surface->getCanvas()->clear(SK_ColorBLACK);

    CanvasWidget cw;
    cw.set_bounds({0, 0, (float)kW, (float)kH});
    if (!effect.empty()) cw.set_shader_effect(effect, intensity);

    CanvasDrawCmd fill;
    fill.type = CanvasDrawCmd::Type::fill_rect;
    fill.x = 0; fill.y = 0; fill.w = (float)kW; fill.h = (float)kH;
    fill.color = pulp::canvas::Color::rgba8(gray, gray, gray, 255);
    cw.add_command(fill);

    SkiaCanvas canvas(surface->getCanvas());
    cw.paint(canvas);
    return surface;
}

// Min/max luminance-ish (red channel) over an interior grid — used to detect
// that an effect introduced spatial variation into a previously flat field.
struct Range { int lo = 255; int hi = 0; };
Range red_range(SkSurface* s, int step = 4, int margin = 8) {
    Range r;
    for (int y = margin; y < kH - margin; y += step) {
        for (int x = margin; x < kW - margin; x += step) {
            int v = sample_pixel(s, x, y).r;
            r.lo = std::min(r.lo, v);
            r.hi = std::max(r.hi, v);
        }
    }
    return r;
}

} // namespace

TEST_CASE("named shader effect: baseline uniform fill is flat",
          "[canvas_widget][shader_effect][skia]") {
    auto base = render_uniform("", 1.0f, 128);
    // With no effect the interior is a flat gray — this anchors the
    // "effect changed pixels" assertions below.
    Range r = red_range(base.get());
    REQUIRE(r.hi - r.lo <= 2);
    REQUIRE(r.lo >= 120);
    REQUIRE(r.hi <= 136);
}

TEST_CASE("named shader effect: unknown name is a graceful no-op",
          "[canvas_widget][shader_effect][skia]") {
    auto base = render_uniform("", 1.0f, 128);
    auto bogus = render_uniform("definitely-not-an-effect", 1.0f, 128);
    // Unknown effect names must NOT change the render (and must not throw or
    // reject the build). Every sampled interior pixel matches the baseline.
    for (int y = 8; y < kH - 8; y += 8) {
        for (int x = 8; x < kW - 8; x += 8) {
            Pixel b = sample_pixel(base.get(), x, y);
            Pixel g = sample_pixel(bogus.get(), x, y);
            REQUIRE(b.r == g.r);
            REQUIRE(b.g == g.g);
            REQUIRE(b.b == g.b);
            REQUIRE(b.a == g.a);
        }
    }
}

TEST_CASE("named shader effect: 'none' disables the effect",
          "[canvas_widget][shader_effect][skia]") {
    auto base = render_uniform("", 1.0f, 128);
    auto none = render_uniform("none", 1.0f, 128);
    Pixel b = sample_pixel(base.get(), kW / 2, kH / 2);
    Pixel n = sample_pixel(none.get(), kW / 2, kH / 2);
    REQUIRE(b.r == n.r);
    REQUIRE(b.a == n.a);
}

TEST_CASE("named shader effect: vignette darkens the corners",
          "[canvas_widget][shader_effect][skia]") {
    auto vig = render_uniform("vignette", 1.0f, 200);
    int center = sample_pixel(vig.get(), kW / 2, kH / 2).r;
    int corner = sample_pixel(vig.get(), 3, 3).r;
    // Center stays near the source gray; the corner is meaningfully darker.
    REQUIRE(center > 150);
    REQUIRE(corner < center - 30);
}

TEST_CASE("named shader effect: crt introduces scanlines",
          "[canvas_widget][shader_effect][skia]") {
    auto crt = render_uniform("crt", 1.0f, 200);
    // Adjacent device rows near the center alternate bright/dark (scanlines),
    // so two vertically-adjacent pixels differ substantially.
    int a = sample_pixel(crt.get(), kW / 2, kH / 2).r;
    int b = sample_pixel(crt.get(), kW / 2, kH / 2 + 1).r;
    REQUIRE(std::abs(a - b) > 20);
    // Corner is pulled toward the tube bezel / vignette — darker than center.
    int bright = std::max(a, b);
    int corner = sample_pixel(crt.get(), 2, 2).r;
    REQUIRE(corner < bright);
}

TEST_CASE("named shader effect: grain adds per-pixel variation",
          "[canvas_widget][shader_effect][skia]") {
    auto grain = render_uniform("grain", 1.0f, 128);
    Range r = red_range(grain.get());
    // Static film grain breaks the flat field into a spread of values.
    REQUIRE(r.hi - r.lo > 10);
}

TEST_CASE("named shader effect: noise adds spatial variation",
          "[canvas_widget][shader_effect][skia]") {
    auto noise = render_uniform("noise", 1.0f, 128);
    Range r = red_range(noise.get());
    REQUIRE(r.hi - r.lo > 6);
}

TEST_CASE("named shader effect: brushed adds streak variation",
          "[canvas_widget][shader_effect][skia]") {
    auto brushed = render_uniform("brushed", 1.0f, 128);
    Range r = red_range(brushed.get());
    REQUIRE(r.hi - r.lo > 4);
}

TEST_CASE("named shader effect: intensity 0 is (near) identity",
          "[canvas_widget][shader_effect][skia]") {
    auto base = render_uniform("", 1.0f, 128);
    auto vig0 = render_uniform("vignette", 0.0f, 128);
    // At intensity 0 the vignette mix collapses to the source, so the corner
    // is no longer darkened away from the baseline.
    int base_corner = sample_pixel(base.get(), 3, 3).r;
    int vig0_corner = sample_pixel(vig0.get(), 3, 3).r;
    REQUIRE(std::abs(base_corner - vig0_corner) <= 3);
}

TEST_CASE("named shader effect: vignette stays centered under 2x canvas scale",
          "[canvas_widget][shader_effect][skia]") {
    // HiDPI parity: a 96x96 logical widget on a 2x-scaled canvas covers a
    // 192x192 device surface. The effect runs in device space, so the dark
    // corners must track the DEVICE geometry — bright at the device center,
    // dark at the device corner — not collapse to the logical resolution.
    SkImageInfo info = SkImageInfo::Make(192, 192, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    surface->getCanvas()->clear(SK_ColorBLACK);
    surface->getCanvas()->scale(2.0f, 2.0f);

    CanvasWidget cw;
    cw.set_bounds({0, 0, (float)kW, (float)kH});
    cw.set_shader_effect("vignette", 1.0f);
    CanvasDrawCmd fill;
    fill.type = CanvasDrawCmd::Type::fill_rect;
    fill.x = 0; fill.y = 0; fill.w = (float)kW; fill.h = (float)kH;
    fill.color = pulp::canvas::Color::rgba8(200, 200, 200, 255);
    cw.add_command(fill);

    SkiaCanvas canvas(surface->getCanvas());
    cw.paint(canvas);

    int center = sample_pixel(surface.get(), 96, 96).r;   // device center
    int corner = sample_pixel(surface.get(), 6, 6).r;     // device corner
    REQUIRE(center > 150);
    REQUIRE(corner < center - 30);
}

TEST_CASE("named shader effect: bloom bleeds light past bright edges",
          "[canvas_widget][shader_effect][skia]") {
    // Dark field with a bright square in the center. Without bloom, a pixel
    // just outside the square stays dark; with bloom the glow bleeds outward.
    auto build = [](const std::string& effect) {
        auto surface = make_surface();
        REQUIRE(surface != nullptr);
        surface->getCanvas()->clear(SK_ColorBLACK);
        CanvasWidget cw;
        cw.set_bounds({0, 0, (float)kW, (float)kH});
        if (!effect.empty()) cw.set_shader_effect(effect, 1.0f);
        CanvasDrawCmd bg;
        bg.type = CanvasDrawCmd::Type::fill_rect;
        bg.x = 0; bg.y = 0; bg.w = (float)kW; bg.h = (float)kH;
        bg.color = pulp::canvas::Color::rgba8(10, 10, 10, 255);
        cw.add_command(bg);
        CanvasDrawCmd spot;
        spot.type = CanvasDrawCmd::Type::fill_rect;
        spot.x = 40; spot.y = 40; spot.w = 16; spot.h = 16;
        spot.color = pulp::canvas::Color::rgba8(255, 255, 255, 255);
        cw.add_command(spot);
        SkiaCanvas canvas(surface->getCanvas());
        cw.paint(canvas);
        return surface;
    };
    auto base = build("");
    auto bloom = build("bloom");
    // A pixel a few px outside the bright square (which is [40,56)).
    const int px = 60, py = 48;
    int base_r = sample_pixel(base.get(), px, py).r;
    int bloom_r = sample_pixel(bloom.get(), px, py).r;
    REQUIRE(base_r < 40);              // outside the square stays dark w/o bloom
    REQUIRE(bloom_r > base_r + 10);    // bloom bleeds light outward
}

#endif // PULP_HAS_SKIA

// ── Backend-agnostic tests (compiled on every backend, incl. no-Skia) ──────
// These cover the always-compiled JS-facing plumbing — the widget's sticky
// effect state, the paint() branch that routes an active effect through
// save_layer_with_shader_effect, and the canvasSetShaderEffect bridge
// registration — so the capability is exercised even in the CPU/no-Skia
// coverage build where the SkSL library above is #ifdef'd out.

using pulp::canvas::RecordingCanvas;

TEST_CASE("shader effect state: set / read / clamp",
          "[canvas_widget][shader_effect]") {
    CanvasWidget cw;
    // Default is disabled.
    REQUIRE(cw.shader_effect() == "none");

    cw.set_shader_effect("crt", 0.5f);
    REQUIRE(cw.shader_effect() == "crt");
    REQUIRE(cw.shader_effect_intensity() == 0.5f);

    // Intensity clamps to [0,1].
    cw.set_shader_effect("grain", 2.5f);
    REQUIRE(cw.shader_effect_intensity() == 1.0f);
    cw.set_shader_effect("grain", -1.0f);
    REQUIRE(cw.shader_effect_intensity() == 0.0f);

    // "none" / "" disable.
    cw.set_shader_effect("none", 1.0f);
    REQUIRE(cw.shader_effect() == "none");
}

TEST_CASE("shader effect: paint routes through the effect layer when active",
          "[canvas_widget][shader_effect]") {
    // With an effect set, paint() must take the save_layer_with_shader_effect
    // branch. On a non-Skia backend that virtual falls back to a plain
    // save_layer (effect skipped) — the widget still paints and stays balanced.
    CanvasWidget cw;
    cw.set_bounds({0, 0, 64, 64});
    cw.set_shader_effect("crt", 0.8f);
    CanvasDrawCmd fill;
    fill.type = CanvasDrawCmd::Type::fill_rect;
    fill.x = 0; fill.y = 0; fill.w = 64; fill.h = 64;
    fill.color = pulp::canvas::Color::rgba8(128, 128, 128, 255);
    cw.add_command(fill);

    RecordingCanvas rc;
    cw.paint(rc);
    // The draw replay (incl. the effect layer + fill) was recorded without
    // crashing; the effect state is unchanged (sticky across paints).
    REQUIRE(rc.commands().size() > 0);
    REQUIRE(cw.shader_effect() == "crt");

    // Disabling the effect still paints (the non-effect branch).
    cw.set_shader_effect("none");
    RecordingCanvas rc2;
    cw.paint(rc2);
    REQUIRE(rc2.commands().size() > 0);
}

namespace {
// Minimal bridge harness: build a WidgetBridge, create a <canvas> from JS,
// and expose the live CanvasWidget by reading the element's generated _id.
struct ShaderEffectBridge {
    pulp::view::ScriptEngine engine;
    pulp::view::View root;
    pulp::state::StateStore store;
    std::unique_ptr<pulp::view::WidgetBridge> bridge;

    ShaderEffectBridge() {
        root.set_bounds({0, 0, 200, 200});
        bridge = std::make_unique<pulp::view::WidgetBridge>(engine, root, store);
    }
    void load(const std::string& js) { bridge->load_script(js); }
    CanvasWidget* canvas() {
        auto v = engine.evaluate("(globalThis.__fx_el__ && "
                                 "globalThis.__fx_el__._id) || ''");
        if (!v.isString()) return nullptr;
        std::string id = std::string(v.getString());
        if (id.empty()) return nullptr;
        return dynamic_cast<CanvasWidget*>(bridge->widget(id));
    }
};
}  // namespace

TEST_CASE("canvasSetShaderEffect bridge sets the widget effect",
          "[canvas_widget][shader_effect][bridge]") {
    ShaderEffectBridge b;
    b.load(R"(
        var c = document.createElement('canvas');
        c.width = 100; c.height = 100;
        document.body.appendChild(c);
        globalThis.__fx_el__ = c;
    )");
    CanvasWidget* cw = b.canvas();
    REQUIRE(cw != nullptr);

    // Intensity above 1 clamps host-side.
    b.load("canvasSetShaderEffect(globalThis.__fx_el__._id, 'crt', 2.0);");
    REQUIRE(cw->shader_effect() == "crt");
    REQUIRE(cw->shader_effect_intensity() == 1.0f);

    // Unknown name is accepted by the bridge (stored; a graceful no-op at
    // paint time) — never throws / rejects.
    b.load("canvasSetShaderEffect(globalThis.__fx_el__._id, 'bogus-fx', 0.5);");
    REQUIRE(cw->shader_effect() == "bogus-fx");

    // "none" disables.
    b.load("canvasSetShaderEffect(globalThis.__fx_el__._id, 'none');");
    REQUIRE(cw->shader_effect() == "none");
}
