// shaped_text.hpp
//
// `ShapedText` is the canonical artifact produced by `TextRunPlanner::shape`.
// Measurement and paint paths are intended to consume this same artifact:
// measurement uses the per-run advances and line-break opportunities, while
// paint can walk the same runs, clusters, and eventual glyph buffers. Keeping
// the text artifact shared prevents layout and rendering from inventing
// incompatible segmentation rules.
//
// `TextRunPlanner` fills this structure with resolved font traces, bidi/script
// runs, UAX #29-lite grapheme clusters, heuristic line-break opportunities, and
// UTF-8 / UTF-16 / cluster index maps. Glyph buffers remain optional until the
// painting path asks SkShaper for per-glyph output.

#pragma once

#include "pulp/canvas/font_options.hpp"
#include "pulp/canvas/font_resolver.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::canvas {

// ── Metrics ──────────────────────────────────────────────────────────────

/// Per-line / per-run typographic metrics. All in pixels, all measured
/// from the baseline. `ascent` and `descent` are POSITIVE distances
/// (above + below baseline) — Skia's fAscent is flipped to match
/// `PreparedText::ascent()`.
struct RunMetrics {
    float ascent  = 0.0f;
    float descent = 0.0f;
    float leading = 0.0f;

    float line_height() const noexcept { return ascent + descent + leading; }
};

// ── ShapedRun ────────────────────────────────────────────────────────────

/// One run inside a `ShapedText` — a maximal slice of the input text
/// that shares (bidi level × script × language × resolved typeface ×
/// applied features × applied variation axes). The current planner splits
/// on bidi/script iterator boundaries when Skia's ICU-backed iterators are
/// available, or on the fallback bidi analyzer when they are not.
struct ShapedRun {
    ResolvedFont font;            ///< Resolved typeface + trace.
    RunMetrics   metrics;
    std::uint8_t bidi_level = 0;  ///< 0 = LTR; odd = RTL.
    std::uint32_t script_tag = 0; ///< ISO 15924 four-char as packed u32; 0 = Common.
    std::string  locale;          ///< BCP-47; "" inherits from FontOptions.

    /// Glyph data, parallel arrays of length `glyph_ids.size()`.
    std::vector<std::uint16_t> glyph_ids;
    std::vector<float>         advances;
    std::vector<float>         offsets_x;
    std::vector<float>         offsets_y;
    /// `cluster_indices[i]` is the UTF-8 byte index of the cluster
    /// the i-th glyph belongs to. Multiple glyphs can share a cluster
    /// (combining marks, ZWJ sequences). Multiple clusters can map
    /// to one glyph in Indic conjuncts.
    std::vector<std::uint32_t> cluster_indices;

    /// Logical (input-order) UTF-8 byte range this run covers.
    std::size_t logical_start = 0;
    std::size_t logical_end   = 0;

    /// Sum of `advances` plus letter/word spacing. Computed once at
    /// shape time; consumed by `ShapedText::total_width`.
    float advance_total = 0.0f;
};

// ── Clusters + line breaks ───────────────────────────────────────────────

/// One grapheme cluster in the input. The planner builds these with the
/// shared `cluster_step()` walker so editor movement, selection, and text
/// measurement use the same UTF-8 cluster ranges.
struct ClusterEntry {
    std::uint32_t utf8_start  = 0;  ///< UTF-8 byte offset (start, inclusive).
    std::uint32_t utf8_end    = 0;  ///< UTF-8 byte offset (end, exclusive).
    std::uint32_t run_index   = 0;  ///< Index into `ShapedText::runs`.
    std::uint32_t glyph_start = 0;  ///< First glyph in that run; 0 until glyph buffers are populated.
    std::uint32_t glyph_count = 0;  ///< Glyph count for this cluster; 0 until glyph buffers are populated.
};

/// Line-break opportunity used by the line layout step. The planner currently
/// emits hard-newline and whitespace break opportunities; a future ICU
/// BreakIterator path can add locale-aware dictionary breaks without changing
/// this data shape.
struct LineBreakOpportunity {
    std::uint32_t utf8_offset = 0;
    enum class Kind : std::uint8_t {
        Soft,  ///< Wrap allowed (whitespace / discretionary).
        Hard,  ///< Wrap required (U+000A, U+2028, U+2029).
    } kind = Kind::Soft;
};

// ── Unicode index map ────────────────────────────────────────────────────

/// JS counts UTF-16, ICU APIs vary between UTF-16 and scalar offsets,
/// HarfBuzz reports cluster indices, accessibility APIs vary by platform,
/// and Pulp's internal storage is UTF-8. This index map gives consumers a
/// shared conversion table instead of making each path recompute offsets.
struct UnicodeIndexMap {
    /// scalar_offsets[i] = UTF-8 byte index of the i-th Unicode
    /// scalar value (codepoint). One entry per codepoint plus a
    /// sentinel equal to the input byte length.
    std::vector<std::uint32_t> scalar_offsets;

    /// utf16_offsets[i] = UTF-16 code-unit index of the i-th Unicode
    /// scalar. One entry per codepoint plus a sentinel equal to the total
    /// UTF-16 code-unit count.
    std::vector<std::uint32_t> utf16_offsets;

    /// utf8_byte → cluster index. One entry per UTF-8 byte plus a trailing
    /// sentinel equal to `clusters.size()`.
    std::vector<std::uint32_t> byte_to_cluster;

    std::size_t scalar_count() const noexcept {
        return scalar_offsets.empty() ? 0 : scalar_offsets.size() - 1;
    }
};

// ── ShapedText ───────────────────────────────────────────────────────────

/// The canonical output of `TextRunPlanner::shape(text, FontOptions)`.
/// Owned, immutable once produced. Consumed by both measurement
/// (`TextShaper` line layout) and paint (`SkiaCanvas`); the same
/// instance flows through both pipelines so they cannot diverge.
struct ShapedText {
    std::string  text;     ///< Owned UTF-8 copy of the input.
    FontOptions  options;  ///< Full FontOptions blob the shape was produced for.

    std::vector<ShapedRun>           runs;
    std::vector<ClusterEntry>        clusters;
    std::vector<LineBreakOpportunity> line_breaks;
    UnicodeIndexMap                  index_map;

    /// Sum of per-run `advance_total`. The width the painter will
    /// produce when this `ShapedText` is rendered on a single line
    /// without wrapping.
    float total_width = 0.0f;

    /// Worst-case ascent / descent / leading across all runs on a
    /// single notional line. Used for mixed-fontSize line layout
    /// (max ascent + max descent).
    RunMetrics overall_metrics;

    bool empty() const noexcept { return runs.empty(); }
};

} // namespace pulp::canvas
