// shaped_text.hpp
//
// Pulp #2163 follow-up — Phase 1 / Slice 1.2.a of the font-subsystem-
// hardening v2 roadmap (planning/2026-05-17-font-subsystem-hardening-v2.md).
//
// `ShapedText` is the canonical artifact produced by `TextRunPlanner::shape`.
// Both `TextShaper::prepare()` (measurement) and `SkiaCanvas::fill_text`
// (paint) are intended to consume the SAME `ShapedText` instance — the
// measurement pipeline computes line layout from the artifact's per-run
// advances; the paint pipeline walks the same artifact's glyph IDs. They
// cannot diverge. That's how v2 closes the v1 doc's "measurement ≠ paint"
// killer gap.
//
// Slice 1.2.a (this header) lays down the data structures + planner entry
// point. The Phase 1 implementation wraps existing TextShaper / SkShaper
// behind a single-run ShapedText so downstream consumers can be wired
// without forcing the full bidi/script iterator replacement at the same
// time. Slice 1.2.a finish (separate commit) swaps the trivial bidi/script
// iterators for the real `SkShaper::MakeIcuBidiRunIterator` /
// `MakeHbIcuScriptRunIterator` and emits multiple runs when warranted.
// The data shape declared here doesn't change between the skeleton and
// the finish — Phase 2 consumers can target it today.

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
/// applied features × applied variation axes). Slice 1.2.a skeleton
/// emits one or more runs; the planner only splits when fallback
/// changes the typeface mid-text (Latin + emoji within one input).
/// The real bidi/script/cluster split arrives in Slice 1.2.a finish.
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

/// One grapheme cluster in the input. Slice 1.2.a skeleton produces a
/// simple per-codepoint cluster table; UAX #29 + ZWJ + RI-pair correct
/// clustering arrives in 1.2.a finish (the multilingual torture corpus
/// at `test/text_corpus/corpus.json` documents the discrepancy in
/// `SCHEMA_NOTES.md`).
struct ClusterEntry {
    std::uint32_t utf8_start  = 0;  ///< UTF-8 byte offset (start, inclusive).
    std::uint32_t utf8_end    = 0;  ///< UTF-8 byte offset (end, exclusive).
    std::uint32_t run_index   = 0;  ///< Index into `ShapedText::runs`.
    std::uint32_t glyph_start = 0;  ///< First glyph in that run.
    std::uint32_t glyph_count = 0;  ///< Number of glyphs belonging to this cluster.
};

/// Line-break opportunity emitted by ICU's `BreakIterator`. Used by the
/// line layout step (Slice 1.2.a finish + Phase 2 / Slice 2.4 locale-
/// aware line breaking). Slice 1.2.a skeleton populates only the obvious
/// whitespace + hard-newline breaks; ICU dictionary breaks for Thai /
/// Japanese arrive in Phase 2.
struct LineBreakOpportunity {
    std::uint32_t utf8_offset = 0;
    enum class Kind : std::uint8_t {
        Soft,  ///< Wrap allowed (whitespace / discretionary).
        Hard,  ///< Wrap required (U+000A, U+2028, U+2029).
    } kind = Kind::Soft;
};

// ── Unicode index map ────────────────────────────────────────────────────

/// Per the v2 plan tar pit #3: "Unicode indexing is a tax everywhere."
/// JS counts UTF-16, ICU counts UTF-16-or-scalar-depending-on-API,
/// HarfBuzz counts cluster, a11y APIs vary by platform, Pulp's internal
/// storage is UTF-8. This index map is the single computed structure
/// every consumer can convert through without re-computing offsets ad
/// hoc.
///
/// Slice 1.2.a skeleton populates UTF-8 ↔ Unicode-scalar-offset only;
/// UTF-16 (for JS / IME / macOS+Windows a11y) and cluster ↔ glyph
/// range arrives in 1.2.a finish.
struct UnicodeIndexMap {
    /// scalar_offsets[i] = UTF-8 byte index of the i-th Unicode
    /// scalar value (codepoint). One entry per codepoint plus a
    /// sentinel equal to the input byte length.
    std::vector<std::uint32_t> scalar_offsets;

    /// utf16_offsets[i] = UTF-16 code-unit index of the i-th Unicode
    /// scalar. Empty in the skeleton; populated in 1.2.a finish.
    std::vector<std::uint32_t> utf16_offsets;

    /// utf8_byte → cluster index. One entry per UTF-8 byte. Empty in
    /// the skeleton; populated in 1.2.a finish.
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
    /// without wrapping. Slice 1.3 parity harness asserts this
    /// matches the painted pixel bbox within ±0.5 px.
    float total_width = 0.0f;

    /// Worst-case ascent / descent / leading across all runs on a
    /// single notional line. Used for mixed-fontSize line layout
    /// (max ascent + max descent) — the property v2 calls out
    /// as Phase 1 / P0 (it's parity, not polish).
    RunMetrics overall_metrics;

    bool empty() const noexcept { return runs.empty(); }
};

} // namespace pulp::canvas
