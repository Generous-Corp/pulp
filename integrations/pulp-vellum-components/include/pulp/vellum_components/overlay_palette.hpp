#pragma once

/// @file overlay_palette.hpp
/// The inspector overlay's colours. Every channel is a float in [0, 1], the
/// range `canvas::Color` stores; passing 0-255 values here would saturate.

#include <pulp/vellum_components/aliases.hpp>

#include <array>

namespace pulp::vellum_components::overlay {

using Color = canvas::Color;

inline const Color kHighlightFill = Color::rgba(0.25f, 0.5f, 1.0f, 0.12f);
inline const Color kHighlightStroke = Color::rgba(0.25f, 0.5f, 1.0f, 0.7f);
inline const Color kSelectedFill = Color::rgba(1.0f, 0.5f, 0.0f, 0.12f);
inline const Color kSelectedStroke = Color::rgba(1.0f, 0.5f, 0.0f, 0.7f);
inline const Color kHandleFill = Color::rgba(1.0f, 0.5f, 0.0f, 0.9f);
inline const Color kHandleStroke = Color::rgba(0.0f, 0.0f, 0.0f, 0.6f);
inline const Color kBadgeBackground = Color::rgba(0.08f, 0.08f, 0.1f, 0.94f);
inline const Color kBadgeText = Color::rgba(0.85f, 0.85f, 0.9f, 1.0f);
inline const Color kDistanceLine = Color::rgba(1.0f, 0.2f, 0.3f, 0.8f);
inline const Color kPaddingColor = Color::rgba(0.2f, 0.8f, 0.3f, 0.15f);
inline const Color kMarginColor = Color::rgba(1.0f, 0.6f, 0.1f, 0.15f);

inline const Color kTextSelection = Color::rgba(0.31f, 0.63f, 1.0f, 0.35f);
inline const Color kTextCaret = Color::rgba(0.95f, 0.95f, 0.98f, 0.95f);

// The docked panels (render passes).
inline const Color kPanelBackground = Color::rgba(0.08f, 0.08f, 0.1f, 0.94f);
inline const Color kPanelText = Color::rgba(0.85f, 0.85f, 0.9f, 1.0f);
inline const Color kPanelDim = Color::rgba(0.5f, 0.5f, 0.55f, 1.0f);
inline const Color kPanelHeading = Color::rgba(0.25f, 0.5f, 1.0f, 0.7f);
inline const Color kPanelWarn = Color::rgba(1.0f, 0.4f, 0.3f, 1.0f);
inline const Color kSparkTrack = Color::rgba(0.0f, 0.0f, 0.0f, 0.35f);

/// One colour per pass type in compositing order: cool for the cheap
/// structural passes, warm for the expensive ones.
inline const std::array<Color, 5> kPassColors = {
    Color::rgba(0.40f, 0.55f, 0.85f, 1.0f), // background
    Color::rgba(0.45f, 0.80f, 0.55f, 1.0f), // content
    Color::rgba(0.90f, 0.70f, 0.30f, 1.0f), // effects
    Color::rgba(0.85f, 0.45f, 0.80f, 1.0f), // overlay
    Color::rgba(0.90f, 0.45f, 0.35f, 1.0f), // post
};

// The atlas occupancy bars: green with headroom, amber as an atlas fills, red
// when it is nearly full and likely to thrash.
inline const Color kBarTrack = Color::rgba(0.20f, 0.20f, 0.24f, 1.0f);
inline const Color kBarLow = Color::rgba(0.35f, 0.85f, 0.45f, 1.0f);
inline const Color kBarMid = Color::rgba(0.95f, 0.65f, 0.25f, 1.0f);
inline const Color kBarHigh = Color::rgba(0.95f, 0.40f, 0.38f, 1.0f);

inline const Color kZoomPanelBackground = Color::rgba(0.06f, 0.06f, 0.08f, 0.97f);
inline const Color kZoomBorder = Color::rgba(0.25f, 0.5f, 1.0f, 0.9f);
inline const Color kZoomGridLine = Color::rgba(0.0f, 0.0f, 0.0f, 0.25f);
inline const Color kZoomCrosshair = Color::rgba(1.0f, 0.5f, 0.0f, 0.95f);
inline const Color kZoomReadoutText = Color::rgba(0.9f, 0.9f, 0.95f, 1.0f);
inline const Color kZoomReadoutDim = Color::rgba(0.55f, 0.55f, 0.6f, 1.0f);
inline const Color kZoomCheckerA = Color::rgba(0.22f, 0.22f, 0.26f, 1.0f);
inline const Color kZoomCheckerB = Color::rgba(0.30f, 0.30f, 0.34f, 1.0f);

} // namespace pulp::vellum_components::overlay
