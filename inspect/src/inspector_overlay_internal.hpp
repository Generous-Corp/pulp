// inspector_overlay_internal.hpp — PRIVATE shared declarations for the
// inspector-overlay translation units.
//
// Created in the 2026-05 refactor (roadmap P10-2) that split the
// per-feature overlay clusters out of inspector_overlay.cpp into
// sibling TUs (field-edit, zoom loupe, pass-attribution viewer). The
// public method declarations all live in
// pulp/inspect/inspector_overlay.hpp; this header carries only the
// file-local shared state that crosses the new TU boundary — the
// overlay's color palette. The structural layout constants
// (kRowHeight, kZoomGridCells, kPassTypeCount, …) are already
// static-constexpr members of InspectorOverlay, so the split TUs reach
// them through the public header and they are intentionally not
// duplicated here.
//
// PRIVATE: lives under inspect/src/, not the public include tree
// (inspect/include/).

#pragma once

#include <pulp/canvas/canvas.hpp>

#include <iomanip>
#include <sstream>
#include <string>

namespace pulp::inspect {

using pulp::canvas::Color;

// Format a Color as a CSS hex string (#rrggbb or #rrggbbaa). Shared across the
// inspector-overlay TUs (used by the eyedropper pick path + the paint TU);
// `inline` so each TU sees a single definition. P11-5 (#2647) — relocated from
// a file-local static in inspector_overlay.cpp when paint_* split out.
inline std::string color_to_hex(const Color& c) {
    std::ostringstream oss;
    oss << '#' << std::hex << std::nouppercase << std::setfill('0');
    oss << std::setw(2) << static_cast<int>(c.r8())
        << std::setw(2) << static_cast<int>(c.g8())
        << std::setw(2) << static_cast<int>(c.b8());
    if (c.a8() != 255)
        oss << std::setw(2) << static_cast<int>(c.a8());
    return oss.str();
}

// ── Overlay color palette ───────────────────────────────────────────────────
// Shared across the inspector-overlay TUs. `inline` so each TU sees a
// single definition. Byte-identical to the file-local statics they
// replaced in inspector_overlay.cpp.

inline const Color kHighlightFill    = Color::rgba(0.25f, 0.5f, 1.0f, 0.12f);
inline const Color kHighlightStroke  = Color::rgba(0.25f, 0.5f, 1.0f, 0.7f);
inline const Color kSelectedFill     = Color::rgba(1.0f, 0.5f, 0.0f, 0.12f);
inline const Color kSelectedStroke   = Color::rgba(1.0f, 0.5f, 0.0f, 0.7f);
inline const Color kPanelBg          = Color::rgba(0.08f, 0.08f, 0.1f, 0.94f);
inline const Color kPanelText        = Color::rgba(0.85f, 0.85f, 0.9f, 1.0f);
inline const Color kPanelDim         = Color::rgba(0.5f, 0.5f, 0.55f, 1.0f);
inline const Color kPanelHighlight   = Color::rgba(0.25f, 0.5f, 1.0f, 0.25f);
inline const Color kTreeSelected     = Color::rgba(1.0f, 0.5f, 0.0f, 0.3f);
inline const Color kDistanceLine     = Color::rgba(1.0f, 0.2f, 0.3f, 0.8f);
inline const Color kPaddingColor     = Color::rgba(0.2f, 0.8f, 0.3f, 0.15f);
inline const Color kMarginColor      = Color::rgba(1.0f, 0.6f, 0.1f, 0.15f);
inline const Color kStatsBg          = Color::rgba(0.0f, 0.0f, 0.0f, 0.7f);
inline const Color kStatsText        = Color::rgba(0.6f, 1.0f, 0.6f, 1.0f);
inline const Color kStatsWarn        = Color::rgba(1.0f, 0.4f, 0.3f, 1.0f);

// Phase 3b — editable-field visual treatment
inline const Color kFieldEditCaret   = Color::rgba(0.95f, 0.6f, 0.2f, 1.0f);
inline const Color kFieldEditUnder   = Color::rgba(0.95f, 0.6f, 0.2f, 0.9f);
inline const Color kFieldEditBg      = Color::rgba(0.95f, 0.6f, 0.2f, 0.18f);

// Phase 3c — eyedropper cursor swatch chrome
inline const Color kEyedropChromeBg  = Color::rgba(0.08f, 0.08f, 0.1f, 0.95f);
inline const Color kEyedropBorder    = Color::rgba(1.0f, 1.0f, 1.0f, 0.85f);
inline const Color kEyedropText      = Color::rgba(0.95f, 0.95f, 1.0f, 1.0f);

// Phase 3e — zoom loupe visual treatment
inline const Color kZoomPanelBg      = Color::rgba(0.06f, 0.06f, 0.08f, 0.97f);
inline const Color kZoomBorder       = Color::rgba(0.25f, 0.5f, 1.0f, 0.9f);
inline const Color kZoomGridLine     = Color::rgba(0.0f, 0.0f, 0.0f, 0.25f);
inline const Color kZoomCrosshair    = Color::rgba(1.0f, 0.5f, 0.0f, 0.95f);
inline const Color kZoomReadoutText  = Color::rgba(0.9f, 0.9f, 0.95f, 1.0f);
inline const Color kZoomReadoutDim   = Color::rgba(0.55f, 0.55f, 0.6f, 1.0f);
// Two-tone checkerboard for the no-readback fallback grid.
inline const Color kZoomCheckerA     = Color::rgba(0.22f, 0.22f, 0.26f, 1.0f);
inline const Color kZoomCheckerB     = Color::rgba(0.30f, 0.30f, 0.34f, 1.0f);

}  // namespace pulp::inspect
