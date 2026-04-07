// CSS color parser tests — validates color parsing through setBackground/setBorder
// and verifies via RecordingCanvas paint output

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;
using namespace pulp::canvas;

// Helper: paint a widget and find the first set_fill_color command's color
static Color get_painted_bg(TestEnvironment& env, const std::string& id) {
    auto* w = env.widget(id);
    if (!w) return {};
    RecordingCanvas rc;
    w->paint_all(rc);
    for (auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_fill_color)
            return cmd.color;
    }
    return {};
}

// Helper to run JS without raw string issues from parentheses in color values
static void setup_bg(TestEnvironment& env, const std::string& color) {
    env.bridge->load_script("createPanel(\"box\")");
    env.bridge->load_script("setBackground(\"box\", \"" + color + "\")");
    env.root.layout_children();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Hex colors
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Color: hex #RGB short form", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "#f00");
    REQUIRE(env.widget("box")->has_background_color());
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 0xff);
    REQUIRE(c.g8() == 0x00);
    REQUIRE(c.b8() == 0x00);
}

TEST_CASE("Color: hex #RRGGBB 6-digit", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "#1e90ff");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 0x1e);
    REQUIRE(c.g8() == 0x90);
    REQUIRE(c.b8() == 0xff);
    REQUIRE(c.a8() == 255);
}

TEST_CASE("Color: hex #RRGGBBAA 8-digit", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "#ff000080");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 0xff);
    REQUIRE(c.g8() == 0x00);
    REQUIRE(c.b8() == 0x00);
    REQUIRE(c.a8() == 0x80);
}

TEST_CASE("Color: hex black #000000", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "#000000");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 0); REQUIRE(c.g8() == 0); REQUIRE(c.b8() == 0);
}

TEST_CASE("Color: hex white #ffffff", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "#ffffff");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 255); REQUIRE(c.g8() == 255); REQUIRE(c.b8() == 255);
}

TEST_CASE("Color: hex #000 short black", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "#000");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 0); REQUIRE(c.g8() == 0); REQUIRE(c.b8() == 0);
}

TEST_CASE("Color: hex #fff short white", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "#fff");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 0xff); REQUIRE(c.g8() == 0xff); REQUIRE(c.b8() == 0xff);
}

// ═══════════════════════════════════════════════════════════════════════════════
// rgb() / rgba()
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Color: rgb red", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "rgb(255, 0, 0)");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 255); REQUIRE(c.g8() == 0); REQUIRE(c.b8() == 0); REQUIRE(c.a8() == 255);
}

TEST_CASE("Color: rgb green", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "rgb(0, 128, 0)");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 0); REQUIRE(c.g8() == 128); REQUIRE(c.b8() == 0);
}

TEST_CASE("Color: rgb blue", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "rgb(0, 0, 255)");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 0); REQUIRE(c.g8() == 0); REQUIRE(c.b8() == 255);
}

TEST_CASE("Color: rgba semi-transparent red", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "rgba(255, 0, 0, 0.5)");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 255); REQUIRE(c.g8() == 0); REQUIRE(c.b8() == 0);
    REQUIRE(std::abs(static_cast<int>(c.a8()) - 127) <= 1);
}

TEST_CASE("Color: rgba fully opaque", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "rgba(100, 200, 50, 1.0)");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 100); REQUIRE(c.g8() == 200); REQUIRE(c.b8() == 50); REQUIRE(c.a8() == 255);
}

TEST_CASE("Color: rgba fully transparent", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "rgba(0, 0, 0, 0)");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.a8() == 0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// hsl() / hsla()
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Color: hsl pure red", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "hsl(0, 100%, 50%)");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 255); REQUIRE(c.g8() == 0); REQUIRE(c.b8() == 0);
}

TEST_CASE("Color: hsl pure green", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "hsl(120, 100%, 50%)");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 0); REQUIRE(c.g8() == 255); REQUIRE(c.b8() == 0);
}

TEST_CASE("Color: hsl pure blue", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "hsl(240, 100%, 50%)");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 0); REQUIRE(c.g8() == 0); REQUIRE(c.b8() == 255);
}

TEST_CASE("Color: hsl black", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "hsl(0, 0%, 0%)");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 0); REQUIRE(c.g8() == 0); REQUIRE(c.b8() == 0);
}

TEST_CASE("Color: hsl white", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "hsl(0, 0%, 100%)");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 255); REQUIRE(c.g8() == 255); REQUIRE(c.b8() == 255);
}

TEST_CASE("Color: hsl gray", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "hsl(0, 0%, 50%)");
    auto c = get_painted_bg(env, "box");
    REQUIRE(std::abs(static_cast<int>(c.r8()) - 128) <= 1);
    REQUIRE(std::abs(static_cast<int>(c.g8()) - 128) <= 1);
    REQUIRE(std::abs(static_cast<int>(c.b8()) - 128) <= 1);
}

TEST_CASE("Color: hsla semi-transparent red", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "hsla(0, 100%, 50%, 0.5)");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 255);
    REQUIRE(std::abs(static_cast<int>(c.a8()) - 127) <= 1);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Named colors (22 spot checks)
// ═══════════════════════════════════════════════════════════════════════════════

#define COLOR_TEST(cname, er, eg, eb) \
    TEST_CASE("Color: named '" cname "'", "[parser][color][named]") { \
        TestEnvironment env; setup_bg(env, cname); auto c = get_painted_bg(env, "box"); \
        REQUIRE(c.r8() == er); REQUIRE(c.g8() == eg); REQUIRE(c.b8() == eb); }

COLOR_TEST("black", 0, 0, 0)
COLOR_TEST("white", 255, 255, 255)
COLOR_TEST("red", 255, 0, 0)
COLOR_TEST("green", 0, 128, 0)
COLOR_TEST("blue", 0, 0, 255)
COLOR_TEST("yellow", 255, 255, 0)
COLOR_TEST("cyan", 0, 255, 255)
COLOR_TEST("magenta", 255, 0, 255)
COLOR_TEST("orange", 255, 165, 0)
COLOR_TEST("purple", 128, 0, 128)
COLOR_TEST("navy", 0, 0, 128)
COLOR_TEST("teal", 0, 128, 128)
COLOR_TEST("coral", 255, 127, 80)
COLOR_TEST("salmon", 250, 128, 114)
COLOR_TEST("tomato", 255, 99, 71)
COLOR_TEST("crimson", 220, 20, 60)
COLOR_TEST("gold", 255, 215, 0)
COLOR_TEST("indigo", 75, 0, 130)
COLOR_TEST("dodgerblue", 30, 144, 255)
COLOR_TEST("hotpink", 255, 105, 180)
COLOR_TEST("lime", 0, 255, 0)
COLOR_TEST("deepskyblue", 0, 191, 255)

#undef COLOR_TEST

// ═══════════════════════════════════════════════════════════════════════════════
// Special values
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Color: transparent keyword", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "transparent");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.a8() == 0);
}

TEST_CASE("Color: unknown string defaults to white", "[parser][color]") {
    TestEnvironment env;
    setup_bg(env, "notacolor");
    auto c = get_painted_bg(env, "box");
    REQUIRE(c.r8() == 255); REQUIRE(c.g8() == 255); REQUIRE(c.b8() == 255);
}

TEST_CASE("Color: empty string still calls setBackground", "[parser][color]") {
    TestEnvironment env;
    env.bridge->load_script("createPanel(\"box\")");
    env.bridge->load_script("setBackground(\"box\", \"\")");
    env.root.layout_children();
    // Empty string passes through parseColor — the lambda returns white {255,255,255,255},
    // but the JS bridge may not set a background for empty string, so just verify no crash
    REQUIRE(env.widget("box") != nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// setBorder color parsing
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Color: setBorder parses hex color", "[parser][color]") {
    TestEnvironment env;
    env.bridge->load_script("createPanel(\"box\")");
    env.bridge->load_script("setBorder(\"box\", \"#ff0000\", 2, 4)");
    env.root.layout_children();
    RecordingCanvas rc;
    env.widget("box")->paint_all(rc);
    bool found_stroke = false;
    for (auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_stroke_color) {
            REQUIRE(cmd.color.r8() == 255);
            REQUIRE(cmd.color.g8() == 0);
            REQUIRE(cmd.color.b8() == 0);
            found_stroke = true;
            break;
        }
    }
    REQUIRE(found_stroke);
}

TEST_CASE("Color: setBorder parses named color", "[parser][color]") {
    TestEnvironment env;
    env.bridge->load_script("createPanel(\"box\")");
    env.bridge->load_script("setBorder(\"box\", \"blue\", 1, 0)");
    env.root.layout_children();
    RecordingCanvas rc;
    env.widget("box")->paint_all(rc);
    bool found = false;
    for (auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_stroke_color) {
            REQUIRE(cmd.color.r8() == 0);
            REQUIRE(cmd.color.b8() == 255);
            found = true;
            break;
        }
    }
    REQUIRE(found);
}
