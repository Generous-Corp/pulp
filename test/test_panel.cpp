// Automated test for Panel widget
#include <catch2/catch_test_macros.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::view;
using pulp::canvas::RecordingCanvas;
using pulp::canvas::DrawCommand;

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
