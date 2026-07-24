#pragma once

#include <cstdint>

namespace pulp::canvas {

/// Queryable backend capabilities. `supports(c) == false` means the
/// corresponding verb *degrades* (documented per-verb — silent no-op, a
/// plain layer with the color/mask/blur op dropped, a filename placeholder,
/// etc.) rather than rendering faithfully. Callers query this to log or
/// branch instead of silently emitting an unfaithful frame. The base Canvas
/// returns false for every capability precisely because its default verb
/// implementations ARE those degradations.
///
/// Lives in its own header (included by canvas.hpp for back-compat) so the
/// enum can be pulled in by capability-query call sites without dragging the
/// whole Canvas interface. Keep `count` LAST — it is the enumerator total,
/// used to statically bound any bitmask keyed by capability ordinal.
enum class CanvasCapability : uint8_t {
    images,               ///< draw_image_from_* really decode + draw
    svg,                  ///< draw_svg renders via SkSVGDOM
    clip_path_svg,        ///< clip_path_svg parses the path and clips
    filter_chain,         ///< save_layer_with_filters honors color ops
    mask_layer,           ///< save_layer_with_mask applies the mask
    backdrop_filter,      ///< save_backdrop_filter really blurs the backdrop
    bloom_layer,          ///< save_layer_with_bloom is a real bloom
    sksl_draw,            ///< draw_with_sksl executes the shader
    sksl_post_effect,     ///< save_layer_with_sksl_post_effect executes
    box_shadow_gaussian,  ///< draw_box_shadow is a true Gaussian, not stacked rects
    scene_cache,          ///< record_scene / draw_scene are functional (FU-3)
    retained_layer_cache, ///< cacheable begin_layer handles survive across frames

    count,                ///< sentinel: number of real capabilities (keep LAST)
};

}  // namespace pulp::canvas
