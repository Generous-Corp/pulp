// test_widget_bridge_css_grid.cpp — extracted from test_widget_bridge.cpp
// in the 2026-05 Phase 5 (P5-1 follow-up) refactor.
//
// pulp #1434 Phase A2-2 — CSS Grid extended surface.
// Round-trips the CSS Grid shim through the bridge:
// grid-template-columns / grid-template-rows / grid-template-areas,
// grid-column / grid-row / grid-area placement shorthand, grid gap
// (row-gap, column-gap, gap), justify-* / align-* / place-*
// container + item alignment, repeat() + minmax() + fr-unit /
// minmax(0, 1fr) / auto sizing tokens.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <thread>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

// ── pulp #1434 Phase A2-2 — CSS Grid extended surface ──────────────────
//
// PR 1 of the multi-PR ladder. Builds on Pulp's existing grid layout
// (template_columns/rows + per-child column/row spans + col/row gaps)
// to add: grid-auto-columns, grid-auto-rows, grid-auto-flow,
// grid-template-areas (named-area parsing), grid-area shorthand
// (named token vs `row / col / row / col` numeric form).

TEST_CASE("setGrid auto_columns / auto_rows / auto_flow",
          "[view][bridge][css][issue-1434-grid]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        setGrid('a', 'auto_columns', '1fr');
        setGrid('a', 'auto_rows', '50px');
        setGrid('a', 'auto_flow', 'column dense');
    )");
    const auto& g = bridge.widget("a")->grid();
    REQUIRE(g.auto_columns.type == GridTrack::Type::fr);
    REQUIRE_THAT(g.auto_columns.value, WithinAbs(1.0f, 0.001f));
    REQUIRE(g.auto_rows.type == GridTrack::Type::fixed);
    REQUIRE_THAT(g.auto_rows.value, WithinAbs(50.0f, 0.001f));
    REQUIRE(g.auto_flow == GridStyle::AutoFlow::column_dense);
}

TEST_CASE("parse_template_areas: simple 3x3 grid",
          "[view][bridge][css][issue-1434-grid]") {
    auto areas = GridStyle::parse_template_areas(
        "'h h h' 'm c c' 'f f f'");
    // Three named areas: h (header), m (main), c (content), f (footer).
    REQUIRE(areas.size() == 4);
    auto find = [&](const std::string& n) -> const GridStyle::NamedArea* {
        for (const auto& a : areas) if (a.name == n) return &a;
        return nullptr;
    };
    auto* h = find("h");
    REQUIRE(h != nullptr);
    REQUIRE(h->row_start == 1);
    REQUIRE(h->col_start == 1);
    REQUIRE(h->row_end == 2);
    REQUIRE(h->col_end == 4);
    auto* c = find("c");
    REQUIRE(c != nullptr);
    REQUIRE(c->row_start == 2);
    REQUIRE(c->col_start == 2);
    REQUIRE(c->row_end == 3);
    REQUIRE(c->col_end == 4);
    auto* f = find("f");
    REQUIRE(f != nullptr);
    REQUIRE(f->row_start == 3);
    REQUIRE(f->col_end == 4);
}

TEST_CASE("parse_template_areas: '.' is the spacer token",
          "[view][bridge][css][issue-1434-grid]") {
    auto areas = GridStyle::parse_template_areas("'a . b'");
    REQUIRE(areas.size() == 2);
    REQUIRE(areas[0].name == "a");
    REQUIRE(areas[1].name == "b");
}

TEST_CASE("setGrid template_areas via bridge",
          "[view][bridge][css][issue-1434-grid]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        setGrid('a', 'template_areas', "'h h' 'm c'");
    )");
    REQUIRE(bridge.widget("a")->grid().template_areas.size() == 3);
}

TEST_CASE("setGrid grid_area: named-token form references a named area",
          "[view][bridge][css][issue-1434-grid]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        setGrid('a', 'grid_area', 'header');
    )");
    REQUIRE(bridge.widget("a")->grid().grid_area_name == "header");
}

TEST_CASE("setGrid grid_area: numeric '1 / 2 / 3 / 4' form sets bounds",
          "[view][bridge][css][issue-1434-grid]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        setGrid('a', 'grid_area', '1 / 2 / 3 / 4');
    )");
    const auto& g = bridge.widget("a")->grid();
    REQUIRE(g.grid_row_start == 1);
    REQUIRE(g.grid_column_start == 2);
    REQUIRE(g.grid_row_end == 3);
    REQUIRE(g.grid_column_end == 4);
}

TEST_CASE("CSSStyleDeclaration forwards gridTemplateAreas",
          "[view][bridge][css][issue-1434-grid]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        var s = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        s._applyProperty('gridTemplateAreas', "'h h' 'm c'");
        s._applyProperty('gridAutoFlow', 'column');
    )");
    REQUIRE(bridge.widget("a")->grid().template_areas.size() == 3);
    REQUIRE(bridge.widget("a")->grid().auto_flow == GridStyle::AutoFlow::column);
}

