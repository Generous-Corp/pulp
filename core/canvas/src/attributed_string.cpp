#include <pulp/canvas/attributed_string.hpp>

namespace pulp::canvas {

TextLayout layout_attributed_string(const AttributedString& text, float max_width,
                                    float line_height) {
    TextLayout layout;

    if (text.empty()) return layout;

    float lh = line_height > 0 ? line_height : 20.0f;  // Default line height
    float y = 0;

    // Simple word-wrapping: treat each span as a single line for now
    // Full implementation would use HarfBuzz for text shaping
    for (auto& span : text.spans()) {
        TextLayoutLine line;
        line.x = 0;
        line.y = y;
        line.width = max_width;  // Simplified — real impl would measure text
        line.height = lh;
        line.spans.push_back(span);

        layout.lines.push_back(std::move(line));
        y += lh;
    }

    layout.total_width = max_width;
    layout.total_height = y;
    return layout;
}

}  // namespace pulp::canvas
