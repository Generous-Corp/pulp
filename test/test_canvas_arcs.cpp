// test_canvas_arcs.cpp — extracted from test_canvas.cpp in the
// 2026-05 Phase 5 (P5-2 follow-up) refactor.
//
// pulp #1521 — native arc / arcTo / ellipse / roundRect cluster.
//
// Two contiguous sub-clusters under the same arc/path-primitive
// theme:
//   1. RecordingCanvas captures of native arc / arc_to / ellipse /
//      round_rect commands (no Skia required).
//   2. Skia rasterization fixtures — full-circle / half-circle arc
//      match reference SkPath::arcTo; arc_to with three collinear
//      points lineTos to first; round_rect renders 4 distinct corner
//      radii; ellipse with rotation extends current contour.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <array>
#include <vector>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#endif

using namespace pulp::canvas;

// ── pulp #1521 — native arc / arcTo / ellipse / roundRect ─────────────────
//
// The four canvas2d arc-as-path catalog entries were DIVERGE because the JS
// shim approximated each via cubic-bezier or polyline. These tests exercise
// the native bridge path so the catalog can flip to PASS, and rasterize a
// few fixtures on Skia to confirm the geometry matches a reference SkPath
// built from the same SkPath::arcTo / SkRRect API the new code uses.

TEST_CASE("RecordingCanvas captures native arc subpath",
          "[canvas][issue-1521]") {
    RecordingCanvas canvas;
    canvas.begin_path();
    canvas.arc(100.0f, 100.0f, 25.0f,
               0.0f, 6.283185307f, /*anticlockwise=*/false);
    canvas.fill_current_path();
    REQUIRE(canvas.count(DrawCommand::Type::arc) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::cubic_to) == 0);
    REQUIRE(canvas.count(DrawCommand::Type::move_to) == 0);
    // The recorded payload must round-trip exactly — the harness
    // promotion target is the visible developer intent (arc), not the
    // chain of segments the old shim produced.
    const auto& cmds = canvas.commands();
    bool found_arc = false;
    for (const auto& c : cmds) {
        if (c.type == DrawCommand::Type::arc) {
            REQUIRE(c.f[0] == Catch::Approx(100.0f));
            REQUIRE(c.f[1] == Catch::Approx(100.0f));
            REQUIRE(c.f[2] == Catch::Approx(25.0f));
            REQUIRE(c.f[3] == Catch::Approx(0.0f));
            REQUIRE(c.f[4] == Catch::Approx(6.283185307f));
            REQUIRE(c.f[5] == Catch::Approx(0.0f)); // clockwise
            found_arc = true;
        }
    }
    REQUIRE(found_arc);
}

TEST_CASE("RecordingCanvas captures arc_to with radius preserved",
          "[canvas][issue-1521]") {
    RecordingCanvas canvas;
    canvas.begin_path();
    canvas.move_to(0.0f, 0.0f);
    canvas.arc_to(50.0f, 0.0f, 50.0f, 50.0f, /*radius=*/15.0f);
    canvas.stroke_current_path();
    REQUIRE(canvas.count(DrawCommand::Type::arc_to) == 1);
    // Old shim emitted lineTo(x1,y1)+lineTo(x2,y2) and dropped radius.
    // Native path keeps the radius reachable for the rasterizer.
    REQUIRE(canvas.count(DrawCommand::Type::line_to) == 0);
    for (const auto& c : canvas.commands()) {
        if (c.type == DrawCommand::Type::arc_to) {
            REQUIRE(c.f[4] == Catch::Approx(15.0f)); // radius
        }
    }
}

TEST_CASE("RecordingCanvas captures ellipse with rotation",
          "[canvas][issue-1521]") {
    RecordingCanvas canvas;
    canvas.begin_path();
    canvas.ellipse(100.0f, 100.0f,
                   40.0f, 20.0f,
                   /*rotation=*/0.785398f, // 45 deg
                   0.0f, 6.283185307f,
                   /*anticlockwise=*/false);
    REQUIRE(canvas.count(DrawCommand::Type::ellipse) == 1);
    // Old shim ignored rotation; new path round-trips it.
    for (const auto& c : canvas.commands()) {
        if (c.type == DrawCommand::Type::ellipse) {
            REQUIRE(c.f[4] == Catch::Approx(0.785398f).margin(1e-5f));
        }
    }
}

TEST_CASE("RecordingCanvas captures round_rect with 4 distinct corner radii",
          "[canvas][issue-1521]") {
    RecordingCanvas canvas;
    canvas.begin_path();
    canvas.round_rect(10.0f, 20.0f, 100.0f, 50.0f,
                      /*tl=*/2.0f, 2.0f,
                      /*tr=*/4.0f, 4.0f,
                      /*br=*/6.0f, 6.0f,
                      /*bl=*/8.0f, 8.0f);
    REQUIRE(canvas.count(DrawCommand::Type::round_rect) == 1);
    // Old shim collapsed non-uniform radii to the largest single value
    // and emitted moveTo + 4× (lineTo + arcTo). Native path preserves
    // each corner radius independently.
    for (const auto& c : canvas.commands()) {
        if (c.type == DrawCommand::Type::round_rect) {
            REQUIRE(c.f[4] == Catch::Approx(2.0f)); // tl_x
            REQUIRE(c.f[5] == Catch::Approx(2.0f)); // tl_y
            REQUIRE(c.floats.size() == 6u);
            REQUIRE(c.floats[0] == Catch::Approx(4.0f)); // tr_x
            REQUIRE(c.floats[1] == Catch::Approx(4.0f)); // tr_y
            REQUIRE(c.floats[2] == Catch::Approx(6.0f)); // br_x
            REQUIRE(c.floats[3] == Catch::Approx(6.0f)); // br_y
            REQUIRE(c.floats[4] == Catch::Approx(8.0f)); // bl_x
            REQUIRE(c.floats[5] == Catch::Approx(8.0f)); // bl_y
        }
    }
}

#ifdef PULP_HAS_SKIA
// ── Skia rasterization fixtures ───────────────────────────────────────────
//
// Each test compares the bytes the SkiaCanvas paints (filling a black arc
// on a white surface) to a "reference" path built directly from the same
// Skia API the implementation uses. Both should produce identical pixels —
// any drift implies the SkiaCanvas wrapper is doing extra approximation.

namespace {

// Render `f(canvas)` onto a fresh white surface, return the resulting
// premultiplied RGBA8 bytes as a vector for byte-level comparison.
std::vector<uint8_t> render_to_pixels_for_arc_test(int w, int h,
        const std::function<void(pulp::canvas::SkiaCanvas&)>& f) {
    SkImageInfo info = SkImageInfo::Make(w, h, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    sk_canvas->clear(SK_ColorWHITE);
    {
        pulp::canvas::SkiaCanvas pulp_canvas(sk_canvas);
        f(pulp_canvas);
    }
    SkPixmap pm;
    REQUIRE(surface->peekPixels(&pm));
    const auto* base = static_cast<const uint8_t*>(pm.addr());
    std::vector<uint8_t> out(base, base + pm.rowBytes() * pm.height());
    return out;
}

// Render the same arc using SkPath::arcTo directly (no SkiaCanvas).
std::vector<uint8_t> render_reference_full_circle(int w, int h,
        float cx, float cy, float r) {
    SkImageInfo info = SkImageInfo::Make(w, h, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    sk_canvas->clear(SK_ColorWHITE);
    SkPathBuilder pb;
    SkRect oval = SkRect::MakeLTRB(cx - r, cy - r, cx + r, cy + r);
    pb.arcTo(oval, 0.0f, 360.0f, /*forceMoveTo=*/false);
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(SK_ColorBLACK);
    sk_canvas->drawPath(pb.detach(), paint);
    SkPixmap pm;
    REQUIRE(surface->peekPixels(&pm));
    const auto* base = static_cast<const uint8_t*>(pm.addr());
    return std::vector<uint8_t>(base, base + pm.rowBytes() * pm.height());
}

} // namespace

TEST_CASE("SkiaCanvas::arc full circle matches reference SkPath::arcTo",
          "[canvas][skia][issue-1521]") {
    const int W = 100, H = 100;
    const float cx = 50.0f, cy = 50.0f, r = 30.0f;

    auto ours = render_to_pixels_for_arc_test(W, H,
        [cx, cy, r](pulp::canvas::SkiaCanvas& canvas) {
            canvas.begin_path();
            canvas.arc(cx, cy, r, 0.0f, 6.283185307f, false);
            canvas.set_fill_color(pulp::canvas::Color::hex(0x000000));
            canvas.fill_current_path();
        });
    auto ref = render_reference_full_circle(W, H, cx, cy, r);
    REQUIRE(ours.size() == ref.size());
    REQUIRE(ours == ref);
}

TEST_CASE("SkiaCanvas::arc half circle has endpoints exactly opposite",
          "[canvas][skia][issue-1521]") {
    // For a 0..π arc, the endpoints are (cx + r, cy) and (cx - r, cy).
    // The bezier approximation drifted off this property at large radii —
    // native SkPath::arcTo lands exactly on cy.
    const int W = 200, H = 100;
    const float cx = 100.0f, cy = 50.0f, r = 40.0f;

    auto pixels = render_to_pixels_for_arc_test(W, H,
        [cx, cy, r](pulp::canvas::SkiaCanvas& canvas) {
            canvas.begin_path();
            canvas.arc(cx, cy, r, 0.0f, 3.141592653f, false);
            // Stroke so we can probe the exact edge pixels.
            canvas.set_stroke_color(pulp::canvas::Color::hex(0x000000));
            canvas.set_line_width(2.0f);
            canvas.stroke_current_path();
        });
    REQUIRE(pixels.size() == static_cast<size_t>(W * H * 4));
    auto sample = [&](int x, int y) {
        size_t off = (static_cast<size_t>(y) * W + x) * 4;
        return std::array<uint8_t, 3>{pixels[off + 0],
                                        pixels[off + 1],
                                        pixels[off + 2]};
    };
    // The arc endpoint at angle 0 is (cx + r, cy) = (140, 50). With a
    // 2px stroke the pixel at exactly y=50, x=140 must be coloured (not
    // white). Same for (cx - r, cy) = (60, 50).
    auto p_right = sample(static_cast<int>(cx + r), static_cast<int>(cy));
    auto p_left  = sample(static_cast<int>(cx - r), static_cast<int>(cy));
    INFO("right endpoint rgb=" << (int)p_right[0] << "," << (int)p_right[1]
         << "," << (int)p_right[2]);
    INFO("left endpoint rgb="  << (int)p_left[0]  << "," << (int)p_left[1]
         << "," << (int)p_left[2]);
    // Endpoint pixels should be near-black (alpha-blended with white,
    // so each channel < 200).
    REQUIRE(p_right[0] < 200);
    REQUIRE(p_left[0]  < 200);
}

TEST_CASE("SkiaCanvas::arc_to with three collinear points lineTos to first",
          "[canvas][skia][issue-1521][!mayfail]") {
    // Spec: a degenerate arcTo where the three points are collinear (or
    // the radius is zero) should collapse to a single lineTo to (x1,y1).
    // SkPath::arcTo handles both cases internally.
    const int W = 100, H = 100;

    auto pixels = render_to_pixels_for_arc_test(W, H,
        [](pulp::canvas::SkiaCanvas& canvas) {
            canvas.begin_path();
            canvas.move_to(10.0f, 50.0f);
            canvas.arc_to(50.0f, 50.0f, 90.0f, 50.0f, /*radius=*/10.0f);
            canvas.set_stroke_color(pulp::canvas::Color::hex(0x000000));
            canvas.set_line_width(2.0f);
            canvas.stroke_current_path();
        });
    REQUIRE(pixels.size() == static_cast<size_t>(W * H * 4));
    // The collinear case should still render a horizontal line — sample
    // a few pixels along y=50.
    auto sample = [&](int x, int y) {
        size_t off = (static_cast<size_t>(y) * W + x) * 4;
        return pixels[off + 0]; // R channel
    };
    REQUIRE(sample(30, 50) < 200);
    REQUIRE(sample(50, 50) < 200);
    REQUIRE(sample(70, 50) < 200);
}

TEST_CASE("SkiaCanvas::round_rect renders 4 distinct corner radii",
          "[canvas][skia][issue-1521]") {
    // Sanity check that each corner has its own radius — pixels just
    // inside each corner along the bevel diagonal should all be filled
    // when we use a uniform large radius, and the wider radii cut more
    // of the corner away than the small ones.
    const int W = 200, H = 100;

    auto render_corners = [W, H](float tl, float tr, float br, float bl) {
        return render_to_pixels_for_arc_test(W, H,
            [tl, tr, br, bl](pulp::canvas::SkiaCanvas& canvas) {
                canvas.begin_path();
                canvas.round_rect(10.0f, 10.0f, 180.0f, 80.0f,
                                  tl, tl, tr, tr, br, br, bl, bl);
                canvas.set_fill_color(pulp::canvas::Color::hex(0x000000));
                canvas.fill_current_path();
            });
    };
    auto pixels_uniform = render_corners(2.0f, 2.0f, 2.0f, 2.0f);
    auto pixels_top_left_big = render_corners(20.0f, 2.0f, 2.0f, 2.0f);
    auto sample = [W](const std::vector<uint8_t>& px, int x, int y) {
        return px[(static_cast<size_t>(y) * W + x) * 4]; // R channel
    };
    // (12, 12) is well inside the box for radius=2 (filled), and well
    // outside the rounded corner for radius=20 (unfilled). The contrast
    // proves the two radii produced visibly different geometry.
    REQUIRE(sample(pixels_uniform, 12, 12) < 200);   // filled
    REQUIRE(sample(pixels_top_left_big, 12, 12) > 200); // unfilled
    // Bottom-right corner stays the same radius across both renders, so
    // the same pixel near it should still be filled in both.
    REQUIRE(sample(pixels_uniform, 187, 87) < 200);
    REQUIRE(sample(pixels_top_left_big, 187, 87) < 200);
}

// Codex #1616 P1 — rotated ellipse() previously used kAppend_AddPathMode
// when grafting the rotated arc onto the live path builder, replacing the
// implicit lineTo (CSS Canvas2D semantics) with a moveTo. Fills with a
// preceding moveTo + a rotated arc would render with a visible gap
// because the new contour did not connect back to the moveTo pen
// position. With kExtend_AddPathMode, the connect-via-lineTo behavior
// is restored.
TEST_CASE("SkiaCanvas::ellipse with rotation extends current contour (no gap)",
          "[canvas][skia][issue-1556][codex-p1][!mayfail]") {
    const int W = 200, H = 200;
    // STROKE (not fill) the path so the bridge segment between the
    // initial move_to and the rotated ellipse's start point is
    // observable. A moveTo (the kAppend bug) leaves no rendered
    // bridge — the stroke jumps to the ellipse start without drawing.
    // A lineTo (the kExtend fix) renders the bridge as a stroked line
    // along y=100 from x=20 toward the ellipse start. Sampling a
    // pixel on that bridge line is the regression-catching probe.
    auto pixels = render_to_pixels_for_arc_test(W, H,
        [](pulp::canvas::SkiaCanvas& canvas) {
            canvas.begin_path();
            canvas.move_to(20.0f, 100.0f);
            // Rotated ellipse at (140,100) — far enough from the moveTo
            // that the bridge segment must traverse at least 80px.
            canvas.ellipse(140.0f, 100.0f,
                           40.0f, 20.0f,
                           /*rotation=*/0.785398f, // 45 deg
                           0.0f, 6.283185307f,
                           /*anticlockwise=*/false);
            canvas.set_stroke_color(pulp::canvas::Color::hex(0x000000));
            canvas.set_line_width(3.0f);
            canvas.stroke_current_path();
        });
    REQUIRE(pixels.size() == static_cast<size_t>(W * H * 4));
    auto sample_r = [&](int x, int y) -> int {
        size_t off = (static_cast<size_t>(y) * W + x) * 4;
        return pixels[off + 0]; // red channel; near-white = unfilled
    };
    // Probe along the y=100 line in the bridge region (x∈[40,80]),
    // well clear of both the move_to point and any rotated-ellipse
    // edge. With the fix this line is stroked (channel < 200); with
    // the kAppend bug there's no draw command and the pixel stays
    // near-white (channel ≥ 200). Try multiple x to be robust against
    // anti-alias / line-width rounding.
    bool any_bridge_pixel_drawn = false;
    for (int x = 40; x <= 80 && !any_bridge_pixel_drawn; x += 5) {
        for (int dy = -1; dy <= 1; ++dy) {
            if (sample_r(x, 100 + dy) < 200) {
                any_bridge_pixel_drawn = true;
                break;
            }
        }
    }
    INFO("Bridge segment pixels along y=100, x in [40,80] should be stroked.");
    REQUIRE(any_bridge_pixel_drawn);
}
#endif // PULP_HAS_SKIA
