// SPDX-License-Identifier: MIT
//
// The pixels an inline `<svg>` icon reaches the screen as.
//
// The sibling lowering cases assert the IR — what the tree holds, what strings
// it carries. That is not the same claim: a vector node whose renderer never
// draws it is indistinguishable, in the IR, from one that does. So this file
// renders the lowered capture through the same native materializer a plugin
// uses and reads the answer off the composite.
//
// Every expected colour comes from Chrome's own resolution of the fixture page
// (test/fixtures/browser-capture-svg-icons), not from arithmetic performed here
// — a value derived the same way the code derives it agrees by construction.

#include <catch2/catch_test_macros.hpp>

#include "tools/import-design/browser_capture_ir.hpp"

#include <pulp/view/design_codegen.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/screenshot.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>
#include <filesystem>
#include <string>
#include <vector>

using namespace pulp::import_design;
using pulp::view::DesignIR;

namespace {

namespace fs = std::filesystem;

struct Render {
    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool empty() const { return rgba.empty(); }

    /// One pixel in DESIGN coordinates, resolved through the render scale, so a
    /// case names the page position it cares about rather than a device pixel.
    struct Pixel {
        int r = 0, g = 0, b = 0, a = 0;
        bool near(int er, int eg, int eb, int tolerance) const {
            return std::abs(r - er) <= tolerance &&
                   std::abs(g - eg) <= tolerance &&
                   std::abs(b - eb) <= tolerance;
        }
        std::string text() const {
            return "rgba(" + std::to_string(r) + ", " + std::to_string(g) +
                   ", " + std::to_string(b) + ", " + std::to_string(a) + ")";
        }
    };

    Pixel at(double design_x, double design_y, double scale) const {
        const auto x = static_cast<std::uint32_t>(design_x * scale);
        const auto y = static_cast<std::uint32_t>(design_y * scale);
        if (x >= width || y >= height) return {};
        const std::size_t offset =
            (static_cast<std::size_t>(y) * width + x) * 4;
        return Pixel{rgba[offset], rgba[offset + 1], rgba[offset + 2],
                     rgba[offset + 3]};
    }

    /// Whether any pixel in a design-space box matches, which is what a 1.8px
    /// stroke needs: its exact centre line lands between device pixels and
    /// antialiasing splits the colour across the two either side.
    bool contains(int er, int eg, int eb, int tolerance, double left,
                  double top, double right, double bottom,
                  double scale) const {
        for (double y = top; y < bottom; y += 0.5) {
            for (double x = left; x < right; x += 0.5) {
                if (at(x, y, scale).near(er, eg, eb, tolerance)) return true;
            }
        }
        return false;
    }
};

constexpr double kScale = 2.0;

/// A node's box in page coordinates, summed back up the emitted tree — the
/// lowering stores every offset relative to its parent, so a node's absolute
/// position is the telescoping sum and NOT any single field.
struct Placed {
    const pulp::view::IRNode* node = nullptr;
    double left = 0.0, top = 0.0, width = 0.0, height = 0.0;
};

void collect_placed(const pulp::view::IRNode& node, double left, double top,
                    std::vector<Placed>& out) {
    for (const auto& child : node.children) {
        const double x = left + child.style.left.value_or(0.0f);
        const double y = top + child.style.top.value_or(0.0f);
        out.push_back(Placed{&child, x, y, child.style.width.value_or(0.0f),
                             child.style.height.value_or(0.0f)});
        collect_placed(child, x, y, out);
    }
}

struct Fixture {
    /// Owned, because `placed` points into this tree. Held behind a pointer so
    /// returning the fixture by value cannot move the nodes out from under
    /// those pointers.
    std::unique_ptr<DesignIR> ir;
    Render render;
    std::vector<Placed> placed;

    /// WHERE to look comes from Chrome's own solved box for that shape, which
    /// the lowering carried through untouched. Naming page coordinates by hand
    /// would encode a guess about how a flex row shrank six cells into 420px.
    Placed vector_node(std::string_view path_data) const {
        for (const auto& entry : placed) {
            const auto found = entry.node->attributes.find("path_data");
            if (found != entry.node->attributes.end() &&
                found->second == path_data) {
                return entry;
            }
        }
        return {};
    }
};

Fixture render_svg_fixture() {
    BrowserCaptureIrOptions options;
    options.native_panel_lowering = true;
    auto lowered = lower_browser_capture_to_ir(
        fs::path(PULP_BROWSER_CAPTURE_FIXTURE_ROOT) /
            "browser-capture-svg-icons" / "capture.json",
        options);
    REQUIRE(lowered.design_ir);

    Fixture out;
    out.ir = std::make_unique<DesignIR>(std::move(*lowered.design_ir));
    const DesignIR& ir = *out.ir;

    auto root = pulp::view::build_native_view_tree(ir, ir.asset_manifest, {});
    REQUIRE(root != nullptr);

    collect_placed(ir.root, 0.0, 0.0, out.placed);
    const auto width = static_cast<std::uint32_t>(
        std::lround(ir.root.style.width.value_or(0.0f)));
    const auto height = static_cast<std::uint32_t>(
        std::lround(ir.root.style.height.value_or(0.0f)));
    REQUIRE(width > 0);
    REQUIRE(height > 0);
    out.render.rgba = pulp::view::render_to_rgba(*root, width, height,
                                                 static_cast<float>(kScale),
                                                 &out.render.width,
                                                 &out.render.height);
    // A composite to look at. Off unless asked for: the assertions below are
    // what gate, and a test that writes a file on every run litters whatever
    // directory CTest happened to start in.
    if (const char* path = std::getenv("PULP_SVG_RENDER_OUT"))
        pulp::view::render_to_file(*root, width, height, path,
                                   static_cast<float>(kScale));
    return out;
}

}  // namespace

namespace {

std::string attribute_of(const pulp::view::IRNode& node,
                         const std::string& key) {
    const auto found = node.attributes.find(key);
    return found == node.attributes.end() ? std::string{} : found->second;
}

/// Assert a colour appears somewhere inside a node's own box.
void check_drawn(const Fixture& fixture, std::string_view path_data, int r,
                 int g, int b, int tolerance = 6) {
    const auto placed = fixture.vector_node(path_data);
    INFO("path " << path_data);
    REQUIRE(placed.node != nullptr);
    REQUIRE(placed.width > 0.0);
    CHECK(fixture.render.contains(r, g, b, tolerance, placed.left, placed.top,
                                  placed.left + placed.width,
                                  placed.top + placed.height, kScale));
}

}  // namespace

TEST_CASE("a lowered svg icon reaches the composite as drawn geometry",
          "[browser-capture][native-lowering][svg][render]") {
    const auto fixture = render_svg_fixture();
    // An empty buffer is a build without Skia, not a failing render — and a
    // case that quietly passed on one would be certifying nothing.
    if (fixture.render.empty()) {
        WARN("no raster backend in this build; SVG render not exercised");
        return;
    }

    // The first icon: a stroke-only waveform, #e8b552 on the cell's #1b2229.
    check_drawn(fixture, "M1 15 L5 3 L9 13 L13 1 L17 11 L23 6", 232, 181, 82);
    // Its dot — a `<circle>` synthesized into path data. Small enough that
    // antialiasing eats most of it, hence the wider tolerance.
    check_drawn(fixture, "M11.4 1 A1.6 1.6 0 0 1 14.6 1 A1.6 1.6 0 0 1 11.4 1 Z",
                212, 84, 74, 12);

    // A `<path>` carrying a fill AND a stroke: both have to land.
    check_drawn(fixture, "M2 18 L10 2 L18 18 Z", 74, 144, 212);
    check_drawn(fixture, "M2 18 L10 2 L18 18 Z", 255, 255, 255, 12);

    // `fill="currentColor"` — the colour lives on the DIV around the icon, so
    // nothing in the SVG markup names it.
    check_drawn(fixture,
                "M23 5 H28 A2 2 0 0 1 30 7 V13 A2 2 0 0 1 28 15 H23 A2 2 0 0 1"
                " 21 13 V7 A2 2 0 0 1 23 5 Z",
                124, 214, 193);

    // A colour that exists only in a stylesheet rule. A lowering that read the
    // authored attribute back would find nothing to draw with.
    check_drawn(fixture, "M2 13 L10 2 L18 13 Z", 200, 106, 208);
}

TEST_CASE("a stroke-only shape is not filled black",
          "[browser-capture][native-lowering][svg][render]") {
    const auto fixture = render_svg_fixture();
    if (fixture.render.empty()) {
        WARN("no raster backend in this build; SVG render not exercised");
        return;
    }

    // `fill: none` has to be STATED, not left out. SVG's default fill is
    // opaque black and the renderer's default matches it, so a lowering that
    // simply omits the fill turns a stroke-only waveform into a black blob
    // between its own strokes — and every colour assertion above still passes,
    // because the stroke is still there.
    //
    // Found by looking at the render, not by reasoning about it.
    const auto wave =
        fixture.vector_node("M1 15 L5 3 L9 13 L13 1 L17 11 L23 6");
    REQUIRE(wave.node != nullptr);
    CHECK(attribute_of(*wave.node, "svg_fill") == "none");
    CHECK_FALSE(fixture.render.contains(0, 0, 0, 8, wave.left, wave.top,
                                        wave.left + wave.width,
                                        wave.top + wave.height, kScale));
}

TEST_CASE("a refused svg keeps its whole subtree out of the composite",
          "[browser-capture][native-lowering][svg][render]") {
    const auto fixture = render_svg_fixture();
    if (fixture.render.empty()) {
        WARN("no raster backend in this build; SVG render not exercised");
        return;
    }

    // The control for every assertion above. A `<rect>` filled from a `<defs>`
    // gradient and one inside a rotated `<g>` are both still pooled, so nothing
    // may paint over the capture that holds them. #8fd694 is the rotated
    // square's fill and exists nowhere else on the page — if it turns up, a
    // refusal leaked half a subtree and drew it in the wrong place.
    CHECK_FALSE(fixture.render.contains(143, 214, 148, 4, 0, 0, 420, 92,
                                        kScale));
}

TEST_CASE("the emitted JS carries the same geometry the native tree draws",
          "[browser-capture][native-lowering][svg][render]") {
    // Two emitters consume this IR and they are still separate code paths, so
    // a lowering proven through the native materializer says nothing about the
    // `ui.js` a plugin actually loads. Asserted on the emitted TEXT rather than
    // on pixels: that is the level at which the two can disagree.
    const auto fixture = render_svg_fixture();
    const auto js = pulp::view::generate_pulp_js(*fixture.ir, {});

    CHECK(js.find("createSvgPath(") != std::string::npos);
    CHECK(js.find("setSvgPath('") != std::string::npos);
    CHECK(js.find("M1 15 L5 3 L9 13 L13 1 L17 11 L23 6") != std::string::npos);
    CHECK(js.find("setSvgViewBox(") != std::string::npos);
    // The stroke-only icon again: `none` has to survive into the artifact, or
    // the browser lane paints the same black blob the native one used to.
    CHECK(js.find("setSvgFill('") != std::string::npos);
    CHECK(js.find("'none'") != std::string::npos);
    // The colour Chrome resolved from a stylesheet rule, in the shipped text.
    CHECK(js.find("rgb(200, 106, 208)") != std::string::npos);
}

TEST_CASE("an icon is drawn through its viewBox, not at 1:1",
          "[browser-capture][native-lowering][svg][render]") {
    const auto fixture = render_svg_fixture();
    if (fixture.render.empty()) {
        WARN("no raster backend in this build; SVG render not exercised");
        return;
    }

    // The waveform's `viewBox` is 24×16 inside a 54×36 box, so its two ends
    // reach the far corners of that box. Ignoring the viewBox would draw the
    // whole icon into a 24×16 patch at the top-left and every colour assertion
    // above would still pass.
    const auto wave =
        fixture.vector_node("M1 15 L5 3 L9 13 L13 1 L17 11 L23 6");
    REQUIRE(wave.node != nullptr);
    const double quarter_w = wave.width / 4.0;
    const double quarter_h = wave.height / 4.0;
    // Starts at user (1,15) — the bottom-left quarter of the box.
    CHECK(fixture.render.contains(232, 181, 82, 6, wave.left,
                                  wave.top + 3.0 * quarter_h,
                                  wave.left + quarter_w,
                                  wave.top + wave.height, kScale));
    // Ends at user (23,6) — the right-hand quarter, above the middle.
    CHECK(fixture.render.contains(232, 181, 82, 6,
                                  wave.left + 3.0 * quarter_w, wave.top,
                                  wave.left + wave.width,
                                  wave.top + 3.0 * quarter_h, kScale));
}
