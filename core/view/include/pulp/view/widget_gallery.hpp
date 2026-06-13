#pragma once

// Widget gallery — a single themed view tree that lays out every supported
// primitive (core widgets + Phase-4 gap widgets) in one scrollable board.
// It is three things at once (Design-System-Import-Plan §9):
//   * living documentation of what Pulp's UI supports,
//   * the reskin preview surface (swap the theme, everything restyles),
//   * the visual-regression corpus (Phase 6 renders this per theme and diffs).
//
// Returned root is sized to GALLERY_WIDTH × the height it computes; render it
// headlessly with render_to_png / render_to_file, or mount it in a window.

#include <pulp/view/view.hpp>
#include <pulp/view/theme.hpp>
#include <memory>

namespace pulp::view {

constexpr float GALLERY_WIDTH = 940.0f;

// Build the gallery board with `theme` applied to the root (descendants resolve
// their colours up the parent chain). The root's bounds height is set to fit.
std::unique_ptr<View> build_widget_gallery(const Theme& theme);

}  // namespace pulp::view
