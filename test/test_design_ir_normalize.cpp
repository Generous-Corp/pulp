// test_design_ir_normalize.cpp — isolated coverage for normalize_design_ir()
// (core/view/src/design_ir_normalize.cpp), the post-parse heuristic geometry
// pass extracted from parse_ir_node. Each rule is exercised directly on a
// hand-built IRNode — no JSON parse involved — which is exactly what the
// extraction bought: the rewrites were previously inline in the parser and
// untestable in isolation.

#include "../core/view/src/design_import_internal.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace pulp::view;

TEST_CASE("separator promotion: degenerate stroked image becomes a colored rect",
          "[design-ir-normalize]") {
    IRNode node;
    node.type = "image";
    node.style.width = 5e-6f;   // Figma vector line: effectively-zero width
    node.style.height = 120.0f;
    node.style.border_width = 1.0f;
    node.style.border_color = "#ff00ff";
    node.style.border = "1px solid #ff00ff";
    node.style.border_style = "solid";
    node.attributes["asset_ref"] = "asset-1.png";

    normalize_design_ir(node);

    // Stroke weight promoted to the visible dimension (min 1px).
    REQUIRE(node.style.width.has_value());
    CHECK(*node.style.width == 1.0f);
    CHECK(*node.style.height == 120.0f);
    // Stroke color became the fill; the stroke itself is dropped so codegen
    // does not double-draw the hairline.
    REQUIRE(node.style.background_color.has_value());
    CHECK(*node.style.background_color == "#ff00ff");
    CHECK_FALSE(node.style.border.has_value());
    CHECK_FALSE(node.style.border_color.has_value());
    CHECK_FALSE(node.style.border_width.has_value());
    CHECK_FALSE(node.style.border_style.has_value());
    // Degenerate PNG dropped; node demoted image -> frame and tagged.
    CHECK(node.attributes.count("asset_ref") == 0);
    CHECK(node.type == "frame");
    CHECK(node.attributes.at("__stroke_demoted") == "1");
}

TEST_CASE("separator promotion: auto-sized stroked frame is left alone",
          "[design-ir-normalize]") {
    // A nullopt (auto-sized) dimension must NOT be treated as 0 — that would
    // misfire on round-trip parses of legitimately auto-sized stroked frames.
    IRNode node;
    node.type = "frame";
    node.style.border_width = 1.0f;
    node.style.border_color = "#ffffff";
    node.style.height = 24.0f;  // width intentionally unset

    normalize_design_ir(node);

    CHECK_FALSE(node.style.width.has_value());
    CHECK(node.style.border_width.has_value());
    CHECK(node.style.border_color.has_value());
    CHECK(node.attributes.count("__stroke_demoted") == 0);
}

TEST_CASE("rounded corners propagate to an exactly-filling child",
          "[design-ir-normalize]") {
    IRNode parent;
    parent.style.border_radius = 8.0f;
    parent.style.width = 100.0f;
    parent.style.height = 50.0f;

    IRNode fill;  // gradient rect that exactly fills the parent
    fill.style.left = 0.0f;
    fill.style.top = 0.0f;
    fill.style.width = 100.0f;
    fill.style.height = 50.0f;

    IRNode offset = fill;  // same size but offset — must NOT inherit
    offset.style.left = 10.0f;

    IRNode own_radius = fill;  // child radius already set — respected
    own_radius.style.border_radius = 2.0f;

    parent.children = {fill, offset, own_radius};
    normalize_design_ir(parent);

    REQUIRE(parent.children[0].style.border_radius.has_value());
    CHECK(*parent.children[0].style.border_radius == 8.0f);
    CHECK_FALSE(parent.children[1].style.border_radius.has_value());
    CHECK(*parent.children[2].style.border_radius == 2.0f);
}

TEST_CASE("shadow snap closes the gap under a drop-shadowed sibling",
          "[design-ir-normalize]") {
    IRNode parent;

    IRNode panel;  // F: absolute child with a downward drop shadow
    panel.style.position = "absolute";
    panel.style.top = 0.0f;
    panel.style.height = 50.0f;
    IRBoxShadow sh;
    sh.offset_y = 4.0f;
    sh.blur = 8.0f;  // shadow reach = 4 + 8/2 = 8
    panel.style.box_shadow.push_back(sh);

    IRNode below;  // S: gap of 6 (< reach 8) -> snap up, preserving offset_y
    below.style.position = "absolute";
    below.style.top = 56.0f;
    below.style.height = 30.0f;

    parent.children = {panel, below};
    normalize_design_ir(parent);

    // close = gap - offset_y = 6 - 4 = 2 -> new top 54.
    REQUIRE(parent.children[1].style.top.has_value());
    CHECK_THAT(*parent.children[1].style.top,
               Catch::Matchers::WithinAbs(54.0, 1e-4));
}

TEST_CASE("shadow snap leaves a gap beyond the shadow reach alone",
          "[design-ir-normalize]") {
    IRNode parent;

    IRNode panel;
    panel.style.position = "absolute";
    panel.style.top = 0.0f;
    panel.style.height = 50.0f;
    IRBoxShadow sh;
    sh.offset_y = 4.0f;
    sh.blur = 8.0f;
    panel.style.box_shadow.push_back(sh);

    IRNode below;
    below.style.position = "absolute";
    below.style.top = 70.0f;  // gap 20 >= reach 8 -> untouched
    below.style.height = 30.0f;

    parent.children = {panel, below};
    normalize_design_ir(parent);

    CHECK_THAT(*parent.children[1].style.top,
               Catch::Matchers::WithinAbs(70.0, 1e-4));
}

TEST_CASE("connector hairline spans the row and centers vertically",
          "[design-ir-normalize]") {
    IRNode row;
    row.layout.direction = LayoutDirection::row;
    row.style.width = 400.0f;
    row.style.height = 40.0f;

    IRNode line;  // horizontal hairline first child
    line.style.width = 100.0f;
    line.style.height = 2.0f;

    IRNode box;  // widget-sized followers that sit ON TOP of the line
    box.style.width = 80.0f;
    box.style.height = 32.0f;

    row.children = {line, box, box, box};
    normalize_design_ir(row);

    auto& first = row.children.front();
    REQUIRE(first.style.position.has_value());
    CHECK(*first.style.position == "absolute");
    CHECK(*first.style.left == 0.0f);
    CHECK_THAT(*first.style.top, Catch::Matchers::WithinAbs(19.0, 1e-4));
    CHECK(*first.style.width == 400.0f);  // full row span
    CHECK(*first.style.height == 2.0f);   // stroke weight kept
}

TEST_CASE("connector span pulls back before a trailing add affordance",
          "[design-ir-normalize]") {
    IRNode row;
    row.layout.direction = LayoutDirection::row;
    row.layout.gap = 10.0f;
    row.style.width = 400.0f;
    row.style.height = 40.0f;

    IRNode line;
    line.style.width = 100.0f;
    line.style.height = 2.0f;

    IRNode box;
    box.style.width = 80.0f;
    box.style.height = 32.0f;

    IRNode add;  // "+" affordance: <= 60% of the median follower width
    add.style.width = 24.0f;
    add.style.height = 32.0f;

    row.children = {line, box, box, add};
    normalize_design_ir(row);

    // span = row_w - last - gap = 400 - 24 - 10 = 366.
    auto& first = row.children.front();
    REQUIRE(first.style.position.has_value());
    CHECK_THAT(*first.style.width, Catch::Matchers::WithinAbs(366.0, 1e-4));
}

TEST_CASE("wide first child is not treated as a connector",
          "[design-ir-normalize]") {
    // A first child >= 50% of the row width is a content element (progress
    // bar, slider track, divider) that must keep participating in flex.
    IRNode row;
    row.layout.direction = LayoutDirection::row;
    row.style.width = 400.0f;
    row.style.height = 40.0f;

    IRNode track;
    track.style.width = 300.0f;  // > 50% of 400
    track.style.height = 2.0f;

    IRNode box;
    box.style.width = 80.0f;
    box.style.height = 32.0f;

    row.children = {track, box, box};
    normalize_design_ir(row);

    CHECK_FALSE(row.children.front().style.position.has_value());
    CHECK(*row.children.front().style.width == 300.0f);
}
