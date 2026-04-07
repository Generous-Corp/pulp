#pragma once

#include <pulp/canvas/canvas.hpp>
#include <string>
#include <vector>

namespace pulp::canvas {

/// Font style specification for text layout.
struct FontSpec {
    std::string family = "Inter";
    float size = 14.0f;
    int weight = 400;      ///< 100-900, 400=normal, 700=bold
    bool italic = false;
    float letter_spacing = 0.0f;
};

/// A run of text with uniform style within a paragraph.
struct TextRun {
    std::string text;
    FontSpec font;
    Color color;
    size_t start = 0;  ///< Start index in the full text
    size_t length = 0; ///< Length in code units
};

/// Result of laying out a line of text — position and metrics for each glyph.
struct GlyphPosition {
    float x = 0, y = 0;         ///< Position relative to the line origin
    float advance = 0;          ///< Horizontal advance to next glyph
    uint32_t codepoint = 0;     ///< Unicode codepoint
    size_t text_index = 0;      ///< Index into the source text
};

/// A single line of laid-out text.
struct TextLine {
    std::vector<GlyphPosition> glyphs;
    float width = 0;       ///< Total line width
    float ascent = 0;      ///< Distance above baseline
    float descent = 0;     ///< Distance below baseline
    float leading = 0;     ///< Extra spacing between lines
    float baseline_y = 0;  ///< Y position of baseline in the layout
};

/// Glyph arrangement — the result of text layout, containing positioned glyphs
/// for each line. This is the Pulp equivalent of shaped, laid-out text.
///
/// Uses Skia's HarfBuzz-based text shaper internally for:
/// - Complex script shaping (Arabic, Devanagari, Thai, etc.)
/// - Bidirectional text (via ICU)
/// - Font fallback for missing glyphs
/// - Kerning and ligatures
class GlyphArrangement {
public:
    GlyphArrangement() = default;

    /// Add a line of laid-out text.
    void add_line(TextLine line) { lines_.push_back(std::move(line)); }

    /// Get all lines.
    const std::vector<TextLine>& lines() const { return lines_; }
    int line_count() const { return static_cast<int>(lines_.size()); }

    /// Get the bounding box of all laid-out text.
    float total_width() const {
        float w = 0;
        for (auto& line : lines_) w = std::max(w, line.width);
        return w;
    }

    float total_height() const {
        if (lines_.empty()) return 0;
        auto& last = lines_.back();
        return last.baseline_y + last.descent;
    }

    /// Find the text index nearest to a point (for cursor positioning).
    size_t hit_test(float x, float y) const;

    /// Get the x position for a given text index (for cursor rendering).
    float position_for_index(size_t index) const;

    /// Clear all layout data.
    void clear() { lines_.clear(); }

private:
    std::vector<TextLine> lines_;
};

/// Layout a paragraph of text with word wrapping.
/// Uses Skia's SkShaper/SkParagraph internally for proper text shaping.
///
/// Parameters:
///   text — the full paragraph text (UTF-8)
///   font — font specification
///   max_width — maximum line width for wrapping (0 = no wrapping)
///   color — text color
///
/// Returns a GlyphArrangement with positioned glyphs for each line.
GlyphArrangement layout_paragraph(const std::string& text,
                                    const FontSpec& font,
                                    float max_width = 0,
                                    Color color = Color::rgba(1.0f, 1.0f, 1.0f));

/// Parallelogram — a skewed rectangle defined by base point and two edge vectors.
struct Parallelogram {
    float x0 = 0, y0 = 0;   ///< Top-left corner
    float x1 = 0, y1 = 0;   ///< Top-right corner
    float x2 = 0, y2 = 0;   ///< Bottom-right corner
    float x3 = 0, y3 = 0;   ///< Bottom-left corner

    /// Create from a rectangle with horizontal shear.
    static Parallelogram from_rect_shear(float x, float y, float w, float h, float shear) {
        return {x + shear, y,  x + w + shear, y,  x + w, y + h,  x, y + h};
    }

    /// Check if a point is inside.
    bool contains(float px, float py) const {
        // Cross product method for convex quad
        auto cross = [](float ax, float ay, float bx, float by) { return ax * by - ay * bx; };
        float d1 = cross(x1 - x0, y1 - y0, px - x0, py - y0);
        float d2 = cross(x2 - x1, y2 - y1, px - x1, py - y1);
        float d3 = cross(x3 - x2, y3 - y2, px - x2, py - y2);
        float d4 = cross(x0 - x3, y0 - y3, px - x3, py - y3);
        return (d1 >= 0 && d2 >= 0 && d3 >= 0 && d4 >= 0) ||
               (d1 <= 0 && d2 <= 0 && d3 <= 0 && d4 <= 0);
    }
};

} // namespace pulp::canvas
