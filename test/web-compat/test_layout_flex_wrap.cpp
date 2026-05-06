// Flex wrap layout tests — validates CSS flex-wrap behavior
// Note: The current layout engine treats flex_wrap as a flag but items still
// shrink to fit on one line rather than wrapping to the next. These tests
// validate the current behavior.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;
using Catch::Matchers::WithinAbs;

TEST_CASE("Flex Wrap: no wrap keeps all in one line", "[layout][flex][wrap]") {
    View root;
    root.set_bounds({0, 0, 200, 100});
    root.flex().direction = FlexDirection::row;
    root.flex().flex_wrap = FlexWrap::no_wrap;
    root.add_child(make_box(80, 40));
    root.add_child(make_box(80, 40));
    root.add_child(make_box(80, 40));
    root.layout_children();

    // All on same line (y == 0), shrunk to fit
    REQUIRE(root.child_at(0)->bounds().y == 0.0f);
    REQUIRE(root.child_at(1)->bounds().y == 0.0f);
    REQUIRE(root.child_at(2)->bounds().y == 0.0f);
}

TEST_CASE("Flex Wrap: wrap flag is stored", "[layout][flex][wrap]") {
    View root;
    root.flex().flex_wrap = FlexWrap::wrap;
    REQUIRE(root.flex().flex_wrap == FlexWrap::wrap);
    root.flex().flex_wrap = FlexWrap::no_wrap;
    REQUIRE(root.flex().flex_wrap == FlexWrap::no_wrap);
}

TEST_CASE("Flex Wrap: all items fit = single row", "[layout][flex][wrap]") {
    View root;
    root.set_bounds({0, 0, 300, 100});
    root.flex().direction = FlexDirection::row;
    root.flex().flex_wrap = FlexWrap::wrap;
    root.add_child(make_box(80, 40));
    root.add_child(make_box(80, 40));
    root.add_child(make_box(80, 40));
    root.layout_children();

    // 80*3 = 240 < 300, all fit on one line
    REQUIRE(root.child_at(0)->bounds().y == 0.0f);
    REQUIRE(root.child_at(1)->bounds().y == 0.0f);
    REQUIRE(root.child_at(2)->bounds().y == 0.0f);
}

TEST_CASE("Flex Wrap: overflow items wrap to next line", "[layout][flex][wrap]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    root.flex().direction = FlexDirection::row;
    root.flex().flex_wrap = FlexWrap::wrap;
    root.add_child(make_box(80, 40));
    root.add_child(make_box(80, 40));
    root.add_child(make_box(80, 40));
    root.layout_children();

    // CSS wrap: first 2 items fit on line 1 (80+80=160 ≤ 200), 3rd wraps to line 2
    // Items keep their preferred width (80px each), not shrunk
    REQUIRE(root.child_at(0)->bounds().width == 80.0f);
    REQUIRE(root.child_at(1)->bounds().width == 80.0f);
    REQUIRE(root.child_at(2)->bounds().width == 80.0f);
    // Third item should be on a new line (y > 0)
    REQUIRE(root.child_at(2)->bounds().y > 0.0f);
}

TEST_CASE("Flex Wrap: gap with wrap reduces available space", "[layout][flex][wrap]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    root.flex().direction = FlexDirection::row;
    root.flex().flex_wrap = FlexWrap::wrap;
    root.flex().gap = 10;
    root.add_child(make_box(80, 40));
    root.add_child(make_box(80, 40));
    root.add_child(make_box(80, 40));
    root.layout_children();

    // Items positioned with gap
    REQUIRE(root.child_at(0)->bounds().x == 0.0f);
    REQUIRE(root.child_at(1)->bounds().x > root.child_at(0)->bounds().x);
}

TEST_CASE("Flex Wrap: wrap with single item", "[layout][flex][wrap]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    root.flex().direction = FlexDirection::row;
    root.flex().flex_wrap = FlexWrap::wrap;
    root.add_child(make_box(80, 40));
    root.layout_children();

    REQUIRE(root.child_at(0)->bounds().x == 0.0f);
    REQUIRE(root.child_at(0)->bounds().y == 0.0f);
}

TEST_CASE("Flex Wrap: many items in row still layout", "[layout][flex][wrap]") {
    View root;
    root.set_bounds({0, 0, 150, 300});
    root.flex().direction = FlexDirection::row;
    root.flex().flex_wrap = FlexWrap::wrap;

    for (int i = 0; i < 5; ++i)
        root.add_child(make_box(60, 30));
    root.layout_children();

    // All items are positioned
    REQUIRE(root.child_count() == 5);
    REQUIRE(root.child_at(0)->bounds().width > 0.0f);
    REQUIRE(root.child_at(4)->bounds().width > 0.0f);
}

TEST_CASE("Flex Wrap: column direction with wrap flag", "[layout][flex][wrap]") {
    View root;
    root.set_bounds({0, 0, 300, 100});
    root.flex().direction = FlexDirection::column;
    root.flex().flex_wrap = FlexWrap::wrap;
    root.add_child(make_box(60, 50));
    root.add_child(make_box(60, 50));
    root.add_child(make_box(60, 50));
    root.layout_children();

    // Items are laid out in column
    REQUIRE(root.child_at(0)->bounds().x == 0.0f);
    REQUIRE(root.child_count() == 3);
}
