// Automated test for CanvasWidget (JS-driven custom drawing)
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/theme.hpp>
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

TEST_CASE("CanvasWidget: add and clear commands", "[canvas_widget]") {
    CanvasWidget cw;
    REQUIRE(cw.command_count() == 0);

    CanvasDrawCmd cmd;
    cmd.type = CanvasDrawCmd::Type::fill_rect;
    cmd.x = 0; cmd.y = 0; cmd.w = 100; cmd.h = 50;
    cmd.color = {255, 0, 0, 255};
    cw.add_command(cmd);
    REQUIRE(cw.command_count() == 1);

    cw.clear_commands();
    REQUIRE(cw.command_count() == 0);
}

TEST_CASE("CanvasWidget: paint replays commands", "[canvas_widget]") {
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 200, 100});

    // Add a filled rect
    CanvasDrawCmd rect;
    rect.type = CanvasDrawCmd::Type::fill_rect;
    rect.x = 10; rect.y = 10; rect.w = 80; rect.h = 40;
    rect.color = {255, 0, 0, 255};
    cw.add_command(rect);

    // Add a circle
    CanvasDrawCmd circle;
    circle.type = CanvasDrawCmd::Type::fill_circle;
    circle.x = 50; circle.y = 50; circle.extra = 20; // radius
    circle.color = {0, 255, 0, 255};
    cw.add_command(circle);

    // Add a line
    CanvasDrawCmd line;
    line.type = CanvasDrawCmd::Type::stroke_line;
    line.x = 0; line.y = 0; line.w = 100; line.h = 100; // endpoint
    line.extra = 2.0f; // width
    line.color = {0, 0, 255, 255};
    cw.add_command(line);

    cw.paint(rc);
    // Should have produced fill_rect, fill_circle, stroke_line commands
    REQUIRE(rc.commands().size() >= 3);
}

TEST_CASE("CanvasWidget: fill_text command", "[canvas_widget]") {
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 200, 50});

    CanvasDrawCmd text;
    text.type = CanvasDrawCmd::Type::fill_text;
    text.x = 10; text.y = 20;
    text.extra = 14.0f; // font size
    text.color = {255, 255, 255, 255};
    text.text = "Hello World";
    cw.add_command(text);

    cw.paint(rc);
    REQUIRE(rc.commands().size() > 0);
}

TEST_CASE("CanvasWidget: clear command fills background", "[canvas_widget]") {
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 100, 100});

    CanvasDrawCmd clear;
    clear.type = CanvasDrawCmd::Type::clear;
    clear.color = {30, 30, 46, 255};
    cw.add_command(clear);

    cw.paint(rc);
    REQUIRE(rc.commands().size() > 0);
}

// Issue-897 P1 follow-up: CanvasWidget::paint() must snapshot the inbound
// device matrix at entry so JS-driven setTransform() composes onto the
// parent View transform instead of overwriting it. Without this baseline
// the SkiaCanvas backend's setTransform would wipe the parent translation
// applied by View::paint_all.
TEST_CASE("CanvasWidget::paint captures paint baseline transform at entry",
          "[canvas_widget][issue-897]") {
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 100, 100});

    REQUIRE(rc.baseline_capture_count() == 0);
    cw.paint(rc);
    REQUIRE(rc.baseline_capture_count() == 1);

    cw.paint(rc);
    REQUIRE(rc.baseline_capture_count() == 2);
}

TEST_CASE("CanvasWidget::paint baseline capture is idempotent and ordered",
          "[canvas_widget][issue-897]") {
    // The baseline must be snapshot before any draw work — otherwise an
    // intervening transform command would corrupt it.
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 100, 100});

    CanvasDrawCmd r;
    r.type = CanvasDrawCmd::Type::fill_rect;
    r.x = 0; r.y = 0; r.w = 50; r.h = 50;
    r.color = {255, 0, 0, 255};
    cw.add_command(r);

    REQUIRE(rc.commands().empty());
    REQUIRE(rc.baseline_capture_count() == 0);
    cw.paint(rc);
    REQUIRE(rc.baseline_capture_count() == 1);
    REQUIRE_FALSE(rc.commands().empty());
}

// ── pulp #929 — Canvas widget transparent default + clearRect semantics ──

// Empty-command paint must not introduce a full-bounds fill that would
// hide the parent's painted surface. Asserting against RecordingCanvas
// gives us a backend-independent regression guard, even on platforms
// where Skia raster isn't available in the test build.
TEST_CASE("CanvasWidget::paint with no JS draw commands does not pre-fill bounds",
          "[canvas_widget][issue-929]") {
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 200, 100});

    cw.paint(rc);

    // Allowed bookkeeping commands: capture_paint_baseline_transform()
    // does not add to commands_, so the recorded list should stay empty.
    // What MUST NOT happen: a fill_rect covering the widget bounds. We
    // walk the full command list to be defensive against future state-
    // tracking commands sneaking in (set_blend_mode, save, etc.).
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::fill_rect) {
            const bool covers_bounds = cmd.f[0] <= 0.0f && cmd.f[1] <= 0.0f &&
                                       cmd.f[2] >= 200.0f && cmd.f[3] >= 100.0f;
            INFO("CanvasWidget::paint emitted a fill_rect that covers the full "
                 "widget bounds — this would mask the parent's paint surface "
                 "(pulp #929).");
            REQUIRE_FALSE(covers_bounds);
        }
    }
}

// JS-issued clearRect should produce a single clear_rect command, not a
// SrcOver fill that is a no-op. RecordingCanvas captures the dedicated
// DrawCommand::Type::clear_rect introduced in pulp #929 so this is a
// straightforward identity assertion.
TEST_CASE("CanvasWidget::paint translates clearRect into a real clear_rect command",
          "[canvas_widget][issue-929]") {
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 200, 100});

    CanvasDrawCmd c;
    c.type = CanvasDrawCmd::Type::clear_rect;
    c.x = 10; c.y = 20; c.w = 80; c.h = 40;
    cw.add_command(c);

    cw.paint(rc);

    // Exactly one clear_rect command, matching the JS-issued bounds.
    size_t clear_count = 0;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::clear_rect) {
            ++clear_count;
            REQUIRE(cmd.f[0] == 10.0f);
            REQUIRE(cmd.f[1] == 20.0f);
            REQUIRE(cmd.f[2] == 80.0f);
            REQUIRE(cmd.f[3] == 40.0f);
        }
    }
    REQUIRE(clear_count == 1);

    // And no SrcOver fill_rect snuck in — the previous implementation was
    // save / clip / setFillColor(0,0,0,0) / fill_rect, which is a visual
    // no-op (#929 root cause).
    for (const auto& cmd : rc.commands()) {
        REQUIRE(cmd.type != DrawCommand::Type::fill_rect);
    }
}

// pulp #949 — backend-independent guard for the FilterBank regression. A
// parent View with a background paints its bg, then a CanvasWidget child
// with a clearRect+fillRect pair runs. The recorded command sequence must
// be:
//   1. parent's bg fill_rect (covering parent's local bounds)
//   2. parent translates to child origin
//   3. (optional clip / no save_layer for default opacity == 1)
//   4. child's clear_rect (in the child's local coordinate space)
//   5. child's fill_rect (in the child's local coordinate space)
//
// What we are guarding against is the regression Spectr saw at v0.57.0
// where the canvas-widget commands fired (≥1600 per frame) but appeared
// to have no visible effect. If the child's clear_rect lands BEFORE the
// parent's bg fill, or if the child's fill_rect is missing, the bug
// is reproduced at the recorded-command level.
TEST_CASE("CanvasWidget::paint_all under parent View emits correct command order",
          "[canvas_widget][issue-949]") {
    RecordingCanvas rc;

    View parent;
    parent.set_bounds({0, 0, 64, 64});
    parent.set_background_color(pulp::canvas::Color::rgba8(8, 12, 24, 255));

    auto cw = std::make_unique<CanvasWidget>();
    cw->set_bounds({0, 0, 64, 64});
    {
        CanvasDrawCmd c;
        c.type = CanvasDrawCmd::Type::clear_rect;
        c.x = 0; c.y = 0; c.w = 64; c.h = 64;
        cw->add_command(c);
    }
    {
        CanvasDrawCmd r;
        r.type = CanvasDrawCmd::Type::fill_rect;
        r.x = 0; r.y = 0; r.w = 64; r.h = 64;
        r.color = pulp::canvas::Color::rgba8(255, 0, 255, 255);
        cw->add_command(r);
    }
    parent.add_child(std::move(cw));
    parent.paint_all(rc);

    // Walk the recorded commands and verify ordering.
    int parent_bg_idx = -1;
    int child_clear_idx = -1;
    int child_fill_idx = -1;
    int idx = 0;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::fill_rect && parent_bg_idx < 0 &&
            cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
            cmd.f[2] == 64.0f && cmd.f[3] == 64.0f) {
            parent_bg_idx = idx;
        } else if (cmd.type == DrawCommand::Type::clear_rect) {
            child_clear_idx = idx;
        } else if (cmd.type == DrawCommand::Type::fill_rect && parent_bg_idx >= 0) {
            child_fill_idx = idx;
        }
        ++idx;
    }

    INFO("parent_bg_idx=" << parent_bg_idx
         << " child_clear_idx=" << child_clear_idx
         << " child_fill_idx=" << child_fill_idx);
    REQUIRE(parent_bg_idx >= 0);
    REQUIRE(child_clear_idx > parent_bg_idx);
    REQUIRE(child_fill_idx > child_clear_idx);
}

#ifdef PULP_HAS_SKIA

// Pixel + sample_pixel moved to the shared canvas_pixel_probe.hpp in the
// pulp #2462 fix (was duplicated here and in test_canvas2d_shim.cpp).
#include "canvas_pixel_probe.hpp"
using pulp::canvas_test::Pixel;
using pulp::canvas_test::sample_pixel;

TEST_CASE("CanvasWidget paints transparent by default on a Skia raster surface",
          "[canvas_widget][skia][issue-929]") {
    // Premultiplied RGBA8 raster surface — Skia initializes the backing
    // store to all-zero (fully transparent) on first access. If the
    // CanvasWidget were pre-filling its bounds with white, every sampled
    // pixel would become (255, 255, 255, 255).
    SkImageInfo info = SkImageInfo::Make(64, 64, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    REQUIRE(sk_canvas != nullptr);

    pulp::canvas::SkiaCanvas canvas(sk_canvas);
    CanvasWidget cw;
    cw.set_bounds({0, 0, 64, 64});
    cw.paint(canvas);

    // Spot-check four corners + center — all pixels stay fully transparent.
    for (auto [x, y] : std::initializer_list<std::pair<int, int>>{
             {0, 0}, {63, 0}, {0, 63}, {63, 63}, {32, 32}}) {
        auto px = sample_pixel(surface.get(), x, y);
        INFO("Pixel (" << x << "," << y << ") rgba=("
             << int(px.r) << "," << int(px.g) << ","
             << int(px.b) << "," << int(px.a) << ")");
        REQUIRE(px.a == 0);
        REQUIRE(px.r == 0);
        REQUIRE(px.g == 0);
        REQUIRE(px.b == 0);
    }
}

TEST_CASE("CanvasWidget partial fillRect leaves untouched pixels transparent",
          "[canvas_widget][skia][issue-929]") {
    // 64x64 surface; widget fills a 32x64 left half with opaque red. The
    // right half must stay at the surface's initial transparent black.
    SkImageInfo info = SkImageInfo::Make(64, 64, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());

    CanvasWidget cw;
    cw.set_bounds({0, 0, 64, 64});
    CanvasDrawCmd r;
    r.type = CanvasDrawCmd::Type::fill_rect;
    r.x = 0; r.y = 0; r.w = 32; r.h = 64;
    r.color = {255, 0, 0, 255};
    cw.add_command(r);
    cw.paint(canvas);

    // Inside the fill: opaque red.
    auto inside = sample_pixel(surface.get(), 16, 32);
    REQUIRE(inside.a == 255);
    REQUIRE(inside.r == 255);
    // Outside the fill: still transparent.
    auto outside = sample_pixel(surface.get(), 48, 32);
    REQUIRE(outside.a == 0);
    REQUIRE(outside.r == 0);
}

TEST_CASE("CanvasWidget clearRect zeros its own backing store, not the parent",
          "[canvas_widget][skia][issue-929][issue-1368]") {
    // pulp #1368 contract update: each CanvasWidget paints into its own
    // save_layer-backed offscreen so JS-driven clearRect (kClear blend)
    // can only zero THIS canvas's backing store. The parent surface is
    // untouched by the clear; on layer restore the empty layer composites
    // back as a no-op SrcOver and the parent pixels survive.
    //
    // Pre-#1368, clearRect was emitted directly on the inbound parent
    // surface, so it nuked sibling-painted pixels. The check we now want:
    //
    //   parent surface pre-painted white
    //     → CanvasWidget with clearRect over its full bounds runs alone
    //     → parent stays WHITE (HTML <canvas> spec — sibling canvases
    //       have their own backing stores, so clear on canvas A cannot
    //       affect the parent surface or canvas B's content).
    SkImageInfo info = SkImageInfo::Make(32, 32, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    sk_canvas->clear(SkColorSetARGB(255, 255, 255, 255));

    pulp::canvas::SkiaCanvas canvas(sk_canvas);
    CanvasWidget cw;
    cw.set_bounds({0, 0, 32, 32});
    CanvasDrawCmd c;
    c.type = CanvasDrawCmd::Type::clear_rect;
    c.x = 0; c.y = 0; c.w = 32; c.h = 32;
    cw.add_command(c);
    cw.paint(canvas);

    // The parent surface's white must be preserved because the clearRect
    // landed inside the canvas widget's per-canvas backing store and the
    // empty layer composited back as a SrcOver no-op.
    auto px = sample_pixel(surface.get(), 16, 16);
    INFO("Pixel after canvas clearRect rgba=("
         << int(px.r) << "," << int(px.g) << ","
         << int(px.b) << "," << int(px.a) << ")");
    REQUIRE(px.a == 255);
    REQUIRE(px.r == 255);
    REQUIRE(px.g == 255);
    REQUIRE(px.b == 255);
}

// pulp #949 — FilterBank-style repro: a parent View paints a dark navy
// background, then a CanvasWidget child paints into the same surface using
// the v0.57.0 sequence (clearRect → fillStyle = gradient → fillRect →
// path-based fillPath). The regression Spectr saw was that the canvas
// content never appeared on top of the parent's bg even though 1600+
// canvas calls were firing per frame.
//
// Acceptance: after parent.paint_all(canvas), the canvas widget's center
// pixel is NOT the parent's dark navy — it has been replaced by the
// content the canvas widget drew.
TEST_CASE("CanvasWidget content paints over parent View bg via paint_all",
          "[canvas_widget][skia][issue-949]") {
    SkImageInfo info = SkImageInfo::Make(64, 64, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());

    // Parent View paints a dark-navy background — the colour Spectr's
    // FilterBank container shows when the canvas widget's content fails
    // to render.
    View parent;
    parent.set_bounds({0, 0, 64, 64});
    parent.set_background_color(pulp::canvas::Color::rgba8(8, 12, 24, 255));

    auto cw = std::make_unique<CanvasWidget>();
    cw->set_bounds({0, 0, 64, 64});

    // Mimic the FilterBank render: clear, then fill_rect across the bounds.
    // The fill_rect carries an opaque magenta colour so we can tell the
    // canvas widget's paint actually landed on top of the parent's navy.
    {
        CanvasDrawCmd c;
        c.type = CanvasDrawCmd::Type::clear_rect;
        c.x = 0; c.y = 0; c.w = 64; c.h = 64;
        cw->add_command(c);
    }
    {
        CanvasDrawCmd r;
        r.type = CanvasDrawCmd::Type::fill_rect;
        r.x = 0; r.y = 0; r.w = 64; r.h = 64;
        r.color = pulp::canvas::Color::rgba8(255, 0, 255, 255);   // magenta
        cw->add_command(r);
    }

    parent.add_child(std::move(cw));
    parent.paint_all(canvas);

    // Center pixel: must be the canvas-widget's magenta, not the parent's
    // navy. If the v0.57.0 regression is present, this samples (8,12,24)
    // (parent navy) or (0,0,0,0) (kClear leaked outside a layer).
    auto px = sample_pixel(surface.get(), 32, 32);
    INFO("Centre pixel rgba=("
         << int(px.r) << "," << int(px.g) << ","
         << int(px.b) << "," << int(px.a) << ")");
    REQUIRE(px.a == 255);
    REQUIRE(px.r == 255);
    REQUIRE(px.g == 0);
    REQUIRE(px.b == 255);
}

// pulp #949 — same scenario but the parent has overflow:visible and an
// additional sibling. Verifies the canvas widget's draws still land on
// top, and the parent's bg outside the child's bounds is preserved.
TEST_CASE("CanvasWidget draws on top of sibling-painted parent surface",
          "[canvas_widget][skia][issue-949]") {
    SkImageInfo info = SkImageInfo::Make(80, 60, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());

    View parent;
    parent.set_bounds({0, 0, 80, 60});
    parent.set_background_color(pulp::canvas::Color::rgba8(8, 12, 24, 255));  // dark navy

    // CanvasWidget covers only the inner 60x40 region centred at (40, 30).
    auto cw = std::make_unique<CanvasWidget>();
    cw->set_bounds({10, 10, 60, 40});
    {
        CanvasDrawCmd c;
        c.type = CanvasDrawCmd::Type::clear_rect;
        c.x = 0; c.y = 0; c.w = 60; c.h = 40;
        cw->add_command(c);
    }
    {
        CanvasDrawCmd r;
        r.type = CanvasDrawCmd::Type::fill_rect;
        r.x = 0; r.y = 0; r.w = 60; r.h = 40;
        r.color = pulp::canvas::Color::rgba8(0, 200, 100, 255);   // bright green
        cw->add_command(r);
    }
    parent.add_child(std::move(cw));
    parent.paint_all(canvas);

    // Inside the canvas widget: bright green.
    auto inside = sample_pixel(surface.get(), 40, 30);
    INFO("Inside child rgba=(" << int(inside.r) << "," << int(inside.g) << ","
         << int(inside.b) << "," << int(inside.a) << ")");
    REQUIRE(inside.a == 255);
    REQUIRE(inside.r == 0);
    REQUIRE(inside.g == 200);
    REQUIRE(inside.b == 100);

    // Outside the canvas widget but inside the parent: dark navy preserved.
    // The kClear in the canvas widget's clearRect must not have escaped
    // the child's bounds.
    auto outside = sample_pixel(surface.get(), 4, 4);
    INFO("Outside child rgba=(" << int(outside.r) << "," << int(outside.g) << ","
         << int(outside.b) << "," << int(outside.a) << ")");
    REQUIRE(outside.a == 255);
    REQUIRE(outside.r == 8);
    REQUIRE(outside.g == 12);
    REQUIRE(outside.b == 24);
}

#endif  // PULP_HAS_SKIA

// pulp #964 / #967 — Spectr's FilterBank scenario, asserted at the
// command-stream level so the test runs without Skia.
//
// A parent View has a green setBackground() applied. It contains two
// empty CanvasWidget children that occupy the full bounds and have
// NO draw commands queued — exactly what Spectr's React port produces
// for FilterBank's two canvas children on first frame.
//
// Contract: View::paint_all + CanvasWidget::paint together must NOT
// emit any opaque-white fill_rect that covers the wrap's bounds. The
// only fill_rect we expect at full bounds is the parent's green bg
// fill. If a future change re-introduces an opaque default in View,
// CanvasWidget, or anywhere between, the recorded stream will pick it
// up and this test fails.
TEST_CASE("FilterBank repro: empty canvas children don't paint opaque white",
          "[canvas_widget][issue-964][issue-967]") {
    RecordingCanvas rc;

    View wrap;
    wrap.set_bounds({0, 0, 64, 64});
    wrap.set_background_color(pulp::canvas::Color::rgba8(0, 200, 0, 255));

    auto cw1 = std::make_unique<CanvasWidget>();
    cw1->set_bounds({0, 0, 64, 64});
    auto cw2 = std::make_unique<CanvasWidget>();
    cw2->set_bounds({0, 0, 64, 64});
    wrap.add_child(std::move(cw1));
    wrap.add_child(std::move(cw2));

    wrap.paint_all(rc);

    // Walk the command stream, tracking the most recent set_fill_color
    // (RecordingCanvas captures color separately from fill_rect). For
    // each fill_rect that covers the full wrap bounds, classify by the
    // active fill colour: only green (parent's bg) is allowed. Any
    // opaque-white or other full-bounds fill is the bug.
    pulp::canvas::Color last_fill{};
    int full_bounds_fills = 0;
    bool saw_opaque_white = false;
    bool saw_green_bg = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_fill_color) {
            last_fill = cmd.color;
            continue;
        }
        if (cmd.type != DrawCommand::Type::fill_rect) continue;
        const bool covers_full = (cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
                                  cmd.f[2] == 64.0f && cmd.f[3] == 64.0f);
        if (!covers_full) continue;
        ++full_bounds_fills;
        const uint8_t r8 = last_fill.r8();
        const uint8_t g8 = last_fill.g8();
        const uint8_t b8 = last_fill.b8();
        const uint8_t a8 = last_fill.a8();
        if (r8 == 255 && g8 == 255 && b8 == 255 && a8 == 255) {
            saw_opaque_white = true;
        }
        if (r8 == 0 && g8 == 200 && b8 == 0 && a8 == 255) {
            saw_green_bg = true;
        }
    }

    INFO("full_bounds_fills=" << full_bounds_fills
         << " saw_green=" << saw_green_bg
         << " saw_white=" << saw_opaque_white);
    REQUIRE(saw_green_bg);
    REQUIRE_FALSE(saw_opaque_white);
    REQUIRE(full_bounds_fills == 1);
}

// pulp #967 — A bare View with no setBackground() must emit zero
// fill_rect commands of any kind. HTML <div> default is transparent.
TEST_CASE("Bare View with no setBackground emits no background fill",
          "[view][issue-967]") {
    RecordingCanvas rc;

    View v;
    v.set_bounds({0, 0, 32, 32});
    REQUIRE_FALSE(v.has_background_color());

    v.paint_all(rc);

    int fill_rect_count = 0;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::fill_rect) ++fill_rect_count;
    }
    REQUIRE(fill_rect_count == 0);
}

// ── pulp #968 — canvasRect must honour active fillStyle when no color arg ──
//
// The Canvas2D `ctx.fillRect(x,y,w,h)` API has no color argument — it paints
// with whatever `ctx.fillStyle` (a CSS color OR a CanvasGradient) was set
// most recently. The bridge's `canvasRect(id, x, y, w, h, color="#fff")`
// previously baked in literal white when the caller omitted the color, which
// makes a JS fillRect shim unable to honour the active fillStyle and breaks
// gradient fills entirely (gradient is not a string color so the shim has
// nothing to pass).
//
// Contract: when CanvasDrawCmd.use_active_style is true, the paint loop
// must NOT call set_fill_color / set_stroke_color on the underlying canvas
// before drawing — the previously-set fill/stroke state stays active.

// Case 1 — fill_rect with use_active_style honours the active fill colour.
TEST_CASE("CanvasWidget fill_rect with use_active_style preserves prior set_fill_color",
          "[canvas_widget][issue-968]") {
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 100, 100});

    // Active fill = magenta.
    CanvasDrawCmd set_color;
    set_color.type = CanvasDrawCmd::Type::set_fill_color;
    set_color.color = pulp::canvas::Color::rgba8(255, 0, 255, 255);
    cw.add_command(set_color);

    // fillRect with no explicit colour (use_active_style = true). The
    // baked-in cmd.color stays at its default of opaque white — the test
    // asserts that white is NOT what the underlying canvas actually paints
    // with.
    CanvasDrawCmd r;
    r.type = CanvasDrawCmd::Type::fill_rect;
    r.x = 10; r.y = 10; r.w = 50; r.h = 50;
    r.use_active_style = true;
    cw.add_command(r);

    cw.paint(rc);

    // Walk the recorded stream tracking the most recent set_fill_color.
    // For each fill_rect, the active fill at draw time is what the rect
    // is painted with. We expect the magenta to still be active when the
    // fill_rect is emitted — i.e. no intervening set_fill_color(white).
    pulp::canvas::Color active_fill{};
    bool saw_fill_rect = false;
    pulp::canvas::Color fill_at_draw{};
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_fill_color) {
            active_fill = cmd.color;
            continue;
        }
        if (cmd.type == DrawCommand::Type::fill_rect) {
            // Match the rect we recorded (skip any incidental fills).
            const bool matches = (cmd.f[0] == 10.0f && cmd.f[1] == 10.0f &&
                                  cmd.f[2] == 50.0f && cmd.f[3] == 50.0f);
            if (matches) {
                saw_fill_rect = true;
                fill_at_draw = active_fill;
            }
        }
    }
    REQUIRE(saw_fill_rect);
    const bool is_magenta = (fill_at_draw.r8() == 255 && fill_at_draw.g8() == 0
                          && fill_at_draw.b8() == 255 && fill_at_draw.a8() == 255);
    REQUIRE(is_magenta);
}

// Case 2 — fill_rect with explicit colour (use_active_style = false) keeps
// the legacy behaviour: cmd.color overrides whatever was set before.
TEST_CASE("CanvasWidget fill_rect with explicit color overrides active fill",
          "[canvas_widget][issue-968]") {
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 100, 100});

    // Active fill = magenta.
    CanvasDrawCmd set_color;
    set_color.type = CanvasDrawCmd::Type::set_fill_color;
    set_color.color = pulp::canvas::Color::rgba8(255, 0, 255, 255);
    cw.add_command(set_color);

    // fillRect with explicit cyan colour, use_active_style = false.
    CanvasDrawCmd r;
    r.type = CanvasDrawCmd::Type::fill_rect;
    r.x = 10; r.y = 10; r.w = 50; r.h = 50;
    r.color = pulp::canvas::Color::rgba8(0, 255, 255, 255);
    r.use_active_style = false;
    cw.add_command(r);

    cw.paint(rc);

    pulp::canvas::Color active_fill{};
    bool saw_fill_rect = false;
    pulp::canvas::Color fill_at_draw{};
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_fill_color) {
            active_fill = cmd.color;
            continue;
        }
        if (cmd.type == DrawCommand::Type::fill_rect) {
            const bool matches = (cmd.f[0] == 10.0f && cmd.f[1] == 10.0f &&
                                  cmd.f[2] == 50.0f && cmd.f[3] == 50.0f);
            if (matches) {
                saw_fill_rect = true;
                fill_at_draw = active_fill;
            }
        }
    }
    REQUIRE(saw_fill_rect);
    const bool is_cyan = (fill_at_draw.r8() == 0 && fill_at_draw.g8() == 255
                       && fill_at_draw.b8() == 255 && fill_at_draw.a8() == 255);
    REQUIRE(is_cyan);
}

// Case 3 — fill_rect with use_active_style after a gradient set keeps the
// gradient active. RecordingCanvas's default set_fill_gradient_linear falls
// back to set_fill_color(colors[0]); we use that to verify no white-on-top
// set_fill_color overwrites the gradient's first stop before the draw.
TEST_CASE("CanvasWidget fill_rect with use_active_style preserves active gradient",
          "[canvas_widget][issue-968]") {
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 100, 100});

    // Active fill = linear gradient red→blue. RecordingCanvas's default
    // overload of set_fill_gradient_linear records a set_fill_color of the
    // first stop (red).
    CanvasDrawCmd grad;
    grad.type = CanvasDrawCmd::Type::set_fill_gradient_linear;
    grad.x = 0; grad.y = 0; grad.x2 = 100; grad.y2 = 0;
    grad.gradient_colors = {pulp::canvas::Color::rgba8(255, 0, 0, 255),
                            pulp::canvas::Color::rgba8(0, 0, 255, 255)};
    grad.gradient_positions = {0.0f, 1.0f};
    cw.add_command(grad);

    // fillRect with no explicit colour — use_active_style = true.
    CanvasDrawCmd r;
    r.type = CanvasDrawCmd::Type::fill_rect;
    r.x = 10; r.y = 10; r.w = 50; r.h = 50;
    r.use_active_style = true;
    cw.add_command(r);

    cw.paint(rc);

    // The active fill at draw time must still be the gradient's first stop
    // (red) — NOT white, which is what would have been overwritten if the
    // paint loop had called set_fill_color(cmd.color) before fill_rect.
    pulp::canvas::Color active_fill{};
    bool saw_fill_rect = false;
    pulp::canvas::Color fill_at_draw{};
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_fill_color) {
            active_fill = cmd.color;
            continue;
        }
        if (cmd.type == DrawCommand::Type::fill_rect) {
            const bool matches = (cmd.f[0] == 10.0f && cmd.f[1] == 10.0f &&
                                  cmd.f[2] == 50.0f && cmd.f[3] == 50.0f);
            if (matches) {
                saw_fill_rect = true;
                fill_at_draw = active_fill;
            }
        }
    }
    REQUIRE(saw_fill_rect);
    const bool is_red = (fill_at_draw.r8() == 255 && fill_at_draw.g8() == 0
                      && fill_at_draw.b8() == 0 && fill_at_draw.a8() == 255);
    REQUIRE(is_red);
}

// Case 4 — stroke_rect mirrors fill_rect: with use_active_style the prior
// strokeStyle stays active.
TEST_CASE("CanvasWidget stroke_rect with use_active_style preserves prior set_stroke_color",
          "[canvas_widget][issue-968]") {
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 100, 100});

    CanvasDrawCmd set_color;
    set_color.type = CanvasDrawCmd::Type::set_stroke_color;
    set_color.color = pulp::canvas::Color::rgba8(0, 255, 0, 255); // green
    cw.add_command(set_color);

    CanvasDrawCmd r;
    r.type = CanvasDrawCmd::Type::stroke_rect;
    r.x = 5; r.y = 5; r.w = 40; r.h = 40;
    r.extra = 2.0f;
    r.use_active_style = true;
    cw.add_command(r);

    cw.paint(rc);

    pulp::canvas::Color active_stroke{};
    bool saw_stroke_rect = false;
    pulp::canvas::Color stroke_at_draw{};
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_stroke_color) {
            active_stroke = cmd.color;
            continue;
        }
        if (cmd.type == DrawCommand::Type::stroke_rect) {
            const bool matches = (cmd.f[0] == 5.0f && cmd.f[1] == 5.0f &&
                                  cmd.f[2] == 40.0f && cmd.f[3] == 40.0f);
            if (matches) {
                saw_stroke_rect = true;
                stroke_at_draw = active_stroke;
            }
        }
    }
    REQUIRE(saw_stroke_rect);
    const bool is_green = (stroke_at_draw.r8() == 0 && stroke_at_draw.g8() == 255
                        && stroke_at_draw.b8() == 0 && stroke_at_draw.a8() == 255);
    REQUIRE(is_green);
}

// ── pulp #964 — `ctx.fillRect()` from web-compat-canvas.js must reach the bridge ──
//
// Pre-#964, `core/view/js/web-compat-canvas.js` defined
//   CanvasRenderingContext2D.prototype.fillRect = function(x, y, w, h) {
//       this._applyFillStyle();
//       if (typeof canvasFillRect === "function") canvasFillRect(this._id, x, y, w, h);
//   };
// but the WidgetBridge only registered `canvasRect`, not `canvasFillRect`.
// The `typeof` guard then short-circuited every fillRect invocation — silently.
// `canvasStrokeRect`, `canvasFillPath`, and `canvasStrokePath` were registered
// under their natural names so they kept working, which is what made the bug
// look like a Skia compositing failure: a Canvas2D scene that mixed paths
// (visible) and fillRects (invisible) produced partial output.
//
// Contract: the bridge must register both `canvasFillRect` (the web-compat
// shim's name) and `canvasRect` (the legacy direct-bridge name used by
// hand-written examples) so neither path silently drops fills.

#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>

TEST_CASE("WidgetBridge: canvasFillRect bridges ctx.fillRect to a fill_rect command",
          "[canvas_widget][bridge][issue-964]") {
    pulp::view::ScriptEngine engine;
    pulp::view::View root;
    root.set_bounds({0, 0, 200, 100});
    pulp::state::StateStore store;
    pulp::view::WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createCanvas('cv', '');
        canvasFillRect('cv', 5, 7, 40, 30, '#ff0000');
    )");

    auto* cw = dynamic_cast<CanvasWidget*>(bridge.widget("cv"));
    REQUIRE(cw != nullptr);
    REQUIRE(cw->command_count() == 1);

    // Replay onto a RecordingCanvas and verify a single fill_rect with the
    // correct geometry and colour landed.
    RecordingCanvas rc;
    cw->paint(rc);

    int fill_rect_count = 0;
    pulp::canvas::Color last_fill{};
    bool saw_correct_geom = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_fill_color) {
            last_fill = cmd.color;
            continue;
        }
        if (cmd.type == DrawCommand::Type::fill_rect) {
            ++fill_rect_count;
            const bool matches = (cmd.f[0] == 5.0f && cmd.f[1] == 7.0f &&
                                  cmd.f[2] == 40.0f && cmd.f[3] == 30.0f);
            if (matches) saw_correct_geom = true;
        }
    }
    REQUIRE(fill_rect_count == 1);
    REQUIRE(saw_correct_geom);
    REQUIRE(last_fill.r8() == 255);
    REQUIRE(last_fill.g8() == 0);
    REQUIRE(last_fill.b8() == 0);
    REQUIRE(last_fill.a8() == 255);
}

TEST_CASE("WidgetBridge: HTML5 ctx.fillRect via web-compat shim reaches the bridge",
          "[canvas_widget][bridge][issue-964]") {
    // The user-facing path: a JS author writes `ctx.fillRect(...)`. The
    // web-compat-canvas.js prelude installs CanvasRenderingContext2D, the
    // shim's fillRect calls canvasFillRect, the bridge dispatches to a
    // CanvasDrawCmd::fill_rect on the widget. Pre-#964 this whole chain
    // silently produced zero commands.
    pulp::view::ScriptEngine engine;
    pulp::view::View root;
    root.set_bounds({0, 0, 200, 100});
    pulp::state::StateStore store;
    pulp::view::WidgetBridge bridge(engine, root, store);

    // Build a minimal Canvas element via the DOM-ish bridge surface, then
    // exercise the actual `ctx.fillRect()` path the FilterBank scenario
    // hits in production.
    bridge.load_script(R"JS(
        // Manually create a CanvasWidget bridged element. createCanvas binds
        // the C++ widget; the web-compat shim wraps it in a JS Element-like
        // object so getContext('2d') returns a real CanvasRenderingContext2D.
        createCanvas('cv', '');
        // The web-compat layer expects an Element with _id; fabricate the
        // smallest viable one so getContext('2d') and the prototype chain
        // see the canvas widget.
        var el = { _id: 'cv', tagName: 'CANVAS' };
        // The Canvas2D shim references Element.prototype.getContext, so
        // borrow the prototype method directly.
        var ctx = Element.prototype.getContext.call(el, '2d');
        ctx.fillStyle = '#00ff00';
        ctx.fillRect(10, 10, 80, 40);
    )JS");

    auto* cw = dynamic_cast<CanvasWidget*>(bridge.widget("cv"));
    REQUIRE(cw != nullptr);
    // Pre-fix the shim's fillRect produces zero commands. The fix wires
    // canvasFillRect at the bridge so the call is actually recorded.
    REQUIRE(cw->command_count() >= 1);

    RecordingCanvas rc;
    cw->paint(rc);

    bool saw_green_full_fill = false;
    pulp::canvas::Color active_fill{};
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_fill_color) {
            active_fill = cmd.color;
            continue;
        }
        if (cmd.type != DrawCommand::Type::fill_rect) continue;
        const bool matches = (cmd.f[0] == 10.0f && cmd.f[1] == 10.0f &&
                              cmd.f[2] == 80.0f && cmd.f[3] == 40.0f);
        if (!matches) continue;
        const bool is_green = (active_fill.r8() == 0 && active_fill.g8() == 255 &&
                               active_fill.b8() == 0 && active_fill.a8() == 255);
        if (is_green) saw_green_full_fill = true;
    }
    REQUIRE(saw_green_full_fill);
}

// ── pulp #1368 — CanvasWidget::paint balances save/restore across frames ──
//
// Spectr's filterbank uses two `<canvas>` widgets in identical configuration.
// One paints visibly, the other doesn't. The visible one (overlay sibling)
// runs after the invisible one (main sibling). Fillrects on the invisible
// canvas with huge bounds tinted the title-bar region above the canvas —
// strongly suggesting a transform leak: an unrestored `ctx.save()` from a
// prior frame's draw script left GState (transform, clip) on the canvas
// stack, which the parent View's outer `canvas.restore()` can't pop because
// it only pops one level.
//
// Defense: CanvasWidget::paint snapshots `save_count()` at entry and calls
// `restore_to_count()` after the JS replay so any leftover save is dropped
// and the parent always sees the canvas at the depth it expects.

TEST_CASE("CanvasWidget::paint balances save/restore even when JS misses restore",
          "[canvas_widget][issue-1368]") {
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 200, 100});

    // Simulate an unbalanced JS draw script: ctx.save() + ctx.scale(2,2)
    // followed by a fillRect, but the matching ctx.restore() is missing
    // (e.g., the JS hit an early-return path).
    CanvasDrawCmd s;
    s.type = CanvasDrawCmd::Type::save;
    cw.add_command(s);

    CanvasDrawCmd sc;
    sc.type = CanvasDrawCmd::Type::scale;
    sc.x = 2.0f; sc.y = 2.0f;
    cw.add_command(sc);

    CanvasDrawCmd r;
    r.type = CanvasDrawCmd::Type::fill_rect;
    r.x = 0; r.y = 0; r.w = 50; r.h = 50;
    r.color = {255, 0, 255, 255};
    cw.add_command(r);
    // NOTE: no matching restore command — JS bug.

    const int depth_before = rc.save_count();
    cw.paint(rc);
    const int depth_after = rc.save_count();

    // The whole point of the fix: even with an unbalanced draw script,
    // the canvas's save-stack depth must return to where it started so
    // the parent View's outer save/restore is balanced and no GState
    // leaks onto sibling widgets.
    REQUIRE(depth_after == depth_before);
}

TEST_CASE("CanvasWidget::paint twice with unbalanced JS does not accumulate depth",
          "[canvas_widget][issue-1368]") {
    // Same scenario painted twice — without the fix the second frame
    // would land at depth 2 (1 leaked save per frame). Asserting depth
    // stays at the entry baseline across multiple frames is the regression
    // guard for the Spectr filterbank repro.
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 200, 100});

    CanvasDrawCmd s; s.type = CanvasDrawCmd::Type::save; cw.add_command(s);
    CanvasDrawCmd r;
    r.type = CanvasDrawCmd::Type::fill_rect;
    r.x = 0; r.y = 0; r.w = 10; r.h = 10;
    r.color = {255, 0, 255, 255};
    cw.add_command(r);
    // Deliberately no restore.

    const int depth_before = rc.save_count();
    cw.paint(rc);
    cw.paint(rc);
    cw.paint(rc);
    REQUIRE(rc.save_count() == depth_before);
}

TEST_CASE("CanvasWidget::paint preserves baseline depth even with extra restores",
          "[canvas_widget][issue-1368]") {
    // Inverse pathology: JS calls ctx.restore() more times than it
    // ctx.save()'d. RecordingCanvas guards depth at zero so the over-pop
    // doesn't underflow; CanvasWidget::paint must still leave the depth
    // at exactly the captured entry depth, not below.
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 200, 100});

    CanvasDrawCmd r1;
    r1.type = CanvasDrawCmd::Type::restore;
    cw.add_command(r1);
    CanvasDrawCmd r2;
    r2.type = CanvasDrawCmd::Type::restore;
    cw.add_command(r2);

    const int depth_before = rc.save_count();
    cw.paint(rc);
    REQUIRE(rc.save_count() == depth_before);
}

TEST_CASE("CanvasWidget::paint nested under a parent save still balances",
          "[canvas_widget][issue-1368]") {
    // Mimic View::paint_all wrapping: outer save → bounds translate → call
    // CanvasWidget::paint → outer restore. With an unbalanced JS save
    // inside paint(), the outer restore must still bring the canvas back
    // to exactly the depth that existed before the wrapper's save() was
    // pushed. This is the Spectr filterbank invariant — sibling widgets
    // painted after this canvas must see a clean parent canvas state.
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 200, 100});

    CanvasDrawCmd s; s.type = CanvasDrawCmd::Type::save; cw.add_command(s);
    CanvasDrawCmd r;
    r.type = CanvasDrawCmd::Type::fill_rect;
    r.x = 0; r.y = 0; r.w = 50; r.h = 50;
    r.color = {255, 0, 255, 255};
    cw.add_command(r);
    // No restore in the JS list — the unbalanced case.

    const int depth_at_root = rc.save_count();
    rc.save();                  // outer wrapper, mirrors View::paint_all
    rc.translate(10.0f, 20.0f); // bounds translate
    cw.paint(rc);
    rc.restore();               // outer wrapper restore

    REQUIRE(rc.save_count() == depth_at_root);
}

// ── pulp #1368 — per-canvas backing-store isolation via save_layer ─────
//
// Root cause beyond the save/restore depth defense above: JS-driven
// `clearRect` lowers to a kClear blend (SkBlendMode::kClear /
// CGContextClearRect) which unconditionally zeros destination texels
// regardless of source alpha. Without per-canvas isolation the kClear
// hits the shared parent surface and erases pixels that sibling
// canvases just painted. HTML <canvas> semantics require each <canvas>
// to have its own backing store; the fix wraps every CanvasWidget
// paint in `save_layer()` over its local bounds so kClear can only
// affect the layer, then `restore()` composites the layer back into
// the parent via SrcOver — preserving sibling pixels.
//
// These tests assert the structural property at the recording-canvas
// level (depth bracketing, save count contribution) so they run on
// every platform without needing Skia. The pixel-level proof lives
// in the [skia] tests below.

TEST_CASE("CanvasWidget::paint opens a per-canvas save_layer over its bounds",
          "[canvas_widget][issue-1368]") {
    // The save_layer() default falls back to save() on RecordingCanvas,
    // so the depth must rise by exactly one between entry and the JS
    // replay. We sample the depth mid-replay using a draw command we
    // queue between the entry point and the natural end of paint().
    // RecordingCanvas tracks save_depth_ via save() / restore(), so the
    // depth observable to a queued sub-command would be entry_depth + 1
    // when the layer is open.
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 200, 100});

    CanvasDrawCmd r;
    r.type = CanvasDrawCmd::Type::fill_rect;
    r.x = 0; r.y = 0; r.w = 50; r.h = 50;
    r.color = {255, 0, 0, 255};
    cw.add_command(r);

    REQUIRE(rc.save_count() == 0);
    cw.paint(rc);
    // Post-paint depth returns to entry depth — bracketed correctly.
    REQUIRE(rc.save_count() == 0);

    // The recorded stream must contain exactly one save (the layer push)
    // attributable to the canvas widget itself. Future state-tracking
    // additions to paint() may add nested saves; but at minimum, paint()
    // must contribute a leading save. Walk the recorded commands,
    // verify a `save` appears before the queued fill_rect.
    int save_idx = -1, fill_idx = -1;
    int idx = 0;
    for (const auto& cmd : rc.commands()) {
        if (save_idx < 0 && cmd.type == DrawCommand::Type::save) save_idx = idx;
        if (cmd.type == DrawCommand::Type::fill_rect) { fill_idx = idx; break; }
        ++idx;
    }
    REQUIRE(save_idx >= 0);
    REQUIRE(fill_idx > save_idx);
}

TEST_CASE("CanvasWidget::paint skips save_layer when bounds are degenerate",
          "[canvas_widget][issue-1368]") {
    // A zero-size canvas widget would open a degenerate layer if we
    // unconditionally pushed save_layer. Skip the layer in that case
    // — there is nothing to isolate, and Skia rejects zero-area
    // saveLayer in some configurations. Confirm by asserting no `save`
    // is emitted when the JS draw queue is empty AND bounds are 0.
    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 0, 0});

    cw.paint(rc);

    // No layer push, no draws, no saves. Pre-fix this also held; the
    // assertion now also holds post-fix because of the degenerate-bounds
    // guard in CanvasWidget::paint.
    int save_count_in_stream = 0;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::save) ++save_count_in_stream;
    }
    REQUIRE(save_count_in_stream == 0);
}

TEST_CASE("Two sibling CanvasWidgets each open their own backing store",
          "[canvas_widget][issue-1368]") {
    // Spectr filterbank scenario in command-stream form: two canvas
    // widgets share a parent View and paint sequentially. Each one's
    // paint() must contribute its own save (the per-canvas layer
    // push). Without the fix sibling-2's clearRect would hit the
    // parent surface; the structural guard here is "each canvas
    // contributes the bracketing save" — pixel-level isolation is
    // covered by the [skia] tests below.
    RecordingCanvas rc;

    View parent;
    parent.set_bounds({0, 0, 64, 64});

    auto cw1 = std::make_unique<CanvasWidget>();
    cw1->set_bounds({0, 0, 64, 64});
    {
        CanvasDrawCmd r;
        r.type = CanvasDrawCmd::Type::fill_rect;
        r.x = 0; r.y = 0; r.w = 64; r.h = 64;
        r.color = {255, 0, 0, 255};
        cw1->add_command(r);
    }

    auto cw2 = std::make_unique<CanvasWidget>();
    cw2->set_bounds({0, 0, 64, 64});
    {
        // Sibling 2 starts with a clearRect — the smoking-gun bug pattern.
        CanvasDrawCmd c;
        c.type = CanvasDrawCmd::Type::clear_rect;
        c.x = 0; c.y = 0; c.w = 64; c.h = 64;
        cw2->add_command(c);
    }

    parent.add_child(std::move(cw1));
    parent.add_child(std::move(cw2));

    parent.paint_all(rc);

    // Count clear_rects and saves AFTER the first canvas's fill_rect
    // landed. Because each canvas widget opens its own layer, the
    // clear_rect must appear AFTER an additional save (the second
    // canvas's layer push) and before the matching restores. If the
    // layer were not pushed, the clear_rect would land on the parent
    // surface — outside any save/restore bracket attributable to
    // CanvasWidget::paint.
    int first_fill = -1;
    int clear_after_first_fill = -1;
    int save_between = 0;
    int idx = 0;
    for (const auto& cmd : rc.commands()) {
        if (first_fill < 0 && cmd.type == DrawCommand::Type::fill_rect) {
            first_fill = idx;
        } else if (first_fill >= 0 && cmd.type == DrawCommand::Type::save) {
            ++save_between;
        } else if (first_fill >= 0 && clear_after_first_fill < 0 &&
                   cmd.type == DrawCommand::Type::clear_rect) {
            clear_after_first_fill = idx;
        }
        ++idx;
    }
    REQUIRE(first_fill >= 0);
    REQUIRE(clear_after_first_fill > first_fill);
    // Between the first fill_rect and the second canvas's clear_rect,
    // there must be at least one extra save — the second canvas's
    // per-canvas backing store layer.
    REQUIRE(save_between >= 1);
}

#ifdef PULP_HAS_SKIA

// pulp #1368 — visual sibling isolation. Two CanvasWidgets paint
// against a shared parent surface; sibling-2 starts its frame with
// clearRect over its full bounds. Pre-fix, the clearRect zeroed the
// shared parent texels and erased sibling-1's pixels. Post-fix, each
// canvas paints into its own save_layer-backed offscreen so kClear
// only affects the layer and sibling-1's draws survive.
TEST_CASE("Sibling CanvasWidget clearRect does not erase prior sibling's pixels",
          "[canvas_widget][skia][issue-1368]") {
    SkImageInfo info = SkImageInfo::Make(64, 64, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());

    View parent;
    parent.set_bounds({0, 0, 64, 64});

    // Sibling 1: paints opaque red across its bounds.
    auto cw1 = std::make_unique<CanvasWidget>();
    cw1->set_bounds({0, 0, 64, 64});
    {
        CanvasDrawCmd r;
        r.type = CanvasDrawCmd::Type::fill_rect;
        r.x = 0; r.y = 0; r.w = 64; r.h = 64;
        r.color = pulp::canvas::Color::rgba8(255, 0, 0, 255);
        cw1->add_command(r);
    }

    // Sibling 2: starts its frame with clearRect over its full bounds
    // (mirrors a JS canvas idiom). Pre-fix this kClear nuked sibling 1's
    // red on the shared parent. Post-fix, kClear is contained to
    // sibling 2's own layer and the parent surface keeps sibling 1's red.
    auto cw2 = std::make_unique<CanvasWidget>();
    cw2->set_bounds({0, 0, 64, 64});
    {
        CanvasDrawCmd c;
        c.type = CanvasDrawCmd::Type::clear_rect;
        c.x = 0; c.y = 0; c.w = 64; c.h = 64;
        cw2->add_command(c);
    }

    parent.add_child(std::move(cw1));
    parent.add_child(std::move(cw2));
    parent.paint_all(canvas);

    auto px = sample_pixel(surface.get(), 32, 32);
    INFO("Sibling-isolation pixel rgba=("
         << int(px.r) << "," << int(px.g) << ","
         << int(px.b) << "," << int(px.a) << ")");
    // Sibling 1's red survived sibling 2's clearRect.
    REQUIRE(px.a == 255);
    REQUIRE(px.r == 255);
    REQUIRE(px.g == 0);
    REQUIRE(px.b == 0);
}

// pulp #1368 — destination-out compositing must also be contained.
// `globalCompositeOperation = "destination-out"` punches alpha holes
// in the destination. Without per-canvas isolation, sibling-2's
// destination-out fill would punch a hole through sibling-1's pixels
// on the shared parent surface. With the layer fix, the destination-
// out lands inside sibling-2's own layer; sibling-1's pixels survive.
TEST_CASE("Sibling CanvasWidget destination-out does not punch holes in siblings",
          "[canvas_widget][skia][issue-1368]") {
    SkImageInfo info = SkImageInfo::Make(64, 64, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());

    View parent;
    parent.set_bounds({0, 0, 64, 64});

    auto cw1 = std::make_unique<CanvasWidget>();
    cw1->set_bounds({0, 0, 64, 64});
    {
        CanvasDrawCmd r;
        r.type = CanvasDrawCmd::Type::fill_rect;
        r.x = 0; r.y = 0; r.w = 64; r.h = 64;
        r.color = pulp::canvas::Color::rgba8(0, 200, 0, 255);  // green
        cw1->add_command(r);
    }

    auto cw2 = std::make_unique<CanvasWidget>();
    cw2->set_bounds({0, 0, 64, 64});
    {
        // Switch sibling 2 into destination-out mode and fill — without
        // the layer this would punch holes through sibling-1's green
        // on the shared parent surface.
        CanvasDrawCmd mode;
        mode.type = CanvasDrawCmd::Type::set_blend_mode;
        // BlendMode enum value for destination-out — RecordingCanvas /
        // SkiaCanvas both accept it via set_blend_mode(static_cast<
        // BlendMode>(int_val)). The exact int doesn't matter for the
        // test; we just need a punch-through compositing operator.
        mode.int_val = static_cast<int>(pulp::canvas::Canvas::BlendMode::destination_out);
        cw2->add_command(mode);
        CanvasDrawCmd r;
        r.type = CanvasDrawCmd::Type::fill_rect;
        r.x = 0; r.y = 0; r.w = 64; r.h = 64;
        r.color = pulp::canvas::Color::rgba8(0, 0, 0, 255);
        cw2->add_command(r);
    }

    parent.add_child(std::move(cw1));
    parent.add_child(std::move(cw2));
    parent.paint_all(canvas);

    auto px = sample_pixel(surface.get(), 32, 32);
    INFO("destination-out isolation pixel rgba=("
         << int(px.r) << "," << int(px.g) << ","
         << int(px.b) << "," << int(px.a) << ")");
    REQUIRE(px.a == 255);
    REQUIRE(px.r == 0);
    REQUIRE(px.g == 200);
    REQUIRE(px.b == 0);
}

// pulp #1368 — single-canvas paints should not change output. The
// per-canvas layer must be visually equivalent to direct-on-parent
// painting for an isolated canvas; the layer's existence is a
// correctness fix for the multi-canvas case, not a behaviour change
// for single-canvas use.
TEST_CASE("Single CanvasWidget paint is pixel-equivalent with the layer wrap",
          "[canvas_widget][skia][issue-1368]") {
    SkImageInfo info = SkImageInfo::Make(32, 32, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());

    View parent;
    parent.set_bounds({0, 0, 32, 32});
    parent.set_background_color(pulp::canvas::Color::rgba8(8, 12, 24, 255));

    auto cw = std::make_unique<CanvasWidget>();
    cw->set_bounds({0, 0, 32, 32});
    {
        CanvasDrawCmd c;
        c.type = CanvasDrawCmd::Type::clear_rect;
        c.x = 0; c.y = 0; c.w = 32; c.h = 32;
        cw->add_command(c);
    }
    {
        CanvasDrawCmd r;
        r.type = CanvasDrawCmd::Type::fill_rect;
        r.x = 4; r.y = 4; r.w = 24; r.h = 24;
        r.color = pulp::canvas::Color::rgba8(255, 0, 255, 255);
        cw->add_command(r);
    }
    parent.add_child(std::move(cw));
    parent.paint_all(canvas);

    // Inside the painted rect: magenta. Pre- and post-#1368 both must
    // produce magenta here because the layer composites the painted
    // pixels back onto the parent.
    auto inside = sample_pixel(surface.get(), 16, 16);
    REQUIRE(inside.a == 255);
    REQUIRE(inside.r == 255);
    REQUIRE(inside.g == 0);
    REQUIRE(inside.b == 255);

    // In the cleared region (everything in the canvas widget bounds
    // outside the small fill_rect), the layer is transparent and
    // composites back over the parent's navy bg. The parent's navy
    // therefore shows through. Pre-#1368 the kClear leaked onto the
    // parent surface and would have produced (0,0,0,0) here instead.
    auto cleared = sample_pixel(surface.get(), 1, 1);
    INFO("Cleared-region pixel rgba=("
         << int(cleared.r) << "," << int(cleared.g) << ","
         << int(cleared.b) << "," << int(cleared.a) << ")");
    REQUIRE(cleared.a == 255);
    REQUIRE(cleared.r == 8);
    REQUIRE(cleared.g == 12);
    REQUIRE(cleared.b == 24);
}

#endif  // PULP_HAS_SKIA
// ── pulp #1368 round 2 — current_transform() snapshot for paint diagnostics ──
//
// The Spectr filterbank repro shows pr_1's first-instruction `fillRect(50,50,…)`
// landing in the title-bar region above the canvas. Either bounds_.y is wrong
// (Track B layout test) or the inbound CTM has a translation that pushes draws
// off-window (Track A diagnostic).  Canvas::current_transform() is the new
// virtual the env-gated `PULP_LOG_CANVAS_PAINT=1` trace reads to log the CTM
// as it actually exists at paint() entry, so Spectr can paste a per-frame line
// like
//   [pulp:canvas-paint] id=pr_1 bounds=(0,0,1320,760) ctm=[1,0,0,1,0,-100]
// into #1368.  The tests below pin the introspection contract on
// RecordingCanvas (the only backend exercised in Catch2): identity at start,
// updated by translate/scale, and reset across save/restore.

TEST_CASE("RecordingCanvas::current_transform tracks identity at start",
          "[canvas_widget][issue-1368][round2]") {
    using namespace pulp::canvas;
    RecordingCanvas rc;
    auto t = rc.current_transform();
    REQUIRE(t.a == 1.0f);
    REQUIRE(t.b == 0.0f);
    REQUIRE(t.c == 0.0f);
    REQUIRE(t.d == 1.0f);
    REQUIRE(t.e == 0.0f);
    REQUIRE(t.f == 0.0f);
}

TEST_CASE("RecordingCanvas::current_transform reflects translate",
          "[canvas_widget][issue-1368][round2]") {
    using namespace pulp::canvas;
    RecordingCanvas rc;
    rc.translate(10.0f, 20.0f);
    auto t = rc.current_transform();
    REQUIRE(t.a == 1.0f);
    REQUIRE(t.d == 1.0f);
    REQUIRE(t.e == 10.0f);
    REQUIRE(t.f == 20.0f);
}

TEST_CASE("RecordingCanvas::current_transform restores across save/restore",
          "[canvas_widget][issue-1368][round2]") {
    using namespace pulp::canvas;
    RecordingCanvas rc;
    rc.save();
    rc.translate(50.0f, 60.0f);
    auto inner = rc.current_transform();
    REQUIRE(inner.e == 50.0f);
    REQUIRE(inner.f == 60.0f);
    rc.restore();
    auto outer = rc.current_transform();
    REQUIRE(outer.e == 0.0f);
    REQUIRE(outer.f == 0.0f);
}

TEST_CASE("RecordingCanvas::current_transform composes translate + scale",
          "[canvas_widget][issue-1368][round2]") {
    using namespace pulp::canvas;
    RecordingCanvas rc;
    // CanvasRenderingContext2D semantics: translate THEN scale in user space
    // means current = T * S, so a unit-square corner (1,1) maps to (10+2, 20+3).
    rc.translate(10.0f, 20.0f);
    rc.scale(2.0f, 3.0f);
    auto t = rc.current_transform();
    REQUIRE(t.a == 2.0f);
    REQUIRE(t.d == 3.0f);
    REQUIRE(t.e == 10.0f);
    REQUIRE(t.f == 20.0f);
}

// ── pulp #1368 round 2 — env-gated paint trace: coverage exercise ───────────
//
// PULP_LOG_CANVAS_PAINT=1 is the diagnostic switch Spectr uses to capture
// per-frame bounds + CTM + cmd_total + per-type summary. The fprintf+map
// walk in CanvasWidget::paint() is purely instrumentation — guarded behind
// a single getenv per paint and otherwise a complete no-op. These tests
// exercise the gated path so diff-coverage isn't blocked by
// "instrumentation isn't covered" against a behavior change that only
// surfaces when the env var is set.

#include <cstdlib>
#include <string>

namespace {

// Cross-platform setenv/unsetenv wrappers — Windows MSVC ships _putenv_s
// instead of POSIX setenv/unsetenv. The instrumentation tests don't care
// about thread-safety; both POSIX setenv() and _putenv_s() are documented
// non-reentrant against getenv() on the same key.
inline void portable_setenv(const char* key, const char* value) {
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    ::setenv(key, value, 1);
#endif
}

inline void portable_unsetenv(const char* key) {
#if defined(_WIN32)
    _putenv_s(key, "");  // empty string clears on Windows
#else
    ::unsetenv(key);
#endif
}

class ScopedEnv {
public:
    ScopedEnv(const char* key, const char* value) : key_(key) {
        const char* prev = std::getenv(key);
        prev_value_ = prev ? std::string(prev) : std::string();
        had_prev_ = prev != nullptr;
        portable_setenv(key, value);
    }
    ~ScopedEnv() {
        if (had_prev_) portable_setenv(key_, prev_value_.c_str());
        else portable_unsetenv(key_);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
private:
    const char* key_;
    std::string prev_value_;
    bool had_prev_;
};

} // namespace

TEST_CASE("CanvasWidget::paint logging path runs when PULP_LOG_CANVAS_PAINT=1",
          "[canvas_widget][issue-1368][round2][instrumentation]") {
    ScopedEnv guard("PULP_LOG_CANVAS_PAINT", "1");

    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({10, 20, 200, 100});

    // Mix of command types so the per-type tally has multiple entries.
    CanvasDrawCmd r;
    r.type = CanvasDrawCmd::Type::fill_rect;
    r.x = 0; r.y = 0; r.w = 50; r.h = 50;
    r.color = {255, 0, 0, 255};
    cw.add_command(r);

    CanvasDrawCmd c;
    c.type = CanvasDrawCmd::Type::clear_rect;
    c.x = 5; c.y = 5; c.w = 10; c.h = 10;
    cw.add_command(c);

    CanvasDrawCmd s;
    s.type = CanvasDrawCmd::Type::save;
    cw.add_command(s);
    CanvasDrawCmd rs;
    rs.type = CanvasDrawCmd::Type::restore;
    cw.add_command(rs);

    // Should not throw, should run the env-gated logging block once, and the
    // command replay must still happen normally on the recording canvas.
    REQUIRE_NOTHROW(cw.paint(rc));

    // The replay still happens — fill_rect and clear_rect both reach rc.
    REQUIRE(rc.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(rc.count(DrawCommand::Type::clear_rect) == 1);
}

// Hit every CanvasDrawCmd::Type case in canvas_cmd_type_name's switch by
// queueing one command of each type. Coverage of canvas_cmd_type_name is
// only reached when the env-gated logging path runs over commands_ — so
// this test sets PULP_LOG_CANVAS_PAINT=1 and enumerates the full type set.
TEST_CASE("CanvasWidget::paint logging summary covers every CanvasDrawCmd type",
          "[canvas_widget][issue-1368][round2][instrumentation]") {
    ScopedEnv guard("PULP_LOG_CANVAS_PAINT", "1");

    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 200, 200});

    auto add = [&](CanvasDrawCmd::Type t) {
        CanvasDrawCmd c;
        c.type = t;
        c.x = 0; c.y = 0; c.w = 10; c.h = 10;
        c.color = {128, 128, 128, 255};
        cw.add_command(c);
    };

    add(CanvasDrawCmd::Type::fill_rect);
    add(CanvasDrawCmd::Type::stroke_rect);
    add(CanvasDrawCmd::Type::fill_rounded_rect);
    add(CanvasDrawCmd::Type::stroke_rounded_rect);
    add(CanvasDrawCmd::Type::fill_circle);
    add(CanvasDrawCmd::Type::stroke_circle);
    add(CanvasDrawCmd::Type::stroke_line);
    add(CanvasDrawCmd::Type::stroke_arc);
    add(CanvasDrawCmd::Type::fill_text);
    add(CanvasDrawCmd::Type::set_font);
    add(CanvasDrawCmd::Type::set_text_align);
    add(CanvasDrawCmd::Type::set_text_baseline);
    add(CanvasDrawCmd::Type::set_fill_color);
    add(CanvasDrawCmd::Type::set_stroke_color);
    add(CanvasDrawCmd::Type::set_line_width);
    add(CanvasDrawCmd::Type::set_line_cap);
    add(CanvasDrawCmd::Type::set_line_join);
    add(CanvasDrawCmd::Type::set_global_alpha);
    add(CanvasDrawCmd::Type::set_blend_mode);
    add(CanvasDrawCmd::Type::set_fill_gradient_linear);
    add(CanvasDrawCmd::Type::set_fill_gradient_radial);
    add(CanvasDrawCmd::Type::clear_fill_gradient);
    add(CanvasDrawCmd::Type::begin_path);
    add(CanvasDrawCmd::Type::move_to);
    add(CanvasDrawCmd::Type::line_to);
    add(CanvasDrawCmd::Type::quad_to);
    add(CanvasDrawCmd::Type::cubic_to);
    add(CanvasDrawCmd::Type::close_path);
    add(CanvasDrawCmd::Type::fill_path);
    add(CanvasDrawCmd::Type::stroke_path);
    add(CanvasDrawCmd::Type::clip_path);
    // save/restore covered in the previous test
    add(CanvasDrawCmd::Type::translate);
    add(CanvasDrawCmd::Type::scale);
    add(CanvasDrawCmd::Type::rotate);
    add(CanvasDrawCmd::Type::clip_rect);
    add(CanvasDrawCmd::Type::set_transform);
    add(CanvasDrawCmd::Type::clip);
    add(CanvasDrawCmd::Type::draw_image);
    add(CanvasDrawCmd::Type::set_line_dash);
    add(CanvasDrawCmd::Type::put_image_data);
    add(CanvasDrawCmd::Type::clear);
    // clear_rect covered in the previous test

    REQUIRE_NOTHROW(cw.paint(rc));
    // No assertion on rc — some types are no-ops on RecordingCanvas; the
    // point is that canvas_cmd_type_name's switch covered each case.
}

TEST_CASE("CanvasWidget::paint logging path is silent when env unset or empty",
          "[canvas_widget][issue-1368][round2][instrumentation]") {
    // No env var set — the logging block must be skipped entirely (gated
    // behind canvas_paint_logging_enabled()).
    portable_unsetenv("PULP_LOG_CANVAS_PAINT");

    RecordingCanvas rc;
    CanvasWidget cw;
    cw.set_bounds({0, 0, 100, 100});
    CanvasDrawCmd r;
    r.type = CanvasDrawCmd::Type::fill_rect;
    r.x = 0; r.y = 0; r.w = 10; r.h = 10;
    cw.add_command(r);

    REQUIRE_NOTHROW(cw.paint(rc));
    REQUIRE(rc.count(DrawCommand::Type::fill_rect) == 1);

    // "0" / empty also disable.
    {
        ScopedEnv g0("PULP_LOG_CANVAS_PAINT", "0");
        REQUIRE_NOTHROW(cw.paint(rc));
    }
    {
        ScopedEnv ge("PULP_LOG_CANVAS_PAINT", "");
        REQUIRE_NOTHROW(cw.paint(rc));
    }
}

// ─────────────────────────────────────────────────────────────────────
// pulp #1387 gap #2 — NaN / Infinity defense at add_command boundary
// ─────────────────────────────────────────────────────────────────────
//
// Spectr's filterbank reproduced this: an off-screen drag computed a
// gain that briefly went through divide-by-zero on a transient layout
// (canvas-y/halfH where halfH was 0 mid-resize), feeding NaN into one
// canvasLineTo call. Skia/CG's coordinate state then carried the NaN
// for the rest of the frame, painting the bands as solid grey blobs
// (the same color all subsequent draws inherited from the corrupted
// transform). One bad coord was enough to taint the whole frame.
//
// The defense lives at CanvasWidget::add_command. Sanitization is
// recording-side (not paint-side) so every backend — Skia GPU, CG
// CPU, RecordingCanvas, headless capture — gets clean numerics
// without retrofitting each one.

TEST_CASE("CanvasWidget paint dispatches set_direction to the canvas",
          "[canvas_widget][issue-1520]") {
    CanvasWidget cw;
    cw.set_bounds({0, 0, 100, 100});

    CanvasDrawCmd dir;
    dir.type = CanvasDrawCmd::Type::set_direction;
    dir.int_val = 1; // rtl
    cw.add_command(dir);

    RecordingCanvas rec;
    cw.paint(rec);

    int direction_cmds = 0;
    float observed_value = -1.0f;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == DrawCommand::Type::set_direction) {
            ++direction_cmds;
            observed_value = cmd.f[0];
        }
    }
    REQUIRE(direction_cmds == 1);
    REQUIRE(observed_value == static_cast<float>(
        pulp::canvas::Canvas::TextDirection::rtl));
}

TEST_CASE("CanvasWidget paint dispatches set_filter to the canvas",
          "[canvas_widget][issue-1520]") {
    CanvasWidget cw;
    cw.set_bounds({0, 0, 100, 100});

    CanvasDrawCmd filt;
    filt.type = CanvasDrawCmd::Type::set_filter;
    filt.text = "blur(5px) sepia(60%)";
    cw.add_command(filt);

    RecordingCanvas rec;
    cw.paint(rec);

    bool saw_filter = false;
    std::string observed_string;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == DrawCommand::Type::set_filter) {
            saw_filter = true;
            observed_string = cmd.text;
        }
    }
    REQUIRE(saw_filter);
    REQUIRE(observed_string == "blur(5px) sepia(60%)");
}

// ── pulp #1739 codecov backfill — 9-arg drawImage paint-replay coverage ──
//
// PR #1739 wired the canvas2d/drawImage sprite-sheet form
// (drawImage(img, sx,sy,sw,sh, dx,dy,dw,dh)) end-to-end through the JS
// shim → WidgetBridge → CanvasWidget::paint() → Canvas::draw_image_*_rect.
// The end-to-end test in test_widget_bridge.cpp [issue-1737] passes, but
// codecov reported 0% patch coverage on the new lines because the bridge
// → JS-shim → widget-paint dispatch chain was too indirect for per-line
// attribution. These focused tests construct the CanvasDrawCmd directly
// and call CanvasWidget::paint() against a RecordingCanvas — codecov
// can attribute the `has_source_rect` branch in canvas_widget.cpp and
// the `draw_image_from_file_rect` recording in recording_canvas.cpp
// unambiguously to these test cases.

// 9-arg form: `has_source_rect=true` routes through the `_rect` overload
// so x2/y2/x3/y3 (sx,sy,sw,sh) ride alongside x/y/w/h (dx,dy,dw,dh) to
// the canvas backend. Asserting on RecordingCanvas:
//   - cmd.f[0..3] == dst rect
//   - cmd.floats[0..3] == src rect
// proves draw_image_from_file_rect was called (the dst-only path leaves
// `floats` empty — see test below).
TEST_CASE("CanvasWidget::paint routes draw_image through _rect overload when has_source_rect",
          "[canvas_widget][issue-1739][canvas2d][coverage]") {
    CanvasWidget cw;
    cw.set_bounds({0, 0, 200, 100});

    CanvasDrawCmd img;
    img.type        = CanvasDrawCmd::Type::draw_image;
    img.text        = "/sprites/walk.png";   // non-empty, non-data-URI path
    // Destination rect (paint into 64x32 tile at 10,20):
    img.x = 10; img.y = 20; img.w = 64; img.h = 32;
    // Source rect (slice the (32,0)→(64,32) tile out of the sprite strip):
    img.x2 = 32; img.y2 = 0; img.x3 = 32; img.y3 = 32;
    img.has_source_rect = true;
    cw.add_command(img);

    RecordingCanvas rec;
    cw.paint(rec);

    // Find the recorded draw_image command. RecordingCanvas's
    // draw_image_from_file_rect override stashes the src rect in
    // `floats[0..3]` — the dst-only draw_image_from_file leaves it empty.
    int drawIndex = -1;
    int idx = 0;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == DrawCommand::Type::draw_image) {
            drawIndex = idx;
            REQUIRE(cmd.text == "/sprites/walk.png");
            // dst rect in f[0..3].
            REQUIRE(cmd.f[0] == Catch::Approx(10.0f));
            REQUIRE(cmd.f[1] == Catch::Approx(20.0f));
            REQUIRE(cmd.f[2] == Catch::Approx(64.0f));
            REQUIRE(cmd.f[3] == Catch::Approx(32.0f));
            // src rect in floats[0..3] — proves the _rect overload fired.
            REQUIRE(cmd.floats.size() == 4);
            REQUIRE(cmd.floats[0] == Catch::Approx(32.0f));
            REQUIRE(cmd.floats[1] == Catch::Approx(0.0f));
            REQUIRE(cmd.floats[2] == Catch::Approx(32.0f));
            REQUIRE(cmd.floats[3] == Catch::Approx(32.0f));
        }
        ++idx;
    }
    REQUIRE(drawIndex >= 0);
}

// 5-arg form fallback: `has_source_rect=false` must route through the
// dst-only draw_image_from_file path. RecordingCanvas leaves the
// `floats` payload empty in that case, which is how the renderer (Skia
// or CG) distinguishes the two forms downstream. This pins the else
// branch of canvas_widget.cpp's has_source_rect check.
TEST_CASE("CanvasWidget::paint routes draw_image through dst-only path when has_source_rect is false",
          "[canvas_widget][issue-1739][canvas2d][coverage]") {
    CanvasWidget cw;
    cw.set_bounds({0, 0, 200, 100});

    CanvasDrawCmd img;
    img.type            = CanvasDrawCmd::Type::draw_image;
    img.text            = "/icons/play.png";
    img.x = 5; img.y = 5; img.w = 16; img.h = 16;
    // x2/y2/x3/y3 stay at zero; flag stays false — dst-only path.
    img.has_source_rect = false;
    cw.add_command(img);

    RecordingCanvas rec;
    cw.paint(rec);

    int drawIndex = -1;
    int idx = 0;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == DrawCommand::Type::draw_image) {
            drawIndex = idx;
            REQUIRE(cmd.text == "/icons/play.png");
            REQUIRE(cmd.f[0] == Catch::Approx(5.0f));
            REQUIRE(cmd.f[1] == Catch::Approx(5.0f));
            REQUIRE(cmd.f[2] == Catch::Approx(16.0f));
            REQUIRE(cmd.f[3] == Catch::Approx(16.0f));
            // dst-only path leaves `floats` untouched — RecordingCanvas's
            // draw_image_from_file override doesn't populate it.
            REQUIRE(cmd.floats.empty());
        }
        ++idx;
    }
    REQUIRE(drawIndex >= 0);
}

// Empty-path + has_source_rect=true: canvas_widget's draw_image case
// guards the `_rect` call behind `!src.empty()`, so an empty path falls
// through to the labeled placeholder fallback. This pins the empty-src
// early-exit edge of the new branch.
TEST_CASE("CanvasWidget::paint draws placeholder when image path is empty even with source rect",
          "[canvas_widget][issue-1739][canvas2d][coverage]") {
    CanvasWidget cw;
    cw.set_bounds({0, 0, 200, 100});

    CanvasDrawCmd img;
    img.type            = CanvasDrawCmd::Type::draw_image;
    img.text            = "";  // empty path — skip the file-decode branch
    img.x = 10; img.y = 10; img.w = 32; img.h = 32;
    img.x2 = 1; img.y2 = 2; img.x3 = 3; img.y3 = 4;
    img.has_source_rect = true;
    cw.add_command(img);

    RecordingCanvas rec;
    cw.paint(rec);

    // No draw_image command should land because the empty-path branch
    // doesn't call canvas.draw_image_from_file_rect — instead the
    // placeholder fill_rect / fill_text path fires.
    bool saw_draw_image = false;
    bool saw_placeholder_fill = false;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == DrawCommand::Type::draw_image) saw_draw_image = true;
        if (cmd.type == DrawCommand::Type::fill_rect) saw_placeholder_fill = true;
    }
    REQUIRE_FALSE(saw_draw_image);
    REQUIRE(saw_placeholder_fill);
}
