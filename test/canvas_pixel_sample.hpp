#pragma once

#include <catch2/catch_test_macros.hpp>

#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

#include <cstdint>

namespace pulp::test {

struct Pixel {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

// Sample a single RGBA8 pixel from a Skia raster surface so tests can
// assert on the actual texels produced by CanvasWidget / Canvas2D paths.
inline Pixel sample_pixel(SkSurface* surface, int x, int y) {
    SkPixmap pix;
    REQUIRE(surface->peekPixels(&pix));
    auto* row = static_cast<const uint8_t*>(pix.addr(0, y));
    return {row[4 * x + 0], row[4 * x + 1], row[4 * x + 2], row[4 * x + 3]};
}

}  // namespace pulp::test
