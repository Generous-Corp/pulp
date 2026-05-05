#pragma once

// TextShaper — measure-once, reflow-forever text layout.
// Inspired by Cheng Lou's PreText pattern: expensive text shaping runs once,
// then line breaking uses just arithmetic over cached segment widths.
//
// Default ON when PULP_TEXT_SHAPING is enabled (requires Skia with SkParagraph).
// Falls back to character-width estimation when disabled.

#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/attributed_string.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <mutex>

namespace pulp::canvas {

/// A measured text segment — cached after shaping
struct ShapedSegment {
    std::string text;
    float width = 0;            // Measured width in pixels
    float ascent = 0;           // Distance above baseline
    float descent = 0;          // Distance below baseline
    bool is_whitespace = false;  // Can break here
    bool is_newline = false;     // Hard line break
};

/// Prepared text — shaped once, can be reflowed at any width
class PreparedText {
public:
    PreparedText() = default;

    /// The segments (measured)
    const std::vector<ShapedSegment>& segments() const { return segments_; }

    /// Line height for this text
    float line_height() const { return line_height_; }

    /// Total width if laid out on a single line
    float total_width() const;

    /// Whether this prepared text has content
    bool empty() const { return segments_.empty(); }

private:
    friend class TextShaper;
    std::vector<ShapedSegment> segments_;
    float line_height_ = 0;
    std::string font_family_;
    float font_size_ = 0;
};

/// Result of laying out prepared text at a specific width
struct ShapedLayout {
    struct Line {
        float width = 0;
        float y = 0;
        int first_segment = 0;
        int segment_count = 0;
        std::string text;  // Materialized line text (optional)
    };

    std::vector<Line> lines;
    float total_width = 0;
    float total_height = 0;
    int line_count = 0;
};

/// Text shaper — the PreText-style measure-once-reflow-forever engine.
///
/// Usage:
///   auto prepared = shaper.prepare("Hello world", "system", 14);
///   auto layout = shaper.layout(prepared, 200.0f);  // Just arithmetic!
///   // On resize:
///   auto new_layout = shaper.layout(prepared, 300.0f);  // Still just arithmetic!
///
class TextShaper {
public:
    TextShaper();
    ~TextShaper();

    /// Prepare text for layout — this is the expensive call.
    /// Runs text shaping (HarfBuzz via SkShaper when available) and caches segment widths.
    /// Call once per (text, font) pair. Do NOT call on every resize.
    PreparedText prepare(std::string_view text, std::string_view font_family, float font_size);

    /// Prepare an attributed string (mixed styles)
    PreparedText prepare(const AttributedString& text);

    /// Layout prepared text at a specific width — this is the cheap call.
    /// Pure arithmetic over cached segment widths. Call on every resize.
    ///
    /// `max_lines` (pulp #1410): when > 0, truncate the result to N
    /// lines. `max_lines == 1` is the canonical CSS `white-space: nowrap`
    /// path — it ignores wrapping at the right edge and emits a single
    /// line whose width is the text's full intrinsic width (segments
    /// past `max_width` still count toward the line width so consumers
    /// that pair this with #1407's ellipsis truncation can see they
    /// overflow). 0 = unlimited (default).
    ShapedLayout layout(const PreparedText& prepared, float max_width,
                        float line_height = 0, int max_lines = 0) const;

    /// Layout and materialize line text (slightly more expensive than layout())
    ShapedLayout layout_with_lines(const PreparedText& prepared, float max_width,
                                    float line_height = 0, int max_lines = 0) const;

    /// Quick height calculation (fastest path — just returns height, no line details)
    float measure_height(const PreparedText& prepared, float max_width,
                         float line_height = 0) const;

    /// Quick line count
    int count_lines(const PreparedText& prepared, float max_width) const;

    /// Clear the segment measurement cache
    void clear_cache();

    /// Whether this shaper uses real font shaping (SkParagraph) or estimation
    bool uses_real_shaping() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Global text shaper singleton (uses the best available backend)
TextShaper& global_text_shaper();

}  // namespace pulp::canvas
