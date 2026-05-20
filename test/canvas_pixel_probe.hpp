// canvas_pixel_probe.hpp — shared raster-pixel probe for the canvas
// test suites.
//
// Created in the 2026-05 fix for pulp #2462. The Skia raster tests in
// test_canvas_widget.cpp, test_canvas2d_shim.cpp, and
// test_canvas_widget_shadow.cpp each need to read back a single texel
// from an SkSurface to assert on what the canvas actually painted.
// test_canvas_widget.cpp and test_canvas2d_shim.cpp each carried their
// own identical copy; test_canvas_widget_shadow.cpp (extracted from
// test_canvas_widget.cpp in the Phase 5 P5-3 split, #2418) referenced
// `sample_pixel` without a definition and failed to compile under a
// Skia-enabled build. This header is the single shared definition.
//
// Skia-gated: the body only exists when PULP_HAS_SKIA is defined, so a
// no-Skia build sees an empty header. The call sites are themselves
// inside `#ifdef PULP_HAS_SKIA`.

#pragma once

#ifdef PULP_HAS_SKIA

#include <catch2/catch_test_macros.hpp>

#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

#include <cstdint>

namespace pulp::canvas_test {

// A single RGBA8 (premultiplied) texel.
struct Pixel {
    uint8_t r, g, b, a;
};

// Sample one RGBA8 premul texel from a Skia raster surface so tests can
// assert on the actual pixels a canvas produced. REQUIRE-fails if the
// surface has no readable pixels.
inline Pixel sample_pixel(SkSurface* surface, int x, int y) {
    SkPixmap pix;
    REQUIRE(surface->peekPixels(&pix));
    auto* row = static_cast<const uint8_t*>(pix.addr(0, y));
    return {row[4 * x + 0], row[4 * x + 1], row[4 * x + 2], row[4 * x + 3]};
}

}  // namespace pulp::canvas_test

#endif  // PULP_HAS_SKIA
