// Grid layout tests — validates CSS Grid Level 1 subset

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;
using Catch::Matchers::WithinAbs;

TEST_CASE("Grid: 2-column equal fr", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("1fr 1fr");
    root.add_child(make_box(0, 50));
    root.add_child(make_box(0, 50));
    root.layout_children();

    REQUIRE_THAT(root.child_at(0)->bounds().width, WithinAbs(200.0f, 1.0f));
    REQUIRE_THAT(root.child_at(1)->bounds().width, WithinAbs(200.0f, 1.0f));
    REQUIRE_THAT(root.child_at(1)->bounds().x, WithinAbs(200.0f, 1.0f));
}

TEST_CASE("Grid: 3-column 1fr 2fr 1fr", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("1fr 2fr 1fr");
    root.add_child(make_box(0, 50));
    root.add_child(make_box(0, 50));
    root.add_child(make_box(0, 50));
    root.layout_children();

    REQUIRE_THAT(root.child_at(0)->bounds().width, WithinAbs(100.0f, 1.0f));
    REQUIRE_THAT(root.child_at(1)->bounds().width, WithinAbs(200.0f, 1.0f));
    REQUIRE_THAT(root.child_at(2)->bounds().width, WithinAbs(100.0f, 1.0f));
}

TEST_CASE("Grid: fixed + fr columns", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("100 1fr");
    root.add_child(make_box(0, 50));
    root.add_child(make_box(0, 50));
    root.layout_children();

    REQUIRE_THAT(root.child_at(0)->bounds().width, WithinAbs(100.0f, 1.0f));
    REQUIRE_THAT(root.child_at(1)->bounds().width, WithinAbs(300.0f, 1.0f));
}

TEST_CASE("Grid: column gap", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 410, 200});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("1fr 1fr");
    root.grid().column_gap = 10;
    root.add_child(make_box(0, 50));
    root.add_child(make_box(0, 50));
    root.layout_children();

    REQUIRE_THAT(root.child_at(0)->bounds().width, WithinAbs(200.0f, 1.0f));
    REQUIRE_THAT(root.child_at(1)->bounds().width, WithinAbs(200.0f, 1.0f));
    REQUIRE_THAT(root.child_at(1)->bounds().x, WithinAbs(210.0f, 1.0f));
}

TEST_CASE("Grid: row gap", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 200, 400});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("1fr");
    root.grid().row_gap = 20;
    root.add_child(make_box(0, 50));
    root.add_child(make_box(0, 50));
    root.layout_children();

    // Auto rows are 30px, so with 20px gap: row1 at y=0, row2 at y=30+20=50
    REQUIRE(root.child_at(0)->bounds().y == 0.0f);
    REQUIRE_THAT(root.child_at(1)->bounds().y, WithinAbs(50.0f, 1.0f));
}

TEST_CASE("Grid: auto row wrapping with 2 columns", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 200, 400});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("1fr 1fr");
    for (int i = 0; i < 4; ++i)
        root.add_child(make_box(0, 50));
    root.layout_children();

    // Implicit auto rows are 30px high
    REQUIRE(root.child_at(0)->bounds().y == 0.0f);
    REQUIRE(root.child_at(1)->bounds().y == 0.0f);
    // Row 2 at y=30 (auto row height)
    REQUIRE_THAT(root.child_at(2)->bounds().y, WithinAbs(30.0f, 1.0f));
    REQUIRE_THAT(root.child_at(3)->bounds().y, WithinAbs(30.0f, 1.0f));
}

TEST_CASE("Grid: implicit auto rows expand nested container content", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 200, 400});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("1fr");
    root.grid().row_gap = 6;

    auto card = std::make_unique<View>();
    card->flex().direction = FlexDirection::column;
    card->flex().gap = 4;
    card->flex().padding = 5;
    card->add_child(make_box(0, 40));
    card->add_child(make_box(0, 20));

    root.add_child(std::move(card));
    root.add_child(make_box(0, 50));
    root.layout_children();

    REQUIRE_THAT(root.child_at(0)->bounds().height, WithinAbs(74.0f, 1.0f));
    REQUIRE_THAT(root.child_at(1)->bounds().y, WithinAbs(80.0f, 1.0f));
    REQUIRE_THAT(root.child_at(1)->bounds().height, WithinAbs(30.0f, 1.0f));
}

TEST_CASE("Grid: auto wrapping with 3 columns and 7 items", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 300, 400});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("1fr 1fr 1fr");
    for (int i = 0; i < 7; ++i)
        root.add_child(make_box(0, 40));
    root.layout_children();

    // Implicit auto rows are 30px
    REQUIRE(root.child_at(0)->bounds().y == 0.0f);
    REQUIRE_THAT(root.child_at(3)->bounds().y, WithinAbs(30.0f, 1.0f));
    REQUIRE_THAT(root.child_at(6)->bounds().y, WithinAbs(60.0f, 1.0f));
}

TEST_CASE("Grid: column positions correct", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 300, 200});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("1fr 1fr 1fr");
    root.add_child(make_box(0, 50));
    root.add_child(make_box(0, 50));
    root.add_child(make_box(0, 50));
    root.layout_children();

    REQUIRE_THAT(root.child_at(0)->bounds().x, WithinAbs(0.0f, 1.0f));
    REQUIRE_THAT(root.child_at(1)->bounds().x, WithinAbs(100.0f, 1.0f));
    REQUIRE_THAT(root.child_at(2)->bounds().x, WithinAbs(200.0f, 1.0f));
}

TEST_CASE("Grid: all fixed columns", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("100 150 100");
    root.add_child(make_box(0, 50));
    root.add_child(make_box(0, 50));
    root.add_child(make_box(0, 50));
    root.layout_children();

    REQUIRE_THAT(root.child_at(0)->bounds().width, WithinAbs(100.0f, 1.0f));
    REQUIRE_THAT(root.child_at(1)->bounds().width, WithinAbs(150.0f, 1.0f));
    REQUIRE_THAT(root.child_at(2)->bounds().width, WithinAbs(100.0f, 1.0f));
}

TEST_CASE("Grid: gap with wrapping rows", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 200, 400});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("1fr 1fr");
    root.grid().column_gap = 10;
    root.grid().row_gap = 10;
    for (int i = 0; i < 4; ++i)
        root.add_child(make_box(0, 40));
    root.layout_children();

    // Column widths: (200-10)/2 = 95 each
    REQUIRE_THAT(root.child_at(0)->bounds().width, WithinAbs(95.0f, 1.0f));
    // Auto row height is 30px, so row 2 at y=30+10=40
    REQUIRE_THAT(root.child_at(2)->bounds().y, WithinAbs(40.0f, 1.0f));
}

TEST_CASE("Grid: single column (1fr)", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 200, 400});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("1fr");
    root.add_child(make_box(0, 50));
    root.add_child(make_box(0, 50));
    root.layout_children();

    REQUIRE_THAT(root.child_at(0)->bounds().width, WithinAbs(200.0f, 1.0f));
    REQUIRE(root.child_at(0)->bounds().y == 0.0f);
    // Auto row height is 30px
    REQUIRE_THAT(root.child_at(1)->bounds().y, WithinAbs(30.0f, 1.0f));
}

TEST_CASE("Grid: empty container no crash", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("1fr 1fr");
    root.layout_children();
    REQUIRE(root.child_count() == 0);
}

TEST_CASE("Grid: JS API creates grid and sets template", "[layout][grid]") {
    TestEnvironment env(400, 200);
    env.run(R"JS(
        createGrid("g");
        setGrid("g", "template_columns", "1fr 1fr");
        setFlex("g", "width", 400);
        setFlex("g", "height", 200);
        createPanel("a", "g");
        setFlex("a", "height", 50);
        createPanel("b", "g");
        setFlex("b", "height", 50);
    )JS");

    auto* g = env.widget("g");
    REQUIRE(g != nullptr);
    REQUIRE(g->grid().template_columns.size() == 2);
}
