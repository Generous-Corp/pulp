// The line metrics a browser laid captured text out with.
#pragma once

#include <cmath>
#include <optional>

namespace pulp::view {

/// A text run's primary-font ascent and descent in CSS px, as the browser
/// rounded and adjusted them when it built the run's content area. A captured
/// line box exactly ascent + descent tall is the text fragment itself, so its
/// baseline sits `ascent` below the box top.
struct LineMetrics {
    float ascent = 0.0f;
    float descent = 0.0f;

    /// Finite, with a positive ascent and a non-negative descent. Every
    /// producer and consumer of captured metrics gates on this one check.
    bool valid() const {
        return std::isfinite(ascent) && std::isfinite(descent) &&
               ascent > 0.0f && descent >= 0.0f;
    }
};

/// Where the baseline of a captured line sits below the top of its line box,
/// for a box `line_height` CSS px tall. `css_ascent` / `css_descent` are the
/// face's own typographic metrics, used only when `measured` is absent.
///
/// The box's surplus over the content area is split evenly above and below it
/// (half-leading). A box exactly as tall as the content area is the text
/// fragment itself: then the baseline sits the measured ascent below the top,
/// or, with nothing measured, the face's ascent rounded to a whole CSS px, as
/// Chromium rounds it when it builds the fragment.
float captured_baseline_offset(float line_height, float css_ascent,
                               float css_descent,
                               const std::optional<LineMetrics>& measured);

/// The baseline of a captured line whose box top is at `line_top` in the
/// label's coordinates, snapped to a whole CSS px the way Chromium paints it.
/// `root_y` is the label's y offset from the captured frame, the grid the
/// snap is measured on, so an embed offset, scroll or transform outside the
/// frame never moves it. Returns label-local coordinates.
float captured_baseline(float line_top, float line_height, float css_ascent,
                        float css_descent,
                        const std::optional<LineMetrics>& measured,
                        float root_y);

}  // namespace pulp::view
