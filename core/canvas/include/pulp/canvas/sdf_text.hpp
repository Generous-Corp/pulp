#pragma once

// Shared SDF/MSDF text rendering options and pen-position helpers.
//
// These live in a dedicated header (not sdf_atlas.hpp) because both the
// single-channel SdfAtlas path and the multi-channel MsdfAtlas path in
// Phase 2 share the same sampler uniforms and the same pen-snapping
// policy. Keeping them here avoids a circular include between the atlas
// headers and the canvas text entry points.

#include <cmath>

namespace pulp::canvas {

// Snapping policy for fractional pen positions in animated UIs.
//
// Free           — use the floating-point pen position directly. Gives
//                  the smoothest motion; may shimmer at sub-pixel scales.
// Nearest        — round pen x/y to the nearest integer device pixel.
//                  Crisp at rest, visible quantization when animating.
// SubpixelThird  — round pen x to the nearest 1/3 px (y to integer).
//                  Works for LCD subpixel output where the atlas is
//                  sampled in an R-G-B stripe order.
enum class SdfPenSnap {
    Free,
    Nearest,
    SubpixelThird,
};

// Tunables shared by SDF and MSDF samplers. Mirrors the uniforms in
// core/canvas/shaders/sdf_text.sksl and core/canvas/shaders/msdf_text.sksl
// so both CPU-side draw calls and SkSL shaders agree on the contract.
struct SdfTextOptions {
    // Distance value marking the glyph edge. 0.5 for an SDF atlas whose
    // texels store distance mapped linearly to [0, 1] with 0.5 on the
    // edge — the convention produced by SdfAtlas.
    float edge = 0.5f;

    // Extra edge softening added on top of fwidth-derived AA width.
    // 0 = crispest. Small positive values (0.02..0.05) suit glow prep.
    float softness = 0.0f;

    // Pixel-scale bias applied to the AA width. Positive values
    // (0.25..1.0) reduce shimmer at very small sizes. Negative values
    // (-0.5..0) sharpen at extreme zoom. 0 is the default.
    float mip_bias = 0.0f;

    // Output gamma. 1.0 = linear (correct on linear framebuffers),
    // 2.2 ≈ sRGB perceptual correction for 8-bit targets.
    float gamma = 2.2f;

    // Pen snapping policy. See SdfPenSnap for semantics.
    SdfPenSnap snap = SdfPenSnap::Free;
};

// Apply the pen-snapping policy to a fractional pen position. The
// returned coordinate is the "effective" pen position that glyph quads
// should be built from. Separated from the canvas because demos, tests,
// and Phase 2 MSDF sites all share the same rule.
inline float snap_pen_x(float x, SdfPenSnap policy) {
    switch (policy) {
        case SdfPenSnap::Free:
            return x;
        case SdfPenSnap::Nearest:
            return std::round(x);
        case SdfPenSnap::SubpixelThird:
            return std::round(x * 3.0f) / 3.0f;
    }
    return x;
}

inline float snap_pen_y(float y, SdfPenSnap policy) {
    // y is always snapped to integer device pixels if any snapping is
    // requested — vertical antialiasing does not benefit from subpixel
    // offsets on standard LCDs (stripe order is horizontal).
    return policy == SdfPenSnap::Free ? y : std::round(y);
}

} // namespace pulp::canvas
