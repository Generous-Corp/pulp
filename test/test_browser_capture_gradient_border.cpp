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
// The fixture has two parts. Three bars on a flat grey page separate the
// shapes cleanly:
//
//   #padding   initial values: the tile repeats into the border strip.
//   #border    `background-origin: border-box`: the tile IS the border box.
//   #norepeat  padding-box tile, `no-repeat`: nothing under the border.
//
// Then the two bars a dark plugin panel authors, border-box sized, each next
// to an otherwise identical `background-origin: border-box` probe:
//
//   #header    `to bottom` gradient, bottom border, on a near-black page.
//   #toolbar   `to top` gradient, top border, on a lighter canvas colour.
//
// Both wrap the gradient's 0.8 stop under their border. On the near-black page
// that stop is LIGHTER than the transparent end, on the canvas it is DARKER,
// so a renderer that sizes the gradient to the border box draws the header's
// hairline too dark and the toolbar's too light: one cause, opposite signs.
//
// The border-box probes are the diagnosis check. They differ from the default
// bars only in `background-origin`, so if they did not already match Chrome
// the cause would lie elsewhere and no paint change should be built on it.
//
// Every expectation is read off `browser.png`. The first case pins what Chrome
// painted to the CSS arithmetic, so the fixture itself is shown to separate the
// shapes before any render is judged against it.

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
constexpr int kPanelHeight = 460;
constexpr int kScale = 2;

// CSS row of each bar's 1px border, as authored in `b.html`.
constexpr int kPaddingBorder = 44;      // #padding, content-box 44px tall
constexpr int kBorderBoxBorder = 104;   // #border
constexpr int kNoRepeatBorder = 164;    // #norepeat
constexpr int kHeaderBorder = 180 + 43;           // #header, border-box 44px
constexpr int kHeaderProbeBorder = 180 + 60 + 43;  // #header-border-box
constexpr int kToolbarBorder = 300 + 10;          // #toolbar, top border
constexpr int kToolbarProbeBorder = 300 + 90;     // #toolbar-border-box

// CSS rows of flat backdrop with no bar over them.
constexpr int kGreyPageRow = 52;
constexpr int kDarkPageRow = 180 + 50;
constexpr int kCanvasRow = 300 + 80;

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

/// The two device rows of a 1px border at CSS row `border_row`.
std::array<int, 2> border_rows(int border_row) {
    const int first = border_row * kScale;
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

Rgb backdrop(const ImportPngImage& image, int css_row) {
    return row_mean(image, css_row * kScale);
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
                                      int border_row, const char* what) {
    for (const int row : border_rows(border_row)) {
        const Rgb expected = row_mean(reference, row);
        const Rgb got = row_mean(render, row);
        INFO(what << " border row " << row << ": chrome (" << expected[0]
                  << ", " << expected[1] << ", " << expected[2] << ") render ("
                  << got[0] << ", " << got[1] << ", " << got[2] << ")");
        CHECK(max_channel_delta(expected, got) <= kTolerance);
    }
}

/// Chrome's two border rows match `expected` within the tolerance.
void require_chrome_rows(const ImportPngImage& reference, int border_row,
                         const Rgb& expected, const char* what) {
    for (const int row : border_rows(border_row)) {
        const Rgb got = row_mean(reference, row);
        INFO(what << " border row " << row << ": (" << got[0] << ", " << got[1]
                  << ", " << got[2] << ") expected (" << expected[0] << ", "
                  << expected[1] << ", " << expected[2] << ")");
        CHECK(max_channel_delta(got, expected) <= kTolerance);
    }
}

/// Mean over the three channels of `a - b`: positive when `a` is lighter.
float mean_signed_delta(const Rgb& a, const Rgb& b) {
    return ((a[0] - b[0]) + (a[1] - b[1]) + (a[2] - b[2])) / 3.0f;
}

}  // namespace

TEST_CASE("Chrome repeats a padding-box gradient tile into its border strip",
          "[browser-capture][native-lowering][background-origin]") {
    const auto reference = decode(read_bytes(fixture_dir() / "browser.png"));
    const Rgb grey = backdrop(reference, kGreyPageRow);
    const Rgb dark = backdrop(reference, kDarkPageRow);
    const Rgb canvas = backdrop(reference, kCanvasRow);

    // Default origin: the hairline sits over the tile's wrapped first stop,
    // whichever side the border is on and whichever way the gradient runs.
    require_chrome_rows(reference, kPaddingBorder,
                        hairline_over(first_stop_over(grey)), "#padding");
    require_chrome_rows(reference, kHeaderBorder,
                        hairline_over(first_stop_over(dark)), "#header");
    require_chrome_rows(reference, kToolbarBorder,
                        hairline_over(first_stop_over(canvas)), "#toolbar");

    // No repeat: nothing under the hairline but the page.
    require_chrome_rows(reference, kNoRepeatBorder, hairline_over(grey),
                        "#norepeat");

    // Negative control on Chrome's own output: the origin decides the border
    // row's colour by far more than any tolerance used here, so a render that
    // ignores `background-origin` cannot match both bars of a pair.
    for (std::size_t i = 0; i < 2; ++i) {
        const Rgb padding = row_mean(reference, border_rows(kPaddingBorder)[i]);
        const Rgb border_box =
            row_mean(reference, border_rows(kBorderBoxBorder)[i]);
        CHECK(max_channel_delta(padding, border_box) >= 10.0f);
    }

    // The Spectr-shaped pair: the header's wrapped stop is lighter than its
    // probe's transparent end, the toolbar's darker. One cause, opposite
    // signs, so a fix that only brightens or only darkens fails one of them.
    for (std::size_t i = 0; i < 2; ++i) {
        const Rgb header = row_mean(reference, border_rows(kHeaderBorder)[i]);
        const Rgb header_probe =
            row_mean(reference, border_rows(kHeaderProbeBorder)[i]);
        const Rgb toolbar = row_mean(reference, border_rows(kToolbarBorder)[i]);
        const Rgb toolbar_probe =
            row_mean(reference, border_rows(kToolbarProbeBorder)[i]);
        INFO("header - probe " << mean_signed_delta(header, header_probe)
             << ", toolbar - probe " << mean_signed_delta(toolbar, toolbar_probe));
        CHECK(mean_signed_delta(header, header_probe) >= 3.0f);
        CHECK(mean_signed_delta(toolbar, toolbar_probe) <= -10.0f);
    }
}

TEST_CASE("a border-box gradient origin and an unrepeated tile paint what "
          "Chrome painted under the border",
          "[browser-capture][native-lowering][background-origin]") {
    // These bars differ from the failing ones only in `background-origin` or
    // `background-repeat`. They must match Chrome as paint stands today: if a
    // probe did not, the border-row error would have another cause.
    const auto reference = decode(read_bytes(fixture_dir() / "browser.png"));
    const auto render = decode(render_panel());
    require_border_rows_match_chrome(reference, render, kBorderBoxBorder,
                                     "#border: background-origin: border-box");
    require_border_rows_match_chrome(reference, render, kNoRepeatBorder,
                                     "#norepeat: background-repeat: no-repeat");
    require_border_rows_match_chrome(reference, render, kHeaderProbeBorder,
                                     "#header-border-box");
    require_border_rows_match_chrome(reference, render, kToolbarProbeBorder,
                                     "#toolbar-border-box");
}

// Pulp sizes a background gradient to the border box regardless of
// `background-origin`, so each default-origin hairline shows the gradient's
// transparent end. Each bar is its own `!shouldfail` case so the gap is
// executed rather than skipped, and each turns red on its own the moment paint
// honours the padding-box origin for it; the tag comes off with that change.
TEST_CASE("a padding-box gradient tile repeats under a translucent border",
          "[browser-capture][native-lowering][background-origin][!shouldfail]") {
    const auto reference = decode(read_bytes(fixture_dir() / "browser.png"));
    const auto render = decode(render_panel());
    require_border_rows_match_chrome(reference, render, kPaddingBorder,
                                     "#padding");
}

TEST_CASE("a header gradient wraps its first stop under its bottom border",
          "[browser-capture][native-lowering][background-origin][!shouldfail]") {
    const auto reference = decode(read_bytes(fixture_dir() / "browser.png"));
    const auto render = decode(render_panel());
    require_border_rows_match_chrome(reference, render, kHeaderBorder,
                                     "#header");
}

TEST_CASE("a toolbar gradient wraps its first stop under its top border",
          "[browser-capture][native-lowering][background-origin][!shouldfail]") {
    const auto reference = decode(read_bytes(fixture_dir() / "browser.png"));
    const auto render = decode(render_panel());
    require_border_rows_match_chrome(reference, render, kToolbarBorder,
                                     "#toolbar");
}
