// emoji_segmenter.hpp — Unicode-aware run segmentation for color-emoji
// fallback.
//
// HarfBuzz can only shape a multi-codepoint emoji sequence (ZWJ family,
// regional flag, keycap, skin-tone modifier, tag sequence, FE0F-promoted
// base) as a single cluster if the entire sequence lands on one font run.
// Naive per-codepoint fallback splits 👨 + ZWJ + 👩 + ZWJ + 👧 into five
// separate font runs and you get five glyphs back — exactly the failure
// mode the per-codepoint EmojiRasterizer approach has.
//
// `segment_emoji_runs(utf8)` walks codepoints, classifies them with a
// curated subset of the Unicode 16.0 emoji property tables (the ranges
// that actually contain emoji as of cutoff), forms grapheme clusters
// according to UAX #29 / UTS #51 cluster-extending rules, and emits a
// minimal list of runs tagged `Default` or `Emoji`. Adjacent runs of the
// same role are coalesced. Variation selectors are NEVER stripped:
//   - U+FE0F forces emoji presentation → the containing cluster routes
//     to the emoji font.
//   - U+FE0E forces text presentation → the containing cluster routes to
//     the default font even when the base codepoint is emoji-default.
//
// The segmenter is intentionally Skia-free so it can be unit-tested in
// isolation and reused by both `SkiaCanvas::fill_text` and
// `text_shaper.cpp::measure_segment` for parity.

#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace pulp::canvas {

/// Which font role a run should resolve against.
/// - `Default`: walks the existing typeface cascade (registered →
///   bundled → SkFontMgr) for the current `font-family`.
/// - `Emoji`: resolves to the color-emoji typeface registered via
///   `register_emoji_fallback(...)`. If no emoji typeface is
///   registered, the caller falls back to the primary face (which
///   typically tofus — that's the diagnostic).
enum class FontRunRole : unsigned char {
    Default = 0,
    Emoji = 1,
};

struct FontRun {
    /// Inclusive byte offset into the input.
    std::size_t byte_start;
    /// Exclusive byte offset into the input.
    std::size_t byte_end;
    FontRunRole role;
};

/// Segment `utf8_text` into runs that respect emoji grapheme clusters.
/// Postconditions:
///   - Runs are non-overlapping and contiguous; the first run starts at
///     0 and the last run ends at `utf8_text.size()`.
///   - No emoji grapheme cluster is ever split across two runs.
///   - Adjacent runs of the same role are merged into one.
///   - Lone or malformed UTF-8 continuation bytes are tagged `Default`.
std::vector<FontRun> segment_emoji_runs(std::string_view utf8_text);

/// Cheap test: returns true if `utf8_text` contains at least one
/// codepoint that the segmenter would route to the emoji font (with
/// FE0F/FE0E rules applied). Faster than `segment_emoji_runs()` because
/// it short-circuits on the first hit; use this to skip fallback set-up
/// for ASCII-only labels.
bool contains_emoji(std::string_view utf8_text);

} // namespace pulp::canvas
