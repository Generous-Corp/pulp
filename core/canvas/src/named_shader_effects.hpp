#pragma once

// named_shader_effects.hpp — curated, named GPU post-effects for the
// scripted-UI canvas.
//
// This is the SAFE counterpart to `draw_with_sksl` / `CustomShaderEffect`:
// generated plugin UIs (Forge) can request one of a small, vetted set of
// SkSL post-processing effects BY NAME with 0-1 clamped params, but can
// NEVER inject arbitrary shader source. Each effect is a whole-canvas
// post-process applied as an `SkImageFilters::RuntimeShader` on the
// per-canvas compositing layer (see SkiaCanvas::save_layer_with_shader_effect
// and CanvasWidget::paint).
//
// The effect library follows the repo's SkSL convention (single source of
// truth for the shader string; compile-once + process-lifetime cache via
// RuntimeEffectCache). Unknown names and compile failures both return
// nullptr — callers treat that as "skip the effect", never a hard error.
//
// Skia-gated: this header only has a body when PULP_HAS_SKIA is defined.
// Non-Skia backends never see it (the effect degrades to a plain layer).

#ifdef PULP_HAS_SKIA

#include "include/core/SkImageFilter.h"
#include "include/core/SkRefCnt.h"

#include <string>

namespace pulp::canvas {

/// Curated effect names understood by make_named_shader_effect().
/// Kept in sync with the switch in named_shader_effects.cpp and with the
/// JS-facing `canvasSetShaderEffect(id, name, intensity)` contract.
///   "crt"      — scanlines + barrel curvature + vignette + aperture mask
///   "grain"    — static film grain
///   "vignette" — radial edge darkening
///   "noise"    — isotropic procedural value-noise overlay
///   "brushed"  — horizontal brushed-metal streaks
///   "bloom"    — threshold + small-kernel glow (neon/tube look)
bool is_known_shader_effect(const std::string& name);

/// Build an SkImageFilter that applies the named curated effect to its
/// input (the implicit source image — i.e. the canvas layer's content).
///
/// @param name       curated effect name; anything not in the table -> nullptr.
/// @param intensity  effect strength, clamped to [0,1] internally.
/// @param w,h        layer size in px; used for the `resolution` uniform and
///                   to size the child-sampling radius for offset-sampling
///                   effects (crt curvature, bloom kernel).
/// @param time       optional animation phase in seconds (default 0 = static).
/// @returns nullptr for an unknown name, degenerate size, or SkSL compile
///          failure. Callers must treat nullptr as "skip the effect".
sk_sp<SkImageFilter> make_named_shader_effect(const std::string& name,
                                              float intensity,
                                              float w, float h,
                                              float time = 0.0f);

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
