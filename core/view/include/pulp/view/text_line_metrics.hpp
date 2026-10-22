// Where captured text lines put their baselines, from the browser-measured
// line metrics (design_line_metrics.hpp).
#pragma once

#include <pulp/view/design_line_metrics.hpp>

#include <optional>

namespace pulp::view {

/// Where the baseline of a captured line sits below the top of its run rect,
/// for a rect `line_height` CSS px tall. `css_descent` is the face's own
/// typographic descent, used only when `measured` is absent.
///
/// Chromium's run rect is the text's content area whatever the line-height:
/// round(fAscent) + round(fDescent), plus an ascent adjustment Blink applies to
/// some faces on macOS (Times, Helvetica, Courier). Its baseline sits the
/// descent above the rect's bottom: the measured descent when the capture
/// recorded one, otherwise the face's descent rounded to a whole CSS px, as
/// Chromium rounds it.
float captured_baseline_offset(float line_height, float css_descent,
                               const std::optional<LineMetrics>& measured);

/// The baseline of a captured line whose rect top is at `line_top` in the
/// label's coordinates and whose baseline sits `offset` below that top,
/// snapped to a whole CSS px the way Chromium paints it. `root_y` is the
/// label's y offset from the captured frame, the grid the snap is measured
/// on, so an embed offset, scroll or transform outside the frame never moves
/// it. Returns label-local coordinates.
float captured_baseline(float line_top, float offset, float root_y);

}  // namespace pulp::view
