// The line metrics a browser laid captured text out with.
#pragma once

#include <cmath>

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

}  // namespace pulp::view
