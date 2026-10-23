// SPDX-License-Identifier: MIT
//
// Group opacity in a flattened native panel, judged against Chrome's own pixels.
//
// `browser-capture-group-opacity/b.html` holds three `opacity: .5` groups over
// black, and a captured canvas whose paint order forces the panel onto the flat
// paint-order emission:
//
//   (i)   two children that do not overlap,
//   (ii)  a bordered button whose chip and label overlap its own fill,
//   (iii) the same button inside a second `opacity: .5` group.
//
// The expected colours are never computed here: each patch is cropped out of
// the capture's `browser.png` and out of the render at the same rectangle. Two
// planted alternatives are rendered from the same lowered tree and must fail
// where they are wrong: pushing the group's alpha down into each descendant
// (right only where descendants do not overlap), and dropping it altogether.

#include <catch2/catch_test_macros.hpp>

#include "tools/import-design/browser_capture_ir.hpp"

#include <pulp/view/design_import.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/view.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace pulp::import_design;
using pulp::view::analyze_screenshot_content;
using pulp::view::compare_screenshots;
using pulp::view::crop_png;
using pulp::view::IRNode;

namespace {

namespace fs = std::filesystem;

constexpr const char* kFixture = "browser-capture-group-opacity";
constexpr int kPanelWidth = 400;
constexpr int kPanelHeight = 200;
constexpr int kScale = 2;

fs::path fixture_dir() {
    return fs::path(PULP_BROWSER_CAPTURE_FIXTURE_ROOT) / kFixture;
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string attribute(const IRNode& node, const std::string& key) {
    const auto it = node.attributes.find(key);
    return it == node.attributes.end() ? std::string{} : it->second;
}

bool any_capture_node(const IRNode& node) {
    if (node.render_mode == pulp::view::NodeRenderMode::faithful_capture)
        return true;
    for (const auto& child : node.children)
        if (any_capture_node(child))
            return true;
    return false;
}

BrowserCaptureIrResult lower_panel() {
    BrowserCaptureIrOptions options;
    options.native_panel_lowering = true;
    return lower_browser_capture_to_ir(fixture_dir() / "capture.json", options);
}

std::vector<uint8_t> render(const pulp::view::DesignIR& ir) {
    auto root = pulp::view::build_native_view_tree(ir, ir.asset_manifest,
                                                   {.asset_base_directory = fixture_dir()});
    REQUIRE(root != nullptr);
    root->set_bounds(
        {0.0f, 0.0f, static_cast<float>(kPanelWidth), static_cast<float>(kPanelHeight)});
    return pulp::view::render_to_png(*root, kPanelWidth, kPanelHeight, static_cast<float>(kScale),
                                     pulp::view::ScreenshotBackend::skia);
}

/// How a group's compositing is re-expressed once its descendants are hung off
/// the group's parent: their alpha multiplied by the group's, or left alone.
enum class Hoist { multiply_alpha, drop_alpha };

/// Undo the nesting the lowering keeps: every translucent group's children move
/// up beside it, re-expressed in the parent's frame, and the group's alpha is
/// either pushed down into them or dropped. This is the seam the planted
/// negatives render through.
void hoist_groups(IRNode& parent, Hoist hoist) {
    for (auto& child : parent.children)
        hoist_groups(child, hoist);
    std::vector<IRNode> rebuilt;
    for (auto& node : parent.children) {
        const bool group =
            node.style.opacity && *node.style.opacity < 1.0f && !node.children.empty();
        auto lifted = std::move(node.children);
        node.children.clear();
        const float alpha = node.style.opacity.value_or(1.0f);
        const float border_left =
            node.style.border_left_width.value_or(node.style.border_width.value_or(0.0f));
        const float border_top =
            node.style.border_top_width.value_or(node.style.border_width.value_or(0.0f));
        const float left = node.style.left.value_or(0.0f) + border_left;
        const float top = node.style.top.value_or(0.0f) + border_top;
        rebuilt.push_back(std::move(node));
        if (!group) {
            rebuilt.back().children = std::move(lifted);
            continue;
        }
        for (auto& child : lifted) {
            child.style.left = left + child.style.left.value_or(0.0f);
            child.style.top = top + child.style.top.value_or(0.0f);
            if (hoist == Hoist::multiply_alpha)
                child.style.opacity = alpha * child.style.opacity.value_or(1.0f);
            rebuilt.push_back(std::move(child));
        }
    }
    int z = 0;
    for (auto& node : rebuilt)
        node.style.z_index = z++;
    parent.children = std::move(rebuilt);
}

struct Patch {
    const char* what;
    int left; ///< logical px
    int top;
    int size;
};

// Inset well inside each flat region so the seam, where Chrome's and Skia's
// edge coverage legitimately differ, is never sampled.
constexpr Patch kNonOverlapping[] = {
    {"(i) first child", 28, 33, 24},
    {"(i) second child", 78, 83, 24},
};
constexpr Patch kOverlapping[] = {
    {"(ii) chip over the button's fill", 185, 76, 30},
    {"(iii) chip over the button's fill, nested", 320, 76, 30},
};
constexpr Patch kFillOnly[] = {
    {"(ii) the button's fill", 222, 40, 20},
    {"(iii) the button's fill, nested", 357, 40, 20},
};

std::vector<uint8_t> crop(const std::vector<uint8_t>& png, const Patch& patch) {
    return crop_png(
        png, static_cast<uint32_t>(patch.left * kScale), static_cast<uint32_t>(patch.top * kScale),
        static_cast<uint32_t>(patch.size * kScale), static_cast<uint32_t>(patch.size * kScale));
}

bool matches(const std::vector<uint8_t>& reference, const std::vector<uint8_t>& render,
             const Patch& patch) {
    const auto expected = crop(reference, patch);
    const auto got = crop(render, patch);
    REQUIRE_FALSE(expected.empty());
    REQUIRE_FALSE(got.empty());
    const auto result = compare_screenshots(expected, got, /*tol=*/12);
    REQUIRE(result.valid);
    UNSCOPED_INFO(patch.what << ": similarity " << result.similarity << " mean_error "
                             << result.mean_error);
    return result.similarity > 0.99f;
}

/// How much lighter the render's patch is than Chrome's, in 0-255 levels.
double lighter_by(const std::vector<uint8_t>& reference, const std::vector<uint8_t>& render,
                  const Patch& patch) {
    return analyze_screenshot_content(crop(render, patch)).luminance_mean -
           analyze_screenshot_content(crop(reference, patch)).luminance_mean;
}

struct Rendered {
    std::vector<uint8_t> reference;
    std::vector<uint8_t> nested;
    std::vector<uint8_t> multiplied;
    std::vector<uint8_t> dropped;
};

Rendered render_all() {
    // A build without Skia cannot raster this panel, and every comparison below
    // would fail for a reason unrelated to opacity. Keyed on the build, never
    // on an empty render.
    if (!pulp::view::raw_rgba_render_available())
        SKIP("no Skia backend compiled in — this panel render cannot run in this build");
    const auto lowered = lower_panel();
    REQUIRE(lowered.error.empty());
    REQUIRE(lowered.design_ir);
    const auto& ir = *lowered.design_ir;
    // `browser.png` sits beside the capture; a tree still holding it would blit
    // Chrome's pixels and pass every comparison without compositing anything.
    REQUIRE_FALSE(any_capture_node(ir.root));
    // The case under test is the FLAT emission: the canvas makes a nested tree
    // reorder overlapping paint, so the lowering has to choose the flat list.
    REQUIRE(attribute(ir.root, "native_tree_flattened_overlapping_reorders") != "");
    // g1, g2, g3 and the group inside g3.
    REQUIRE(attribute(ir.root, "native_tree_isolating_groups_nested") == "4");
    CHECK(attribute(ir.root, "native_tree_isolating_groups_refused").empty());

    Rendered out;
    out.reference = read_bytes(fixture_dir() / "browser.png");
    REQUIRE_FALSE(out.reference.empty());
    out.nested = render(ir);
    auto multiplied = ir;
    hoist_groups(multiplied.root, Hoist::multiply_alpha);
    out.multiplied = render(multiplied);
    auto dropped = ir;
    hoist_groups(dropped.root, Hoist::drop_alpha);
    out.dropped = render(dropped);
    REQUIRE_FALSE(out.nested.empty());
    REQUIRE_FALSE(out.multiplied.empty());
    REQUIRE_FALSE(out.dropped.empty());
    return out;
}

} // namespace

TEST_CASE("translucent groups in a flattened panel composite as Chrome's do",
          "[browser-capture][native-lowering][opacity]") {
    const auto r = render_all();
    for (const auto& patch : kNonOverlapping)
        CHECK(matches(r.reference, r.nested, patch));
    for (const auto& patch : kOverlapping)
        CHECK(matches(r.reference, r.nested, patch));
    for (const auto& patch : kFillOnly)
        CHECK(matches(r.reference, r.nested, patch));
}

TEST_CASE("alpha pushed into each descendant fails only where descendants overlap",
          "[browser-capture][native-lowering][opacity]") {
    const auto r = render_all();
    // Where nothing in the group overlaps, one layer at .5 and each child at .5
    // are the same picture, so the seam must agree with Chrome there...
    for (const auto& patch : kNonOverlapping)
        CHECK(matches(r.reference, r.multiplied, patch));
    for (const auto& patch : kFillOnly)
        CHECK(matches(r.reference, r.multiplied, patch));
    // ...and fail where the chip covers the fill. The group composites the chip
    // over an OPAQUE fill, then halves the result: a/2 over black. Per child, the
    // chip is halved over a half-strength fill: a/2 + (1 - 1/2) * f/2, lighter by
    // f/4 = 50 levels for f = 200, and by 3f/16 = 37.5 inside the nested group.
    constexpr double kPredicted[] = {50.0, 37.5};
    for (std::size_t i = 0; i < std::size(kOverlapping); ++i) {
        const auto& patch = kOverlapping[i];
        CHECK_FALSE(matches(r.reference, r.multiplied, patch));
        const double lighter = lighter_by(r.reference, r.multiplied, patch);
        INFO(patch.what << ": lighter by " << lighter << ", predicted " << kPredicted[i]);
        CHECK(lighter >= 12.0);
        CHECK(std::abs(lighter - kPredicted[i]) < 5.0);
    }
}

TEST_CASE("descendants hung off the root without their group's alpha fail every case",
          "[browser-capture][native-lowering][opacity]") {
    const auto r = render_all();
    for (const auto& patch : kNonOverlapping)
        CHECK_FALSE(matches(r.reference, r.dropped, patch));
    for (const auto& patch : kOverlapping)
        CHECK_FALSE(matches(r.reference, r.dropped, patch));
}
