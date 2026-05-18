#include <pulp/canvas/path_to_sdf.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace pulp::canvas {

namespace {

// Brute-force edge distance: for each pixel, the distance to the
// nearest pixel that straddles the mask boundary. Correct for
// arbitrary shapes; complexity is quadratic in texel count, which is
// fine for the modest runtime sizes this helper targets (icons,
// procedural decorations). The atlas glyph path uses SdfAtlas's
// Felzenszwalb EDT for larger bulk generation.
float nearest_edge_dist(const std::uint8_t* mask,
                        int w, int h, int x, int y) {
    const bool self_inside = mask[y * w + x] > 127;
    float best = std::numeric_limits<float>::infinity();
    for (int ny = 0; ny < h; ++ny) {
        for (int nx = 0; nx < w; ++nx) {
            const bool other_inside = mask[ny * w + nx] > 127;
            if (other_inside == self_inside) continue;
            const float dx = static_cast<float>(nx - x);
            const float dy = static_cast<float>(ny - y);
            const float d2 = dx * dx + dy * dy;
            if (d2 < best) best = d2;
        }
    }
    return std::sqrt(best);
}

}  // namespace

std::vector<std::uint8_t> path_to_sdf(const std::uint8_t* mask,
                                      int width, int height,
                                      int spread) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width * height), 0);
    if (width <= 0 || height <= 0 || spread <= 0 || mask == nullptr) return out;

    // Degenerate cases.
    bool any_inside = false;
    bool any_outside = false;
    for (int i = 0; i < width * height; ++i) {
        if (mask[i] > 127) any_inside = true;
        else               any_outside = true;
        if (any_inside && any_outside) break;
    }
    if (!any_inside)  { std::fill(out.begin(), out.end(), static_cast<std::uint8_t>(0));   return out; }
    if (!any_outside) { std::fill(out.begin(), out.end(), static_cast<std::uint8_t>(255)); return out; }

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool inside = mask[y * width + x] > 127;
            const float d = nearest_edge_dist(mask, width, height, x, y);
            // Positive outside the shape, negative inside.
            const float signed_d = inside ? -d : d;
            const float norm = 0.5f - 0.5f * signed_d / static_cast<float>(spread);
            out[y * width + x] = static_cast<std::uint8_t>(
                std::clamp(norm * 255.0f, 0.0f, 255.0f));
        }
    }
    return out;
}

}  // namespace pulp::canvas
