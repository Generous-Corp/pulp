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
