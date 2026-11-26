#pragma once

/// @file overlay_palette.hpp
/// The inspector overlay's colours. Every channel is a float in [0, 1], the
/// range `canvas::Color` stores; passing 0-255 values here would saturate.

#include <pulp/vellum_components/aliases.hpp>

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

} // namespace pulp::vellum_components::overlay
