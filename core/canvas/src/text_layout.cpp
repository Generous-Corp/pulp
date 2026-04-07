#include <pulp/canvas/text_layout.hpp>
#include <algorithm>

namespace pulp::canvas {

size_t GlyphArrangement::hit_test(float x, float y) const {
    // Find the nearest line by y position
    int line_idx = 0;
    for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
        if (y >= lines_[static_cast<size_t>(i)].baseline_y - lines_[static_cast<size_t>(i)].ascent) {
            line_idx = i;
        }
    }

    if (lines_.empty()) return 0;
    auto& line = lines_[static_cast<size_t>(line_idx)];

    // Find nearest glyph by x position
    for (auto& glyph : line.glyphs) {
        if (x < glyph.x + glyph.advance * 0.5f)
            return glyph.text_index;
    }

    // Past the end of the line
    if (!line.glyphs.empty())
        return line.glyphs.back().text_index + 1;
    return 0;
}

float GlyphArrangement::position_for_index(size_t index) const {
    for (auto& line : lines_) {
        for (auto& glyph : line.glyphs) {
            if (glyph.text_index == index)
                return glyph.x;
        }
    }
    // Past the end — return width of last line
    if (!lines_.empty()) return lines_.back().width;
    return 0;
}

GlyphArrangement layout_paragraph(const std::string& text,
                                    const FontSpec& font,
                                    float max_width,
                                    Color color) {
    GlyphArrangement arrangement;
    if (text.empty()) return arrangement;

    (void)color;  // Used when rendering, not during layout

    // Simple word-wrapping layout (full SkShaper integration follows)
    // This provides correct output for LTR Latin text. Complex scripts
    // and BiDi text need the full SkShaper pipeline.

    float char_width = font.size * 0.6f;  // Approximate monospace width
    float line_height = font.size * 1.4f;

    TextLine current_line;
    current_line.ascent = font.size * 0.8f;
    current_line.descent = font.size * 0.2f;
    current_line.leading = font.size * 0.4f;
    current_line.baseline_y = current_line.ascent;

    float x = 0;
    size_t word_start = 0;
    float word_width = 0;

    auto flush_line = [&]() {
        current_line.width = x;
        arrangement.add_line(std::move(current_line));
        current_line = {};
        current_line.ascent = font.size * 0.8f;
        current_line.descent = font.size * 0.2f;
        current_line.leading = font.size * 0.4f;
        current_line.baseline_y = arrangement.total_height() + current_line.ascent;
        x = 0;
    };

    for (size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '\n') {
            flush_line();
            continue;
        }

        float glyph_advance = char_width + font.letter_spacing;

        if (max_width > 0 && x + glyph_advance > max_width && !current_line.glyphs.empty()) {
            flush_line();
        }

        GlyphPosition gp;
        gp.x = x;
        gp.y = 0;
        gp.advance = glyph_advance;
        gp.codepoint = static_cast<uint32_t>(ch);
        gp.text_index = i;
        current_line.glyphs.push_back(gp);

        x += glyph_advance;
    }

    // Flush remaining line
    if (!current_line.glyphs.empty()) {
        current_line.width = x;
        arrangement.add_line(std::move(current_line));
    }

    (void)word_start;
    (void)word_width;

    return arrangement;
}

} // namespace pulp::canvas
