// Automated test for Panel widget
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>

using Catch::Matchers::WithinAbs;
using namespace pulp::view;
using pulp::canvas::RecordingCanvas;
using pulp::canvas::DrawCommand;

namespace {

const DrawCommand* first_command(const RecordingCanvas& canvas, DrawCommand::Type type) {
    for (const auto& command : canvas.commands()) {
        if (command.type == type)
            return &command;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Panel: default token names", "[panel]") {
    Panel panel;
    REQUIRE(panel.background_token() == "bg.surface");
    REQUIRE(panel.border_token() == "control.border");
    REQUIRE(panel.corner_radius() == 8.0f);
    REQUIRE(panel.border_width() == 1.0f);
}

TEST_CASE("Panel: custom token names", "[panel]") {
    Panel panel;
    panel.set_background_token("bg.elevated");
    panel.set_border_token("accent.primary");
    panel.set_corner_radius(12.0f);
    panel.set_border_width(2.0f);

    REQUIRE(panel.background_token() == "bg.elevated");
    REQUIRE(panel.border_token() == "accent.primary");
    REQUIRE(panel.corner_radius() == 12.0f);
    REQUIRE(panel.border_width() == 2.0f);
}

TEST_CASE("Panel: paints without crash under all themes", "[panel]") {
    RecordingCanvas rc;
    for (auto theme_fn : {Theme::dark, Theme::light, Theme::pro_audio}) {
        Panel panel;
        panel.set_theme(theme_fn());
        panel.set_bounds({0, 0, 200, 100});
        panel.paint(rc);
    }
    REQUIRE(rc.commands().size() > 0);
}

TEST_CASE("Panel: zero border width skips border", "[panel]") {
    RecordingCanvas rc;
    Panel panel;
    panel.set_theme(Theme::dark());
    panel.set_bounds({0, 0, 100, 50});
    panel.set_border_width(0);
    panel.paint(rc);
    bool has_stroke = false;
    for (auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::stroke_rounded_rect)
            has_stroke = true;
    }
    REQUIRE_FALSE(has_stroke);
}

TEST_CASE("Panel: custom theme tokens drive fill and stroke colors",
          "[panel][coverage][phase3]") {
    Theme theme;
    theme.colors["panel.fill"] = color_from_hex(0x112233);
    theme.colors["panel.stroke"] = color_from_hex(0x445566);

    Panel panel;
    panel.set_theme(theme);
    panel.set_bounds({0, 0, 120, 60});
    panel.set_background_token("panel.fill");
    panel.set_border_token("panel.stroke");

    RecordingCanvas canvas;
    panel.paint(canvas);

    auto* fill = first_command(canvas, DrawCommand::Type::set_fill_color);
    auto* stroke = first_command(canvas, DrawCommand::Type::set_stroke_color);
    REQUIRE(fill != nullptr);
    REQUIRE(stroke != nullptr);
    REQUIRE(fill->color.r8() == 0x11);
    REQUIRE(fill->color.g8() == 0x22);
    REQUIRE(fill->color.b8() == 0x33);
    REQUIRE(stroke->color.r8() == 0x44);
    REQUIRE(stroke->color.g8() == 0x55);
    REQUIRE(stroke->color.b8() == 0x66);
}

TEST_CASE("Panel: missing theme tokens use documented fallback colors",
          "[panel][coverage][phase3]") {
    Panel panel;
    panel.set_theme(Theme{});
    panel.set_bounds({0, 0, 80, 40});
    panel.set_background_token("missing.fill");
    panel.set_border_token("missing.stroke");

    RecordingCanvas canvas;
    panel.paint(canvas);

    auto* fill = first_command(canvas, DrawCommand::Type::set_fill_color);
    auto* stroke = first_command(canvas, DrawCommand::Type::set_stroke_color);
    REQUIRE(fill != nullptr);
    REQUIRE(stroke != nullptr);
    REQUIRE(fill->color.r8() == 45);
    REQUIRE(fill->color.g8() == 45);
    REQUIRE(fill->color.b8() == 60);
    REQUIRE(stroke->color.r8() == 80);
    REQUIRE(stroke->color.g8() == 80);
    REQUIRE(stroke->color.b8() == 100);
}

TEST_CASE("Panel: border geometry is inset and radius is clamped by border width",
          "[panel][coverage][phase3]") {
    Panel panel;
    panel.set_bounds({0, 0, 100, 50});
    panel.set_corner_radius(3.0f);
    panel.set_border_width(8.0f);

    RecordingCanvas canvas;
    panel.paint(canvas);

    auto* line_width = first_command(canvas, DrawCommand::Type::set_line_width);
    auto* fill = first_command(canvas, DrawCommand::Type::fill_rounded_rect);
    auto* stroke = first_command(canvas, DrawCommand::Type::stroke_rounded_rect);
    REQUIRE(line_width != nullptr);
    REQUIRE(fill != nullptr);
    REQUIRE(stroke != nullptr);
    REQUIRE_THAT(line_width->f[0], WithinAbs(8.0f, 0.001f));
    REQUIRE_THAT(fill->f[4], WithinAbs(3.0f, 0.001f));
    REQUIRE_THAT(stroke->f[0], WithinAbs(4.0f, 0.001f));
    REQUIRE_THAT(stroke->f[1], WithinAbs(4.0f, 0.001f));
    REQUIRE_THAT(stroke->f[2], WithinAbs(92.0f, 0.001f));
    REQUIRE_THAT(stroke->f[3], WithinAbs(42.0f, 0.001f));
    REQUIRE_THAT(stroke->f[4], WithinAbs(0.0f, 0.001f));
}
