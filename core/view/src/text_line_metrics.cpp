// Baseline placement for text drawn from a browser-captured line layout.
#include <pulp/view/text_line_metrics.hpp>

namespace pulp::view {

float captured_baseline_offset(float line_height, float css_ascent,
                               float css_descent,
                               const std::optional<LineMetrics>& measured) {
    // The browser's own metrics outrank anything derived from the font file:
    // they carry its rounding and any per-platform adjustment it made to the
    // face, for the half-leading split as much as for the fragment itself.
    const float ascent = measured ? measured->ascent : css_ascent;
    const float descent = measured ? measured->descent : css_descent;
    // Unclamped: a content area taller than the box has negative leading,
    // and CSS lets the glyphs overflow evenly, top and bottom.
    float offset = (line_height - (ascent + descent)) * 0.5f + ascent;
    const float rounded_ascent = std::round(ascent);
    if (std::abs(line_height - (rounded_ascent + std::round(descent))) < 0.01f)
        offset = rounded_ascent;
    // Measured metrics are already on the browser's grid; a box exactly that
    // tall is the fragment they built, so descend by them unrounded.
    if (measured && std::abs(line_height - (ascent + descent)) < 0.01f)
        offset = ascent;
    return offset;
}

float captured_baseline(float line_top, float line_height, float css_ascent,
                        float css_descent,
                        const std::optional<LineMetrics>& measured,
                        float root_y) {
    const float local = line_top + captured_baseline_offset(
        line_height, css_ascent, css_descent, measured);
    return std::floor(root_y + local + 0.5f) - root_y;
}

}  // namespace pulp::view
