// Automated test for CanvasWidget (JS-driven custom drawing)
#include <catch2/catch_test_macros.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>

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

namespace {
struct Pixel {
    uint8_t r, g, b, a;
};

// Sample a single RGBA8 pixel (premul) from a Skia raster surface so
// tests can assert on the actual texels CanvasWidget produced.
Pixel sample_pixel(SkSurface* surface, int x, int y) {
    SkPixmap pix;
    REQUIRE(surface->peekPixels(&pix));
    auto* row = static_cast<const uint8_t*>(pix.addr(0, y));
    return {row[4 * x + 0], row[4 * x + 1], row[4 * x + 2], row[4 * x + 3]};
}
}  // namespace

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

TEST_CASE("CanvasWidget clearRect actually clears Skia pixels",
          "[canvas_widget][skia][issue-929]") {
    // Pre-fill the surface with opaque white (simulates a parent's paint
    // pass having already drawn the underlying area). A subsequent
    // clearRect issued through CanvasWidget must replace those pixels
    // with transparent black, not SrcOver-blend a transparent fill.
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

    auto px = sample_pixel(surface.get(), 16, 16);
    INFO("Cleared pixel rgba=("
         << int(px.r) << "," << int(px.g) << ","
         << int(px.b) << "," << int(px.a) << ")");
    REQUIRE(px.a == 0);
    REQUIRE(px.r == 0);
    REQUIRE(px.g == 0);
    REQUIRE(px.b == 0);
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
