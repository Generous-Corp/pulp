#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/effects.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::canvas;

TEST_CASE("BlurEffect parameters", "[canvas][effects]") {
    BlurEffect blur;
    REQUIRE(blur.radius_x == 4.0f);
    REQUIRE(blur.radius_y == 4.0f);

    blur.radius_x = 8.0f;
    blur.radius_y = 12.0f;
    REQUIRE(blur.radius_x == 8.0f);
}

TEST_CASE("ShadowEffect parameters", "[canvas][effects]") {
    ShadowEffect shadow;
    REQUIRE(shadow.offset_x == 2.0f);
    REQUIRE(shadow.offset_y == 2.0f);
    REQUIRE(shadow.blur_radius == 4.0f);
    REQUIRE(shadow.color.a8() == 128);
}

TEST_CASE("BloomEffect parameters", "[canvas][effects]") {
    BloomEffect bloom;
    REQUIRE(bloom.threshold > 0);
    REQUIRE(bloom.intensity > 0);
    REQUIRE(bloom.radius > 0);
}

TEST_CASE("ColorAdjust defaults", "[canvas][effects]") {
    ColorAdjust adj;
    REQUIRE(adj.brightness == 0.0f);
    REQUIRE(adj.contrast == 1.0f);
    REQUIRE(adj.saturation == 1.0f);
    REQUIRE(adj.opacity == 1.0f);
}

TEST_CASE("Effect layer with RecordingCanvas", "[canvas][effects]") {
    RecordingCanvas canvas;

    begin_effect_layer(canvas, BlurEffect{8.0f, 8.0f});
    canvas.fill_rect(10, 10, 100, 50);
    end_effect_layer(canvas);

    // Should have save + fill_rect + restore
    REQUIRE(canvas.count(DrawCommand::Type::save) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::restore) >= 1);
}

TEST_CASE("Shadow effect with RecordingCanvas", "[canvas][effects]") {
    RecordingCanvas canvas;

    ShadowEffect shadow;
    shadow.offset_x = 3;
    shadow.offset_y = 3;
    shadow.color = Color::rgba8(0, 0, 0, 100);

    begin_effect_layer(canvas, shadow);
    canvas.fill_rounded_rect(20, 20, 80, 40, 8);
    end_effect_layer(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::save) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
}

TEST_CASE("apply_shadow records generic canvas setup commands",
          "[canvas][effects][coverage]") {
    RecordingCanvas canvas;
    ShadowEffect shadow;
    shadow.offset_x = 6.0f;
    shadow.offset_y = -3.0f;
    shadow.color = Color::rgba8(10, 20, 30, 160);

    apply_shadow(canvas, shadow);

    REQUIRE(canvas.save_count() == 1);
    REQUIRE(canvas.count(DrawCommand::Type::save) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::translate) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::set_fill_color) == 1);

    const auto& commands = canvas.commands();
    REQUIRE(commands[1].type == DrawCommand::Type::translate);
    REQUIRE(commands[1].f[0] == 6.0f);
    REQUIRE(commands[1].f[1] == -3.0f);
    REQUIRE(commands[2].color == shadow.color);

    canvas.restore();
    REQUIRE(canvas.save_count() == 0);
}

TEST_CASE("direct blur and color adjustment calls are generic no-ops",
          "[canvas][effects][coverage][phase3]") {
    RecordingCanvas canvas;

    apply_blur(canvas, BlurEffect{2.0f, 5.0f});
    apply_color_adjust(canvas, ColorAdjust{0.25f, 1.5f, 0.75f, 0.5f});

    REQUIRE(canvas.command_count() == 0);
}

TEST_CASE("effect layers can be nested and restore in order",
          "[canvas][effects][coverage][phase3]") {
    RecordingCanvas canvas;

    begin_effect_layer(canvas, BlurEffect{1.0f, 2.0f});
    begin_effect_layer(canvas, ShadowEffect{});
    canvas.fill_rect(1, 2, 3, 4);
    end_effect_layer(canvas);
    end_effect_layer(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::save) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::restore) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
}
