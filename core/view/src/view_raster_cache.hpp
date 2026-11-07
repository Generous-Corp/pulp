#pragma once

#include <pulp/canvas/canvas.hpp>

namespace pulp::view::detail {

// Uses the existing scene-cache holder and invalidation paths without changing
// View's object layout. The renderer owns the bounded texture store, so every
// use must recheck layer_valid (eviction and device replacement are normal).
struct RasterContentCache final : canvas::SceneRecording {
    canvas::Canvas::LayerHandle layer;
    float width = 0.0f;
    float height = 0.0f;
    float scale_x = 0.0f;
    float scale_y = 0.0f;
};

} // namespace pulp::view::detail
