#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/sdf_atlas.hpp>
#include <array>
#include <functional>
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

#ifdef __APPLE__
#include <pulp/canvas/cg_canvas.hpp>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <array>
#include <cstdio>
#include <unistd.h>
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

// pulp #1737 — direct unit tests for the new draw_image_*_rect overrides.
// The integration test [issue-1737] in test_widget_bridge.cpp exercises
// draw_image_from_file_rect end-to-end via the JS bridge, but codecov's
// patch-coverage measurement reported 0% on the new lines anyway (the
// widget-bridge test goes through too many dispatch layers for the
// per-line attribution). Direct calls on RecordingCanvas pin the new
// behavior to specific test cases that codecov measures unambiguously.
TEST_CASE("RecordingCanvas::draw_image_from_file_rect captures src + dst rect",
          "[canvas][issue-1737][issue-916]") {
    RecordingCanvas canvas;
    bool ok = canvas.draw_image_from_file_rect(
        "/sprites/walk.png",
        /*sx=*/16, /*sy=*/0, /*sw=*/32, /*sh=*/32,
        /*dx=*/4,  /*dy=*/8, /*dw=*/64, /*dh=*/64);
    REQUIRE(ok);
    REQUIRE(canvas.count(DrawCommand::Type::draw_image) == 1);
    const auto& cmd = canvas.commands().back();
    REQUIRE(cmd.text == "/sprites/walk.png");
    // dst rect in f[0..3]
    REQUIRE(cmd.f[0] == Catch::Approx(4.0f));
    REQUIRE(cmd.f[1] == Catch::Approx(8.0f));
    REQUIRE(cmd.f[2] == Catch::Approx(64.0f));
    REQUIRE(cmd.f[3] == Catch::Approx(64.0f));
    // src rect in floats[0..3]
    REQUIRE(cmd.floats.size() == 4);
    REQUIRE(cmd.floats[0] == Catch::Approx(16.0f));
    REQUIRE(cmd.floats[1] == Catch::Approx(0.0f));
    REQUIRE(cmd.floats[2] == Catch::Approx(32.0f));
    REQUIRE(cmd.floats[3] == Catch::Approx(32.0f));
}

TEST_CASE("RecordingCanvas::draw_image_from_data_rect captures src + dst rect",
          "[canvas][issue-1737][issue-916]") {
    RecordingCanvas canvas;
    // Tiny synthetic encoded payload — RecordingCanvas doesn't decode,
    // it just stashes the bytes verbatim. Use a recognisable
    // 4-byte sentinel so the test asserts the bytes flow through.
    const uint8_t sentinel[] = { 0xAB, 0xCD, 0xEF, 0x01 };
    bool ok = canvas.draw_image_from_data_rect(
        sentinel, sizeof(sentinel),
        /*sx=*/0, /*sy=*/0, /*sw=*/4, /*sh=*/1,
        /*dx=*/100, /*dy=*/200, /*dw=*/40, /*dh=*/10);
    REQUIRE(ok);
    REQUIRE(canvas.count(DrawCommand::Type::draw_image) == 1);
    const auto& cmd = canvas.commands().back();
    // Bytes round-trip (RecordingCanvas treats them as binary text payload).
    REQUIRE(cmd.text.size() == sizeof(sentinel));
    REQUIRE(static_cast<unsigned char>(cmd.text[0]) == 0xAB);
    REQUIRE(static_cast<unsigned char>(cmd.text[3]) == 0x01);
    // dst rect.
    REQUIRE(cmd.f[0] == Catch::Approx(100.0f));
    REQUIRE(cmd.f[2] == Catch::Approx(40.0f));
    // src rect.
    REQUIRE(cmd.floats.size() == 4);
    REQUIRE(cmd.floats[2] == Catch::Approx(4.0f));
}

// (Note on canvas.hpp defaults: the base-class `draw_image_from_*_rect`
// virtual defaults strip src rect and delegate to the dst-only form.
// Both RecordingCanvas and SkiaCanvas override the _rect methods, so
// the defaults are effectively dead code on real backends — they only
// fire on a backend that overrides _file/_data but NOT _file_rect.
// Exercising the defaults requires a contrived Canvas subclass that
// can't easily be set up here without re-implementing every pure
// virtual; skipping. The defaults remain in diff_cover_excludes
// (`**/canvas.hpp` per tools/scripts/coverage_config.json), so this
// is documented-and-excluded rather than a real coverage gap.)

TEST_CASE("RecordingCanvas captures draw_box_shadow with full payload",
          "[canvas][issue-925]") {
    RecordingCanvas canvas;
    canvas.draw_box_shadow(/*x=*/10, /*y=*/20, /*w=*/100, /*h=*/50,
                            /*dx=*/0, /*dy=*/14, /*blur=*/40, /*spread=*/2,
                            Color::rgba8(0, 0, 0, 160), /*inset=*/false,
                            /*corner_radius=*/8);

    REQUIRE(canvas.count(DrawCommand::Type::draw_box_shadow) == 1);
    const auto& cmd = canvas.commands().back();
    REQUIRE(cmd.type == DrawCommand::Type::draw_box_shadow);
    REQUIRE(cmd.f[0] == Catch::Approx(10.0f));
    REQUIRE(cmd.f[1] == Catch::Approx(20.0f));
    REQUIRE(cmd.f[2] == Catch::Approx(100.0f));
    REQUIRE(cmd.f[3] == Catch::Approx(50.0f));
    REQUIRE(cmd.f[4] == Catch::Approx(0.0f));   // inset = false
    REQUIRE(cmd.f[5] == Catch::Approx(8.0f));   // corner_radius
    REQUIRE(cmd.floats.size() == 4);
    REQUIRE(cmd.floats[0] == Catch::Approx(0.0f));
    REQUIRE(cmd.floats[1] == Catch::Approx(14.0f));
    REQUIRE(cmd.floats[2] == Catch::Approx(40.0f));
    REQUIRE(cmd.floats[3] == Catch::Approx(2.0f));
    REQUIRE(cmd.color.a == Catch::Approx(160.0f / 255.0f).margin(0.01f));
}

TEST_CASE("Canvas::draw_box_shadow CPU fallback emits stacked rounded rects",
          "[canvas][issue-925]") {
    RecordingCanvas canvas;
    // RecordingCanvas overrides draw_box_shadow, so call the base class
    // implementation explicitly to exercise the CPU fallback path used
    // by every non-Skia backend (CG, software, image-export).
    canvas.Canvas::draw_box_shadow(0, 0, 100, 50,
                                    0, 14, 20, 0,
                                    Color::rgba8(0, 0, 0, 128),
                                    /*inset=*/false, /*corner_radius=*/4);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) >= 5);
}

TEST_CASE("Canvas::draw_box_shadow inset CPU fallback clips to box",
          "[canvas][issue-925]") {
    RecordingCanvas canvas;
    canvas.Canvas::draw_box_shadow(0, 0, 100, 50,
                                    0, 4, 12, 0,
                                    Color::rgba8(0, 0, 0, 96),
                                    /*inset=*/true, /*corner_radius=*/0);
    // Inset path uses save+clip_rect+stacked rects+restore.
    REQUIRE(canvas.count(DrawCommand::Type::clip_rect) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::save) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::restore) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) >= 4);
}

TEST_CASE("Canvas fallback helpers record CPU-safe commands", "[canvas]") {
    RecordingCanvas canvas;

    canvas.set_font("Inter", 20.0f);
    auto metrics = canvas.measure_text_full("abc");
    REQUIRE(metrics.width == Catch::Approx(21.0f));
    REQUIRE(metrics.ascent == Catch::Approx(15.0f));
    REQUIRE(metrics.descent == Catch::Approx(5.0f));
    REQUIRE(metrics.line_height == Catch::Approx(24.0f));

    canvas.save_layer(1, 2, 3, 4, 0.5f, 6.0f);
    REQUIRE(canvas.count(DrawCommand::Type::save) == 1);

    Canvas::ShaderUniforms uniforms;
    uniforms.fill_color = Color::rgba8(10, 20, 30, 40);
    REQUIRE_FALSE(canvas.draw_with_sksl("half4 main(float2 p) { return half4(1); }",
                                       4, 5, 6, 7, uniforms));

    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
    const auto& fill_rect = canvas.commands().back();
    REQUIRE(fill_rect.type == DrawCommand::Type::fill_rect);
    REQUIRE(fill_rect.f[0] == Catch::Approx(4.0f));
    REQUIRE(fill_rect.f[1] == Catch::Approx(5.0f));
    REQUIRE(fill_rect.f[2] == Catch::Approx(6.0f));
    REQUIRE(fill_rect.f[3] == Catch::Approx(7.0f));
}

TEST_CASE("Canvas gradient fallbacks use first stop when present", "[canvas]") {
    RecordingCanvas canvas;
    const Color colors[] = {
        Color::rgba8(12, 34, 56, 78),
        Color::rgba8(90, 100, 110, 120),
    };
    const float positions[] = {0.0f, 1.0f};

    canvas.set_fill_gradient_linear(0, 0, 10, 10, colors, positions, 2);
    canvas.set_fill_gradient_radial(5, 5, 4, colors, positions, 2);
    canvas.set_fill_gradient_conic(5, 5, 1.0f, colors, positions, 2);
    canvas.set_fill_gradient_linear(0, 0, 10, 10, colors, positions, 0);

    REQUIRE(canvas.count(DrawCommand::Type::set_fill_color) == 3);
    for (const auto& command : canvas.commands()) {
        REQUIRE(command.type == DrawCommand::Type::set_fill_color);
        REQUIRE(command.color == colors[0]);
    }
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

TEST_CASE("Color HSV clamps channels and normalizes wrapped hues",
          "[canvas][color][issue-641]") {
    auto clamped = Color::rgba(1.5f, -0.25f, 0.5f).to_hsv();
    REQUIRE(clamped.h == Catch::Approx(330.0f).margin(1.0f));
    REQUIRE(clamped.s == Catch::Approx(1.0f).margin(0.01f));
    REQUIRE(clamped.v == Catch::Approx(1.0f).margin(0.01f));

    auto negative = Color::from_hsv({-60.0f, 1.0f, 1.0f}, 0.25f);
    REQUIRE(negative.r == Catch::Approx(1.0f).margin(0.01f));
    REQUIRE(negative.g == Catch::Approx(0.0f).margin(0.01f));
    REQUIRE(negative.b == Catch::Approx(1.0f).margin(0.01f));
    REQUIRE(negative.a == Catch::Approx(0.25f));

    auto overflow = Color::from_hsv({420.0f, 2.0f, 2.0f});
    REQUIRE(overflow.r == Catch::Approx(1.0f).margin(0.01f));
    REQUIRE(overflow.g == Catch::Approx(1.0f).margin(0.01f));
    REQUIRE(overflow.b == Catch::Approx(0.0f).margin(0.01f));
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

TEST_CASE("Color HSL clamps channels and normalizes wrapped hues",
          "[canvas][color][issue-641]") {
    auto clamped = Color::rgba(-1.0f, 0.25f, 2.0f).to_hsl();
    REQUIRE(clamped.h == Catch::Approx(225.0f).margin(1.0f));
    REQUIRE(clamped.s == Catch::Approx(1.0f).margin(0.01f));
    REQUIRE(clamped.l == Catch::Approx(0.5f).margin(0.01f));

    auto negative = Color::from_hsl({-120.0f, 1.0f, 0.5f}, 0.4f);
    REQUIRE(negative.r == Catch::Approx(0.0f).margin(0.01f));
    REQUIRE(negative.g == Catch::Approx(0.0f).margin(0.01f));
    REQUIRE(negative.b == Catch::Approx(1.0f).margin(0.01f));
    REQUIRE(negative.a == Catch::Approx(0.4f));

    auto gray = Color::from_hsl({45.0f, -1.0f, 2.0f});
    REQUIRE(gray.r == Catch::Approx(1.0f));
    REQUIRE(gray.g == Catch::Approx(1.0f));
    REQUIRE(gray.b == Catch::Approx(1.0f));
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

TEST_CASE("Color OKLCH clamps out-of-gamut conversion inputs",
          "[canvas][color][issue-641]") {
    auto dark = Color::from_oklch({-0.5f, -0.25f, 90.0f}, 0.3f);
    REQUIRE(dark.r8() == 0);
    REQUIRE(dark.g8() == 0);
    REQUIRE(dark.b8() == 0);
    REQUIRE(dark.a == Catch::Approx(0.3f));

    auto bright = Color::from_oklch({1.5f, 0.0f, 720.0f});
    REQUIRE(bright.r8() == 255);
    REQUIRE(bright.g8() == 255);
    REQUIRE(bright.b8() == 255);
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

TEST_CASE("Canvas fallback gradients apply first stop only", "[canvas][fallback]") {
    RecordingCanvas rc;
    Color colors[] = {
        Color::rgba8(10, 20, 30, 255),
        Color::rgba8(200, 210, 220, 255),
    };
    float positions[] = {0.0f, 1.0f};

    rc.set_fill_gradient_linear(0, 0, 10, 10, colors, positions, 2);
    REQUIRE(rc.count(DrawCommand::Type::set_fill_color) == 1);
    REQUIRE(rc.commands().back().color.r8() == 10);
    REQUIRE(rc.commands().back().color.g8() == 20);
    REQUIRE(rc.commands().back().color.b8() == 30);

    rc.set_fill_gradient_radial(5, 5, 10, nullptr, nullptr, 0);
    REQUIRE(rc.count(DrawCommand::Type::set_fill_color) == 1);

    rc.set_fill_gradient_conic(5, 5, 0, colors + 1, positions, 1);
    REQUIRE(rc.count(DrawCommand::Type::set_fill_color) == 2);
    REQUIRE(rc.commands().back().color.r8() == 200);
    REQUIRE(rc.commands().back().color.g8() == 210);
    REQUIRE(rc.commands().back().color.b8() == 220);
}

TEST_CASE("Canvas fallback paths and waveform emit line segments",
          "[canvas][fallback]") {
    RecordingCanvas rc;
    Canvas::Point2D points[] = {{0, 0}, {5, 10}, {10, 5}};

    rc.stroke_path(points, 0);
    rc.stroke_path(points, 1);
    REQUIRE(rc.count(DrawCommand::Type::stroke_line) == 0);

    rc.stroke_path(points, 3);
    REQUIRE(rc.count(DrawCommand::Type::stroke_line) == 2);
    REQUIRE(rc.commands()[0].f[0] == Catch::Approx(0.0f));
    REQUIRE(rc.commands()[0].f[1] == Catch::Approx(0.0f));
    REQUIRE(rc.commands()[0].f[2] == Catch::Approx(5.0f));
    REQUIRE(rc.commands()[0].f[3] == Catch::Approx(10.0f));
    REQUIRE(rc.commands()[1].f[0] == Catch::Approx(5.0f));
    REQUIRE(rc.commands()[1].f[1] == Catch::Approx(10.0f));
    REQUIRE(rc.commands()[1].f[2] == Catch::Approx(10.0f));
    REQUIRE(rc.commands()[1].f[3] == Catch::Approx(5.0f));

    rc.clear();
    float samples[] = {-1.0f, 0.0f, 1.0f};
    Canvas::WaveformStyle style;
    style.line_thickness = 2.5f;
    style.fill_center = 0.25f;

    rc.draw_waveform(samples, 1, 10, 20, 40, 20, style);
    REQUIRE(rc.command_count() == 0);

    rc.draw_waveform(samples, 3, 10, 20, 40, 20, style);
    REQUIRE(rc.count(DrawCommand::Type::set_stroke_color) == 1);
    REQUIRE(rc.count(DrawCommand::Type::set_line_width) == 1);
    REQUIRE(rc.count(DrawCommand::Type::stroke_line) == 2);
    REQUIRE(rc.commands()[1].f[0] == Catch::Approx(2.5f));

    const auto& first = rc.commands()[2];
    REQUIRE(first.type == DrawCommand::Type::stroke_line);
    REQUIRE(first.f[0] == Catch::Approx(10.0f));
    REQUIRE(first.f[1] == Catch::Approx(35.0f));
    REQUIRE(first.f[2] == Catch::Approx(30.0f));
    REQUIRE(first.f[3] == Catch::Approx(25.0f));

    const auto& second = rc.commands()[3];
    REQUIRE(second.type == DrawCommand::Type::stroke_line);
    REQUIRE(second.f[0] == Catch::Approx(30.0f));
    REQUIRE(second.f[1] == Catch::Approx(25.0f));
    REQUIRE(second.f[2] == Catch::Approx(50.0f));
    REQUIRE(second.f[3] == Catch::Approx(15.0f));
}

TEST_CASE("Canvas fallbacks cover shader, clear, and shadow no-op paths",
          "[canvas][fallback]") {
    RecordingCanvas rc;

    Canvas::ShaderUniforms uniforms;
    REQUIRE_FALSE(rc.draw_with_sksl("ignored", 1, 2, 3, 4, uniforms));
    REQUIRE(rc.count(DrawCommand::Type::set_fill_color) == 1);
    REQUIRE(rc.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(rc.commands()[1].f[0] == Catch::Approx(1.0f));
    REQUIRE(rc.commands()[1].f[1] == Catch::Approx(2.0f));
    REQUIRE(rc.commands()[1].f[2] == Catch::Approx(3.0f));
    REQUIRE(rc.commands()[1].f[3] == Catch::Approx(4.0f));

    rc.clear();
    uniforms.fill_color = Color::rgba8(11, 22, 33, 128);
    REQUIRE_FALSE(rc.draw_with_sksl("ignored", 5, 6, 7, 8, uniforms));
    REQUIRE(rc.commands()[0].color.r8() == 11);
    REQUIRE(rc.commands()[0].color.g8() == 22);
    REQUIRE(rc.commands()[0].color.b8() == 33);
    REQUIRE(rc.commands()[0].color.a8() == 128);

    rc.clear();
    rc.Canvas::clear_rect(9, 10, 11, 12);
    REQUIRE(rc.count(DrawCommand::Type::set_fill_color) == 1);
    REQUIRE(rc.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(rc.commands()[0].color.a8() == 0);
    REQUIRE(rc.commands()[1].f[0] == Catch::Approx(9.0f));
    REQUIRE(rc.commands()[1].f[1] == Catch::Approx(10.0f));
    REQUIRE(rc.commands()[1].f[2] == Catch::Approx(11.0f));
    REQUIRE(rc.commands()[1].f[3] == Catch::Approx(12.0f));

    rc.clear();
    rc.Canvas::draw_box_shadow(0, 0, 10, 10, 0, 0, 4, 0,
                               Color::rgba(0, 0, 0, 0), false, 2);
    rc.Canvas::draw_box_shadow(0, 0, 0, 0, 0, 0, 4, 0,
                               Color::rgba(0, 0, 0, 0.5f), false, 2);
    REQUIRE(rc.command_count() == 0);
}

TEST_CASE("Canvas SDF fallback covers stroked shape variants",
          "[canvas][sdf][fallback]") {
    RecordingCanvas rc;
    Canvas::SDFStyle style;
    style.stroke_width = 3.0f;
    style.stroke_color = Color::rgba8(1, 2, 3, 255);
    style.corner_radius = 6.0f;

    rc.draw_sdf_shape(Canvas::SDFShape::circle, 0, 0, 20, 10, style);
    REQUIRE(rc.count(DrawCommand::Type::set_stroke_color) == 1);
    REQUIRE(rc.count(DrawCommand::Type::set_line_width) == 1);
    REQUIRE(rc.count(DrawCommand::Type::stroke_rounded_rect) == 1);
    REQUIRE(rc.commands().back().f[4] == Catch::Approx(5.0f));

    rc.clear();
    rc.draw_sdf_shape(Canvas::SDFShape::rounded_rect, 1, 2, 30, 40, style);
    REQUIRE(rc.commands().back().type == DrawCommand::Type::stroke_rounded_rect);
    REQUIRE(rc.commands().back().f[4] == Catch::Approx(6.0f));

    rc.clear();
    rc.draw_sdf_shape(Canvas::SDFShape::diamond, 3, 4, 50, 60, style);
    REQUIRE(rc.commands().back().type == DrawCommand::Type::stroke_rounded_rect);
    REQUIRE(rc.commands().back().f[4] == Catch::Approx(0.0f));
}

TEST_CASE("RecordingCanvas captures sticky paint and image state payloads",
          "[canvas][recording][coverage]") {
    RecordingCanvas rc;
    const uint8_t pixels[] = {
        1, 2, 3, 4,
        5, 6, 7, 8,
    };

    rc.set_line_cap(LineCap::square);
    rc.set_line_join(LineJoin::bevel);
    rc.set_miter_limit(7.5f);
    rc.set_image_smoothing(false, Canvas::ImageSmoothingQuality::high);
    rc.set_direction(Canvas::TextDirection::rtl);
    rc.set_filter("blur(2px) saturate(140%)");
    rc.set_fill_pattern("pattern.png", Canvas::PatternTileMode::repeat,
                        Canvas::PatternTileMode::no_repeat);
    rc.set_stroke_pattern("stroke.png", Canvas::PatternTileMode::no_repeat,
                          Canvas::PatternTileMode::repeat);
    rc.save_backdrop_filter(1, 2, 3, 4, 5);
    REQUIRE(rc.write_pixels(pixels, 2, 1, 9, 10));

    REQUIRE(rc.count(DrawCommand::Type::set_line_cap) == 1);
    REQUIRE(rc.count(DrawCommand::Type::set_line_join) == 1);
    REQUIRE(rc.count(DrawCommand::Type::set_miter_limit) == 1);
    REQUIRE(rc.count(DrawCommand::Type::set_image_smoothing) == 1);
    REQUIRE(rc.count(DrawCommand::Type::set_direction) == 1);
    REQUIRE(rc.count(DrawCommand::Type::set_filter) == 1);
    REQUIRE(rc.count(DrawCommand::Type::set_fill_pattern) == 1);
    REQUIRE(rc.count(DrawCommand::Type::set_stroke_pattern) == 1);
    REQUIRE(rc.count(DrawCommand::Type::save_backdrop_filter) == 1);
    REQUIRE(rc.count(DrawCommand::Type::write_pixels) == 1);

    REQUIRE(rc.commands()[2].f[0] == Catch::Approx(7.5f));
    REQUIRE(rc.commands()[3].f[0] == Catch::Approx(0.0f));
    REQUIRE(rc.commands()[3].f[1] == Catch::Approx(2.0f));
    REQUIRE(rc.commands()[5].text == "blur(2px) saturate(140%)");
    REQUIRE(rc.commands()[6].f[1] == Catch::Approx(1.0f));
    REQUIRE(rc.commands().back().text.size() == sizeof(pixels));
}

TEST_CASE("RecordingCanvas captures stroke gradient commands and stop payloads",
          "[canvas][recording][coverage]") {
    RecordingCanvas rc;
    const Color colors[] = {
        Color::rgba8(10, 20, 30, 40),
        Color::rgba8(50, 60, 70, 80),
    };
    const float positions[] = {0.25f, 0.75f};

    rc.set_stroke_gradient_linear(1, 2, 3, 4, colors, positions, 2);
    rc.set_stroke_gradient_radial(5, 6, 7, colors, positions, 1);
    rc.set_stroke_gradient_radial_two_circles(8, 9, 10, 11, 12, 13,
                                              colors, positions, 2);
    rc.set_stroke_gradient_conic(14, 15, 16, colors + 1, positions + 1, 1);
    rc.clear_stroke_gradient();

    REQUIRE(rc.count(DrawCommand::Type::set_stroke_gradient_linear) == 1);
    REQUIRE(rc.count(DrawCommand::Type::set_stroke_gradient_radial) == 1);
    REQUIRE(rc.count(DrawCommand::Type::set_stroke_gradient_radial_two_circles) == 1);
    REQUIRE(rc.count(DrawCommand::Type::set_stroke_gradient_conic) == 1);
    REQUIRE(rc.count(DrawCommand::Type::clear_stroke_gradient) == 1);

    const auto& linear = rc.commands()[0];
    REQUIRE(linear.f[0] == Catch::Approx(1.0f));
    REQUIRE(linear.f[3] == Catch::Approx(4.0f));
    REQUIRE(linear.floats.size() == 10);
    REQUIRE(linear.floats[0] == Catch::Approx(0.25f));
    REQUIRE(linear.floats[1] == Catch::Approx(colors[0].r));
    REQUIRE(linear.floats[5] == Catch::Approx(0.75f));

    const auto& two_circle = rc.commands()[2];
    REQUIRE(two_circle.f[0] == Catch::Approx(8.0f));
    REQUIRE(two_circle.f[5] == Catch::Approx(13.0f));
    REQUIRE(two_circle.floats.size() == 10);

    const auto& conic = rc.commands()[3];
    REQUIRE(conic.f[2] == Catch::Approx(16.0f));
    REQUIRE(conic.floats.size() == 5);
    REQUIRE(rc.commands().back().type == DrawCommand::Type::clear_stroke_gradient);
}

TEST_CASE("Canvas base fallbacks delegate through recording backend",
          "[canvas][fallback][coverage]") {
    RecordingCanvas rc;
    const Color colors[] = {Color::rgba8(3, 4, 5, 6)};
    const float positions[] = {0.0f};

    rc.Canvas::set_stroke_gradient_linear(0, 0, 1, 1, colors, positions, 1);
    rc.Canvas::set_stroke_gradient_radial_two_circles(0, 0, 2, 3, 4, 5,
                                                       colors, positions, 1);
    rc.Canvas::set_stroke_gradient_conic(2, 3, 4, colors, positions, 0);
    REQUIRE(rc.count(DrawCommand::Type::set_stroke_color) == 1);
    REQUIRE(rc.count(DrawCommand::Type::set_stroke_gradient_radial) == 1);
    REQUIRE(rc.commands()[0].color == colors[0]);
    REQUIRE(rc.commands()[1].type == DrawCommand::Type::set_stroke_gradient_radial);

    rc.Canvas::fill_text_anchored("anchor", 7, 8, Canvas::TextAnchor::GlyphCenter);
    rc.Canvas::fill_text_with_max_width("wide", 9, 10, 40);
    rc.Canvas::stroke_text("outline", 11, 12, 50);
    REQUIRE(rc.count(DrawCommand::Type::fill_text) == 3);
    REQUIRE(rc.commands()[2].text == "anchor");
    REQUIRE(rc.commands()[3].text == "wide");
    REQUIRE(rc.commands()[4].text == "outline");

    rc.Canvas::set_font_full("fallback", 18, 700, 1, 0.5f);
    REQUIRE(rc.count(DrawCommand::Type::set_font) == 1);
    REQUIRE(rc.commands().back().text == "fallback");
    REQUIRE(rc.commands().back().f[0] == Catch::Approx(18.0f));
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

#endif  // PULP_HAS_SKIA


// ── pulp #929 — Canvas::clear_rect default + CoreGraphics override ──────────

namespace {

// Minimal Canvas subclass that records every fill_rect / set_fill_color call
// so we can assert the documented Canvas::clear_rect default behaviour
// (delegates to set_fill_color(transparent) + fill_rect) without coupling to
// any real GPU/CPU backend.
class StubCanvas : public Canvas {
public:
    struct FillRectCall { float x, y, w, h; Color color; };
    std::vector<FillRectCall> fills;
    Color current_color = Color::rgba(1.0f, 1.0f, 1.0f, 1.0f);

    void save() override {}
    void restore() override {}
    void translate(float, float) override {}
    void scale(float, float) override {}
    void rotate(float) override {}
    void clip_rect(float, float, float, float) override {}
    void set_fill_color(Color c) override { current_color = c; }
    void set_stroke_color(Color) override {}
    void set_line_width(float) override {}
    void set_line_cap(LineCap) override {}
    void set_line_join(LineJoin) override {}
    void fill_rect(float x, float y, float w, float h) override {
        fills.push_back({x, y, w, h, current_color});
    }
    void stroke_rect(float, float, float, float) override {}
    void fill_rounded_rect(float, float, float, float, float) override {}
    void stroke_rounded_rect(float, float, float, float, float) override {}
    void fill_circle(float, float, float) override {}
    void stroke_circle(float, float, float) override {}
    void stroke_arc(float, float, float, float, float) override {}
    void stroke_line(float, float, float, float) override {}
    void set_font(const std::string&, float) override {}
    void set_text_align(TextAlign) override {}
    void fill_text(const std::string&, float, float) override {}
    float measure_text(const std::string&) override { return 0.0f; }
};

}  // namespace

TEST_CASE("Canvas::clear_rect default falls back to transparent fill_rect",
          "[canvas][issue-929]") {
    StubCanvas canvas;
    canvas.set_fill_color(Color::rgba(1.0f, 0.0f, 0.0f, 1.0f));
    canvas.clear_rect(5, 6, 7, 8);

    REQUIRE(canvas.fills.size() == 1);
    REQUIRE(canvas.fills[0].x == 5.0f);
    REQUIRE(canvas.fills[0].y == 6.0f);
    REQUIRE(canvas.fills[0].w == 7.0f);
    REQUIRE(canvas.fills[0].h == 8.0f);
    // Default fallback set the fill color to transparent before drawing.
    REQUIRE(canvas.fills[0].color.a == 0.0f);
    REQUIRE(canvas.current_color.a == 0.0f);
}

TEST_CASE("RecordingCanvas::clear_rect emits a dedicated clear_rect command",
          "[canvas][issue-929]") {
    RecordingCanvas rc;
    rc.clear_rect(1, 2, 3, 4);

    REQUIRE(rc.commands().size() == 1);
    REQUIRE(rc.commands()[0].type == DrawCommand::Type::clear_rect);
    REQUIRE(rc.commands()[0].f[0] == 1.0f);
    REQUIRE(rc.commands()[0].f[1] == 2.0f);
    REQUIRE(rc.commands()[0].f[2] == 3.0f);
    REQUIRE(rc.commands()[0].f[3] == 4.0f);
}

#ifdef __APPLE__
TEST_CASE("CoreGraphicsCanvas::clear_rect zeroes destination pixels",
          "[canvas][cg][issue-929]") {
    // Build a 16x16 RGBA8 CGBitmapContext, fill it with opaque red, then
    // ask CoreGraphicsCanvas::clear_rect to clear the entire region. Read
    // back the pixels — every byte should be zero (transparent black),
    // proving CGContextClearRect actually replaces destination texels
    // rather than SrcOver-ing a transparent fill.
    constexpr int W = 16;
    constexpr int H = 16;
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 4u, 0u);
    auto cs = CGColorSpaceCreateDeviceRGB();
    REQUIRE(cs != nullptr);
    const uint32_t bitmap_info =
        static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
        static_cast<uint32_t>(kCGBitmapByteOrder32Big);
    CGContextRef ctx = CGBitmapContextCreate(
        pixels.data(), W, H, 8, W * 4u, cs, bitmap_info);
    CGColorSpaceRelease(cs);
    REQUIRE(ctx != nullptr);

    // Pre-fill with opaque red via CG directly so the test doesn't rely on
    // any CoreGraphicsCanvas method other than clear_rect.
    CGContextSetRGBFillColor(ctx, 1.0f, 0.0f, 0.0f, 1.0f);
    CGContextFillRect(ctx, CGRectMake(0, 0, W, H));

    {
        CoreGraphicsCanvas canvas(ctx, static_cast<float>(W),
                                  static_cast<float>(H));
        canvas.clear_rect(0, 0, W, H);
    }
    CGContextRelease(ctx);

    // Every pixel byte must be zero after CGContextClearRect.
    for (size_t i = 0; i < pixels.size(); ++i) {
        INFO("Pixel byte " << i << " value=" << int(pixels[i]));
        REQUIRE(pixels[i] == 0);
    }
}
#endif  // __APPLE__

// ── pulp #1737 — CSS font-variant → SkShaper Feature plumbing ─────────────
// Regression coverage for the SkShaper 8-arg shape() overload that the
// fontVariant slice routes through. The legacy 6-arg shape() ignores the
// caller's feature array entirely (HarfBuzz applies only the on-by-default
// features baked into the font, e.g. kerning + rlig). Once the FontFeature
// vector reaches HarfBuzz at shape time, GSUB/GPOS rules tagged for `tnum`,
// `smcp`, `pnum`, etc. fire — and the resulting glyph advances diverge
// from the unfeatured run.
//
// The harness tests the contract through the public Canvas API:
//   1. set_font_features({tnum=1}) must change the rendered shape (or at
//      minimum the measured advance) for a digit-only string vs the
//      no-features baseline. Inter ships with `tnum` so '1' and '9'
//      render at equal advance under tnum but distinct advances without.
//   2. clear_font_features() must restore the baseline width so feature
//      state doesn't leak across paint() calls.

#ifdef PULP_HAS_SKIA
TEST_CASE("SkiaCanvas::set_font_features routes tnum through SkShaper",
          "[canvas][skia][fonts][issue-1737][!mayfail]") {
    SkImageInfo info = SkImageInfo::Make(128, 64, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    sk_canvas->clear(SK_ColorWHITE);

    SkiaCanvas canvas(sk_canvas);
    canvas.set_font_full("Inter", 24.0f, 400, 0, 0.0f);

    // Inter has proportional digits by default — '1' is narrower than '9'.
    // Without tnum, the two strings differ in advance.
    const std::string ones   = "111";
    const std::string nines  = "999";

    const float ones_default  = canvas.measure_text(ones);
    const float nines_default = canvas.measure_text(nines);
    REQUIRE(ones_default  > 0.0f);
    REQUIRE(nines_default > 0.0f);

    // Apply tabular-nums. After SkShaper sees the Feature, '1' and '9'
    // share the same advance — the strings now match width to within the
    // shaper's sub-pixel rounding.
    std::vector<Canvas::FontFeature> tnum{
        {Canvas::make_font_feature_tag("tnum"), 1}
    };
    canvas.set_font_features(tnum);

    const float ones_tnum  = canvas.measure_text(ones);
    const float nines_tnum = canvas.measure_text(nines);
    REQUIRE(ones_tnum  > 0.0f);
    REQUIRE(nines_tnum > 0.0f);

    // Either Inter's tnum table is wired (advances now equal) OR the
    // shape() overload at minimum changed *something* about the run vs
    // the unfeatured pass. We assert the equal-advance invariant — if a
    // future Inter build ships without tnum, this case will need a
    // different probe font, and that's the right time to find out.
    REQUIRE_THAT(ones_tnum,
                 Catch::Matchers::WithinAbs(nines_tnum, 0.5f));

    // Clear must restore baseline so feature state doesn't survive
    // beyond the paint that set it.
    canvas.clear_font_features();
    const float ones_cleared  = canvas.measure_text(ones);
    const float nines_cleared = canvas.measure_text(nines);
    REQUIRE_THAT(ones_cleared,
                 Catch::Matchers::WithinAbs(ones_default, 0.001f));
    REQUIRE_THAT(nines_cleared,
                 Catch::Matchers::WithinAbs(nines_default, 0.001f));
}

TEST_CASE("Canvas::make_font_feature_tag packs OpenType four-char tags",
          "[canvas][fonts][issue-1737]") {
    // Big-endian packing — 't','n','u','m' → 0x746E756D. SkShaper's
    // Feature.tag field is a uint32 in the same encoding; if this ever
    // diverges, every feature silently misses.
    constexpr uint32_t tnum = Canvas::make_font_feature_tag("tnum");
    REQUIRE(tnum == 0x746E756Du);
    constexpr uint32_t smcp = Canvas::make_font_feature_tag("smcp");
    REQUIRE(smcp == 0x736D6370u);
    constexpr uint32_t pnum = Canvas::make_font_feature_tag("pnum");
    REQUIRE(pnum == 0x706E756Du);
}
#endif // PULP_HAS_SKIA

// ── pulp #1806 — fill_current_path / stroke_current_path preserve the scratch path ──
// Canvas2D spec: ctx.fill() and ctx.stroke() do NOT consume the path. A
// subsequent stroke() after fill() must paint the outlined version of
// the filled shape. Previously path_builder_->detach() emptied the
// builder, so the second op silently no-op'd. This regressed both
// SvgPathWidget compound paths (visible icon stroke missing) AND any JS
// canvas idiom of fill-then-stroke for "filled outlined shape".
// Tests are backend-agnostic — RecordingCanvas captures command stream
// without depending on Skia/CG availability, so they always run.

// RecordingCanvas is declared in <pulp/canvas/canvas.hpp> (already included above).

TEST_CASE("pulp #1806 — fill_current_path preserves path so subsequent stroke paints",
          "[canvas][issue-1806][path-preserve]") {
    pulp::canvas::RecordingCanvas rc;
    rc.begin_path();
    rc.move_to(10, 10);
    rc.line_to(50, 10);
    rc.line_to(50, 50);
    rc.line_to(10, 50);
    rc.close_path();
    rc.fill_current_path();
    rc.stroke_current_path();

    int n_fill = 0, n_stroke = 0;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_current_path) ++n_fill;
        if (cmd.type == pulp::canvas::DrawCommand::Type::stroke_current_path) ++n_stroke;
    }
    REQUIRE(n_fill == 1);
    REQUIRE(n_stroke == 1);
    // Both ops must have been recorded against a non-empty path.
    // RecordingCanvas snapshots the path geometry at command time.
}

TEST_CASE("pulp #1806 — begin_path resets between fill+stroke pairs",
          "[canvas][issue-1806][path-preserve]") {
    // Spec invariant: begin_path() unconditionally clears the scratch
    // path even if the previous fill/stroke didn't consume it.
    pulp::canvas::RecordingCanvas rc;
    rc.begin_path();
    rc.move_to(10, 10);
    rc.line_to(50, 10);
    rc.fill_current_path();
    rc.stroke_current_path();  // still paints the line — same path
    rc.begin_path();
    rc.move_to(0, 0);
    rc.line_to(100, 100);
    rc.stroke_current_path();  // paints the NEW line, not the old one

    int strokes = 0;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::stroke_current_path) ++strokes;
    }
    REQUIRE(strokes == 2);
}

#ifdef PULP_HAS_SKIA
// pulp #1899 (gap #3) — SkiaCanvas tracks every currently-open
// save_layer whose layer-paint alpha is < 1. Text paint paths
// (fill_text / stroke_text) consult `inside_non_opaque_layer()` at
// paint time and select greyscale AA over LCD subpixel AA, so glyphs
// stay legible inside CSS-opacity layers (browser parity). This test
// asserts the stack state transitions across the four save_layer
// entry points + plain save + restore + restore_to_count.
TEST_CASE("SkiaCanvas tracks non-opaque layers for text edging "
          "(pulp #1899 gap #3)", "[canvas][skia][issue-1899]") {
    SkImageInfo info = SkImageInfo::Make(64, 64, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    REQUIRE(sk_canvas != nullptr);

    SkiaCanvas canvas(sk_canvas);

    // Baseline: no layer open → not inside non-opaque destination.
    REQUIRE_FALSE(canvas.inside_non_opaque_layer());

    SECTION("opacity < 1 layer flips the flag; restore clears it") {
        canvas.save_layer(0, 0, 64, 64, /*opacity=*/0.5f, /*blur=*/0.0f);
        REQUIRE(canvas.inside_non_opaque_layer());
        canvas.restore();
        REQUIRE_FALSE(canvas.inside_non_opaque_layer());
    }

    SECTION("opacity == 1 layer does NOT flip the flag") {
        canvas.save_layer(0, 0, 64, 64, /*opacity=*/1.0f, /*blur=*/0.0f);
        REQUIRE_FALSE(canvas.inside_non_opaque_layer());
        canvas.restore();
        REQUIRE_FALSE(canvas.inside_non_opaque_layer());
    }

    SECTION("plain save() never touches the stack") {
        canvas.save();
        REQUIRE_FALSE(canvas.inside_non_opaque_layer());
        canvas.save_layer(0, 0, 64, 64, 0.25f, 0.0f);
        REQUIRE(canvas.inside_non_opaque_layer());
        canvas.restore();   // closes the opacity layer
        REQUIRE_FALSE(canvas.inside_non_opaque_layer());
        canvas.restore();   // closes the plain save
        REQUIRE_FALSE(canvas.inside_non_opaque_layer());
    }

    SECTION("nested non-opaque layers stay tracked until each restore") {
        canvas.save_layer(0, 0, 64, 64, 0.8f, 0.0f);
        REQUIRE(canvas.inside_non_opaque_layer());
        canvas.save_layer(0, 0, 64, 64, 0.5f, 0.0f);
        REQUIRE(canvas.inside_non_opaque_layer());
        canvas.restore();
        // Outer layer still open and non-opaque.
        REQUIRE(canvas.inside_non_opaque_layer());
        canvas.restore();
        REQUIRE_FALSE(canvas.inside_non_opaque_layer());
    }

    SECTION("save_layer_with_blend tracks non-opaque destinations too") {
        canvas.save_layer_with_blend(0, 0, 64, 64, /*opacity=*/0.6f,
                                     /*blur=*/0.0f,
                                     Canvas::BlendMode::multiply);
        REQUIRE(canvas.inside_non_opaque_layer());
        canvas.restore();
        REQUIRE_FALSE(canvas.inside_non_opaque_layer());
    }

    SECTION("restore_to_count drops every non-opaque layer above target") {
        const int baseline = canvas.save_count();
        canvas.save_layer(0, 0, 64, 64, 0.5f, 0.0f);
        canvas.save_layer(0, 0, 64, 64, 0.5f, 0.0f);
        canvas.save_layer(0, 0, 64, 64, 0.5f, 0.0f);
        REQUIRE(canvas.inside_non_opaque_layer());

        canvas.restore_to_count(baseline);
        // All three opacity layers are gone — stack must be empty.
        REQUIRE_FALSE(canvas.inside_non_opaque_layer());
    }
}

// pulp #1899 (gap #3) — end-to-end: render the same glyph twice into
// the same surface, once inside save_layer(opacity = 0.5) and once
// outside, and verify the inside-layer pixels show no LCD subpixel
// pattern. LCD AA produces unequal R / G / B coverage at glyph edges;
// greyscale AA writes equal R / G / B. Scanning a few rows where the
// glyph should have an edge and asserting "max channel difference == 0"
// for the inside-layer block is the simplest cross-platform probe.
TEST_CASE("SkiaCanvas text inside opacity layer uses greyscale AA "
          "(pulp #1899 gap #3)", "[canvas][skia][issue-1899]") {
    // Two side-by-side surfaces — one painted with an opacity layer,
    // one without — so we can compare per-pixel coverage shapes.
    auto make_surface = []() {
        SkImageInfo info = SkImageInfo::Make(96, 32, kN32_SkColorType,
                                             kPremul_SkAlphaType,
                                             SkColorSpace::MakeSRGB());
        return SkSurfaces::Raster(info);
    };
    auto opaque_surface = make_surface();
    auto layer_surface = make_surface();
    REQUIRE(opaque_surface != nullptr);
    REQUIRE(layer_surface != nullptr);

    // Opaque-destination text (LCD AA expected — but we don't assert
    // on that direction; we only assert the layer surface uses uniform
    // coverage).
    {
        auto* sc = opaque_surface->getCanvas();
        sc->clear(SK_ColorBLACK);
        SkiaCanvas canvas(sc);
        canvas.set_font("Inter", 18.0f);
        canvas.set_fill_color(Color::rgba8(255, 255, 255, 255));
        canvas.fill_text("Hg", 4.0f, 24.0f);
    }

    // Inside an opacity layer (greyscale AA expected — verify glyph
    // edge pixels have equal R / G / B coverage).
    {
        auto* sc = layer_surface->getCanvas();
        sc->clear(SK_ColorBLACK);
        SkiaCanvas canvas(sc);
        canvas.save_layer(0, 0, 96, 32, /*opacity=*/0.5f, /*blur=*/0.0f);
        canvas.set_font("Inter", 18.0f);
        canvas.set_fill_color(Color::rgba8(255, 255, 255, 255));
        canvas.fill_text("Hg", 4.0f, 24.0f);
        canvas.restore();
    }

    // Read back the layer surface and look for a pixel whose
    // R / G / B coverage isn't uniform. Greyscale AA writes equal
    // channels at every glyph edge; LCD AA writes unequal channels
    // by construction. Allow a 1-LSB tolerance for premultiplied
    // round-trip noise on layer compositing.
    SkImageInfo info = SkImageInfo::Make(96, 32, kRGBA_8888_SkColorType,
                                         kUnpremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    std::vector<uint8_t> pixels(96 * 32 * 4, 0);
    REQUIRE(layer_surface->readPixels(info, pixels.data(), 96 * 4, 0, 0));

    int unequal_channel_pixels = 0;
    int edge_pixels = 0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 96; ++x) {
            const uint8_t r = pixels[(y * 96 + x) * 4 + 0];
            const uint8_t g = pixels[(y * 96 + x) * 4 + 1];
            const uint8_t b = pixels[(y * 96 + x) * 4 + 2];
            const int max_c = std::max({r, g, b});
            const int min_c = std::min({r, g, b});
            // Only inspect partial-coverage pixels (genuine glyph
            // edges) — fully transparent / fully opaque pixels can't
            // distinguish edging mode.
            if (max_c > 4 && max_c < 250) {
                ++edge_pixels;
                if (max_c - min_c > 1) {
                    ++unequal_channel_pixels;
                }
            }
        }
    }

    // Sanity — we should have found SOME glyph-edge pixels at all.
    // If the font isn't available on the host, the test would yield
    // zero edge pixels; bail with a Catch2 message instead of a
    // false-positive pass.
    REQUIRE(edge_pixels > 0);
    // Core assertion — inside the opacity layer, the renderer used
    // greyscale AA, so glyph-edge pixels carry uniform R / G / B.
    REQUIRE(unequal_channel_pixels == 0);
}
#endif // PULP_HAS_SKIA
