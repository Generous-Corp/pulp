// CSS shorthand expansion tests — validates margin/padding uniform vs per-side,
// gap shorthand, and flex property resolution

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;
using Catch::Matchers::WithinAbs;

// ═══════════════════════════════════════════════════════════════════════════════
// Margin shorthand: uniform applies to all sides
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Shorthand: uniform margin resolves to all sides", "[parser][shorthand]") {
    View v;
    v.flex().margin = 10;
    REQUIRE(v.flex().margin_t() == 10.0f);
    REQUIRE(v.flex().margin_r() == 10.0f);
    REQUIRE(v.flex().margin_b() == 10.0f);
    REQUIRE(v.flex().margin_l() == 10.0f);
}

TEST_CASE("Shorthand: per-side margin overrides uniform", "[parser][shorthand]") {
    View v;
    v.flex().margin = 10;
    v.flex().margin_top = 5;
    REQUIRE(v.flex().margin_t() == 5.0f);
    REQUIRE(v.flex().margin_r() == 10.0f);
    REQUIRE(v.flex().margin_b() == 10.0f);
    REQUIRE(v.flex().margin_l() == 10.0f);
}

TEST_CASE("Shorthand: all sides override uniform margin", "[parser][shorthand]") {
    View v;
    v.flex().margin = 10;
    v.flex().margin_top = 1;
    v.flex().margin_right = 2;
    v.flex().margin_bottom = 3;
    v.flex().margin_left = 4;
    REQUIRE(v.flex().margin_t() == 1.0f);
    REQUIRE(v.flex().margin_r() == 2.0f);
    REQUIRE(v.flex().margin_b() == 3.0f);
    REQUIRE(v.flex().margin_l() == 4.0f);
}

TEST_CASE("Shorthand: margin -1 means use uniform", "[parser][shorthand]") {
    View v;
    v.flex().margin = 20;
    v.flex().margin_top = -1;  // should fall back to uniform
    REQUIRE(v.flex().margin_t() == 20.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Padding shorthand
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Shorthand: padding uniform via FlexStyle", "[parser][shorthand]") {
    FlexStyle fs;
    fs.padding = 16;
    // Per-side defaults to -1, so should resolve to uniform
    REQUIRE(fs.padding_top == -1.0f);
    REQUIRE(fs.padding_right == -1.0f);
    REQUIRE(fs.padding_bottom == -1.0f);
    REQUIRE(fs.padding_left == -1.0f);
    REQUIRE(fs.padding == 16.0f);
}

TEST_CASE("Shorthand: per-side padding set individually", "[parser][shorthand]") {
    FlexStyle fs;
    fs.padding = 0;
    fs.padding_top = 10;
    fs.padding_right = 20;
    fs.padding_bottom = 30;
    fs.padding_left = 40;
    REQUIRE(fs.padding_top == 10.0f);
    REQUIRE(fs.padding_right == 20.0f);
    REQUIRE(fs.padding_bottom == 30.0f);
    REQUIRE(fs.padding_left == 40.0f);
}

TEST_CASE("Shorthand: padding via JS setFlex uniform then per-side", "[parser][shorthand]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "padding", 10);
        setFlex("box", "padding_top", 5);
    )JS");
    auto* w = env.widget("box");
    REQUIRE(w->flex().padding == 10.0f);
    REQUIRE(w->flex().padding_top == 5.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Gap shorthand
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Shorthand: gap uniform applies to effective_gap for row", "[parser][shorthand]") {
    FlexStyle fs;
    fs.gap = 10;
    REQUIRE(fs.effective_gap(FlexDirection::row) == 10.0f);
    REQUIRE(fs.effective_gap(FlexDirection::column) == 10.0f);
}

TEST_CASE("Shorthand: column_gap overrides gap for row direction", "[parser][shorthand]") {
    FlexStyle fs;
    fs.gap = 10;
    fs.column_gap = 20;
    REQUIRE(fs.effective_gap(FlexDirection::row) == 20.0f);
    REQUIRE(fs.effective_gap(FlexDirection::column) == 10.0f);
}

TEST_CASE("Shorthand: row_gap overrides gap for column direction", "[parser][shorthand]") {
    FlexStyle fs;
    fs.gap = 10;
    fs.row_gap = 15;
    REQUIRE(fs.effective_gap(FlexDirection::row) == 10.0f);
    REQUIRE(fs.effective_gap(FlexDirection::column) == 15.0f);
}

TEST_CASE("Shorthand: both row_gap and column_gap override gap", "[parser][shorthand]") {
    FlexStyle fs;
    fs.gap = 10;
    fs.row_gap = 5;
    fs.column_gap = 20;
    REQUIRE(fs.effective_gap(FlexDirection::row) == 20.0f);
    REQUIRE(fs.effective_gap(FlexDirection::column) == 5.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// flex_basis vs preferred_width resolution
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Shorthand: basis_or_preferred uses flex_basis when set", "[parser][shorthand]") {
    FlexStyle fs;
    fs.flex_basis = 100;
    fs.preferred_width = 200;
    REQUIRE(fs.basis_or_preferred(true) == 100.0f);   // row → width
    REQUIRE(fs.basis_or_preferred(false) == 100.0f);   // col → flex_basis still wins
}

TEST_CASE("Shorthand: basis_or_preferred falls back to preferred_width for row", "[parser][shorthand]") {
    FlexStyle fs;
    fs.flex_basis = -1;  // auto
    fs.preferred_width = 200;
    fs.preferred_height = 50;
    REQUIRE(fs.basis_or_preferred(true) == 200.0f);
}

TEST_CASE("Shorthand: basis_or_preferred falls back to preferred_height for col", "[parser][shorthand]") {
    FlexStyle fs;
    fs.flex_basis = -1;  // auto
    fs.preferred_width = 200;
    fs.preferred_height = 50;
    REQUIRE(fs.basis_or_preferred(false) == 50.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Margin shorthand via JS API
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Shorthand: JS margin then margin_top", "[parser][shorthand]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "margin", 12);
        setFlex("box", "margin_top", 4);
    )JS");
    auto& f = env.widget("box")->flex();
    REQUIRE(f.margin == 12.0f);
    REQUIRE(f.margin_t() == 4.0f);
    REQUIRE(f.margin_r() == 12.0f);
}

TEST_CASE("Shorthand: JS all margins individual", "[parser][shorthand]") {
    TestEnvironment env;
    env.run(R"JS(
        createPanel("box");
        setFlex("box", "margin_top", 1);
        setFlex("box", "margin_right", 2);
        setFlex("box", "margin_bottom", 3);
        setFlex("box", "margin_left", 4);
    )JS");
    auto& f = env.widget("box")->flex();
    REQUIRE(f.margin_t() == 1.0f);
    REQUIRE(f.margin_r() == 2.0f);
    REQUIRE(f.margin_b() == 3.0f);
    REQUIRE(f.margin_l() == 4.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Grid shorthand
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Shorthand: setGrid template_columns", "[parser][shorthand]") {
    TestEnvironment env;
    env.run(R"JS(
        createGrid("g");
        setGrid("g", "template_columns", "1fr 2fr 1fr");
    )JS");
    auto& gs = env.widget("g")->grid();
    REQUIRE(gs.template_columns.size() == 3);
    REQUIRE(gs.template_columns[0].value == 1.0f);
    REQUIRE(gs.template_columns[1].value == 2.0f);
    REQUIRE(gs.template_columns[2].value == 1.0f);
}

TEST_CASE("Shorthand: setGrid template_rows", "[parser][shorthand]") {
    TestEnvironment env;
    env.run(R"JS(
        createGrid("g");
        setGrid("g", "template_rows", "auto 100 auto");
    )JS");
    auto& gs = env.widget("g")->grid();
    REQUIRE(gs.template_rows.size() == 3);
    REQUIRE(gs.template_rows[0].type == GridTrack::Type::auto_);
    REQUIRE(gs.template_rows[1].type == GridTrack::Type::fixed);
    REQUIRE(gs.template_rows[2].type == GridTrack::Type::auto_);
}

TEST_CASE("Shorthand: setGrid gap sets both column and row gap", "[parser][shorthand]") {
    TestEnvironment env;
    env.run(R"JS(
        createGrid("g");
        setGrid("g", "gap", 8);
    )JS");
    auto& gs = env.widget("g")->grid();
    REQUIRE(gs.column_gap == 8.0f);
    REQUIRE(gs.row_gap == 8.0f);
}

TEST_CASE("Shorthand: setGrid column_gap and row_gap individually", "[parser][shorthand]") {
    TestEnvironment env;
    env.run(R"JS(
        createGrid("g");
        setGrid("g", "column_gap", 10);
        setGrid("g", "row_gap", 20);
    )JS");
    auto& gs = env.widget("g")->grid();
    REQUIRE(gs.column_gap == 10.0f);
    REQUIRE(gs.row_gap == 20.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Default values
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Shorthand: FlexStyle defaults", "[parser][shorthand]") {
    FlexStyle fs;
    REQUIRE(fs.direction == FlexDirection::column);
    REQUIRE(fs.align_items == FlexAlign::stretch);
    REQUIRE(fs.align_self == FlexAlign::auto_);
    REQUIRE(fs.justify_content == FlexJustify::start);
    REQUIRE(fs.gap == 0.0f);
    REQUIRE(fs.padding == 0.0f);
    REQUIRE(fs.margin == 0.0f);
    REQUIRE(fs.flex_grow == 0.0f);
    REQUIRE(fs.flex_shrink == 1.0f);
    REQUIRE(fs.flex_basis == -1.0f);
    REQUIRE(fs.min_width == 0.0f);
    REQUIRE(fs.max_width == 0.0f);
    REQUIRE(fs.flex_wrap == FlexWrap::no_wrap);
    REQUIRE(fs.order == 0);
}

TEST_CASE("Shorthand: GridStyle defaults", "[parser][shorthand]") {
    GridStyle gs;
    REQUIRE(gs.template_columns.empty());
    REQUIRE(gs.template_rows.empty());
    REQUIRE(gs.column_gap == 0.0f);
    REQUIRE(gs.row_gap == 0.0f);
    REQUIRE(gs.grid_column_start == 0);
    REQUIRE(gs.grid_column_end == 0);
    REQUIRE(gs.grid_row_start == 0);
    REQUIRE(gs.grid_row_end == 0);
}
