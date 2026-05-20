// skia_canvas_internal.hpp — TU-internal helpers shared between
// skia_canvas.cpp and the per-feature split files (skia_canvas_box_shadow,
// skia_canvas_opacity, skia_canvas_shaders, skia_canvas_gradients,
// skia_canvas_mask). Internal to core/canvas/src; not part of the public
// API.
//
// Background: PR #2183 (Phase 4 R2-3 follow-up) split skia_canvas.cpp
// into per-feature TUs but left the shared static helpers
// (`to_sk_color4f`, `skia_blend_mode_for`, the webgpu→SkColorType /
// SkColorSpace mappers) only in skia_canvas.cpp. The split files
// referenced them and failed to link. This header consolidates the
// helpers as `inline` so every split TU sees the same definition.

#pragma once

#ifdef PULP_HAS_SKIA

#include "include/core/SkBlendMode.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRefCnt.h"

#include "pulp/canvas/canvas.hpp"  // Color, Canvas::BlendMode

#include <string>

namespace pulp::canvas {

// Color conversion — RGBA float quad from Pulp's `Color`.
inline SkColor4f to_sk_color4f(Color c) {
    return {c.r, c.g, c.b, c.a};
}

// Canvas2D `globalCompositeOperation` → Skia blend mode mapping. Mirrors
// the table in skia_canvas.cpp (kept side-by-side with the canonical
// definition; static_cast pattern matches the legacy switch).
inline SkBlendMode skia_blend_mode_for(Canvas::BlendMode mode) {
    using BM = Canvas::BlendMode;
    switch (mode) {
        case BM::source_over:      return SkBlendMode::kSrcOver;
        case BM::source_in:        return SkBlendMode::kSrcIn;
        case BM::source_out:       return SkBlendMode::kSrcOut;
        case BM::source_atop:      return SkBlendMode::kSrcATop;
        case BM::destination_over: return SkBlendMode::kDstOver;
        case BM::destination_in:   return SkBlendMode::kDstIn;
        case BM::destination_out:  return SkBlendMode::kDstOut;
        case BM::destination_atop: return SkBlendMode::kDstATop;
        case BM::lighter:          return SkBlendMode::kPlus;
        case BM::copy:             return SkBlendMode::kSrc;
        case BM::xor_mode:         return SkBlendMode::kXor;
        case BM::multiply:         return SkBlendMode::kMultiply;
        case BM::screen:           return SkBlendMode::kScreen;
        case BM::overlay:          return SkBlendMode::kOverlay;
        case BM::darken:           return SkBlendMode::kDarken;
        case BM::lighten:          return SkBlendMode::kLighten;
        case BM::color_dodge:      return SkBlendMode::kColorDodge;
        case BM::color_burn:       return SkBlendMode::kColorBurn;
        case BM::hard_light:       return SkBlendMode::kHardLight;
        case BM::soft_light:       return SkBlendMode::kSoftLight;
        case BM::difference:       return SkBlendMode::kDifference;
        case BM::exclusion:        return SkBlendMode::kExclusion;
        case BM::hue:              return SkBlendMode::kHue;
        case BM::saturation:       return SkBlendMode::kSaturation;
        case BM::color:            return SkBlendMode::kColor;
        case BM::luminosity:       return SkBlendMode::kLuminosity;
    }
    return SkBlendMode::kSrcOver;
}

// WebGPU texture format → Skia color type. Used by the offscreen-
// surface wrap path in skia_canvas_shaders.cpp.
inline SkColorType sk_color_type_from_webgpu_format(const std::string& format) {
    if (format == "rgba16float") return kRGBA_F16_SkColorType;
    if (format == "rgba8unorm" || format == "rgba8unorm-srgb") return kRGBA_8888_SkColorType;
    return kBGRA_8888_SkColorType;
}

// WebGPU texture format → SkColorSpace. RGBA16F is linear sRGB; the
// rest are gamma-corrected sRGB.
inline sk_sp<SkColorSpace> sk_color_space_from_webgpu_format(const std::string& format) {
    if (format == "rgba16float") return SkColorSpace::MakeSRGBLinear();
    return SkColorSpace::MakeSRGB();
}

// ── Text / font helpers (R2-3 text split, 2026-05) ───────────────────────────
// Shared by skia_canvas.cpp and skia_canvas_text.cpp. The definitions
// live in skia_canvas.cpp (they carry process-wide font-manager state
// and are referenced by dozens of internal call sites there).

// Early-return guard for SkiaCanvas methods — canvas_ can be null when a
// swapchain texture wrap fails on Android. A macro so the bare `return;`
// works in void methods.
#define GUARD_CANVAS if (!canvas_) return

// Process-wide platform SkFontMgr. Defined in skia_canvas.cpp.
sk_sp<SkFontMgr> get_font_manager();

// Build a stroke SkPaint for the given color + width. Defined in
// skia_canvas.cpp.
SkPaint make_stroke_paint(Color c, float width);

// Build an SkFont for the given family / size / weight / slant. The
// non_opaque_dst flag selects greyscale vs. LCD antialiasing. Defined
// in skia_canvas.cpp.
SkFont make_font(const std::string& family, float size,
                 int weight = SkFontStyle::kNormal_Weight,
                 int slant = 0,
                 bool non_opaque_dst = false);

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
