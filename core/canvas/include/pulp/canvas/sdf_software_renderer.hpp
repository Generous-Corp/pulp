#pragma once

// CPU/software renderer for SDF text.
//
// Rasterizes an SdfTextQuad run onto an 8-bit grayscale output buffer
// by replicating the SkSL sampler logic (smoothstep + fwidth-like
// pixel-scale AA) on the CPU. Lets headless tests and CI validate the
// SDF pipeline end-to-end without requiring a GPU context, and gives
// backend adapters (CoreGraphics / Skia) a reference implementation to
// diff against.

#include <pulp/canvas/sdf_atlas.hpp>
#include <pulp/canvas/sdf_text.hpp>

#include <cstdint>
#include <vector>

namespace pulp::canvas {

// Rasterize `quads` into `out` (width * height bytes, row-major, A8).
// Each output pixel is the maximum alpha contribution of any glyph
// quad that covers it. `render_size` is the intended render size in
// pixels and is used to scale source-rect samples back to the atlas.
template <typename AtlasT>
inline void render_sdf_text_software(const AtlasT& atlas,
                                     const std::vector<SdfTextQuad>& quads,
                                     std::uint8_t* out, int out_w, int out_h,
                                     const SdfTextOptions& opts = {}) {
    const int aw = atlas.width();
    const int ah = atlas.height();
    if (aw <= 0 || ah <= 0) return;

    const float edge255 = opts.edge * 255.0f;

    for (const auto& q : quads) {
        const int x0 = static_cast<int>(std::max(0.0f, std::floor(q.dst_x)));
        const int y0 = static_cast<int>(std::max(0.0f, std::floor(q.dst_y)));
        const int x1 = static_cast<int>(std::min(
            static_cast<float>(out_w),
            std::ceil(q.dst_x + q.dst_w)));
        const int y1 = static_cast<int>(std::min(
            static_cast<float>(out_h),
            std::ceil(q.dst_y + q.dst_h)));

        if (q.dst_w <= 0 || q.dst_h <= 0) continue;
        const float sx_per_px = q.src_w / q.dst_w;
        const float sy_per_px = q.src_h / q.dst_h;

        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const float sx = q.src_x + (x + 0.5f - q.dst_x) * sx_per_px;
                const float sy = q.src_y + (y + 0.5f - q.dst_y) * sy_per_px;
                const int ix = static_cast<int>(sx);
                const int iy = static_cast<int>(sy);
                if (ix < 0 || iy < 0 || ix >= aw || iy >= ah) continue;

                const std::uint8_t d = atlas.pixels()[iy * aw + ix];
                // Pixel-scale AA: half a texel of source per output pixel.
                const float aa = 0.5f * std::max(sx_per_px, sy_per_px) * 255.0f;
                const float lo = edge255 - aa;
                const float hi = edge255 + aa;
                float a = (d - lo) / std::max(1.0f, (hi - lo));
                if (a < 0.0f) a = 0.0f;
                else if (a > 1.0f) a = 1.0f;
                const auto byte = static_cast<std::uint8_t>(a * 255.0f);
                std::uint8_t& dst = out[y * out_w + x];
                if (byte > dst) dst = byte;
            }
        }
    }
}

}  // namespace pulp::canvas
