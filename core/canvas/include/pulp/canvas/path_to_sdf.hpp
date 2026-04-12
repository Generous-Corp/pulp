#pragma once

// Runtime Path → SDF conversion.
//
// For procedural UI that draws arbitrary shapes (vector icons, custom
// glyphs, generated decorations) we want the same SDF quality as font
// text without pre-building an atlas. `path_to_sdf()` accepts a binary
// mask of a rasterized shape and runs the Felzenszwalb-Huttenlocher EDT
// to produce the signed distance field, mapped to the same [0, 255]
// convention (`128 == edge`) as `SdfAtlas`.
//
// The binary mask is the caller's responsibility — either
// `SkPath::toSimpleSVGString()` + nanosvg rasterization, or a direct
// `SkCanvas::drawPath()` into an off-screen `SkSurface` (the normal
// path for SkiaCanvas-backed callers).

#include <cstdint>
#include <vector>

namespace pulp::canvas {

// Given a width×height mask (0 = outside, 255 = inside), returns a
// width×height SDF where 128 corresponds to the edge. `spread` is the
// maximum distance (in pixels) encoded before saturating to 0 or 255.
std::vector<std::uint8_t> path_to_sdf(const std::uint8_t* mask,
                                      int width, int height,
                                      int spread = 8);

}  // namespace pulp::canvas
