// resetStyleProperty and the removal resets @pulp/react emits.
//
// When a style key disappears from a React commit the applier resets its
// slots; layout slots go through resetStyleProperty, which restores the
// value the widget had before its first layout write. The pixel case replays
// the bridge calls the applier emits (recorded in
// packages/pulp-react/test/fixtures/style-reset-toggle.json, kept current by
// prop-applier-disappearing-style.test.ts) and checks that a widget toggled
// on and back off renders exactly like one that was never styled.

#include <catch2/catch_test_macros.hpp>

#include <pulp/state/store.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#include "../core/view/src/widget_bridge/layout_baselines.hpp"

#include <choc/text/choc_JSON.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

choc::value::Value load_fixture() {
    const auto path = std::filesystem::path(PULP_SOURCE_DIR) / "packages" / "pulp-react" / "test" /
                      "fixtures" / "style-reset-toggle.json";
    std::ifstream in(path);
    REQUIRE(in.good());
    std::stringstream text;
    text << in.rdbuf();
    return choc::json::parse(text.str());
}

// Replay recorded `[fn, ...args]` calls as JS against the bridge globals.
void replay(WidgetBridge& bridge, const choc::value::ValueView& calls) {
    std::string js;
    for (uint32_t i = 0; i < calls.size(); ++i) {
        const auto call = calls[i];
        js += std::string(call[0].getString()) + "(";
        for (uint32_t a = 1; a < call.size(); ++a) {
            if (a > 1)
                js += ", ";
            js += choc::json::toString(call[a], false);
        }
        js += ");\n";
    }
    bridge.load_script(js);
}

std::vector<uint8_t> render_scene(const choc::value::ValueView& fixture,
                                  const std::vector<choc::value::ValueView>& steps) {
    Harness h;
    replay(h.bridge, fixture["widgets"]);
    for (const auto& step : steps)
        replay(h.bridge, step);
    h.root.layout_children();
    uint32_t w = 0, hgt = 0;
    return render_to_rgba(h.root, 320, 200, 1.0f, &w, &hgt);
}

// Pixels (RGBA8 quads) that differ between two equal-sized renders.
std::size_t differing_pixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::size_t count = 0;
    for (std::size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4) {
        if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] || a[i + 3] != b[i + 3])
            ++count;
    }
    return count;
}

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

TEST_CASE("resetStyleProperty on an upgraded scroll container restores its starting layout",
          "[view][bridge][style-reset]") {
    Harness h;
    h.bridge.load_script(R"(
        var panel = document.createElement('div');
        document.body.appendChild(panel);
        globalThis.panelId = panel._id;
        setFlex(panelId, 'width', 120);
        setFlex(panelId, 'margin_top', 9);
        setFlex(panelId, 'padding_top', 7);
    )");
    const auto id = h.engine.evaluate("panelId").getWithDefault<std::string>("");
    REQUIRE_FALSE(id.empty());
    h.engine.evaluate("__domAppend('', panelId, 'div', 'scroll')");
    auto* wrapper = h.bridge.scroll_wrapper(id);
    REQUIRE(wrapper != nullptr);
    auto* content = h.bridge.widget(id);
    REQUIRE(wrapper->flex().preferred_width == 120.0f);

    // Non-padding slots answer from the wrapper, which took the authored
    // style over at the upgrade but must reset to the content's start.
    REQUIRE(h.reset(id, "width"));
    CHECK(wrapper->flex().preferred_width == 0.0f);
    CHECK(wrapper->flex().dim_width.value == 0.0f);
    REQUIRE(h.reset(id, "margin_top"));
    CHECK(wrapper->flex().margin_top == -1.0f);
    // Padding answers from the inner box.
    REQUIRE(h.reset(id, "padding_top"));
    CHECK(content->flex().padding_top == -1.0f);
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

TEST_CASE("layout baselines are dropped when the bridge is destroyed",
          "[view][bridge][style-reset]") {
    const auto before = layout_baseline_count();
    {
        Harness h;
        h.bridge.load_script("createCol('a', ''); createCol('b', 'a'); "
                             "setFlex('a', 'gap', 4); setFlex('b', 'gap', 2);");
        REQUIRE(layout_baseline_count() == before + 2);
    }
    CHECK(layout_baseline_count() == before);
}

TEST_CASE("a style toggled on and off renders like one never styled",
          "[view][bridge][style-reset][pixel]") {
    if (!raw_rgba_render_available())
        SKIP("raw-RGBA capture unavailable in this build");
    const auto fixture = load_fixture();

    const auto neutral = render_scene(fixture, {fixture["neutral"]});
    const auto active = render_scene(fixture, {fixture["active"]});
    const auto toggled = render_scene(fixture, {fixture["active"], fixture["deactivate"]});
    REQUIRE_FALSE(neutral.empty());
    REQUIRE(active.size() == neutral.size());
    REQUIRE(toggled.size() == neutral.size());

    // The active style is visible, so the comparison below can fail.
    REQUIRE(differing_pixels(active, neutral) > 0);
    CHECK(differing_pixels(toggled, neutral) == 0);

    // Base keys that share a slot with an active key (paddingTop with
    // padding, borderRadius with border) are re-applied after the reset;
    // without that the render lands on the bare slot instead.
    const auto no_reapply =
        render_scene(fixture, {fixture["active"], fixture["withoutSurvivorReapply"]});
    CHECK(differing_pixels(no_reapply, neutral) > 0);

    // Each active key on its own still changes the render, so a reset that
    // stopped working for any one of them would break the equality above.
    const auto leave_one = fixture["leaveOneActive"];
    REQUIRE(leave_one.size() > 0);
    for (uint32_t i = 0; i < leave_one.size(); ++i) {
        const auto member = leave_one.getObjectMemberAt(i);
        INFO("active key left on: " << member.name);
        const auto partial = render_scene(fixture, {fixture["active"], member.value});
        CHECK(differing_pixels(partial, neutral) > 0);
    }
}
