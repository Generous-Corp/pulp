#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/canvas/canvas.hpp>

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
