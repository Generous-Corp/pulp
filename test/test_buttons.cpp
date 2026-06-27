#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/theme.hpp>

#include <vector>

using namespace pulp::view;
using pulp::canvas::Color;
using pulp::canvas::RecordingCanvas;
using pulp::canvas::DrawCommand;

namespace {

// Paint `v` into a RecordingCanvas and return every colour passed to
// set_fill_color, in order.
std::vector<Color> fill_colors(View& v) {
    RecordingCanvas rc;
    v.paint(rc);
    std::vector<Color> out;
    for (const auto& c : rc.commands())
        if (c.type == DrawCommand::Type::set_fill_color) out.push_back(c.color);
    return out;
}

bool is_white(Color c) { return c.r8() == 255 && c.g8() == 255 && c.b8() == 255; }

}  // namespace

// Button widgets must use theme colours. HyperlinkButton/ArrowButton passed
// 0–255 ints to Color::rgba() (which takes 0–1 floats and clamps), so they
// rendered solid white; these cases pin token wiring.

TEST_CASE("HyperlinkButton renders its link colour, not clamped white",
          "[view][buttons][reskin]") {
    HyperlinkButton b("docs", "https://example.com");
    b.set_bounds({0, 0, 120, 20});

    auto fills = fill_colors(b);
    REQUIRE_FALSE(fills.empty());
    REQUIRE_FALSE(is_white(fills.front()));     // regression: was clamped white
    REQUIRE(fills.front().b8() > fills.front().r8());  // blue-dominant link
}

TEST_CASE("HyperlinkButton link colour follows the theme token",
          "[view][buttons][reskin]") {
    HyperlinkButton b("docs", "https://example.com");
    b.set_bounds({0, 0, 120, 20});

    Theme t;
    t.colors["text.link"] = color_from_hex(0x16DAC2);  // Ink & Signal teal
    b.set_theme(t);

    auto fills = fill_colors(b);
    REQUIRE_FALSE(fills.empty());
    REQUIRE(fills.front() == color_from_hex(0x16DAC2));
}

TEST_CASE("ArrowButton glyph is a real colour, not clamped white",
          "[view][buttons][reskin]") {
    ArrowButton b(ArrowDirection::right);
    b.set_bounds({0, 0, 24, 24});

    auto fills = fill_colors(b);
    REQUIRE_FALSE(fills.empty());
    REQUIRE_FALSE(is_white(fills.front()));
}

TEST_CASE("TextButton paints a theme-driven face and label",
          "[view][buttons][reskin]") {
    TextButton b("OK");
    b.set_bounds({0, 0, 80, 28});

    Theme t;
    t.colors["bg.elevated"]  = color_from_hex(0x1E2530);
    t.colors["text.primary"] = color_from_hex(0xF3F6F9);
    b.set_theme(t);

    auto fills = fill_colors(b);
    REQUIRE(fills.size() >= 2);  // face + label

    bool saw_face = false, saw_label = false;
    for (const auto& c : fills) {
        if (c == color_from_hex(0x1E2530)) saw_face = true;
        if (c == color_from_hex(0xF3F6F9)) saw_label = true;
    }
    REQUIRE(saw_face);
    REQUIRE(saw_label);
}
