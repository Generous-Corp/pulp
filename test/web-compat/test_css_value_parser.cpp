// CSS value parser tests — validates dimension, flex, and layout property parsing
// through the WidgetBridge JS API

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;
using Catch::Matchers::WithinAbs;

// ═══════════════════════════════════════════════════════════════════════════════
// setFlex dimension parsing
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS Value: setFlex width sets preferred_width", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "width", 100);
    )JS");
    auto* w = env.widget("box");
    REQUIRE(w != nullptr);
    REQUIRE(w->flex().preferred_width == 100.0f);
}

TEST_CASE("CSS Value: setFlex height sets preferred_height", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "height", 50);
    )JS");
    REQUIRE(env.widget("box")->flex().preferred_height == 50.0f);
}

TEST_CASE("CSS Value: setFlex min_width", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "min_width", 80);
    )JS");
    REQUIRE(env.widget("box")->flex().min_width == 80.0f);
}

TEST_CASE("CSS Value: setFlex min_height", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "min_height", 40);
    )JS");
    REQUIRE(env.widget("box")->flex().min_height == 40.0f);
}

TEST_CASE("CSS Value: setFlex max_width", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "max_width", 200);
    )JS");
    REQUIRE(env.widget("box")->flex().max_width == 200.0f);
}

TEST_CASE("CSS Value: setFlex max_height", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "max_height", 150);
    )JS");
    REQUIRE(env.widget("box")->flex().max_height == 150.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Flex grow / shrink / basis
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS Value: flex_grow default is 0", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(createPanel("box");)JS");
    REQUIRE(env.widget("box")->flex().flex_grow == 0.0f);
}

TEST_CASE("CSS Value: setFlex flex_grow", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "flex_grow", 2);
    )JS");
    REQUIRE(env.widget("box")->flex().flex_grow == 2.0f);
}

TEST_CASE("CSS Value: setFlex flex_shrink", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "flex_shrink", 0);
    )JS");
    REQUIRE(env.widget("box")->flex().flex_shrink == 0.0f);
}

TEST_CASE("CSS Value: flex_shrink default is 1", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(createPanel("box");)JS");
    REQUIRE(env.widget("box")->flex().flex_shrink == 1.0f);
}

TEST_CASE("CSS Value: setFlex flex_basis", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "flex_basis", 120);
    )JS");
    REQUIRE(env.widget("box")->flex().flex_basis == 120.0f);
}

TEST_CASE("CSS Value: flex_basis default is -1 (auto)", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(createPanel("box");)JS");
    REQUIRE(env.widget("box")->flex().flex_basis == -1.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Direction
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS Value: direction row via setFlex", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "direction", "row");
    )JS");
    REQUIRE(env.widget("box")->flex().direction == FlexDirection::row);
}

TEST_CASE("CSS Value: direction col via setFlex", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "direction", "col");
    )JS");
    REQUIRE(env.widget("box")->flex().direction == FlexDirection::column);
}

TEST_CASE("CSS Value: createRow sets row direction", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(createRow("r");)JS");
    REQUIRE(env.widget("r")->flex().direction == FlexDirection::row);
}

TEST_CASE("CSS Value: createCol sets column direction", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(createCol("c");)JS");
    REQUIRE(env.widget("c")->flex().direction == FlexDirection::column);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Alignment
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS Value: justify_content start", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "justify_content", "start");
    )JS");
    REQUIRE(env.widget("box")->flex().justify_content == FlexJustify::start);
}

TEST_CASE("CSS Value: justify_content center", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "justify_content", "center");
    )JS");
    REQUIRE(env.widget("box")->flex().justify_content == FlexJustify::center);
}

TEST_CASE("CSS Value: justify_content end", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "justify_content", "end");
    )JS");
    REQUIRE(env.widget("box")->flex().justify_content == FlexJustify::end_);
}

TEST_CASE("CSS Value: justify_content space-between", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "justify_content", "space-between");
    )JS");
    REQUIRE(env.widget("box")->flex().justify_content == FlexJustify::space_between);
}

TEST_CASE("CSS Value: justify_content space-around", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "justify_content", "space-around");
    )JS");
    REQUIRE(env.widget("box")->flex().justify_content == FlexJustify::space_around);
}

TEST_CASE("CSS Value: justify_content space-evenly", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "justify_content", "space-evenly");
    )JS");
    REQUIRE(env.widget("box")->flex().justify_content == FlexJustify::space_evenly);
}

TEST_CASE("CSS Value: align_items start", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "align_items", "start");
    )JS");
    REQUIRE(env.widget("box")->flex().align_items == FlexAlign::start);
}

TEST_CASE("CSS Value: align_items center", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "align_items", "center");
    )JS");
    REQUIRE(env.widget("box")->flex().align_items == FlexAlign::center);
}

TEST_CASE("CSS Value: align_items end", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "align_items", "end");
    )JS");
    REQUIRE(env.widget("box")->flex().align_items == FlexAlign::end);
}

TEST_CASE("CSS Value: align_items stretch", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "align_items", "stretch");
    )JS");
    REQUIRE(env.widget("box")->flex().align_items == FlexAlign::stretch);
}

TEST_CASE("CSS Value: align_self auto", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "align_self", "auto");
    )JS");
    REQUIRE(env.widget("box")->flex().align_self == FlexAlign::auto_);
}

TEST_CASE("CSS Value: align_self center", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "align_self", "center");
    )JS");
    REQUIRE(env.widget("box")->flex().align_self == FlexAlign::center);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Gap
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS Value: gap shorthand sets both row and column gap", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "gap", 10);
    )JS");
    REQUIRE(env.widget("box")->flex().gap == 10.0f);
}

TEST_CASE("CSS Value: row_gap", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "row_gap", 8);
    )JS");
    REQUIRE(env.widget("box")->flex().row_gap == 8.0f);
}

TEST_CASE("CSS Value: column_gap", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "column_gap", 12);
    )JS");
    REQUIRE(env.widget("box")->flex().column_gap == 12.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Padding (uniform + per-side)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS Value: padding uniform", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "padding", 16);
    )JS");
    REQUIRE(env.widget("box")->flex().padding == 16.0f);
}

TEST_CASE("CSS Value: padding_top", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "padding_top", 10);
    )JS");
    REQUIRE(env.widget("box")->flex().padding_top == 10.0f);
}

TEST_CASE("CSS Value: padding_right", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "padding_right", 20);
    )JS");
    REQUIRE(env.widget("box")->flex().padding_right == 20.0f);
}

TEST_CASE("CSS Value: padding_bottom", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "padding_bottom", 15);
    )JS");
    REQUIRE(env.widget("box")->flex().padding_bottom == 15.0f);
}

TEST_CASE("CSS Value: padding_left", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "padding_left", 25);
    )JS");
    REQUIRE(env.widget("box")->flex().padding_left == 25.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Margin (uniform + per-side)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS Value: margin uniform", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "margin", 8);
    )JS");
    REQUIRE(env.widget("box")->flex().margin == 8.0f);
}

TEST_CASE("CSS Value: margin_top", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "margin_top", 5);
    )JS");
    REQUIRE(env.widget("box")->flex().margin_top == 5.0f);
}

TEST_CASE("CSS Value: margin_right", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "margin_right", 10);
    )JS");
    REQUIRE(env.widget("box")->flex().margin_right == 10.0f);
}

TEST_CASE("CSS Value: margin_bottom", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "margin_bottom", 15);
    )JS");
    REQUIRE(env.widget("box")->flex().margin_bottom == 15.0f);
}

TEST_CASE("CSS Value: margin_left", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "margin_left", 20);
    )JS");
    REQUIRE(env.widget("box")->flex().margin_left == 20.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Flex wrap + order
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS Value: flex_wrap on", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "flex_wrap", 1);
    )JS");
    REQUIRE(env.widget("box")->flex().flex_wrap == FlexWrap::wrap);
}

TEST_CASE("CSS Value: flex_wrap off", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "flex_wrap", 0);
    )JS");
    REQUIRE(env.widget("box")->flex().flex_wrap == FlexWrap::no_wrap);
}

TEST_CASE("CSS Value: order", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "order", 3);
    )JS");
    REQUIRE(env.widget("box")->flex().order == 3);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Grid template parsing
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS Value: grid parse_template '1fr'", "[parser][grid]") {
    auto tracks = GridStyle::parse_template("1fr");
    REQUIRE(tracks.size() == 1);
    REQUIRE(tracks[0].type == GridTrack::Type::fr);
    REQUIRE(tracks[0].value == 1.0f);
}

TEST_CASE("CSS Value: grid parse_template '1fr 2fr'", "[parser][grid]") {
    auto tracks = GridStyle::parse_template("1fr 2fr");
    REQUIRE(tracks.size() == 2);
    REQUIRE(tracks[0].value == 1.0f);
    REQUIRE(tracks[1].value == 2.0f);
}

TEST_CASE("CSS Value: grid parse_template 'auto'", "[parser][grid]") {
    auto tracks = GridStyle::parse_template("auto");
    REQUIRE(tracks.size() == 1);
    REQUIRE(tracks[0].type == GridTrack::Type::auto_);
}

TEST_CASE("CSS Value: grid parse_template '100px'", "[parser][grid]") {
    auto tracks = GridStyle::parse_template("100");
    REQUIRE(tracks.size() == 1);
    REQUIRE(tracks[0].type == GridTrack::Type::fixed);
    REQUIRE(tracks[0].value == 100.0f);
}

TEST_CASE("CSS Value: grid parse_template mixed '1fr 2fr auto 100'", "[parser][grid]") {
    auto tracks = GridStyle::parse_template("1fr 2fr auto 100");
    REQUIRE(tracks.size() == 4);
    REQUIRE(tracks[0].type == GridTrack::Type::fr);
    REQUIRE(tracks[1].type == GridTrack::Type::fr);
    REQUIRE(tracks[2].type == GridTrack::Type::auto_);
    REQUIRE(tracks[3].type == GridTrack::Type::fixed);
}

TEST_CASE("CSS Value: grid template empty string", "[parser][grid]") {
    auto tracks = GridStyle::parse_template("");
    REQUIRE(tracks.empty());
}

TEST_CASE("CSS Value: grid template single auto", "[parser][grid]") {
    auto tracks = GridStyle::parse_template("auto");
    REQUIRE(tracks.size() == 1);
}

TEST_CASE("CSS Value: grid template '3fr 1fr 1fr'", "[parser][grid]") {
    auto tracks = GridStyle::parse_template("3fr 1fr 1fr");
    REQUIRE(tracks.size() == 3);
    REQUIRE(tracks[0].value == 3.0f);
    REQUIRE(tracks[1].value == 1.0f);
    REQUIRE(tracks[2].value == 1.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Opacity
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS Value: setOpacity via JS", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setOpacity("box", 0.5);
    )JS");
    REQUIRE_THAT(env.widget("box")->opacity(), WithinAbs(0.5f, 0.01f));
}

TEST_CASE("CSS Value: setOpacity clamps to 0", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setOpacity("box", -1);
    )JS");
    REQUIRE(env.widget("box")->opacity() == 0.0f);
}

TEST_CASE("CSS Value: setOpacity clamps to 1", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setOpacity("box", 2.0);
    )JS");
    REQUIRE(env.widget("box")->opacity() == 1.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Visibility
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS Value: setVisible false", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setVisible("box", false);
    )JS");
    REQUIRE(env.widget("box")->visible() == false);
}

TEST_CASE("CSS Value: setVisible true", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setVisible("box", false);
        setVisible("box", true);
    )JS");
    REQUIRE(env.widget("box")->visible() == true);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Widget creation validates ID
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS Value: createPanel returns correct ID", "[parser][value]") {
    TestEnvironment env;
    env.eval(R"JS(createPanel("my-panel");)JS");
    REQUIRE(env.widget("my-panel") != nullptr);
}

TEST_CASE("CSS Value: createRow returns correct ID", "[parser][value]") {
    TestEnvironment env;
    env.eval(R"JS(createRow("my-row");)JS");
    REQUIRE(env.widget("my-row") != nullptr);
}

TEST_CASE("CSS Value: createCol returns correct ID", "[parser][value]") {
    TestEnvironment env;
    env.eval(R"JS(createCol("my-col");)JS");
    REQUIRE(env.widget("my-col") != nullptr);
}

TEST_CASE("CSS Value: widget with unknown ID returns null", "[parser][value]") {
    TestEnvironment env;
    REQUIRE(env.widget("nonexistent") == nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Parent-child via JS API
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS Value: child created with parent ID", "[parser][value]") {
    TestEnvironment env;
    env.eval(R"JS(
        createRow("parent");
        createPanel("child", "parent");
    )JS");
    auto* parent = env.widget("parent");
    REQUIRE(parent != nullptr);
    REQUIRE(parent->child_count() == 1);
}

TEST_CASE("CSS Value: multiple children under parent", "[parser][value]") {
    TestEnvironment env;
    env.eval(R"JS(
        createCol("col");
        createPanel("a", "col");
        createPanel("b", "col");
        createPanel("c", "col");
    )JS");
    REQUIRE(env.widget("col")->child_count() == 3);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Overflow
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS Value: setOverflow hidden", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setOverflow("box", "hidden");
    )JS");
    REQUIRE(env.widget("box")->overflow() == View::Overflow::hidden);
}

TEST_CASE("CSS Value: setOverflow visible", "[parser][value]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setOverflow("box", "visible");
    )JS");
    REQUIRE(env.widget("box")->overflow() == View::Overflow::visible);
}
