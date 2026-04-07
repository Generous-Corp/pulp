#pragma once

// AttributedString — styled text spans for rich text rendering.
// Each span has font, color, size, weight, and decoration attributes.

#include <pulp/canvas/canvas.hpp>
#include <string>
#include <vector>

namespace pulp::canvas {

/// Text decoration style
enum class TextDecoration { none, underline, strikethrough, overline };

/// A span of text with consistent styling
struct TextSpan {
    std::string text;
    std::string font_family = "system";
    float font_size = 14.0f;
    int font_weight = 400;        // 100-900
    bool italic = false;
    Color color = Color::rgba(255, 255, 255);
    TextDecoration decoration = TextDecoration::none;
    Color decoration_color = Color::rgba(255, 255, 255);
    float letter_spacing = 0;
};

/// Attributed string — a sequence of styled text spans
class AttributedString {
public:
    AttributedString() = default;
    explicit AttributedString(std::string text) {
        append(std::move(text));
    }

    /// Append plain text with default styling
    void append(std::string text) {
        TextSpan span;
        span.text = std::move(text);
        spans_.push_back(std::move(span));
    }

    /// Append a fully styled span
    void append(TextSpan span) {
        spans_.push_back(std::move(span));
    }

    /// Append text with specific color
    void append(std::string text, Color color) {
        TextSpan span;
        span.text = std::move(text);
        span.color = color;
        spans_.push_back(std::move(span));
    }

    /// Append text with specific font size and color
    void append(std::string text, float font_size, Color color) {
        TextSpan span;
        span.text = std::move(text);
        span.font_size = font_size;
        span.color = color;
        spans_.push_back(std::move(span));
    }

    /// Get all spans
    const std::vector<TextSpan>& spans() const { return spans_; }

    /// Get the full plain text (concatenated)
    std::string plain_text() const {
        std::string result;
        for (auto& s : spans_)
            result += s.text;
        return result;
    }

    /// Total character count
    size_t length() const {
        size_t len = 0;
        for (auto& s : spans_)
            len += s.text.size();
        return len;
    }

    /// Whether empty
    bool empty() const { return spans_.empty(); }

    /// Clear all spans
    void clear() { spans_.clear(); }

    /// Set font for all spans
    void set_font(std::string family, float size) {
        for (auto& s : spans_) {
            s.font_family = family;
            s.font_size = size;
        }
    }

    /// Set color for all spans
    void set_color(Color color) {
        for (auto& s : spans_)
            s.color = color;
    }

private:
    std::vector<TextSpan> spans_;
};

/// Multi-line text layout result
struct TextLayoutLine {
    float x = 0, y = 0;           // Position of this line
    float width = 0, height = 0;  // Dimensions
    std::vector<TextSpan> spans;  // Spans on this line
};

struct TextLayout {
    std::vector<TextLayoutLine> lines;
    float total_width = 0;
    float total_height = 0;
};

/// Lay out an attributed string within a given width.
/// Performs word-wrapping and line breaking.
TextLayout layout_attributed_string(const AttributedString& text, float max_width,
                                    float line_height = 0);

}  // namespace pulp::canvas
