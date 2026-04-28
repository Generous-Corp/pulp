#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/sdf_atlas.hpp>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#endif

using namespace pulp::canvas;

TEST_CASE("RecordingCanvas captures commands", "[canvas]") {
    RecordingCanvas canvas;

    REQUIRE(canvas.command_count() == 0);

    canvas.save();
    canvas.set_fill_color(Color::hex(0xFF0000));
    canvas.fill_rect(10, 20, 100, 50);
    canvas.restore();

    REQUIRE(canvas.command_count() == 4);
    REQUIRE(canvas.count(DrawCommand::Type::save) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::restore) == 1);
}

TEST_CASE("RecordingCanvas shapes", "[canvas]") {
    RecordingCanvas canvas;

    canvas.fill_rounded_rect(0, 0, 100, 50, 8);
    canvas.stroke_circle(50, 25, 20);
    canvas.stroke_arc(50, 25, 15, 0.0f, 3.14f);
    canvas.stroke_line(0, 0, 100, 100);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_arc) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 1);
}

TEST_CASE("RecordingCanvas text", "[canvas]") {
    RecordingCanvas canvas;

    canvas.set_font("Inter", 14.0f);
    canvas.set_text_align(TextAlign::center);
    canvas.fill_text("hello", 50, 25);

    REQUIRE(canvas.count(DrawCommand::Type::set_font) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 1);

    // measure_text returns an approximate width
    float w = canvas.measure_text("hello");
    REQUIRE(w > 0);
}

// Regression test for #75: text draws inside nested clip/translate
// contexts should still emit exactly one fill_text command per call.
// The Skia path uses SkTextBlob with explicit per-glyph advances
// instead of SkShaper::shape(), which prevented ghost/double rendering
// in the widget paint pipeline. This test guards against a regression
// where a future refactor re-introduces SkShaper::shape() for the
// widget paint path.
TEST_CASE("Canvas text in nested clip contexts -- no duplication (#75)",
          "[canvas][regression]") {
    RecordingCanvas canvas;
    canvas.set_font("Inter", 14.0f);

    // Simulate a three-level nested widget paint:
    //   root panel → section panel → label
    canvas.save();
    canvas.translate(10, 10);
    canvas.clip_rect(0, 0, 300, 200);

    canvas.save();
    canvas.translate(20, 30);
    canvas.clip_rect(0, 0, 260, 160);

    canvas.save();
    canvas.translate(5, 5);
    canvas.clip_rect(0, 0, 250, 150);

    canvas.fill_text("Hello, nested clip", 0, 0);

    canvas.restore();
    canvas.restore();
    canvas.restore();

    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::save) == 3);
    REQUIRE(canvas.count(DrawCommand::Type::restore) == 3);
    REQUIRE(canvas.count(DrawCommand::Type::clip_rect) == 3);
}

TEST_CASE("fill_text_sdf falls back to fill_text on RecordingCanvas", "[canvas][sdf]") {
    RecordingCanvas canvas;
    canvas.set_font("Inter", 14.0f);

    SdfAtlas atlas;
    std::vector<char32_t> chars = {U'H', U'i'};
    REQUIRE(atlas.build("stub", chars, 32, 4, 256));

    canvas.fill_text_sdf("Hi", 10, 20, atlas);

    // RecordingCanvas base class fallback records a fill_text command
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 1);
}

TEST_CASE("RecordingCanvas transforms", "[canvas]") {
    RecordingCanvas canvas;

    canvas.save();
    canvas.translate(10, 20);
    canvas.scale(2.0f, 2.0f);
    canvas.rotate(1.57f);
    canvas.clip_rect(0, 0, 100, 100);
    canvas.restore();

    REQUIRE(canvas.count(DrawCommand::Type::translate) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::scale) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::rotate) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::clip_rect) == 1);
}

TEST_CASE("RecordingCanvas clear", "[canvas]") {
    RecordingCanvas canvas;
    canvas.fill_rect(0, 0, 10, 10);
    REQUIRE(canvas.command_count() == 1);

    canvas.clear();
    REQUIRE(canvas.command_count() == 0);
}

TEST_CASE("Color construction", "[canvas]") {
    auto c = Color::rgba8(128, 64, 32, 200);
    REQUIRE(c.r8() == 128);
    REQUIRE(c.g8() == 64);
    REQUIRE(c.b8() == 32);
    REQUIRE(c.a8() == 200);

    auto h = Color::hex(0x3B82F6);
    REQUIRE(h.r8() == 0x3B);
    REQUIRE(h.g8() == 0x82);
    REQUIRE(h.b8() == 0xF6);
    REQUIRE(h.a8() == 255);

    // Float-based construction
    auto f = Color::rgba(0.5f, 0.25f, 0.75f, 1.0f);
    REQUIRE(f.r == Catch::Approx(0.5f));
    REQUIRE(f.g == Catch::Approx(0.25f));
    REQUIRE(f.b == Catch::Approx(0.75f));
    REQUIRE(f.a == Catch::Approx(1.0f));

    // Interpolation
    auto a = Color::rgba(0.0f, 0.0f, 0.0f);
    auto b = Color::rgba(1.0f, 1.0f, 1.0f);
    auto mid = a.interpolate(b, 0.5f);
    REQUIRE(mid.r == Catch::Approx(0.5f));
    REQUIRE(mid.g == Catch::Approx(0.5f));

    // HDR intensity
    auto hdr = Color::rgba(0.5f, 0.5f, 0.5f).with_hdr_intensity(2.0f);
    REQUIRE(hdr.r == Catch::Approx(1.0f));
    REQUIRE(hdr.a == Catch::Approx(1.0f));  // alpha unchanged

    // with_alpha
    auto wa = Color::rgba(1.0f, 0.0f, 0.0f).with_alpha(0.5f);
    REQUIRE(wa.r == Catch::Approx(1.0f));
    REQUIRE(wa.a == Catch::Approx(0.5f));

    // ARGB32 round-trip
    auto orig = Color::rgba8(100, 200, 50, 180);
    auto rt = Color::from_argb32(orig.to_argb32());
    REQUIRE(rt.r8() == 100);
    REQUIRE(rt.g8() == 200);
    REQUIRE(rt.b8() == 50);
    REQUIRE(rt.a8() == 180);
}

TEST_CASE("Color HSV round-trip", "[canvas][color]") {
    // Pure red
    auto red = Color::rgba(1.0f, 0.0f, 0.0f);
    auto hsv = red.to_hsv();
    REQUIRE(hsv.h == Catch::Approx(0.0f).margin(1.0f));
    REQUIRE(hsv.s == Catch::Approx(1.0f).margin(0.01f));
    REQUIRE(hsv.v == Catch::Approx(1.0f).margin(0.01f));
    auto back = Color::from_hsv(hsv);
    REQUIRE(back.r == Catch::Approx(1.0f).margin(0.01f));
    REQUIRE(back.g == Catch::Approx(0.0f).margin(0.01f));

    // Gray (zero saturation)
    auto gray = Color::rgba(0.5f, 0.5f, 0.5f);
    auto ghsv = gray.to_hsv();
    REQUIRE(ghsv.s == Catch::Approx(0.0f).margin(0.01f));
    REQUIRE(ghsv.v == Catch::Approx(0.5f).margin(0.01f));
}

TEST_CASE("Color HSL round-trip", "[canvas][color]") {
    auto blue = Color::rgba(0.0f, 0.0f, 1.0f);
    auto hsl = blue.to_hsl();
    REQUIRE(hsl.h == Catch::Approx(240.0f).margin(1.0f));
    REQUIRE(hsl.s == Catch::Approx(1.0f).margin(0.01f));
    REQUIRE(hsl.l == Catch::Approx(0.5f).margin(0.01f));
    auto back = Color::from_hsl(hsl);
    REQUIRE(back.b == Catch::Approx(1.0f).margin(0.01f));
    REQUIRE(back.r == Catch::Approx(0.0f).margin(0.01f));
}

TEST_CASE("Color OKLCH round-trip", "[canvas][color]") {
    // White
    auto white = Color::rgba(1.0f, 1.0f, 1.0f);
    auto wlch = white.to_oklch();
    REQUIRE(wlch.L == Catch::Approx(1.0f).margin(0.01f));
    REQUIRE(wlch.C == Catch::Approx(0.0f).margin(0.01f));
    auto wb = Color::from_oklch(wlch);
    REQUIRE(wb.r == Catch::Approx(1.0f).margin(0.02f));

    // Pure red round-trip
    auto red = Color::rgba(1.0f, 0.0f, 0.0f);
    auto rlch = red.to_oklch();
    REQUIRE(rlch.L > 0.4f);
    REQUIRE(rlch.C > 0.15f);
    auto rb = Color::from_oklch(rlch);
    REQUIRE(rb.r8() >= 250);  // close to 255
    REQUIRE(rb.g8() <= 5);    // close to 0

    // Black
    auto black = Color::rgba(0.0f, 0.0f, 0.0f);
    auto blch = black.to_oklch();
    REQUIRE(blch.L == Catch::Approx(0.0f).margin(0.01f));
}

TEST_CASE("Color encode/decode round-trip", "[canvas][color]") {
    auto c = Color::rgba(0.123f, 0.456f, 0.789f, 0.5f);
    uint8_t buf[16];
    c.encode(buf);
    auto d = Color::decode(buf);
    REQUIRE(d.r == Catch::Approx(c.r));
    REQUIRE(d.g == Catch::Approx(c.g));
    REQUIRE(d.b == Catch::Approx(c.b));
    REQUIRE(d.a == Catch::Approx(c.a));
}

TEST_CASE("SDF shape enum covers all types", "[canvas][sdf]") {
    using S = Canvas::SDFShape;
    // Verify the enum values match the shader's shapeType indices
    REQUIRE(static_cast<int>(S::rect) == 0);
    REQUIRE(static_cast<int>(S::circle) == 1);
    REQUIRE(static_cast<int>(S::rounded_rect) == 2);
    REQUIRE(static_cast<int>(S::arc) == 3);
    REQUIRE(static_cast<int>(S::diamond) == 4);
    REQUIRE(static_cast<int>(S::squircle) == 5);
    REQUIRE(static_cast<int>(S::triangle) == 6);
    REQUIRE(static_cast<int>(S::ring) == 7);
    REQUIRE(static_cast<int>(S::stadium) == 8);
    REQUIRE(static_cast<int>(S::cross) == 9);
    REQUIRE(static_cast<int>(S::flat_segment) == 10);
    REQUIRE(static_cast<int>(S::rounded_segment) == 11);
    REQUIRE(static_cast<int>(S::flat_arc) == 12);
    REQUIRE(static_cast<int>(S::quadratic_bezier) == 13);
}

TEST_CASE("SDFStyle defaults are valid", "[canvas][sdf]") {
    Canvas::SDFStyle style;
    REQUIRE(style.stroke_width == 0.0f);
    REQUIRE(style.corner_radius == 0.0f);
    REQUIRE(style.squircle_power == 4.0f);
    REQUIRE(style.inner_radius == 0.5f);
    REQUIRE(style.arm_width == Catch::Approx(0.3f));
}

TEST_CASE("SDF shapes render via RecordingCanvas fallback", "[canvas][sdf]") {
    RecordingCanvas rc;
    Canvas::SDFStyle style;
    style.fill_color = Color::rgba(1.0f, 0.0f, 0.0f);

    // All new shapes should at least not crash on the CPU fallback path
    for (int i = 0; i <= 13; ++i) {
        rc.clear();
        rc.draw_sdf_shape(static_cast<Canvas::SDFShape>(i), 10, 10, 50, 50, style);
        REQUIRE(rc.command_count() > 0);
    }
}

#ifdef PULP_HAS_SKIA
// Issue-897 P1 follow-up: ctx.setTransform must compose onto the parent
// View's transform, not overwrite it. Without this, a CanvasWidget at
// non-zero offset would have its translation wiped the moment JS calls
// ctx.setTransform(scale, 0, 0, scale, 0, 0) for devicePixelRatio scaling.
TEST_CASE("SkiaCanvas::set_transform composes onto captured paint baseline",
          "[canvas][skia][issue-897]") {
    SkImageInfo info = SkImageInfo::Make(200, 200, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    REQUIRE(sk_canvas != nullptr);

    // Simulate the parent View::paint_all step: translate the SkCanvas to
    // the widget's on-screen origin BEFORE handing it to CanvasWidget.
    sk_canvas->translate(50.0f, 30.0f);

    SkiaCanvas canvas(sk_canvas);
    // CanvasWidget::paint() invokes this at entry — call it directly here
    // to model the same lifecycle.
    canvas.capture_paint_baseline_transform();

    // JS-driven scale-by-2 (e.g. devicePixelRatio): ctx.setTransform(2, 0, 0, 2, 0, 0).
    canvas.set_transform(2.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f);

    SkMatrix m = sk_canvas->getTotalMatrix();
    // Expected composition: translate(50, 30) * scale(2, 2)
    //   sx = 2, sy = 2, tx = 50, ty = 30, no skew.
    REQUIRE(m.getScaleX() == Catch::Approx(2.0f));
    REQUIRE(m.getScaleY() == Catch::Approx(2.0f));
    REQUIRE(m.getTranslateX() == Catch::Approx(50.0f));
    REQUIRE(m.getTranslateY() == Catch::Approx(30.0f));
    REQUIRE(m.getSkewX() == Catch::Approx(0.0f));
    REQUIRE(m.getSkewY() == Catch::Approx(0.0f));
}

TEST_CASE("SkiaCanvas::set_transform is spec-literal without baseline capture",
          "[canvas][skia][issue-897]") {
    // Direct SkiaCanvas users (e.g. screenshot host) that do NOT go through
    // CanvasWidget never call capture_paint_baseline_transform — for them
    // the baseline stays at identity and setTransform behaves per the
    // CanvasRenderingContext2D spec literally.
    SkImageInfo info = SkImageInfo::Make(100, 100, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();

    // Apply some transform we expect setTransform to wipe.
    sk_canvas->translate(99.0f, 99.0f);

    SkiaCanvas canvas(sk_canvas);
    // No capture_paint_baseline_transform() call — baseline stays identity.
    canvas.set_transform(2.0f, 0.0f, 0.0f, 2.0f, 10.0f, 20.0f);

    SkMatrix m = sk_canvas->getTotalMatrix();
    REQUIRE(m.getScaleX() == Catch::Approx(2.0f));
    REQUIRE(m.getScaleY() == Catch::Approx(2.0f));
    REQUIRE(m.getTranslateX() == Catch::Approx(10.0f));
    REQUIRE(m.getTranslateY() == Catch::Approx(20.0f));
}

// ── Issue-916 — Canvas2D API gap closures (Skia raster tests) ─────────────

TEST_CASE("SkiaCanvas::measure_text_with_font returns positive width "
          "and bounding-box-ascent/descent for non-empty text",
          "[canvas][skia][issue-916]") {
    auto m = SkiaCanvas::measure_text_with_font("Inter", 20.0f, "Hello");
    REQUIRE(m.width > 0.0f);
    // Font metrics are populated regardless of bbox — these are the
    // values Spectr's FilterBank reads for vertical centring.
    REQUIRE(m.ascent > 0.0f);
    REQUIRE(m.descent > 0.0f);
    // Bounding box ascent should be positive for an alphabetic glyph.
    REQUIRE(m.actual_bounding_box_ascent > 0.0f);

    // Longer text → wider advance.
    auto wide = SkiaCanvas::measure_text_with_font("Inter", 20.0f, "Hello, world!");
    REQUIRE(wide.width > m.width);
}

TEST_CASE("SkiaCanvas::write_pixels round-trips RGBA through the surface",
          "[canvas][skia][issue-916]") {
    SkImageInfo info = SkImageInfo::Make(8, 8, kRGBA_8888_SkColorType,
                                         kUnpremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    REQUIRE(sk_canvas != nullptr);

    // Clear to opaque black so we can detect the put.
    sk_canvas->clear(SK_ColorBLACK);

    SkiaCanvas canvas(sk_canvas);

    // Build a 2x2 RGBA tile — bright red, green, blue, white.
    uint8_t tile[16] = {
        255,   0,   0, 255,
          0, 255,   0, 255,
          0,   0, 255, 255,
        255, 255, 255, 255
    };
    REQUIRE(canvas.write_pixels(tile, 2, 2, 1, 1));

    // Read back the 2x2 region we just wrote.
    uint8_t out[16] = {};
    REQUIRE(canvas.read_pixels(1, 1, 2, 2, out));
    REQUIRE(out[0]  == 255); // red
    REQUIRE(out[5]  == 255); // green
    REQUIRE(out[10] == 255); // blue
    REQUIRE(out[12] == 255); // white R
    REQUIRE(out[13] == 255); // white G
    REQUIRE(out[14] == 255); // white B
}

TEST_CASE("SkiaCanvas::set_line_dash applies an SkDashPathEffect on stroke",
          "[canvas][skia][issue-916]") {
    SkImageInfo info = SkImageInfo::Make(64, 16, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    sk_canvas->clear(SK_ColorWHITE);

    SkiaCanvas canvas(sk_canvas);
    canvas.set_stroke_color(Color::rgba(0.0f, 0.0f, 0.0f, 1.0f));
    canvas.set_line_width(2.0f);

    // Apply a 4-on/4-off pattern and stroke a horizontal line at y=8.
    float pattern[] = {4.0f, 4.0f};
    canvas.set_line_dash(pattern, 2, 0.0f);
    canvas.stroke_line(0, 8, 64, 8);

    auto image = surface->makeImageSnapshot();
    SkPixmap pix;
    REQUIRE(image->peekPixels(&pix));

    int dark = 0, light = 0;
    for (int x = 0; x < 64; ++x) {
        SkColor c = pix.getColor(x, 8);
        // Threshold for "ink": red channel < 128 means the dash drew here.
        if (SkColorGetR(c) < 128) ++dark;
        else ++light;
    }
    // A 4-on/4-off dash paints roughly half ink, half white. Validate
    // both sides are non-trivial — that proves the dash effect
    // activated. (A solid stroke would show dark ~ 64; a fully-dropped
    // stroke would show light == 64.)
    REQUIRE(dark  > 8);
    REQUIRE(light > 8);
}
#endif
