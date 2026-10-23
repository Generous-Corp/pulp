// SPDX-License-Identifier: MIT
//
// The gradient under a translucent border, judged against Chrome's own pixels.
//
// CSS positions a background image in the PADDING box by default
// (`background-origin: padding-box`) and paints it across the BORDER box
// (`background-clip: border-box`), repeating it (`background-repeat: repeat`).
// So under a bottom border the gradient tile wraps: the border strip shows the
// tile's FIRST rows, not its last. A 1px `rgba(255,255,255,.06)` hairline over
// a dark-to-transparent gradient therefore reads dark in Chrome; sized to the
// border box instead, the same hairline sits over the transparent end and
// reads as the page. Only the two device rows of the border show it, which is
// why a whole-panel score never sees it.
//
// The fixture stacks three otherwise identical bars on a flat grey page:
//
//   #padding   initial values: the tile repeats into the border strip.
//   #border    `background-origin: border-box`: the tile IS the border box.
//   #norepeat  padding-box tile, `no-repeat`: nothing under the border.
//
// Every expectation is read off `browser.png`. The first case pins what Chrome
// painted to the CSS arithmetic, so the fixture itself is shown to separate the
// three shapes before any render is judged against it.

#include <catch2/catch_test_macros.hpp>

#include "tools/import-design/browser_capture_ir.hpp"
#include "tools/import-design/import_png_codec.hpp"

#include <pulp/view/design_import.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pulp::import_design;

namespace {

namespace fs = std::filesystem;

constexpr const char* kFixture = "browser-capture-gradient-border";
constexpr int kPanelWidth = 400;
constexpr int kPanelHeight = 180;
constexpr int kScale = 2;
constexpr int kBarHeight = 44;  // padding box; the border box is 45

// Top edge of each bar in CSS pixels, as authored in `b.html`.
constexpr int kPaddingBarTop = 0;
constexpr int kBorderBoxBarTop = 60;
constexpr int kNoRepeatBarTop = 120;

// Chrome rows within this many levels of the CSS arithmetic, and a render
// within this many levels of Chrome, per channel. Chrome's two border rows
// differ from each other by up to 2 (they show the tile's first and second
// device rows), so this is the tightest bound the reference itself meets.
constexpr float kTolerance = 2.0f;

using Rgb = std::array<float, 3>;

fs::path fixture_dir() {
    return fs::path(PULP_BROWSER_CAPTURE_FIXTURE_ROOT) / kFixture;
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

ImportPngImage decode(const std::vector<uint8_t>& png) {
    auto image = decode_png_rgba(png.data(), png.size());
    REQUIRE(image.valid());
    REQUIRE(image.width == kPanelWidth * kScale);
    REQUIRE(image.height == kPanelHeight * kScale);
    return image;
}

/// Mean colour of one device row, inset from both side edges so the sample is
/// the row's interior and never a box seam.
Rgb row_mean(const ImportPngImage& image, int device_row) {
    constexpr int kInset = 8 * kScale;
    Rgb sum{0.0f, 0.0f, 0.0f};
    const int x_end = image.width - kInset;
    for (int x = kInset; x < x_end; ++x) {
        const auto* px = image.rgba.data() +
                         (static_cast<std::size_t>(device_row) * image.width + x) * 4;
        for (int c = 0; c < 3; ++c) sum[c] += px[c];
    }
    const float n = static_cast<float>(x_end - kInset);
    return {sum[0] / n, sum[1] / n, sum[2] / n};
}

/// The two device rows of a bar's 1px bottom border.
std::array<int, 2> border_rows(int bar_top) {
    const int first = (bar_top + kBarHeight) * kScale;
    return {first, first + 1};
}

float max_channel_delta(const Rgb& a, const Rgb& b) {
    float worst = 0.0f;
    for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(a[c] - b[c]));
    return worst;
}

/// `rgba(255,255,255,.06)` composited over `under`.
Rgb hairline_over(const Rgb& under) {
    constexpr float kAlpha = 0.06f;
    return {under[0] * (1.0f - kAlpha) + 255.0f * kAlpha,
            under[1] * (1.0f - kAlpha) + 255.0f * kAlpha,
            under[2] * (1.0f - kAlpha) + 255.0f * kAlpha};
}

/// The gradient's first stop, `rgba(10,14,20,.8)`, composited over `page`.
Rgb first_stop_over(const Rgb& page) {
    constexpr float kAlpha = 0.8f;
    return {10.0f * kAlpha + page[0] * (1.0f - kAlpha),
            14.0f * kAlpha + page[1] * (1.0f - kAlpha),
            20.0f * kAlpha + page[2] * (1.0f - kAlpha)};
}

/// A page row in the gap between the first two bars.
Rgb page_colour(const ImportPngImage& image) {
    return row_mean(image, 52 * kScale);
}

bool any_capture_node(const pulp::view::IRNode& node) {
    if (node.render_mode == pulp::view::NodeRenderMode::faithful_capture)
        return true;
    for (const auto& child : node.children)
        if (any_capture_node(child)) return true;
    return false;
}

/// The panel rendered from its own lowered nodes at the capture's device
/// scale, through the same materialize-then-raster path the importer ships.
std::vector<uint8_t> render_panel() {
    if (!pulp::view::raw_rgba_render_available()) {
        SKIP("no Skia backend compiled in — this panel render cannot run in "
             "this build");
    }
    BrowserCaptureIrOptions options;
    options.native_panel_lowering = true;
    const auto lowered =
        lower_browser_capture_to_ir(fixture_dir() / "capture.json", options);
    REQUIRE(lowered.error.empty());
    REQUIRE(lowered.design_ir);
    // A tree still holding Chrome's photograph would match every row below
    // without painting a gradient, so its absence is asserted first.
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

void require_border_rows_match_chrome(const ImportPngImage& reference,
                                      const ImportPngImage& render,
                                      int bar_top, const char* what) {
    for (const int row : border_rows(bar_top)) {
        const Rgb expected = row_mean(reference, row);
        const Rgb got = row_mean(render, row);
        INFO(what << " border row " << row << ": chrome (" << expected[0]
                  << ", " << expected[1] << ", " << expected[2] << ") render ("
                  << got[0] << ", " << got[1] << ", " << got[2] << ")");
        CHECK(max_channel_delta(expected, got) <= kTolerance);
    }
}

}  // namespace

TEST_CASE("Chrome repeats a padding-box gradient tile into its border strip",
          "[browser-capture][native-lowering][background-origin]") {
    const auto reference = decode(read_bytes(fixture_dir() / "browser.png"));
    const Rgb page = page_colour(reference);
    INFO("page (" << page[0] << ", " << page[1] << ", " << page[2] << ")");

    // #padding: the hairline sits over the tile's wrapped first stop.
    const Rgb padding_expected = hairline_over(first_stop_over(page));
    for (const int row : border_rows(kPaddingBarTop)) {
        const Rgb got = row_mean(reference, row);
        INFO("padding-box border row " << row << ": (" << got[0] << ", "
                                       << got[1] << ", " << got[2] << ")");
        CHECK(max_channel_delta(got, padding_expected) <= kTolerance);
    }

    // #norepeat: nothing under the hairline but the page.
    const Rgb bare_expected = hairline_over(page);
    for (const int row : border_rows(kNoRepeatBarTop)) {
        const Rgb got = row_mean(reference, row);
        INFO("no-repeat border row " << row << ": (" << got[0] << ", "
                                     << got[1] << ", " << got[2] << ")");
        CHECK(max_channel_delta(got, bare_expected) <= kTolerance);
    }

    // Negative control on Chrome's own output: the origin decides the border
    // row's colour by far more than any tolerance used here, so a render that
    // ignores `background-origin` cannot match both bars.
    for (std::size_t i = 0; i < 2; ++i) {
        const Rgb padding = row_mean(reference, border_rows(kPaddingBarTop)[i]);
        const Rgb border_box =
            row_mean(reference, border_rows(kBorderBoxBarTop)[i]);
        CHECK(max_channel_delta(padding, border_box) >= 10.0f);
    }
}

TEST_CASE("a border-box gradient origin and an unrepeated tile paint what "
          "Chrome painted under the border",
          "[browser-capture][native-lowering][background-origin]") {
    const auto reference = decode(read_bytes(fixture_dir() / "browser.png"));
    const auto render = decode(render_panel());
    require_border_rows_match_chrome(reference, render, kBorderBoxBarTop,
                                     "background-origin: border-box");
    require_border_rows_match_chrome(reference, render, kNoRepeatBarTop,
                                     "background-repeat: no-repeat");
}

// Pulp sizes a background gradient to the border box regardless of
// `background-origin`, so the hairline over #padding shows the gradient's
// transparent end. Marked `!shouldfail` so the gap is executed rather than
// skipped: the case turns red the moment paint honours the padding-box origin,
// and the tag comes off with that change.
TEST_CASE("a padding-box gradient tile repeats under a translucent border",
          "[browser-capture][native-lowering][background-origin][!shouldfail]") {
    const auto reference = decode(read_bytes(fixture_dir() / "browser.png"));
    const auto render = decode(render_panel());
    require_border_rows_match_chrome(reference, render, kPaddingBarTop,
                                     "background-origin: padding-box");
}
