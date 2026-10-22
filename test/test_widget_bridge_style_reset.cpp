// resetStyleProperty and the removal resets @pulp/react emits.
//
// When a style key disappears from a React commit the applier resets its
// slots; layout slots go through resetStyleProperty, which restores the
// value the widget had before its first layout write.

#include <catch2/catch_test_macros.hpp>

#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#include "../core/view/src/widget_bridge/layout_baselines.hpp"

#include <cstdint>
#include <string>

using namespace pulp::view;
using pulp::state::StateStore;

namespace {

struct Harness {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge;

    Harness() : bridge(engine, root, store) {
        root.set_bounds({0, 0, 320, 200});
    }

    bool reset(const std::string& id, const std::string& slot) {
        return engine.evaluate("resetStyleProperty('" + id + "', '" + slot + "')")
            .getWithDefault<bool>(false);
    }
};

} // namespace

TEST_CASE("resetStyleProperty restores the layout a widget was created with",
          "[view][bridge][style-reset]") {
    Harness h;
    h.bridge.load_script(R"(
        createRow('row', '');
        createCol('col', '');
        setFlex('row', 'direction', 'column');
        setFlex('col', 'direction', 'row');
    )");
    REQUIRE(h.bridge.widget("row")->flex().direction == FlexDirection::column);

    // A createRow starts as a row, so that is what a removed direction means;
    // FlexStyle's own default would be column.
    REQUIRE(h.reset("row", "direction"));
    CHECK(h.bridge.widget("row")->flex().direction == FlexDirection::row);
    REQUIRE(h.reset("col", "direction"));
    CHECK(h.bridge.widget("col")->flex().direction == FlexDirection::column);
}

TEST_CASE("resetStyleProperty hands a per-edge padding back to the shorthand",
          "[view][bridge][style-reset]") {
    Harness h;
    h.bridge.load_script(R"(
        createCol('box', '');
        setFlex('box', 'padding', 8);
        setFlex('box', 'padding_top', '25%');
        setFlex('box', 'margin_left', 12);
        setFlex('box', 'width', '50%');
        setFlex('box', 'aspect_ratio', 2);
        setBoxSizing('box', 'content-box');
    )");
    auto& f = h.bridge.widget("box")->flex();
    REQUIRE(f.dim_padding_top.unit == DimensionUnit::percent);

    REQUIRE(h.reset("box", "padding_top"));
    CHECK(f.padding_top == -1.0f);
    CHECK(f.dim_padding_top.unit == DimensionUnit::px);
    CHECK(f.dim_padding_top.value == 0.0f);
    // The uniform padding the edge falls back to is untouched.
    CHECK(f.padding == 8.0f);

    REQUIRE(h.reset("box", "margin_left"));
    CHECK(f.margin_left == -1.0f);
    REQUIRE(h.reset("box", "width"));
    CHECK(f.dim_width.unit == DimensionUnit::px);
    CHECK(f.dim_width.value == 0.0f);
    CHECK(f.preferred_width == 0.0f);
    REQUIRE(h.reset("box", "aspect_ratio"));
    CHECK_FALSE(f.aspect_ratio.has_value());
    REQUIRE(h.reset("box", "box_sizing"));
    CHECK(f.box_sizing == BoxSizing::border_box);
}

TEST_CASE("resetStyleProperty restores grid tracks and placement", "[view][bridge][style-reset]") {
    Harness h;
    h.bridge.load_script(R"(
        createGrid('grid', '');
        createCol('cell', 'grid');
        setGrid('grid', 'template_columns', '1fr 2fr 100px');
        setGrid('grid', 'gap', 6);
        setGrid('cell', 'grid_area', '1 / 2 / 3 / 4');
    )");
    auto& grid = h.bridge.widget("grid")->grid();
    auto& cell = h.bridge.widget("cell")->grid();
    REQUIRE(grid.template_columns.size() == 3);
    REQUIRE(cell.grid_column_start == 2);

    REQUIRE(h.reset("grid", "grid.template_columns"));
    CHECK(grid.template_columns.empty());
    REQUIRE(h.reset("grid", "grid.gap"));
    CHECK(grid.column_gap == 0.0f);
    CHECK(grid.row_gap == 0.0f);
    REQUIRE(h.reset("cell", "grid.grid_area"));
    CHECK(cell.grid_row_start == 0);
    CHECK(cell.grid_column_start == 0);
    CHECK(cell.grid_row_end == 0);
    CHECK(cell.grid_column_end == 0);
}

TEST_CASE("resetStyleProperty stops a CSS animation", "[view][bridge][style-reset]") {
    Harness h;
    h.bridge.load_script(R"(
        defineKeyframes('fade', JSON.stringify([
            { offset: 0,   properties: { opacity: '0' } },
            { offset: 1.0, properties: { opacity: '1' } }
        ]));
        createPanel('a', '');
        setAnimation('a', 'fade', 0.4, 1, 'normal');
    )");
    REQUIRE(h.bridge.widget("a")->active_animations().size() == 1);
    REQUIRE(h.reset("a", "animation"));
    CHECK(h.bridge.widget("a")->active_animations().empty());
}

TEST_CASE("resetStyleProperty refuses an unknown slot or widget", "[view][bridge][style-reset]") {
    Harness h;
    h.bridge.load_script("createCol('box', ''); setFlex('box', 'gap', 4);");
    CHECK_FALSE(h.reset("box", "gapp"));
    CHECK_FALSE(h.reset("box", "grid.nope"));
    CHECK_FALSE(h.reset("missing", "gap"));
    // The refusal leaves the slot alone.
    CHECK(h.bridge.widget("box")->flex().gap == 4.0f);
}

TEST_CASE("layout baselines are dropped when the bridge forgets a widget",
          "[view][bridge][style-reset]") {
    Harness h;
    const auto before = layout_baseline_count();
    h.bridge.load_script("createCol('box', ''); setFlex('box', 'gap', 4);");
    REQUIRE(layout_baseline_count() == before + 1);
    // A second write keeps the first baseline rather than adding another.
    h.bridge.load_script("setFlex('box', 'gap', 9);");
    REQUIRE(layout_baseline_count() == before + 1);
    h.bridge.load_script("removeWidget('box');");
    CHECK(layout_baseline_count() == before);
}
