// Baseline placement for text drawn from a browser-captured line layout.
#include <pulp/view/text_line_metrics.hpp>

#include <cmath>

namespace pulp::view {

float captured_baseline_offset(float line_height, float css_descent,
                               const std::optional<LineMetrics>& measured) {
    // The browser's own metrics outrank anything derived from the font file:
    // they carry its rounding and any per-platform adjustment it made to the
    // face. Reading the baseline off the bottom keeps it right whatever the
    // ascent side of the rect holds.
    const float descent = measured ? measured->descent : std::round(css_descent);
    return line_height - descent;
}

float captured_baseline(float line_top, float offset, float root_y) {
    return std::floor(root_y + line_top + offset + 0.5f) - root_y;
}

}  // namespace pulp::view
