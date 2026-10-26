// SPDX-License-Identifier: MIT
//
// Border, radius and shadow lengths under a CSS `transform: scale()`.
//
// Chrome scales a box's border, corner radii and shadow along with the box.
// The DOM snapshot reports the box post-transform but every one of those
// lengths pre-transform, so a lowering that places the box and copies the
// lengths draws a 1px border inside a 0.3px hairline, which native layout then
// widens to the 2px the two borders need.
//
// `paint-scale/paint.html` is a real Chrome capture of seven boxes:
//
//   #hair    scaleX(0.05) on a 4px box with a 1px border
//   #card    a rounded, shadowed card inside a scale(0.5) wrapper
//   #squash  scale(0.5, 1.5) on a bordered, rounded box
//   #hard    scale(2, 1) under a hard shadow (no blur, no spread)
//   #soft    scale(2, 1) under a soft shadow
//   #skewed  skewX(20deg) on a bordered box
//   #plain   the card again with no transform above it: the control
//
// A border scales per side and a hard shadow per axis. An elliptical corner,
// a soft shadow under two axes, and a skew cannot be carried by one factor per
// axis, so they are refused and recorded rather than approximated.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "tools/import-design/browser_capture_ir.hpp"
#include "tools/import-design/browser_capture_styles.hpp"

#include <pulp/view/design_import.hpp>

#include <filesystem>
#include <string>
#include <string_view>

using namespace pulp::import_design;
using Catch::Matchers::WithinAbs;
using pulp::view::IRNode;

namespace {

namespace fs = std::filesystem;

fs::path fixture_dir() {
    return fs::path(PULP_BROWSER_CAPTURE_FIXTURE_ROOT) / "browser-capture-paint-scale";
}

BrowserCaptureIrResult lower_fixture(std::string_view fixture) {
    BrowserCaptureIrOptions options;
    options.native_panel_lowering = true;
    return lower_browser_capture_to_ir(
        fs::path(PULP_BROWSER_CAPTURE_FIXTURE_ROOT) / fixture / "capture.json", options);
}

const IRNode* find_named(const IRNode& root, std::string_view name) {
    for (const auto& child : root.children) {
        if (child.name == name)
            return &child;
        if (const auto* found = find_named(child, name))
            return found;
    }
    return nullptr;
}

std::string attribute(const IRNode& node, const std::string& key) {
    const auto it = node.attributes.find(key);
    return it == node.attributes.end() ? std::string{} : it->second;
}

/// The node index of the element whose `id` is `id`.
int node_with_id(const CapturedStyleIndex& index, std::string_view id) {
    for (int node = 0; node < index.node_count(); ++node)
        if (index.attribute(node, "id") == id)
            return node;
    FAIL("fixture has no #" << id);
    return -1;
}

} // namespace

TEST_CASE("a node's paint scale is its own transform times its ancestors'",
          "[browser-capture][native-lowering][transform]") {
    auto index = CapturedStyleIndex::load(fixture_dir() / "dom-snapshot.json");
    REQUIRE(index.has_value());

    const auto hair = index->inherited_paint_scale(node_with_id(*index, "hair"));
    REQUIRE(hair.ok());
    CHECK_THAT(hair.x, WithinAbs(0.05, 1e-6));
    CHECK_THAT(hair.y, WithinAbs(1.0, 1e-6));

    // No transform of its own: the wrapper's half scale is the card's.
    const auto card = index->inherited_paint_scale(node_with_id(*index, "card"));
    REQUIRE(card.ok());
    CHECK_THAT(card.x, WithinAbs(0.5, 1e-6));
    CHECK_THAT(card.y, WithinAbs(0.5, 1e-6));

    const auto plain = index->inherited_paint_scale(node_with_id(*index, "plain"));
    REQUIRE(plain.ok());
    CHECK(plain.x == 1.0);
    CHECK(plain.y == 1.0);

    const auto skewed = index->inherited_paint_scale(node_with_id(*index, "skewed"));
    CHECK_FALSE(skewed.ok());
    CHECK(skewed.refused.rfind("matrix(1, 0, 0.36397", 0) == 0);
}

TEST_CASE("a scaled box's border, radius and shadow follow its scale",
          "[browser-capture][native-lowering][transform]") {
    const auto lowered = lower_fixture("browser-capture-paint-scale");
    REQUIRE(lowered.error.empty());
    REQUIRE(lowered.design_ir);
    const auto& root = lowered.design_ir->root;

    // The hairline's 1px side borders are 0.05px wide in Chrome; the top and
    // bottom keep their full pixel.
    const auto* hair = find_named(root, "div#hair");
    REQUIRE(hair != nullptr);
    CHECK_THAT(*hair->style.width, WithinAbs(0.3125, 1e-4));
    CHECK_THAT(*hair->style.border_left_width, WithinAbs(0.05, 1e-6));
    CHECK_THAT(*hair->style.border_right_width, WithinAbs(0.05, 1e-6));
    CHECK_THAT(*hair->style.border_top_width, WithinAbs(1.0, 1e-6));
    CHECK_THAT(*hair->style.border_bottom_width, WithinAbs(1.0, 1e-6));
    // The sides no longer agree, so no uniform width or shorthand survives.
    CHECK_FALSE(hair->style.border_width.has_value());
    CHECK_FALSE(hair->style.border.has_value());

    // Under the half-scale wrapper every length halves, and the shadow is
    // rewritten from its fields rather than its authored text.
    const auto* card = find_named(root, "div#card");
    REQUIRE(card != nullptr);
    CHECK_THAT(*card->style.border_width, WithinAbs(2.0, 1e-6));
    REQUIRE(card->style.border.has_value());
    CHECK(card->style.border->rfind("2px solid ", 0) == 0);
    CHECK_THAT(*card->style.border_radius, WithinAbs(8.0, 1e-6));
    CHECK_THAT(*card->style.border_top_left_radius, WithinAbs(8.0, 1e-6));
    REQUIRE(card->style.box_shadow.size() == 1);
    CHECK_THAT(card->style.box_shadow[0].offset_y, WithinAbs(4.0, 1e-6));
    CHECK_THAT(card->style.box_shadow[0].blur, WithinAbs(8.0, 1e-6));
    CHECK(card->style.box_shadow[0].raw.empty());
    CHECK(attribute(*card, "paint_scale_refused").empty());

    // The control is untouched.
    const auto* plain = find_named(root, "div#plain");
    REQUIRE(plain != nullptr);
    CHECK_THAT(*plain->style.border_width, WithinAbs(4.0, 1e-6));
    CHECK_THAT(*plain->style.border_radius, WithinAbs(16.0, 1e-6));
    REQUIRE(plain->style.box_shadow.size() == 1);
    CHECK_THAT(plain->style.box_shadow[0].blur, WithinAbs(16.0, 1e-6));
    CHECK_FALSE(plain->style.box_shadow[0].raw.empty());

    // A hard shadow under two axes takes one offset per axis.
    const auto* hard = find_named(root, "div#hard");
    REQUIRE(hard != nullptr);
    REQUIRE(hard->style.box_shadow.size() == 1);
    CHECK_THAT(hard->style.box_shadow[0].offset_x, WithinAbs(8.0, 1e-6));
    CHECK_THAT(hard->style.box_shadow[0].offset_y, WithinAbs(4.0, 1e-6));
    CHECK(attribute(*hard, "paint_scale_refused").empty());
}

TEST_CASE("paint a per-axis factor cannot carry is refused and recorded",
          "[browser-capture][native-lowering][transform]") {
    const auto lowered = lower_fixture("browser-capture-paint-scale");
    REQUIRE(lowered.error.empty());
    REQUIRE(lowered.design_ir);
    const auto& root = lowered.design_ir->root;

    // Two axes: the borders still scale per side, the corner does not.
    const auto* squash = find_named(root, "div#squash");
    REQUIRE(squash != nullptr);
    CHECK_THAT(*squash->style.border_left_width, WithinAbs(1.0, 1e-6));
    CHECK_THAT(*squash->style.border_top_width, WithinAbs(3.0, 1e-6));
    CHECK_THAT(*squash->style.border_top_left_radius, WithinAbs(8.0, 1e-6));
    CHECK(attribute(*squash, "paint_scale_refused") == "border-radius under scale(0.5, 1.5)");

    const auto* soft = find_named(root, "div#soft");
    REQUIRE(soft != nullptr);
    REQUIRE(soft->style.box_shadow.size() == 1);
    CHECK_THAT(soft->style.box_shadow[0].offset_y, WithinAbs(6.0, 1e-6));
    CHECK_THAT(soft->style.box_shadow[0].blur, WithinAbs(12.0, 1e-6));
    CHECK(attribute(*soft, "paint_scale_refused") == "box-shadow under scale(2, 1)");

    const auto* skewed = find_named(root, "div#skewed");
    REQUIRE(skewed != nullptr);
    CHECK_THAT(*skewed->style.border_width, WithinAbs(2.0, 1e-6));
    CHECK(attribute(*skewed, "paint_scale_refused")
              .rfind("transform under matrix(1, 0, 0.36397", 0) == 0);

    CHECK(attribute(root, "native_nodes_paint_scaled") == "4");
    CHECK(attribute(root, "native_nodes_paint_scale_refused") == "3");
}

TEST_CASE("a scaled box with no border, radius or shadow counts nothing",
          "[browser-capture][native-lowering][transform]") {
    // type-scale's wrappers carry scale(0.5) and scale(0.5, 1.5) over text and
    // paint nothing of their own: type is the other pass's job, and there is
    // no paint here to scale or refuse.
    const auto lowered = lower_fixture("browser-capture-type-scale");
    REQUIRE(lowered.error.empty());
    REQUIRE(lowered.design_ir);
    const auto& root = lowered.design_ir->root;
    CHECK(attribute(root, "native_nodes_paint_scaled").empty());
    CHECK(attribute(root, "native_nodes_paint_scale_refused").empty());
    CHECK_FALSE(attribute(root, "native_nodes_type_scaled").empty());
}
