// SPDX-License-Identifier: MIT
//
// `backdrop-filter` beyond blur, judged against Chrome's own pixels.
//
// The expected colour of a filtered region is never computed here. Each case
// crops the SAME rectangle out of the capture's `browser.png` and out of the
// render, and requires the two crops to match — so the reference is whatever
// Chrome actually painted, not a second evaluation of the maths the renderer
// runs. A fixture derived from that maths would agree with the renderer by
// construction and could not fail.
//
// Every case also carries its own negative control: Chrome's filtered patch is
// required to be far from Chrome's UNFILTERED backdrop. Without that, "drop the
// filter and paint the backdrop through" would satisfy the comparison on any
// filter whose output happens to sit near its input.

#include <catch2/catch_test_macros.hpp>

#include "paint_probe.hpp"
#include "tools/import-design/browser_capture_ir.hpp"

#include <pulp/view/design_import.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/view.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pulp::import_design;
using pulp::view::compare_screenshots;
using pulp::view::crop_png;

namespace {

namespace fs = std::filesystem;

// `backdrop-filter/b.html`: one flat `#c04020` panel with five 60x60 boxes on
// it, each declaring a different non-blur filter list. Captured at DPR 2.
constexpr const char* kFixture = "browser-capture-backdrop-filter";
constexpr int kPanelWidth = 400;
constexpr int kPanelHeight = 160;
constexpr int kScale = 2;

fs::path fixture_dir() {
    return fs::path(PULP_BROWSER_CAPTURE_FIXTURE_ROOT) / kFixture;
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

bool any_capture_node(const pulp::view::IRNode& node) {
    if (node.render_mode == pulp::view::NodeRenderMode::faithful_capture)
        return true;
    for (const auto& child : node.children)
        if (any_capture_node(child)) return true;
    return false;
}

/// The panel rendered from its own lowered nodes, as PNG bytes at the capture's
/// device scale — the same materialize-then-raster path `pulp import-design`
/// uses, so what is measured is what ships.
///
/// The `skia` backend below is a request, not a guarantee: without Skia the
/// capture falls back to CoreGraphics, which composites no filters. The PNG is
/// then non-empty and wrong rather than absent, so the failure surfaces as a
/// similarity of 0 instead of an empty buffer. A case calling this must first
/// call PULP_SKIP_WITHOUT_PAINT.
std::vector<uint8_t> render_panel() {
    BrowserCaptureIrOptions options;
    options.native_panel_lowering = true;
    const auto lowered =
        lower_browser_capture_to_ir(fixture_dir() / "capture.json", options);
    REQUIRE(lowered.error.empty());
    REQUIRE(lowered.design_ir);
    // `browser.png` sits in the fixture directory this render resolves assets
    // against. A tree still holding the capture would blit Chrome's own pixels
    // and satisfy every comparison below without drawing a single filter, so
    // the photograph's absence is asserted before anything is measured.
    REQUIRE_FALSE(any_capture_node(lowered.design_ir->root));
    auto root = pulp::view::build_native_view_tree(
        *lowered.design_ir, lowered.design_ir->asset_manifest,
        {.asset_base_directory = fixture_dir()});
    REQUIRE(root != nullptr);
    root->set_bounds({0.0f, 0.0f, static_cast<float>(kPanelWidth),
                      static_cast<float>(kPanelHeight)});
    return pulp::view::render_to_png(*root, kPanelWidth, kPanelHeight,
                                     static_cast<float>(kScale),
                                     pulp::view::ScreenshotBackend::skia);
}

/// A square well inside one filtered box, in device pixels. Inset from the
/// box's own edge so the sample is the filtered interior and never the seam,
/// where Chrome's and Skia's edge handling legitimately differ.
std::vector<uint8_t> patch(const std::vector<uint8_t>& png, int logical_left) {
    constexpr int kInset = 12;
    constexpr int kBox = 60;
    return crop_png(png,
                    static_cast<uint32_t>((logical_left + kInset) * kScale),
                    static_cast<uint32_t>((50 + kInset) * kScale),
                    static_cast<uint32_t>((kBox - 2 * kInset) * kScale),
                    static_cast<uint32_t>((kBox - 2 * kInset) * kScale));
}

/// The unfiltered panel colour, cropped the same size as a filtered patch so
/// the two can be compared directly.
std::vector<uint8_t> backdrop_patch(const std::vector<uint8_t>& png) {
    constexpr int kSide = (60 - 24) * kScale;
    return crop_png(png, static_cast<uint32_t>(140 * kScale),
                    static_cast<uint32_t>(10 * kScale),
                    static_cast<uint32_t>(kSide),
                    static_cast<uint32_t>(kSide));
}

/// One filtered box: our pixels must match Chrome's, and Chrome's must differ
/// from the unfiltered backdrop by enough that matching cannot be achieved by
/// leaving the filter out.
void require_filter_matches_chrome(const std::vector<uint8_t>& reference,
                                   const std::vector<uint8_t>& render,
                                   int logical_left,
                                   const char* what) {
    INFO("backdrop-filter: " << what);
    const auto expected = patch(reference, logical_left);
    const auto unfiltered = backdrop_patch(reference);
    REQUIRE_FALSE(expected.empty());
    REQUIRE_FALSE(unfiltered.empty());

    // Negative control, on Chrome's own output: the filter visibly changes the
    // backdrop. A filter this test could pass by doing nothing proves nothing.
    const auto control = compare_screenshots(expected, unfiltered, /*tol=*/12);
    REQUIRE(control.valid);
    INFO("control similarity (filtered vs unfiltered): " << control.similarity);
    REQUIRE(control.similarity < 0.05f);

    const auto got = patch(render, logical_left);
    REQUIRE_FALSE(got.empty());
    const auto match = compare_screenshots(expected, got, /*tol=*/12);
    REQUIRE(match.valid);
    INFO("similarity " << match.similarity
                       << " mean_error " << match.mean_error);
    CHECK(match.similarity > 0.99f);
}

}  // namespace

TEST_CASE("a non-blur backdrop-filter paints what Chrome painted",
          "[browser-capture][native-lowering][backdrop-filter]") {
    PULP_SKIP_WITHOUT_PAINT("non-blur backdrop-filter against Chrome's pixels");
    const auto reference = read_bytes(fixture_dir() / "browser.png");
    REQUIRE_FALSE(reference.empty());
    const auto render = render_panel();
    REQUIRE_FALSE(render.empty());

    require_filter_matches_chrome(reference, render, 10, "grayscale(1)");
    require_filter_matches_chrome(reference, render, 90, "invert(1)");
    require_filter_matches_chrome(reference, render, 170, "saturate(0.1)");
    // Authored `sepia(80%)`. Chrome normalizes it to `sepia(0.8)` before the
    // snapshot, so a computed value never arrives as a percentage — worth
    // knowing before writing a parser case that a capture cannot reach.
    require_filter_matches_chrome(reference, render, 330, "sepia(0.8)");
}

TEST_CASE("a two-function backdrop-filter list composes in order",
          "[browser-capture][native-lowering][backdrop-filter]") {
    PULP_SKIP_WITHOUT_PAINT("backdrop-filter list ordering");
    // `grayscale(1) brightness(1.6)` — greyed first, then brightened. Applied
    // in the other order the result is a brightened colour then greyed, which
    // clamps differently and lands on a different grey. One entry handled and
    // the other dropped lands somewhere else again, so this case separates
    // "reads a list" from "reads the first function of a list".
    const auto reference = read_bytes(fixture_dir() / "browser.png");
    REQUIRE_FALSE(reference.empty());
    const auto render = render_panel();
    REQUIRE_FALSE(render.empty());
    require_filter_matches_chrome(reference, render, 250,
                                  "grayscale(1) brightness(1.6)");
}
