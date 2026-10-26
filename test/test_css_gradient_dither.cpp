// CSS gradients are dithered, as Chromium dithers them.
//
// A shallow gradient spans a handful of code values over hundreds of pixels.
// Quantized without dither it lands as flat bands with hard steps between
// them; Chromium dithers, so a browser capture of the same CSS shows fine
// noise inside each row instead. The measure here is that noise: the number of
// rows whose pixels are not all one colour. An undithered vertical gradient
// has none by construction, whatever its colours.
//
// The expected endpoints and the dithered row counts come from Chrome's own
// render of the same CSS (tools/import-validation/chrome_gradient_oracle.py):
// kShallowCss over 200x600 leaves 540 of 600 rows non-uniform there.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/css_gradient.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>

#include "../core/canvas/src/skia_gradient_compat.hpp"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace pulp::view;

namespace {

constexpr int kW = 200;
constexpr int kH = 600;
constexpr const char* kShallowCss = "linear-gradient(#101418, #161c22)";

// Rows of a tightly packed 4-byte-per-pixel frame whose colour (alpha
// ignored) is not constant across the row.
int non_uniform_rows(const std::vector<uint8_t>& px, int w, int h) {
    int rows = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &px[static_cast<size_t>(y) * w * 4];
        for (int x = 1; x < w; ++x) {
            const uint8_t* p = row + x * 4;
            if (p[0] != row[0] || p[1] != row[1] || p[2] != row[2]) {
                ++rows;
                break;
            }
        }
    }
    return rows;
}

bool near(const uint8_t* p, int r, int g, int b, int tol) {
    return std::abs(p[0] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - b) <= tol;
}

std::vector<uint8_t> raster_gradient(bool use_helper) {
    const SkImageInfo info = SkImageInfo::Make(kW, kH, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface);
    const SkPoint pts[2] = {{0, 0}, {0, static_cast<float>(kH)}};
    const SkColor4f colors[2] = {SkColor4f::FromColor(0xFF101418),
                                 SkColor4f::FromColor(0xFF161C22)};
    auto shader = pulp::canvas::skia_gradient::make_linear(pts, colors, nullptr, 2);
    SkPaint paint;
    if (use_helper)
        pulp::canvas::skia_gradient::set_paint_shader(paint, shader);
    else
        paint.setShader(shader);
    surface->getCanvas()->drawRect(SkRect::MakeWH(kW, kH), paint);
    std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * 4);
    REQUIRE(surface->readPixels(info, px.data(), kW * 4, 0, 0));
    return px;
}

} // namespace

// The instrument itself: the same Skia gradient, drawn once with a plain
// shader and once through set_paint_shader, must read zero and many. If the
// two counts did not separate, a zero from the product path below would mean
// nothing.
TEST_CASE("Gradient dither measure separates dithered from banded Skia output",
          "[canvas][gradient][dither]") {
    const auto banded = raster_gradient(false);
    const auto dithered = raster_gradient(true);
    const int banded_rows = non_uniform_rows(banded, kW, kH);
    const int dithered_rows = non_uniform_rows(dithered, kW, kH);
    INFO("non-uniform rows: plain shader " << banded_rows << ", set_paint_shader "
                                           << dithered_rows);
    CHECK(banded_rows == 0);
    CHECK(dithered_rows >= kH / 2);
}

// Image patterns share the gradient shader slots. Chromium does not dither
// images, so neither does set_paint_shader.
TEST_CASE("set_paint_shader leaves image shaders undithered", "[canvas][gradient][dither]") {
    SkBitmap bm;
    bm.allocN32Pixels(4, 4);
    bm.eraseColor(0xFF336699);
    auto image = bm.asImage();
    REQUIRE(image);
    SkPaint paint;
    pulp::canvas::skia_gradient::set_paint_shader(
        paint, image->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat, SkSamplingOptions()));
    CHECK(paint.getShader() != nullptr);
    CHECK_FALSE(paint.isDither());

    pulp::canvas::skia_gradient::set_paint_shader(paint, nullptr);
    CHECK(paint.getShader() == nullptr);
    CHECK_FALSE(paint.isDither());
}

// The product path an import takes: a CSS background gradient on a View,
// painted by the Skia raster backend.
TEST_CASE("CSS linear gradient background is dithered on the Skia raster backend",
          "[canvas][gradient][dither]") {
    View v;
    v.set_bounds({0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)});
    REQUIRE(apply_css_background_gradient(v, kShallowCss));
    uint32_t w = 0, h = 0;
    const auto px = render_to_rgba(v, kW, kH, 1.0f, &w, &h);
    REQUIRE(w == static_cast<uint32_t>(kW));
    REQUIRE(h == static_cast<uint32_t>(kH));
    REQUIRE(px.size() == static_cast<size_t>(kW) * kH * 4);

    // Endpoints are the CSS stops, so the measure below is reading this
    // gradient and not a blank or a fallback fill.
    CHECK(near(&px[0], 0x10, 0x14, 0x18, 1));
    CHECK(near(&px[(static_cast<size_t>(kH - 1) * kW) * 4], 0x16, 0x1c, 0x22, 1));

    const int rows = non_uniform_rows(px, kW, kH);
    INFO("non-uniform rows: " << rows << " of " << kH << " (Chrome: 540)");
    CHECK(rows >= kH / 2);
}
