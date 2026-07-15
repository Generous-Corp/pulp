#pragma once

// Widget gallery — a single themed view tree laying out a representative board
// of Pulp's UI primitives (buttons, knobs, faders, meters, inputs, an XY pad,
// a dropdown, segmented controls, a channel strip, a MIDI keyboard, status and
// feedback surfaces, …). Not an exhaustive catalog — a legible cross-section.
// It is three things at once (Design-System-Import-Plan §9):
//   * living documentation of what Pulp's UI supports,
//   * the reskin preview surface (swap the theme, everything restyles),
//   * the visual-regression corpus for themed render diffs.
//
// Returned root is sized to GALLERY_WIDTH × the height it computes; render it
// headlessly with render_to_png / render_to_file, or mount it in a window.

#include <pulp/view/view.hpp>
#include <pulp/view/theme.hpp>
#include <memory>

namespace pulp::view {

constexpr float GALLERY_WIDTH = 940.0f;

// Build the gallery board with `theme` applied to the root (descendants resolve
// their colors up the parent chain). The root's bounds height is set to fit.
std::unique_ptr<View> build_widget_gallery(const Theme& theme);

// Wrap build_widget_gallery in a ScrollView sized to a `viewport_w × viewport_h`
// window, scrollable in both axes so a host smaller than the full board can
// still reach every widget. `content_size` is the full board's size; the board
// is the ScrollView's single child. Use this for a live/mounted surface (e.g. a
// plugin editor); use build_widget_gallery directly for a full-content headless
// capture.
std::unique_ptr<View> build_scrolling_widget_gallery(const Theme& theme,
                                                     float viewport_w,
                                                     float viewport_h);

}  // namespace pulp::view
