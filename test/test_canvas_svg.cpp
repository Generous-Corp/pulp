// Canvas::draw_svg — faithful SVG rendering via Skia's SkSVGDOM.
//
// The faithful-vector design-import lane renders a Figma node's own SVG export
// through draw_svg() so gradients, filters, opacities, and transforms come out
// exactly as authored — the capability Pulp's NanoSVG path could not provide
// (it drops gradient fills). These tests pin: a solid fill renders, a LINEAR
// GRADIENT renders (the key win), and malformed input fails safe.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>

#include <string>

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>

#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include "../test/canvas_pixel_probe.hpp"

using pulp::canvas_test::sample_pixel;

namespace {
auto make_surface(int w, int h) {
    SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    return surface;
}
}  // namespace

TEST_CASE("Canvas::draw_svg renders a solid-fill SVG", "[canvas][svg][skia]") {
    auto surface = make_surface(20, 20);
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());

    const std::string svg =
        R"(<svg width="20" height="20" xmlns="http://www.w3.org/2000/svg">)"
        R"(<rect width="20" height="20" fill="#ff0000"/></svg>)";
    REQUIRE(canvas.draw_svg(svg, 0, 0, 20, 20));

    auto px = sample_pixel(surface.get(), 10, 10);
    INFO("rgba=(" << int(px.r) << "," << int(px.g) << "," << int(px.b) << ")");
    CHECK(px.r > 200);
    CHECK(px.g < 60);
    CHECK(px.b < 60);
}

TEST_CASE("Canvas::draw_svg renders a LINEAR GRADIENT (NanoSVG could not)",
          "[canvas][svg][skia]") {
    auto surface = make_surface(40, 8);
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());

    // Left→right red→blue gradient. NanoSVG-only rendering drops gradient fills
    // (flat color); SkSVGDOM honors them, so the two ends must differ.
    const std::string svg = R"SVG(<svg width="40" height="8" xmlns="http://www.w3.org/2000/svg"><defs><linearGradient id="g" x1="0" y1="0" x2="40" y2="0" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient></defs><rect width="40" height="8" fill="url(#g)"/></svg>)SVG";
    REQUIRE(canvas.draw_svg(svg, 0, 0, 40, 8));

    auto left = sample_pixel(surface.get(), 2, 4);
    auto right = sample_pixel(surface.get(), 37, 4);
    INFO("left=(" << int(left.r) << "," << int(left.b) << ") right=("
         << int(right.r) << "," << int(right.b) << ")");
    CHECK(left.r > right.r);    // red dominant on the left
    CHECK(right.b > left.b);    // blue dominant on the right
}

// SVG <image> with a base64 data URI. Two independent defects made every such
// element render BLANK, and the fix needs both halves:
//
//   1. SkSVGDOM only resolves an <image> href through a
//      skresources::ResourceProvider, and Pulp never installed one — so
//      SkSVGImage::LoadImage bailed out on the null provider.
//   2. Skia's SVG parser only matches the attribute NAME "xlink:href". Modern
//      design-tool exports emit the SVG-2 spelling — Figma writes
//      `<image href="data:image/png;base64,...">` with no xlink namespace —
//      which Skia silently drops, leaving the IRI empty.
//
// Design-tool exports carry their bitmap assets exactly this way, so a whole
// class of imported frames came out with holes where the art should be.
//
// The fixture is an 8x8 PNG with four distinct quadrants (TL red, TR green,
// BL blue, BR white), scaled to a 20x20 box. Asserting each quadrant's color
// separately proves the pixels came from the DECODED image, not from a
// placeholder or a flat fill that happens to be non-blank.
#ifdef PULP_HAS_SKRESOURCES
namespace {
// 8x8 PNG: TL red, TR green, BL blue, BR white.
constexpr const char* kQuadrantPngB64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAAGUlEQVR4nGP4z8Dw"
    "HxljgTRXgCaPAeigAADMMJ9hvWSOowAAAABJRU5ErkJggg==";

std::string image_svg(const std::string& href_attr) {
    return R"(<svg width="20" height="20" xmlns="http://www.w3.org/2000/svg" )"
           R"(xmlns:xlink="http://www.w3.org/1999/xlink">)"
           R"(<image x="0" y="0" width="20" height="20" preserveAspectRatio="none" )" +
           href_attr + R"(="data:image/png;base64,)" + kQuadrantPngB64 +
           R"("/></svg>)";
}

// Assert the four quadrants of the decoded image landed in the right places.
void check_quadrants(SkSurface* surface) {
    const auto tl = sample_pixel(surface, 5, 5);
    const auto tr = sample_pixel(surface, 15, 5);
    const auto bl = sample_pixel(surface, 5, 15);
    const auto br = sample_pixel(surface, 15, 15);
    INFO("tl=(" << int(tl.r) << "," << int(tl.g) << "," << int(tl.b) << ","
         << int(tl.a) << ") tr=(" << int(tr.r) << "," << int(tr.g) << ","
         << int(tr.b) << ") bl=(" << int(bl.r) << "," << int(bl.g) << ","
         << int(bl.b) << ") br=(" << int(br.r) << "," << int(br.g) << ","
         << int(br.b) << ")");

    // Non-blank at all: before the fix every one of these was (0,0,0,0).
    CHECK(tl.a > 200);

    CHECK(tl.r > 180); CHECK(tl.g < 70); CHECK(tl.b < 70);    // red
    CHECK(tr.g > 180); CHECK(tr.r < 70); CHECK(tr.b < 70);    // green
    CHECK(bl.b > 180); CHECK(bl.r < 70); CHECK(bl.g < 70);    // blue
    CHECK(br.r > 180); CHECK(br.g > 180); CHECK(br.b > 180);  // white
}
}  // namespace

TEST_CASE("Canvas::draw_svg decodes a base64 data-URI <image> (SVG-2 href)",
          "[canvas][svg][skia][image]") {
    auto surface = make_surface(20, 20);
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
    // The spelling every design-tool export actually uses.
    REQUIRE(canvas.draw_svg(image_svg("href"), 0, 0, 20, 20));
    check_quadrants(surface.get());
}

TEST_CASE("Canvas::draw_svg decodes a base64 data-URI <image> (xlink:href)",
          "[canvas][svg][skia][image]") {
    auto surface = make_surface(20, 20);
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
    // The legacy spelling must keep working — and must not be double-rewritten.
    REQUIRE(canvas.draw_svg(image_svg("xlink:href"), 0, 0, 20, 20));
    check_quadrants(surface.get());
}

// The href rewrite is scoped to attribute NAMES inside a tag: it must not
// corrupt text content, or an attribute VALUE that happens to contain "href=".
TEST_CASE("Canvas::draw_svg href normalization leaves values and text alone",
          "[canvas][svg][skia][image]") {
    auto surface = make_surface(20, 20);
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());

    // The <desc> text and the id VALUE both contain the literal "href=". A
    // naive string replace would rewrite them; the fill must still render.
    const std::string svg =
        R"(<svg width="20" height="20" xmlns="http://www.w3.org/2000/svg">)"
        R"(<desc>href="not-an-attribute"</desc>)"
        R"(<rect id="href=weird" width="20" height="20" fill="#00ff00"/>)"
        R"(</svg>)";
    REQUIRE(canvas.draw_svg(svg, 0, 0, 20, 20));

    const auto px = sample_pixel(surface.get(), 10, 10);
    INFO("rgba=(" << int(px.r) << "," << int(px.g) << "," << int(px.b) << ")");
    CHECK(px.g > 200);
    CHECK(px.r < 60);
}
#endif  // PULP_HAS_SKRESOURCES

TEST_CASE("Canvas::draw_svg fails safe on empty/garbage input",
          "[canvas][svg][skia]") {
    auto surface = make_surface(8, 8);
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
    CHECK_FALSE(canvas.draw_svg("", 0, 0, 8, 8));
    CHECK_FALSE(canvas.draw_svg("<svg>", 0, 0, 0, 0));  // zero box
}

#else  // !PULP_HAS_SKIA

TEST_CASE("Canvas::draw_svg is a no-op without Skia", "[canvas][svg]") {
    // The base Canvas returns false; non-Skia backends can't render SVG.
    SUCCEED("SVG rendering requires the Skia backend");
}

#endif  // PULP_HAS_SKIA
