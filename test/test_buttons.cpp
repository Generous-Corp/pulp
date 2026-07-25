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

// Paint `v` into a RecordingCanvas and return every color passed to
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

// Reskinnability regression: the button widgets used to hardcode their
// colors — and HyperlinkButton/ArrowButton passed 0–255 ints to
// Color::rgba() (which takes 0–1 floats and clamps), so they rendered
// solid white. These guard the bug fix + token wiring.

TEST_CASE("HyperlinkButton renders its link color, not clamped white",
          "[view][buttons][reskin]") {
    HyperlinkButton b("docs", "https://example.com");
    b.set_bounds({0, 0, 120, 20});

    auto fills = fill_colors(b);
    REQUIRE_FALSE(fills.empty());
    REQUIRE_FALSE(is_white(fills.front()));     // regression: was clamped white
    REQUIRE(fills.front().b8() > fills.front().r8());  // blue-dominant link
}

TEST_CASE("HyperlinkButton link color follows the theme token",
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

TEST_CASE("ArrowButton glyph is a real color, not clamped white",
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

TEST_CASE("a button is one hit target, including its icon", "[view][buttons]") {
    // Buttons draw their content as child views. hit_test() returns the
    // topmost hit-testable view, so an icon or label centred in a button used
    // to swallow the click and on_click never fired — while the button's few
    // bare pixels still worked, making it look intermittent rather than dead.
    TextButton button;
    button.set_bounds({0, 0, 38, 38});

    auto icon = std::make_unique<View>();
    auto* icon_raw = icon.get();
    button.add_child(std::move(icon));
    button.layout_children();
    // Placed after layout so flex cannot move it: centred, exactly where
    // anyone aims.
    icon_raw->set_bounds({9, 9, 20, 20});

    // Dead centre is over the icon.
    View* hit = button.hit_test({19, 19});
    CHECK(hit == &button);
    CHECK(hit != icon_raw);

    // An opt-out remains available for a button that really wants an
    // interactive child.
    button.set_pointer_events(View::PointerEvents::auto_);
    CHECK(button.hit_test({19, 19}) == icon_raw);
}
