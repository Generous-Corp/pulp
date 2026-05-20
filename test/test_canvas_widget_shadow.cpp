// test_canvas_widget_shadow.cpp — extracted from test_canvas_widget.cpp
// in the 2026-05 Phase 5 P5-3 follow-up refactor.
//
// CanvasWidget + SkiaCanvas Canvas2D shadow-state cluster. Pins:
//
//   * CanvasWidget replays sticky Canvas2D shadow setters before each
//     paint command (so shadow* survives multiple draws).
//   * Shadow setters can be cleared mid-stream — the next paint must
//     drop the shadow.
//   * SkiaCanvas honors sticky shadow state on fillRect.
//   * SkiaCanvas skips shadow when fully transparent or zero (no
//     wasted draw layers).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/canvas/canvas.hpp>
#include <cmath>
#include <limits>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#endif

using namespace pulp::view;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;

TEST_CASE("CanvasWidget replays shadow setters onto the canvas",
          "[canvas_widget][issue-1434-batch-7]") {
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 100, 100});

    CanvasDrawCmd color;
    color.type = CanvasDrawCmd::Type::set_shadow_color;
    color.color = pulp::canvas::Color::rgba8(255, 0, 0, 200);
    cw.add_command(color);

    CanvasDrawCmd blur;
    blur.type = CanvasDrawCmd::Type::set_shadow_blur;
    blur.extra = 8.0f;
    cw.add_command(blur);

    CanvasDrawCmd ox;
    ox.type = CanvasDrawCmd::Type::set_shadow_offset_x;
    ox.extra = 4.0f;
    cw.add_command(ox);

    CanvasDrawCmd oy;
    oy.type = CanvasDrawCmd::Type::set_shadow_offset_y;
    oy.extra = 6.0f;
    cw.add_command(oy);

    CanvasDrawCmd rect;
    rect.type = CanvasDrawCmd::Type::fill_rect;
    rect.x = 10; rect.y = 10; rect.w = 50; rect.h = 30;
    rect.color = pulp::canvas::Color::rgba8(0, 128, 255, 255);
    cw.add_command(rect);

    cw.paint(rc);

    // Find the four shadow setter commands in the recording.
    int color_idx = -1, blur_idx = -1, ox_idx = -1, oy_idx = -1;
    int rect_idx = -1;
    for (size_t i = 0; i < rc.commands().size(); ++i) {
        switch (rc.commands()[i].type) {
            case DrawCommand::Type::set_shadow_color:    color_idx = (int)i; break;
            case DrawCommand::Type::set_shadow_blur:     blur_idx  = (int)i; break;
            case DrawCommand::Type::set_shadow_offset_x: ox_idx    = (int)i; break;
            case DrawCommand::Type::set_shadow_offset_y: oy_idx    = (int)i; break;
            case DrawCommand::Type::fill_rect:           rect_idx  = (int)i; break;
            default: break;
        }
    }

    REQUIRE(color_idx >= 0);
    REQUIRE(blur_idx  >= 0);
    REQUIRE(ox_idx    >= 0);
    REQUIRE(oy_idx    >= 0);
    REQUIRE(rect_idx  >= 0);

    // Order matters — JS sets shadow* THEN draws. Every setter must
    // land before the draw or the shadow won't apply to it.
    REQUIRE(color_idx < rect_idx);
    REQUIRE(blur_idx  < rect_idx);
    REQUIRE(ox_idx    < rect_idx);
    REQUIRE(oy_idx    < rect_idx);

    // Payload round-trips: color is captured as rgba in [0,1] floats.
    REQUIRE(rc.commands()[color_idx].color.r == Catch::Approx(255.0f / 255.0f));
    REQUIRE(rc.commands()[color_idx].color.a == Catch::Approx(200.0f / 255.0f));
    REQUIRE(rc.commands()[blur_idx].f[0]  == Catch::Approx(8.0f));
    REQUIRE(rc.commands()[ox_idx].f[0]    == Catch::Approx(4.0f));
    REQUIRE(rc.commands()[oy_idx].f[0]    == Catch::Approx(6.0f));
}

TEST_CASE("CanvasWidget shadow setters can be cleared mid-stream",
          "[canvas_widget][issue-1434-batch-7]") {
    // Sanity: setting shadowColor to fully transparent and shadowBlur to
    // 0 emits subsequent setter commands, the canvas backends gate the
    // actual rendering on those values. Tests for the gating predicate
    // live in the SkiaCanvas / CoreGraphicsCanvas backends; here we
    // verify that the widget faithfully forwards every assignment so
    // that subsequent draws see the cleared state.
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 100, 100});

    // Activate.
    CanvasDrawCmd c1; c1.type = CanvasDrawCmd::Type::set_shadow_color;
    c1.color = pulp::canvas::Color::rgba8(0, 0, 0, 255);
    cw.add_command(c1);
    CanvasDrawCmd b1; b1.type = CanvasDrawCmd::Type::set_shadow_blur; b1.extra = 4.0f;
    cw.add_command(b1);

    // Draw 1.
    CanvasDrawCmd r1; r1.type = CanvasDrawCmd::Type::fill_rect;
    r1.x = 0; r1.y = 0; r1.w = 10; r1.h = 10;
    cw.add_command(r1);

    // Deactivate (color → fully transparent).
    CanvasDrawCmd c2; c2.type = CanvasDrawCmd::Type::set_shadow_color;
    c2.color = pulp::canvas::Color::rgba(0.0f, 0.0f, 0.0f, 0.0f);
    cw.add_command(c2);

    // Draw 2.
    CanvasDrawCmd r2; r2.type = CanvasDrawCmd::Type::fill_rect;
    r2.x = 50; r2.y = 50; r2.w = 10; r2.h = 10;
    cw.add_command(r2);

    cw.paint(rc);

    // Two set_shadow_color commands must appear, one before each rect.
    int set_color_count = 0;
    int last_alpha_x255 = -1;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_shadow_color) {
            ++set_color_count;
            last_alpha_x255 = static_cast<int>(cmd.color.a * 255.0f + 0.5f);
        }
    }
    REQUIRE(set_color_count == 2);
    REQUIRE(last_alpha_x255 == 0);  // most-recent shadowColor was transparent
}

#ifdef PULP_HAS_SKIA
TEST_CASE("SkiaCanvas honors sticky Canvas2D shadow state on fillRect",
          "[canvas_widget][issue-1434-batch-7][skia]") {
    // Rasterise a filled rect with shadowColor + shadowBlur + offset
    // active and confirm that pixels OUTSIDE the rect (in the shadow
    // region) are non-transparent — i.e., the DropShadow image filter
    // actually fired. We also verify the inverse: a pixel far from the
    // shadow's expected reach stays at the cleared background colour.
    SkImageInfo info = SkImageInfo::Make(64, 64,
                                          kRGBA_8888_SkColorType,
                                          kPremul_SkAlphaType,
                                          SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);

    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());

    // Clear to opaque white so we can detect "shadow shows through".
    canvas.set_fill_color(pulp::canvas::Color::rgba(1.0f, 1.0f, 1.0f, 1.0f));
    canvas.fill_rect(0, 0, 64, 64);

    // Activate sticky shadow — opaque red, modest blur, large offset.
    canvas.set_shadow_color(pulp::canvas::Color::rgba(1.0f, 0.0f, 0.0f, 1.0f));
    canvas.set_shadow_blur(8.0f);
    canvas.set_shadow_offset_x(8.0f);
    canvas.set_shadow_offset_y(8.0f);

    // Draw a small black rect — the shadow appears ~8px to the
    // bottom-right of it.
    canvas.set_fill_color(pulp::canvas::Color::rgba(0.0f, 0.0f, 0.0f, 1.0f));
    canvas.fill_rect(16, 16, 16, 16);

    // Pixel at (40, 40) is 8px past the bottom-right corner of the rect
    // along both axes — directly under the shadow centre.
    auto shadow_px = sample_pixel(surface.get(), 40, 40);
    INFO("shadow px rgba=(" << int(shadow_px.r) << "," << int(shadow_px.g)
         << "," << int(shadow_px.b) << "," << int(shadow_px.a) << ")");
    // Red channel should dominate (we shadowed with opaque red on white)
    // and the pixel must be tinted away from the white background.
    REQUIRE(shadow_px.r > shadow_px.g);
    REQUIRE(shadow_px.r > shadow_px.b);
    REQUIRE(shadow_px.g < 240);

    // Pixel at (4, 4) — far from both the rect and its shadow — must
    // remain pure white. Confirms the shadow doesn't bleed everywhere.
    auto far_px = sample_pixel(surface.get(), 4, 4);
    REQUIRE(far_px.r == 255);
    REQUIRE(far_px.g == 255);
    REQUIRE(far_px.b == 255);
}

TEST_CASE("SkiaCanvas skips shadow when fully transparent or zero",
          "[canvas_widget][issue-1434-batch-7][skia]") {
    // Sanity: with shadowColor alpha == 0, even non-zero blur+offset
    // must not produce any shadowed pixels — match Canvas2D spec
    // ("if shadowColor's alpha is 0, no shadow is drawn").
    SkImageInfo info = SkImageInfo::Make(32, 32,
                                          kRGBA_8888_SkColorType,
                                          kPremul_SkAlphaType,
                                          SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);

    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
    canvas.set_fill_color(pulp::canvas::Color::rgba(1.0f, 1.0f, 1.0f, 1.0f));
    canvas.fill_rect(0, 0, 32, 32);

    canvas.set_shadow_color(pulp::canvas::Color::rgba(1.0f, 0.0f, 0.0f, 0.0f));
    canvas.set_shadow_blur(4.0f);
    canvas.set_shadow_offset_x(4.0f);
    canvas.set_shadow_offset_y(4.0f);

    canvas.set_fill_color(pulp::canvas::Color::rgba(0.0f, 0.0f, 0.0f, 1.0f));
    canvas.fill_rect(8, 8, 8, 8);

    // Pixel at (20, 20) would be in the shadow region if the shadow
    // were active — confirm it stays white.
    auto px = sample_pixel(surface.get(), 20, 20);
    REQUIRE(px.r == 255);
    REQUIRE(px.g == 255);
    REQUIRE(px.b == 255);
}
#endif  // PULP_HAS_SKIA

// ── pulp #1520 — Canvas2D ctx.direction / ctx.filter dispatch ────────────
//
// Asserts that the CanvasWidget paint loop forwards the new
// `set_direction` / `set_filter` commands through to the underlying
// canvas (here, RecordingCanvas) so the JS shim's setter intent reaches
// the active backend on every frame.

