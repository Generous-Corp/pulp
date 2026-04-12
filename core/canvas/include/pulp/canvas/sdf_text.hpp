#pragma once

// Shared SDF/MSDF text rendering options and pen-position helpers.
//
// These live in a dedicated header (not sdf_atlas.hpp) because both the
// single-channel SdfAtlas path and the multi-channel MsdfAtlas path in
// Phase 2 share the same sampler uniforms and the same pen-snapping
// policy. Keeping them here avoids a circular include between the atlas
// headers and the canvas text entry points.

#include <cmath>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------
// Quad-building helpers
// ---------------------------------------------------------------------
//
// `fill_text_sdf()` and `fill_text_msdf()` produce the draw-call geometry
// (one textured quad per glyph) for a text run against a prebuilt atlas.
// Keeping these as pure helpers — independent of the concrete `Canvas`
// backend — means unit tests can exercise layout without a GPU context,
// and adapters (Skia, CoreGraphics) consume the same output through
// their own textured-quad path.

struct SdfTextQuad {
    // Destination rectangle in device pixels.
    float dst_x = 0.0f, dst_y = 0.0f, dst_w = 0.0f, dst_h = 0.0f;
    // Source rectangle in atlas pixels (top-left origin).
    float src_x = 0.0f, src_y = 0.0f, src_w = 0.0f, src_h = 0.0f;
    char32_t codepoint = 0;
};

// Walk `text` against `atlas`, advancing the pen and emitting one
// `SdfTextQuad` per glyph. Skips codepoints missing from the atlas
// (callers should prebuild with the full required set).
//
// `AtlasT` must expose: `glyph(c) → const Glyph*`, `base_size()`, and
// `Glyph` must carry atlas_x/atlas_y/width/height/bearing_x/bearing_y/advance.
// Both `SdfAtlas` and `MsdfAtlas` (and `PsdfAtlas` via inheritance) satisfy
// this structural contract without further adaptation.
template <typename AtlasT>
inline std::vector<SdfTextQuad> build_text_quads(const AtlasT& atlas,
                                                 const std::u32string& text,
                                                 float x, float y,
                                                 float render_size,
                                                 const SdfTextOptions& opts = {}) {
    std::vector<SdfTextQuad> quads;
    quads.reserve(text.size());
    const float base = static_cast<float>(atlas.base_size());
    if (base <= 0.0f) return quads;
    const float scale = render_size / base;

    float pen_x = snap_pen_x(x, opts.snap);
    const float pen_y = snap_pen_y(y, opts.snap);

    for (char32_t c : text) {
        const auto* g = atlas.glyph(c);
        if (!g) continue;
        SdfTextQuad q;
        q.codepoint = c;
        q.dst_x = snap_pen_x(pen_x + g->bearing_x * scale, opts.snap);
        q.dst_y = pen_y - g->bearing_y * scale;
        q.dst_w = g->width  * scale;
        q.dst_h = g->height * scale;
        q.src_x = static_cast<float>(g->atlas_x);
        q.src_y = static_cast<float>(g->atlas_y);
        q.src_w = static_cast<float>(g->width);
        q.src_h = static_cast<float>(g->height);
        quads.push_back(q);
        pen_x += g->advance * scale;
    }
    return quads;
}

// ---------------------------------------------------------------------
// fill_text_sdf / fill_text_msdf — named entry points
// ---------------------------------------------------------------------
//
// These are thin wrappers around `build_text_quads` that exist so call
// sites read naturally ("fill_text_msdf(...)") and so the two atlas
// families have distinct, type-checked entry points even though the
// quad-building math is shared. Neither touches a GPU context directly;
// adapters consume the returned quads through their own textured-quad
// submission path. This is the Phase 2 completion of the
// "`fill_text_msdf()` in Canvas" checkbox in the plan.

// Single templated entry point; callers include the concrete atlas
// header (`sdf_atlas.hpp`, `msdf_atlas.hpp`, `psdf_atlas.hpp`) before
// calling. Named aliases below keep call sites self-documenting while
// the template below does the actual work.
template <typename AtlasT>
inline std::vector<SdfTextQuad>
fill_text_sdf(const AtlasT& atlas, const std::u32string& text,
              float x, float y, float render_size,
              const SdfTextOptions& opts = {}) {
    return build_text_quads(atlas, text, x, y, render_size, opts);
}

template <typename AtlasT>
inline std::vector<SdfTextQuad>
fill_text_msdf(const AtlasT& atlas, const std::u32string& text,
               float x, float y, float render_size,
               const SdfTextOptions& opts = {}) {
    return build_text_quads(atlas, text, x, y, render_size, opts);
}

template <typename AtlasT>
inline std::vector<SdfTextQuad>
fill_text_psdf(const AtlasT& atlas, const std::u32string& text,
               float x, float y, float render_size,
               const SdfTextOptions& opts = {}) {
    return build_text_quads(atlas, text, x, y, render_size, opts);
}

} // namespace pulp::canvas
