#include <pulp/canvas/attributed_string.hpp>
#include <algorithm>

namespace pulp::canvas {

// Estimate character width based on font size (monospace approximation).
// A real implementation would use HarfBuzz or platform text measurement.
static float estimate_char_width(float font_size) {
    return font_size * 0.6f;
}

static float estimate_text_width(const std::string& text, float font_size) {
    return static_cast<float>(text.size()) * estimate_char_width(font_size);
}

// Find word break position — returns index of last space before max_chars,
// or max_chars if no space found (forces break).
static size_t find_word_break(const std::string& text, size_t start, size_t max_chars) {
    size_t end = std::min(start + max_chars, text.size());
    if (end >= text.size()) return text.size();

    // Look backward for a space
    for (size_t i = end; i > start; --i) {
        if (text[i] == ' ' || text[i] == '\t') return i;
    }
    return end;  // No space found — force break
}

TextLayout layout_attributed_string(const AttributedString& text, float max_width,
                                    float line_height) {
    TextLayout layout;
    if (text.empty() || max_width <= 0) return layout;

    float y = 0;
    float max_used_width = 0;

    for (auto& span : text.spans()) {
        float lh = line_height > 0 ? line_height : span.font_size * 1.5f;
        float char_w = estimate_char_width(span.font_size);
        size_t max_chars = static_cast<size_t>(max_width / std::max(char_w, 1.0f));
        if (max_chars < 1) max_chars = 1;

        // Split span text into lines at newlines and word boundaries
        size_t pos = 0;
        while (pos < span.text.size()) {
            // Check for explicit newline
            size_t newline = span.text.find('\n', pos);
            size_t line_end;

            if (newline != std::string::npos && newline - pos <= max_chars) {
                line_end = newline;
            } else {
                line_end = find_word_break(span.text, pos, max_chars);
            }

            std::string line_text = span.text.substr(pos, line_end - pos);
            float line_w = estimate_text_width(line_text, span.font_size);

            TextLayoutLine layout_line;
            layout_line.x = 0;
            layout_line.y = y;
            layout_line.width = line_w;
            layout_line.height = lh;

            // Create a span for this line with the same styling
            TextSpan line_span = span;
            line_span.text = std::move(line_text);
            layout_line.spans.push_back(std::move(line_span));

            layout.lines.push_back(std::move(layout_line));
            max_used_width = std::max(max_used_width, line_w);
            y += lh;

            // Skip the space/newline
            pos = line_end;
            if (pos < span.text.size()
                && (span.text[pos] == ' ' || span.text[pos] == '\t' || span.text[pos] == '\n'))
                ++pos;
        }

        // Handle empty spans (empty string = one blank line)
        if (span.text.empty()) {
            TextLayoutLine layout_line;
            layout_line.x = 0;
            layout_line.y = y;
            layout_line.width = 0;
            layout_line.height = lh;
            layout.lines.push_back(std::move(layout_line));
            y += lh;
        }
    }

    layout.total_width = max_used_width;
    layout.total_height = y;
    return layout;
}

}  // namespace pulp::canvas
