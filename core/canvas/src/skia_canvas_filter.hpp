// skia_canvas_filter.hpp — entry point for the CSS `filter` parser.
//
// Declares the single cross-TU symbol of the CSS filter-parsing cluster
// that was extracted from skia_canvas.cpp into skia_canvas_filter.cpp.
// Every other function in that cluster (skip_ws, parse_filter_arg,
// parse_css_color_to_skcolor, the SkColorMatrix builders, …) is pure and
// file-local to skia_canvas_filter.cpp — only `parse_filter_chain` needs
// external linkage so `SkiaCanvas::set_filter` can call it.
//
// This is deliberately NOT skia_canvas_internal.hpp: skia_canvas.cpp keeps
// its own `static` copies of generic helpers like `to_sk_color4f`, and
// pulling in skia_canvas_internal.hpp (which defines those `inline`) would
// be a redefinition conflict. A narrow per-feature header avoids that.
//
// Internal to core/canvas/src; not part of the public API.

#pragma once

#ifdef PULP_HAS_SKIA

#include "include/core/SkRefCnt.h"      // sk_sp
#include "include/core/SkImageFilter.h" // SkImageFilter

#include <string>

namespace pulp::canvas {

// Parse a CSS <filter-function-list> string into an SkImageFilter chain.
// Supported functions (Canvas2D `ctx.filter` subset): blur, brightness,
// contrast, grayscale, hue-rotate, invert, opacity, saturate, sepia,
// drop-shadow. Returns nullptr for empty / "none" / fully-invalid input;
// unknown or malformed functions are silently dropped per the CSS spec.
sk_sp<SkImageFilter> parse_filter_chain(const std::string& src);

}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
