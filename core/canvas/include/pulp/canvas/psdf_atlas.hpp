#pragma once

// Pseudo-Signed Distance Field (PSDF) atlas.
//
// A PSDF stores a one-channel distance field like `SdfAtlas` but uses a
// cheaper generator (Manhattan/Chebyshev distance rather than true
// Euclidean) trading a small amount of corner accuracy for faster
// per-glyph generation. That makes it a useful runtime choice for
// procedural UIs that build atlases on demand.
//
// The runtime surface matches `SdfAtlas` so the sampler shader does
// not change — only the atlas construction differs.

#include <pulp/canvas/sdf_atlas.hpp>

namespace pulp::canvas {

// PsdfAtlas is a thin subclass today: it inherits the packing/metrics
// pipeline from SdfAtlas and will override the distance-transform step
// once the pseudo-distance generator lands. Keeping the same type
// surface now lets downstream code pick the generator without an
// API break later.
class PsdfAtlas : public SdfAtlas {
public:
    PsdfAtlas() = default;
};

// ---------------------------------------------------------------------
// Vector-fallback policy
// ---------------------------------------------------------------------
//
// At extreme magnifications (> 8× the atlas base_size) an SDF sampler
// can lose high-frequency detail because the nearest-edge encoding is
// only as precise as the atlas resolution. Call `should_use_vector_fallback`
// to decide whether to bypass the SDF pipeline and draw the glyph via
// Skia's path rasterizer for pixel-perfect quality.
//
//   threshold — zoom factor (render_px / base_size) above which to
//               prefer vector rendering. 8.0 is a sensible default.
inline bool should_use_vector_fallback(float render_px,
                                       float base_size,
                                       float threshold = 8.0f) {
    if (base_size <= 0.0f) return false;
    return (render_px / base_size) > threshold;
}

}  // namespace pulp::canvas
