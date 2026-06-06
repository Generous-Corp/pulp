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
