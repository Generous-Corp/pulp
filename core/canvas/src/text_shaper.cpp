// TextShaper — PreText-style measure-once, reflow-forever text layout.
//
// Two backends:
// 1. PULP_HAS_TEXT_SHAPING: Uses SkShaper/SkParagraph (real HarfBuzz shaping)
// 2. Fallback: Uses character-width estimation (same as before)
//
// The API is identical — only the measurement accuracy differs.

#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/emoji_segmenter.hpp>
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/canvas/font_options.hpp>
#include <pulp/canvas/text_shaper.hpp>
// PULP_LITE_EDITOR builds this file a second time, into the pulp-editor-lite
// objects that a plug-in without a GPU editor links ahead of the library.
// It changes function bodies only; every declaration stays as the headers
// give it, so the two builds agree on every type.
#if defined(PULP_HAS_TEXT_SHAPING) && !(defined(PULP_LITE_EDITOR) && PULP_LITE_EDITOR)
#define PULP_TEXT_SHAPER_SKIA 1
#else
#define PULP_TEXT_SHAPER_SKIA 0
#endif

#if !PULP_TEXT_SHAPER_SKIA && defined(__APPLE__)
#include "coretext_text.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <sstream>
#include <numeric>

#if PULP_TEXT_SHAPER_SKIA
#include "font_tables.hpp"
// SkParagraph headers — available when Skia is built with text shaping
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "skia_unicode.hpp"
#include "modules/skshaper/include/SkShaper.h"
#include "include/core/SkFontMgr.h"

#include <pulp/canvas/text_font_context.hpp>

// The font manager itself comes from `platform_font_manager()`
// (bundled_fonts.cpp), which owns the single OS switch. It must be the same
// manager SkiaCanvas uses, otherwise `mgr->matchFamilyStyle("Inter", ...)`
// returns null and SkFont falls back to a typeface-less default whose
// `measureText()` advance is effectively zero. That collapses
// Label::intrinsic_width() and Yoga reserves no horizontal space for the
// label, which paints into the now-tiny box and clips.
#endif

namespace pulp::canvas {

namespace {
// Monotonic prepare() invocation counter. Relaxed ordering: tests only need
// the delta across two synchronous paints on one thread, and the value carries
// no cross-thread happens-before contract. Function-local static avoids any
// static-init-order dependency on the (header-declared) free accessor.
std::atomic<std::uint64_t>& detail_prepare_calls() {
    static std::atomic<std::uint64_t> calls{0};
    return calls;
}

bool valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80u) {
            ++i;
            continue;
        }

        std::size_t length = 0;
        std::uint32_t scalar = 0;
        if (lead >= 0xC2u && lead <= 0xDFu) {
            length = 2;
            scalar = lead & 0x1Fu;
        } else if (lead >= 0xE0u && lead <= 0xEFu) {
            length = 3;
            scalar = lead & 0x0Fu;
        } else if (lead >= 0xF0u && lead <= 0xF4u) {
            length = 4;
            scalar = lead & 0x07u;
        } else {
            return false;
        }
        if (i + length > text.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto byte = static_cast<unsigned char>(text[i + offset]);
            if ((byte & 0xC0u) != 0x80u) return false;
            scalar = (scalar << 6) | (byte & 0x3Fu);
        }
        if ((length == 3 && scalar < 0x800u) ||
            (length == 3 && scalar >= 0xD800u && scalar <= 0xDFFFu) ||
            (length == 4 && scalar < 0x10000u) ||
            (length == 4 && scalar > 0x10FFFFu))
            return false;
        i += length;
    }
    return true;
}

std::string replace_invalid_utf8(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto suffix = text.substr(i);
        std::size_t length = 1;
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead >= 0xC2u && lead <= 0xDFu) length = 2;
        else if (lead >= 0xE0u && lead <= 0xEFu) length = 3;
        else if (lead >= 0xF0u && lead <= 0xF4u) length = 4;

        if (lead < 0x80u) {
            result.push_back(static_cast<char>(lead));
            ++i;
        } else if (length <= suffix.size() && valid_utf8(suffix.substr(0, length))) {
            result.append(suffix.substr(0, length));
            i += length;
        } else {
            result.append("\xEF\xBF\xBD");
            ++i;
        }
    }
    return result;
}
}  // namespace

// ── Shared: PreText-style arithmetic line breaking ──────────────────────
// This is the "cheap" path — just arithmetic over cached segment widths.
// Works identically regardless of how segments were measured.

// Count UTF-8 codepoints in `text`. Used by the BreakMode::break_word /
// BreakMode::anywhere paths to compute a proportional per-codepoint
// advance for in-segment splits. Skips continuation bytes (0x80–0xBF)
// so multi-byte sequences count as a single codepoint.
static int utf8_codepoint_count(const std::string& text) {
    int n = 0;
    for (unsigned char b : text) {
        if ((b & 0xC0) != 0x80) ++n;
    }
    return n;
}

// Walk `text` to the codepoint boundary that contains at most
// `target_count` codepoints (returns the byte offset of the first byte
// AFTER the Nth codepoint, clamped to text.size()). Used to slice a
// segment without breaking inside a multi-byte UTF-8 sequence.
static size_t utf8_byte_offset_for_codepoints(const std::string& text, int target_count) {
    if (target_count <= 0) return 0;
    int seen = 0;
    size_t i = 0;
    while (i < text.size()) {
        unsigned char b = static_cast<unsigned char>(text[i]);
        if ((b & 0xC0) != 0x80) {
            if (seen == target_count) return i;
            ++seen;
        }
        ++i;
    }
    return text.size();
}

static void append_fragment(ShapedLayout::Line& line,
                            const ShapedSegment& segment,
                            std::string text, float width) {
    if (text.empty()) return;
    line.fragments.push_back(
        {std::move(text), width, segment.style_index});
}

static void append_segment_range(ShapedLayout::Line& line,
                                 const std::vector<ShapedSegment>& segments,
                                 int begin, int end) {
    for (int i = begin; i < end; ++i) {
        if (segments[i].is_newline) continue;
        append_fragment(line, segments[i], segments[i].text, segments[i].width);
    }
}

static ShapedLayout layout_from_segments(const std::vector<ShapedSegment>& segments,
                                          float max_width, float explicit_line_height,
                                          float fallback_line_height, bool materialize,
                                          int max_lines = 0,
                                          BreakMode break_mode = BreakMode::normal) {
    ShapedLayout result;
    if (segments.empty()) return result;
    float y = 0;
    const auto finish_line = [&](ShapedLayout::Line& line, int begin, int end,
                                 int extra_segment = -1) {
        const auto include = [&](const ShapedSegment& segment) {
            if (segment.is_newline) return;
            line.ascent = std::max(line.ascent, segment.ascent);
            line.descent = std::max(line.descent, segment.descent);
            line.leading = std::max(line.leading, segment.leading);
        };
        for (int i = std::max(0, begin);
             i < std::min(end, static_cast<int>(segments.size())); ++i)
            include(segments[static_cast<std::size_t>(i)]);
        if (extra_segment >= 0 && extra_segment < static_cast<int>(segments.size()))
            include(segments[static_cast<std::size_t>(extra_segment)]);
        line.height = explicit_line_height > 0.0f
            ? explicit_line_height
            : line.ascent + line.descent + line.leading;
        if (line.height <= 0.0f) line.height = fallback_line_height;
        line.y = y;
        y += line.height;
    };

    // `white-space: nowrap` path. Force a single line that
    // includes every segment (including hard newlines flattened to
    // spaces, matching CSS nowrap behavior) so the caller sees the full
    // intrinsic width and can decide to truncate via ellipsis.
    // Skips the wrapping loop entirely so segments past `max_width` are
    // not silently dropped.
    if (max_lines == 1) {
        // Find a representative whitespace width so newlines collapsed
        // into spaces under CSS `white-space: nowrap` contribute the
        // same advance the engine would have used for a literal space.
        // Falls back to a line-height-relative estimate (≈ space/line
        // ratio in typical fonts) when the input has no whitespace
        // segments to sample.
        float whitespace_width = 0.0f;
        for (const auto& seg : segments) {
            if (seg.is_whitespace && !seg.is_newline) {
                whitespace_width = seg.width;
                break;
            }
        }
        if (whitespace_width == 0.0f && fallback_line_height > 0)
            whitespace_width = fallback_line_height * 0.18f;

        ShapedLayout::Line line;
        line.first_segment = 0;
        line.segment_count = static_cast<int>(segments.size());
        float w = 0;
        for (const auto& seg : segments) {
            if (seg.is_newline) {
                // CSS `white-space: nowrap` collapses hard breaks into
                // a single space — both visually (materialized text)
                // and in width (so ellipsis overflow detection sees the
                // collapsed advance).
                w += whitespace_width;
                if (materialize) {
                    line.text += ' ';
                    append_fragment(line, seg, " ", whitespace_width);
                }
            } else {
                w += seg.width;
                if (materialize) {
                    line.text += seg.text;
                    append_fragment(line, seg, seg.text, seg.width);
                }
            }
        }
        line.width = w;
        finish_line(line, 0, static_cast<int>(segments.size()));
        result.lines.push_back(std::move(line));
        result.total_width = w;
        result.total_height = y;
        result.line_count = 1;
        return result;
    }

    float current_width = 0;
    int line_start = 0;
    int last_break = -1;
    float width_at_break = 0;
    float max_line_width = 0;
    std::string carried_text;
    std::vector<ShapedLayout::Line::Fragment> carried_fragments;
    int carried_metric_segment = -1;
    const auto append_carried = [&](ShapedLayout::Line& line) {
        if (!materialize || carried_text.empty()) return;
        line.text += carried_text;
        line.fragments.insert(line.fragments.end(), carried_fragments.begin(),
                              carried_fragments.end());
    };
    const auto clear_carried = [&] {
        carried_text.clear();
        carried_fragments.clear();
        carried_metric_segment = -1;
    };

    for (int i = 0; i < static_cast<int>(segments.size()); ++i) {
        auto& seg = segments[i];

        if (seg.is_newline) {
            // Hard line break
            ShapedLayout::Line line;
            line.width = current_width;
            line.first_segment = line_start;
            line.segment_count = i - line_start;
            if (materialize) {
                append_carried(line);
                for (int j = line_start; j < i; ++j)
                    line.text += segments[j].text;
                append_segment_range(line, segments, line_start, i);
            }
            finish_line(line, line_start, i, carried_metric_segment);
            result.lines.push_back(std::move(line));
            clear_carried();
            max_line_width = std::max(max_line_width, current_width);

            current_width = 0;
            line_start = i + 1;
            last_break = -1;
            continue;
        }

        if (seg.is_whitespace) {
            last_break = i;
            width_at_break = current_width;
        }

        // break-word / anywhere also need to fire when the over-wide
        // segment is the FIRST on an empty line (current_width
        // == 0, but seg.width alone exceeds max_width). The legacy
        // `normal` path lets that overflow on its own line; break-word
        // and anywhere are supposed to slice it. Without this, e.g.
        // a single Lorem-style word longer than the column would render
        // identically to `normal`.
        const bool first_seg_overflows =
            (current_width == 0 && seg.width > max_width &&
             (break_mode == BreakMode::break_word ||
              break_mode == BreakMode::anywhere));

        if ((current_width + seg.width > max_width && current_width > 0) ||
            first_seg_overflows) {
            // Overflow-wrap / word-break decision point. CSS Text Module
            // Level 3 §6.1:
            //   - If a whitespace break opportunity exists on the current
            //     line, ALWAYS prefer it (matches `normal`, `break-word`,
            //     and `anywhere` — none of them split words when a soft
            //     break is available).
            //   - Otherwise, the segment-boundary fallback (current
            //     behavior) takes over for `normal`.
            //   - For `break-word` / `anywhere`, split THIS segment at
            //     the codepoint boundary that fits before max_width
            //     instead of overflowing whole-segment-wise. `anywhere`
            //     additionally allows mid-segment breaks even when
            //     subsequent segments would have fit a clean boundary;
            //     here the two modes coincide because we only enter this
            //     branch on actual overflow.
            const bool has_ws_break = last_break > line_start ||
                (!carried_text.empty() && last_break == line_start);
            if (break_mode == BreakMode::normal &&
                seg.joins_previous_word && !has_ws_break) {
                // A rich-text style boundary inside a word is not a soft-wrap
                // opportunity. Normal wrapping lets the whole logical word
                // overflow just as the single-style path does.
                current_width += seg.width;
                continue;
            }
            const bool allow_inside_segment =
                !has_ws_break && (break_mode == BreakMode::break_word ||
                                  break_mode == BreakMode::anywhere);

            if (allow_inside_segment && seg.width > 0) {
                // Proportional split: assume uniform per-codepoint
                // advance within this segment. The contract is "do not
                // overflow when a break opportunity exists" — CSS does
                // not require pixel-perfect break positions for soft-
                // wrap. Re-shaping each fragment would be more accurate
                // but defeats PreText's measure-once-reflow-forever
                // invariant. Browsers themselves use simplified
                // heuristics for this case.
                const float remain = max_width - current_width;
                const int cps = utf8_codepoint_count(seg.text);
                if (cps > 0 && remain > 0) {
                    const float per_cp = seg.width / static_cast<float>(cps);
                    int fit_cps = static_cast<int>(remain / per_cp);
                    // Always advance at least one codepoint so an
                    // unconditional infinite loop is impossible (e.g.
                    // current_width already at max_width with a wide
                    // glyph). The next iteration will start a new line.
                    if (fit_cps < 1) fit_cps = 1;
                    if (fit_cps > cps) fit_cps = cps;
                    const size_t cut = utf8_byte_offset_for_codepoints(seg.text, fit_cps);
                    const std::string head = seg.text.substr(0, cut);
                    const std::string tail = seg.text.substr(cut);
                    const float head_w = per_cp * static_cast<float>(fit_cps);
                    const float tail_w = seg.width - head_w;

                    ShapedLayout::Line line;
                    line.width = current_width + head_w;
                    line.first_segment = line_start;
                    line.segment_count = i - line_start;  // segments BEFORE this one stay grouped
                    if (materialize) {
                        append_carried(line);
                        for (int j = line_start; j < i; ++j)
                            line.text += segments[j].text;
                        line.text += head;
                        append_segment_range(line, segments, line_start, i);
                        append_fragment(line, seg, head, head_w);
                    }
                    finish_line(line, line_start, i + 1, carried_metric_segment);
                    result.lines.push_back(std::move(line));
                    clear_carried();
                    max_line_width = std::max(max_line_width, current_width + head_w);


                    // Emit the tail in repeated max_width chunks until
                    // what remains fits on a single line. For a segment
                    // many multiples wider than max_width (pathological
                    // input — Lorem-style or non-spaced CJK runs), this
                    // produces N - 1 max-width-wide intermediate lines
                    // followed by one trailing line for the remnant.
                    // `normal` mode would just overflow the entire
                    // tail on one line; break-word/anywhere have to do
                    // better than that to honor the CSS contract.
                    std::string remaining_text = tail;
                    float remaining_w = tail_w;
                    int safety = 0;
                    while (remaining_w > max_width && per_cp > 0 && safety < 1024) {
                        int chunk_cps = static_cast<int>(max_width / per_cp);
                        if (chunk_cps < 1) chunk_cps = 1;
                        const size_t chunk_cut = utf8_byte_offset_for_codepoints(remaining_text, chunk_cps);
                        const std::string chunk = remaining_text.substr(0, chunk_cut);
                        const float chunk_w = per_cp * static_cast<float>(chunk_cps);
                        ShapedLayout::Line chunk_line;
                        chunk_line.width = chunk_w;
                        chunk_line.first_segment = i;
                        chunk_line.segment_count = 1;
                        if (materialize) {
                            chunk_line.text = chunk;
                            append_fragment(chunk_line, seg, chunk, chunk_w);
                        }
                        finish_line(chunk_line, i, i + 1);
                        result.lines.push_back(std::move(chunk_line));
                        max_line_width = std::max(max_line_width, chunk_w);
                        remaining_text = remaining_text.substr(chunk_cut);
                        remaining_w -= chunk_w;
                        ++safety;
                    }

                    // Keep the final remnant as the current line prefix so a
                    // following styled span can use the remaining width. Its
                    // fragment retains this span's style even though the next
                    // loop iteration advances beyond segment i.
                    carried_text = std::move(remaining_text);
                    carried_fragments.clear();
                    if (materialize && !carried_text.empty())
                        carried_fragments.push_back(
                            {carried_text, remaining_w, seg.style_index});
                    line_start = i + 1;
                    current_width = remaining_w;
                    carried_metric_segment = carried_text.empty() ? -1 : i;
                    last_break = -1;
                    continue;
                }
                // cps == 0 falls through to legacy whole-segment path
            }

            // `normal`-mode behavior: break at last whitespace or segment
            // boundary, with no inside-word breaks.
            int break_at = has_ws_break ? last_break : i;
            float break_width = has_ws_break ? width_at_break : current_width;

            ShapedLayout::Line line;
            line.width = break_width;
            line.first_segment = line_start;
            line.segment_count = break_at - line_start;
            if (materialize) {
                append_carried(line);
                for (int j = line_start; j < break_at; ++j)
                    line.text += segments[j].text;
                append_segment_range(line, segments, line_start, break_at);
            }
            finish_line(line, line_start, break_at, carried_metric_segment);
            result.lines.push_back(std::move(line));
            clear_carried();
            max_line_width = std::max(max_line_width, break_width);


            // Skip whitespace at break point
            line_start = has_ws_break ? last_break + 1 : i;
            current_width = 0;
            for (int j = line_start; j <= i; ++j)
                current_width += segments[j].width;
            last_break = -1;
            continue;
        }

        current_width += seg.width;
    }

    // Final line
    if (line_start < static_cast<int>(segments.size()) || !carried_text.empty()) {
        ShapedLayout::Line line;
        line.width = current_width;
        line.first_segment = line_start;
        line.segment_count = static_cast<int>(segments.size()) - line_start;
        if (materialize) {
            append_carried(line);
            for (int j = line_start; j < static_cast<int>(segments.size()); ++j)
                line.text += segments[j].text;
            append_segment_range(line, segments, line_start,
                                 static_cast<int>(segments.size()));
        }
        finish_line(line, line_start, static_cast<int>(segments.size()),
                    carried_metric_segment);
        result.lines.push_back(std::move(line));
        max_line_width = std::max(max_line_width, current_width);
    }

    // Clamp to max_lines (>1) by dropping trailing lines.
    // max_lines=1 already short-circuits at the top of the function.
    if (max_lines > 1 && static_cast<int>(result.lines.size()) > max_lines) {
        result.lines.resize(static_cast<std::size_t>(max_lines));
        max_line_width = 0;
        for (const auto& line : result.lines)
            max_line_width = std::max(max_line_width, line.width);
        y = 0.0f;
        for (auto& line : result.lines) {
            line.y = y;
            y += line.height;
        }
    }

    result.total_width = max_line_width;
    result.total_height = y;
    result.line_count = static_cast<int>(result.lines.size());
    return result;
}

// ── PreparedText ────────────────────────────────────────────────────────

float PreparedText::total_width() const {
    float w = 0;
    for (auto& seg : segments_)
        w += seg.width;
    return w;
}

// ── TextShaper::Impl ────────────────────────────────────────────────────

struct TextShaper::Impl {
    bool has_real_shaping = false;

#if PULP_TEXT_SHAPER_SKIA
    static FontSlant font_slant_from_int(int slant) {
        if (slant == 2) return FontSlant::Oblique;
        if (slant == 1) return FontSlant::Italic;
        return FontSlant::Normal;
    }

    static SkFontStyle::Slant skia_slant_from_int(int slant) {
        if (slant == 2) return SkFontStyle::kOblique_Slant;
        if (slant == 1) return SkFontStyle::kItalic_Slant;
        return SkFontStyle::kUpright_Slant;
    }

    sk_sp<SkFontMgr> font_mgr;
    sk_sp<skia::textlayout::FontCollection> font_collection;

    // The platform font manager comes from the canonical helper in
    // bundled_fonts.cpp. Without a manager, SkFontMgr::RefEmpty()
    // returns no typefaces and `font.measureText()` reports near-zero
    // advance, collapsing Label::intrinsic_width().

    Impl() {
        font_mgr = platform_font_manager();
        if (!font_mgr) {
            font_mgr = SkFontMgr::RefEmpty();
        }
        // pulp emoji-parity — kept for backwards compat with the rest of
        // text_shaper.cpp. The real fallback-aware FontCollection lives
        // on the shared TextFontContext, which exposes it via
        // `font_collection()` and rebuilds on registration changes.
        font_collection = sk_sp<skia::textlayout::FontCollection>(
            new skia::textlayout::FontCollection());
        font_collection->setDefaultFontManager(font_mgr);
        has_real_shaping = true;
    }
#else
    Impl() { has_real_shaping = false; }
#endif

    // Segment cache: font_key -> (text -> segment_metrics)
    //
    // The weight is part of the key because it is part of the face: without it
    // the Bold and Regular renderings of one family at one size share a bucket,
    // so whichever is measured first answers for both. That makes resolving the
    // right face below a no-op on the second caller, and it fails in the
    // direction that looks like a wrapping bug rather than a caching one.
    struct CacheKey {
        std::string font_family;
        float font_size;
        int font_weight;
        int font_slant;
        float letter_spacing;
        std::vector<Canvas::FontFeature> font_features;
        bool operator==(const CacheKey& o) const {
            return font_family == o.font_family && font_size == o.font_size &&
                   font_weight == o.font_weight && font_slant == o.font_slant &&
                   letter_spacing == o.letter_spacing &&
                   font_features == o.font_features;
        }
    };
    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            size_t hash = std::hash<std::string>{}(k.font_family) ^
                          (std::hash<float>{}(k.font_size) << 16) ^
                          (std::hash<int>{}(k.font_weight) << 8) ^
                          (std::hash<int>{}(k.font_slant) << 4) ^
                          std::hash<float>{}(k.letter_spacing);
            for (const auto& feature : k.font_features) {
                hash ^= std::hash<std::uint32_t>{}(feature.tag) +
                        0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= std::hash<std::uint32_t>{}(feature.value) +
                        0x9e3779b9u + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    };
    std::unordered_map<CacheKey, std::unordered_map<std::string, float>, CacheKeyHash> cache;
    std::mutex cache_mutex;
    // Snapshot of `font_registration_generation()` when `cache` was last
    // populated. If the generation advances (because `register_font(...)`
    // or `register_emoji_fallback(...)` ran), the cache must be flushed —
    // otherwise a label measured before an emoji fallback was wired up
    // keeps its tofu-width forever.
    std::uint64_t cached_generation = 0;

    // Metrics cache keyed by (font_family, font_size). Stores
    // SkFontMetrics-derived ascent/descent/leading once per typeface so
    // every measure_metrics call after the first is pure cache hit. Same
    // PreText "measure once, reuse forever" model as the segment cache.
    struct LineBox {
        float ascent = 0;   // positive distance above baseline (Skia stores it negative)
        float descent = 0;  // positive distance below baseline
        float leading = 0;  // extra inter-line gap
        float line_height = 0;  // ascent + descent + leading
        // fAscent/fDescent, flipped positive and never widened to fTop/fBottom.
        float typographic_ascent = 0;
        float typographic_descent = 0;
        bool real = false;      // true if derived from SkFontMetrics, false for the heuristic fallback
    };
    std::unordered_map<CacheKey, LineBox, CacheKeyHash> metrics_cache;
    std::uint64_t metrics_cached_generation = 0;
    std::mutex metrics_mutex;

#if PULP_TEXT_SHAPER_SKIA
    // Typeface resolution routes through FontResolver. Comma-list parsing
    // happens once inside the resolver; the registered → bundled →
    // platform cascade is shared with skia_canvas. Returns null when
    // no family matched and there's no platform fallback (non-Skia
    // build, RefEmpty mgr, etc.).
    sk_sp<SkTypeface> resolve_typeface(const std::string& font_family,
                                       int font_weight, int font_slant = 0) {
        FontOptions opts;
        opts.weight = static_cast<float>(font_weight);
        opts.slant = font_slant_from_int(font_slant);
        // Refuse a platform face whose name does not overlap the requested
        // family, so a miss keeps walking the stack instead of stopping at the
        // host default — which is what `measure_segment`'s own walk did before
        // it was folded in here. Line metrics did NOT: they went through this
        // resolver at its permissive default, so a family stack whose first
        // entry the host lacks could take its ascent from the substitute and
        // its advances from a later entry. One mode for both is the point.
        opts.fallback_mode = FallbackMode::Deterministic;
        // Entries keep their quotes; the resolver reads a quoted entry as a
        // literal family name rather than a generic keyword.
        opts.family_stack = split_css_font_family_list(font_family);
        auto resolved = FontResolver::instance().resolve_family_list(opts);
        return resolved.typeface;
    }
#endif

    LineBox measure_metrics(const std::string& font_family, float font_size,
                            int font_weight, int font_slant = 0) {
        CacheKey key{font_family, font_size, font_weight, font_slant, 0.0f, {}};
        std::uint64_t measurement_generation = 0;
        {
            std::lock_guard<std::mutex> lock(metrics_mutex);
            const auto generation = font_registration_generation();
            measurement_generation = generation;
            if (metrics_cached_generation != generation) {
                metrics_cache.clear();
                metrics_cached_generation = generation;
            }
            auto it = metrics_cache.find(key);
            if (it != metrics_cache.end()) return it->second;
        }

        LineBox box{};
        // Keep fallback metrics internally coherent.  GPU-off builds do not
        // have a typeface to query, but attributed layout still needs
        // per-span ascent/descent/leading components so a 28 px run produces
        // a taller line than a 10 px run.  A line-height-only fallback makes
        // every segment carry zero metrics and collapses all automatic
        // attributed lines onto the paragraph-wide maximum.
        box.ascent = font_size * 0.8f;
        box.descent = font_size * 0.2f;
        box.leading = font_size * 0.5f;
        box.line_height = box.ascent + box.descent + box.leading;
        box.typographic_ascent = box.ascent;
        box.typographic_descent = box.descent;
        box.real = false;

#if PULP_TEXT_SHAPER_SKIA
        SkFont font;
        sk_sp<SkTypeface> tf = resolve_typeface(font_family, font_weight, font_slant);
        if (tf) font.setTypeface(std::move(tf));
        font.setSize(font_size);
        if (font.getTypeface()) {
            SkFontMetrics m;
            font.getMetrics(&m);
            // Use fTop / fBottom (the WORST-CASE glyph bbox extents)
            // rather than fAscent / fDescent (the
            // RECOMMENDED ascent/descent for Latin-only layout).
            //
            // The difference matters at small font sizes: fAscent is
            // the line above which TYPICAL glyphs fit, but caps and
            // accents on some fonts (IBM Plex Mono among them) extend
            // above fAscent. Boxes sized to fAscent + fDescent clip
            // the cap-tops on those glyphs. fTop / fBottom guarantee
            // every glyph fits — the cost is slightly taller line
            // boxes for fonts where the worst-case glyph sits well
            // above the typical ascent (display fonts with
            // exaggerated accents), but that's still the right
            // tradeoff for "imported design renders correctly".
            //
            // Skia convention: fAscent / fTop are NEGATIVE (above
            // baseline), fDescent / fBottom POSITIVE (below baseline),
            // fLeading the extra inter-line gap.
            const float top    = m.fTop    < m.fAscent  ? m.fTop    : m.fAscent;
            const float bottom = m.fBottom > m.fDescent ? m.fBottom : m.fDescent;
            box.ascent  = -top;           // worst-case distance above baseline (positive)
            box.descent =  bottom;        // worst-case distance below baseline (positive)
            box.typographic_ascent = -m.fAscent;
            box.typographic_descent = m.fDescent;
            box.leading =  m.fLeading > 0 ? m.fLeading : 0;
            // Raw fTop/fBottom is now the default line box. The legacy
            // empirical `0.5 * font_size` safety margin was a stopgap for
            // small-font clipping before baseline and anchor semantics were
            // aligned; TextShaper/SkFont parity coverage now guards that
            // path, so the margin stays opt-in via
            // `PULP_FONT_LEGACY_SAFETY_MARGIN=1` for bisection or A/B
            // regression triage.
            const bool legacy_margin =
                std::getenv("PULP_FONT_LEGACY_SAFETY_MARGIN") != nullptr;
            const float safety = legacy_margin ? font_size * 0.5f : 0.0f;
            box.line_height = box.ascent + box.descent + box.leading + safety;
            box.real = true;
        }
#elif defined(__APPLE__)
        // Without Skia the CoreGraphics canvas paints through CoreText, so
        // measure the face it will paint with. The definitions mirror Skia's
        // CoreText backend: worst-case extents from the font bounding box,
        // typographic ascent/descent/leading from the font.
        {
            const auto m =
                detail::coretext_face_metrics(font_family, font_size, font_weight, font_slant);
            if (m.ascent > 0.0f) {
                box.ascent = std::max(m.top, m.ascent);
                box.descent = std::max(m.bottom, m.descent);
                box.typographic_ascent = m.ascent;
                box.typographic_descent = m.descent;
                box.leading = std::max(0.0f, m.leading);
                box.line_height = box.ascent + box.descent + box.leading;
                box.real = true;
            }
        }
#endif

        if (std::getenv("PULP_METRICS_TRACE")) {
            std::fprintf(stderr, "[metrics] family='%s' size=%.1f ascent=%.2f descent=%.2f leading=%.2f line_height=%.2f real=%d\n",
                         font_family.c_str(), font_size, box.ascent, box.descent,
                         box.leading, box.line_height, box.real ? 1 : 0);
        }
        {
            std::lock_guard<std::mutex> lock(metrics_mutex);
            // A face may have been registered while this measurement was in
            // flight. Never publish a pre-registration fallback under the new
            // generation; the next caller will measure against the new face.
            const auto generation = font_registration_generation();
            if (generation == measurement_generation &&
                metrics_cached_generation == measurement_generation) {
                metrics_cache[key] = box;
            }
        }
        return box;
    }

    LineBox measure_text_metrics(const std::string& text,
                                 const std::string& font_family,
                                 float font_size, int font_weight,
                                 int font_slant) {
        LineBox box;
#if PULP_TEXT_SHAPER_SKIA
        auto ctx = TextFontContext::shared();
        auto collection = ctx->font_collection();
        if (!collection || text.empty()) return box;
        skia::textlayout::ParagraphStyle paragraph_style;
        skia::textlayout::TextStyle text_style;
        std::vector<SkString> families;
        for (const auto& entry : split_css_font_family_list(font_family))
            families.emplace_back(platform_family_for_css_family(entry).c_str());
        const std::string emoji_family = ctx->emoji_family_name();
        if (!emoji_family.empty()) families.emplace_back(emoji_family.c_str());
        if (families.empty()) families.emplace_back("");
        text_style.setFontFamilies(families);
        text_style.setFontSize(font_size);
        apply_auto_optical_size(
            text_style, resolve_typeface(font_family, font_weight, font_slant).get(), font_size);
        text_style.setFontStyle(SkFontStyle(
            font_weight, SkFontStyle::kNormal_Width,
            skia_slant_from_int(font_slant)));
        paragraph_style.setFakeMissingFontStyles(
            paragraph_fakes_missing_font_styles(font_family, font_weight, font_slant));
        paragraph_style.setTextStyle(text_style);
        auto collection_use = ctx->lock_font_collection_use();
        auto builder = skia::textlayout::ParagraphBuilder::make(
            paragraph_style, collection, shared_sk_unicode());
        if (!builder) return box;
        builder->addText(text.c_str(), text.size());
        auto paragraph = builder->Build();
        if (!paragraph) return box;
        paragraph->layout(SK_ScalarInfinity);
        std::vector<skia::textlayout::LineMetrics> lines;
        paragraph->getLineMetrics(lines);
        std::vector<ParagraphLineExtent> extents;
        extents.reserve(lines.size());
        for (const auto& line : lines) {
            extents.push_back({static_cast<float>(line.fAscent), static_cast<float>(line.fDescent),
                               static_cast<float>(line.fHeight)});
        }
        const auto measured = paragraph_text_extents(extents, font_size);
        box.ascent = measured.ascent;
        box.descent = measured.descent;
        box.leading = measured.leading;
        box.line_height = box.ascent + box.descent + box.leading;
        box.real = box.line_height > 0.0f;
#else
        (void)text;
        (void)font_family;
        (void)font_size;
        (void)font_weight;
        (void)font_slant;
#endif
        return box;
    }

    float measure_segment(
        const std::string& text, const std::string& font_family,
        float font_size, int font_weight, int font_slant,
        float letter_spacing,
        const std::vector<Canvas::FontFeature>& font_features) {
        std::uint64_t current_gen = font_registration_generation();

        // Check cache first
        CacheKey key{font_family, font_size, font_weight, font_slant,
                     letter_spacing, font_features};
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            if (current_gen != cached_generation) {
                cache.clear();
                cached_generation = current_gen;
            }
            auto font_it = cache.find(key);
            if (font_it != cache.end()) {
                auto seg_it = font_it->second.find(text);
                if (seg_it != font_it->second.end())
                    return seg_it->second;
            }
        }

        float width = 0;
        bool styled_measurement = false;

#if PULP_TEXT_SHAPER_SKIA
        // Measure through the same fallback-aware SkParagraph path used by
        // SkiaCanvas paint. Besides honoring slant, tracking, and OpenType
        // features, this keeps plain and styled CJK/emoji runs on the same
        // resolved fallback faces; otherwise changing only tracking can also
        // change the measured typeface and corrupt the apparent advance.
        {
            auto ctx = TextFontContext::shared();
            auto collection = ctx->font_collection();
            if (collection) {
                skia::textlayout::ParagraphStyle paragraph_style;
                skia::textlayout::TextStyle text_style;
                std::vector<SkString> families;
                for (const auto& entry : split_css_font_family_list(font_family))
                    families.emplace_back(platform_family_for_css_family(entry).c_str());
                const std::string emoji_family = ctx->emoji_family_name();
                if (!emoji_family.empty()) families.emplace_back(emoji_family.c_str());
                if (families.empty()) families.emplace_back("");
                text_style.setFontFamilies(families);
                const auto primary = resolve_typeface(font_family, font_weight, font_slant);
                text_style.setFontSize(font_size);
                apply_auto_optical_size(text_style, primary.get(), font_size);
                text_style.setFontStyle(SkFontStyle(
                    font_weight, SkFontStyle::kNormal_Width,
                    skia_slant_from_int(font_slant)));
                // The painter adds the primary face's AAT tracking to the
                // letter-spacing; measure with the same spacing it draws.
                const float tracking = aat_tracking_px(primary.get(), font_size);
                if (letter_spacing + tracking != 0.0f)
                    text_style.setLetterSpacing(letter_spacing + tracking);
                for (const auto& feature : font_features) {
                    char tag_chars[4] = {
                        static_cast<char>((feature.tag >> 24) & 0xFF),
                        static_cast<char>((feature.tag >> 16) & 0xFF),
                        static_cast<char>((feature.tag >> 8) & 0xFF),
                        static_cast<char>(feature.tag & 0xFF),
                    };
                    text_style.addFontFeature(
                        SkString(tag_chars, 4),
                        static_cast<int>(feature.value));
                }
                paragraph_style.setFakeMissingFontStyles(
                    paragraph_fakes_missing_font_styles(font_family, font_weight, font_slant));
                paragraph_style.setTextStyle(text_style);
                auto collection_use = ctx->lock_font_collection_use();
                auto builder = skia::textlayout::ParagraphBuilder::make(
                    paragraph_style, collection, shared_sk_unicode());
                if (builder) {
                    builder->addText(text.c_str(), text.size());
                    auto paragraph = builder->Build();
                    if (paragraph) {
                        paragraph->layout(SK_ScalarInfinity);
                        width = std::max(paragraph->getLongestLine(),
                                         paragraph->getMaxIntrinsicWidth());
                        styled_measurement = width > 0.0f;
                    }
                }
            }
        }

        // Real measurement via SkFont. Use the platform font manager
        // (CoreText/DirectWrite/fontconfig/Android) — RefEmpty() returns
        // no typefaces and silently produces ~0 advance widths, which
        // is what collapses Label measurements.
        SkFont font;
        // One cascade, shared with measure_metrics and with the painter.
        //
        // This used to be a second, hand-rolled walk of the family list with
        // `SkFontStyle::Normal()` written into each lookup, which had two
        // consequences beyond the duplication: a bold run was measured through
        // the regular face, and a VARIABLE font could not be measured at the
        // requested weight at all, because instancing the `wght` axis lives in
        // FontResolver and nothing here called it. Segment widths and line
        // metrics could also resolve different faces for one family stack,
        // since only the metrics path went through the resolver.
        sk_sp<SkTypeface> typeface =
            resolve_typeface(font_family, font_weight, font_slant);

        // Final fallback — platform default. Used only when every
        // family in the list missed (or no list was provided). Still asked for
        // at the requested weight: a fallback face is the wrong family already,
        // and drawing it at the wrong weight too is a second error, not a
        // smaller one.
        if (!typeface && font_mgr && platform_font_db_usable()) {
            typeface = font_mgr->matchFamilyStyle(
                nullptr, SkFontStyle(font_weight, SkFontStyle::kNormal_Width,
                                     SkFontStyle::kUpright_Slant));
        }
        // No system font database at all (browser): the platform default is a
        // glyph-less face that measures every string at zero. Use a bundled
        // face instead so measurement matches what FontResolver will paint.
        if (!typeface) {
            typeface = bundled_fallback_typeface();
        }
        if (typeface) font.setTypeface(std::move(typeface));
        font.setSize(font_size);
        font.setEdging(SkFont::Edging::kSubpixelAntiAlias);

        if (width <= 0.0f && font.getTypeface()) {
            // Use the advance width (return value), not bounds.width().
            // Advance includes whitespace and proper glyph spacing;
            // bounds.width() can exclude trailing spaces and differ for
            // overhangs.
            width = font.measureText(text.c_str(), text.size(),
                                     SkTextEncoding::kUTF8, nullptr);

            // pulp emoji-parity — `SkFont::measureText` runs the
            // primary typeface against every codepoint. Emoji codepoints
            // (no glyph in Inter etc.) return tofu/.notdef advance,
            // which collapses Label widths for any string mixing text +
            // emoji. When `contains_emoji(text)` is true, re-measure
            // via `ParagraphBuilder` using the shared TextFontContext's
            // FontCollection — which has the registered color-emoji
            // typeface in its default-family list, so emoji clusters
            // shape against the right face and report real advance.
            if (contains_emoji(text)) {
                auto ctx = TextFontContext::shared();
                auto fc = ctx->font_collection();
                if (fc) {
                    skia::textlayout::ParagraphStyle pstyle;
                    skia::textlayout::TextStyle tstyle;
                    std::vector<SkString> families;
                    families.emplace_back(font_family.c_str());
                    std::string emoji_family = ctx->emoji_family_name();
                    if (!emoji_family.empty()) {
                        families.emplace_back(emoji_family.c_str());
                    }
                    tstyle.setFontFamilies(families);
                    tstyle.setFontSize(font_size);
                    apply_auto_optical_size(tstyle, typeface.get(), font_size);
                    pstyle.setTextStyle(tstyle);
                    auto collection_use = ctx->lock_font_collection_use();
                    auto pb = skia::textlayout::ParagraphBuilder::make(
                        pstyle, fc, shared_sk_unicode());
                    if (pb) {
                        pb->addText(text.c_str(), text.size());
                        auto paragraph = pb->Build();
                        if (paragraph) {
                            paragraph->layout(SK_ScalarInfinity);
                            float pwidth = paragraph->getMaxIntrinsicWidth();
                            if (pwidth > 0) width = pwidth;
                        }
                    }
                }
            }
        } else if (width <= 0.0f) {
            // No platform font manager (or all matchers failed) — fall
            // back to the same character-width estimator the non-Skia
            // build uses, so callers still get a sane positive width.
            width = static_cast<float>(text.size()) * font_size * 0.6f;
        }
#elif defined(__APPLE__)
        // Measure with the CoreText face the CoreGraphics canvas paints.
        width = detail::coretext_advance(text, font_family, font_size, font_weight, font_slant,
                                         font_features);
        if (width <= 0.0f && !text.empty())
            width = static_cast<float>(text.size()) * font_size * 0.6f;
#else
        // Fallback: character-width estimation
        width = static_cast<float>(text.size()) * font_size * 0.6f;
#endif

        if (letter_spacing != 0.0f && width > 0.0f &&
            !styled_measurement) {
            // The SkParagraph styled path already included tracking. The
            // plain/non-shaping fallback does not, so add one CSS tracking
            // step per codepoint to match Label intrinsic sizing and paint.
            width += static_cast<float>(utf8_codepoint_count(text)) *
                     letter_spacing;
        }

        // Cache the result
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            // Re-check: a concurrent register_font() between the earlier
            // probe and this insert would otherwise leave us caching a
            // measurement that pre-dates the new registration.
            std::uint64_t now_gen = font_registration_generation();
            if (now_gen != cached_generation) {
                cache.clear();
                cached_generation = now_gen;
            }
            if (now_gen == current_gen)
                cache[key][text] = width;
        }

        return width;
    }
};

// ── TextShaper ──────────────────────────────────────────────────────────

TextShaper::TextShaper() : impl_(std::make_unique<Impl>()) {}
TextShaper::~TextShaper() = default;

PreparedText TextShaper::prepare(
    std::string_view text, std::string_view font_family, float font_size,
    int font_weight, int font_slant, float letter_spacing,
    const std::vector<Canvas::FontFeature>& font_features) {
    detail_prepare_calls().fetch_add(1, std::memory_order_relaxed);
    // SkFont's UTF-8 decoder reports malformed input as a negative glyph
    // count; its temporary glyph buffer then treats that as an enormous
    // allocation and aborts the process. Text can cross several external
    // boundaries before reaching the shaper, so make this core seam total:
    // preserve valid input byte-for-byte and replace each malformed byte.
    std::string repaired_text;
    if (!valid_utf8(text)) {
        repaired_text = replace_invalid_utf8(text);
        text = repaired_text;
    }
    PreparedText result;
    result.font_family_ = std::string(font_family);
    result.font_size_ = font_size;
    result.font_weight_ = font_weight;
    // Ask the impl for real SkFontMetrics-derived line height. Falls back
    // to font_size * 1.5 only when there's no
    // resolvable typeface (non-Skia build, empty font manager, etc.).
    // Cached per (family, size) so repeated layout calls hit pure
    // arithmetic — same PreText "measure once" guarantee that already
    // drives the segment width cache.
    auto box = impl_->measure_metrics(result.font_family_, font_size, font_weight,
                                      font_slant);
    const auto shaped_box = impl_->measure_text_metrics(
        std::string(text), result.font_family_, font_size, font_weight,
        font_slant);
    if (shaped_box.real) {
        box.ascent = std::max(box.ascent, shaped_box.ascent);
        box.descent = std::max(box.descent, shaped_box.descent);
        box.leading = std::max(box.leading, shaped_box.leading);
        box.line_height = box.ascent + box.descent + box.leading;
        box.real = true;
    }
    result.line_height_ = box.line_height;
    result.ascent_       = box.ascent;
    result.descent_      = box.descent;
    result.leading_      = box.leading;
    // From the face alone: shaping a string widens only the worst-case extents.
    result.typographic_ascent_ = box.typographic_ascent;
    result.typographic_descent_ = box.typographic_descent;
    result.metrics_real_ = box.real;

    // Segment the text: split on whitespace and newlines
    std::string current;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];

        if (c == '\n') {
            if (!current.empty()) {
                ShapedSegment seg;
                seg.text = current;
                seg.width = impl_->measure_segment(
                    current, result.font_family_, font_size, font_weight,
                    font_slant, letter_spacing, font_features);
                result.segments_.push_back(std::move(seg));
                current.clear();
            }
            ShapedSegment nl;
            nl.text = "\n";
            nl.is_newline = true;
            result.segments_.push_back(std::move(nl));
        } else if (c == ' ' || c == '\t') {
            if (!current.empty()) {
                ShapedSegment seg;
                seg.text = current;
                seg.width = impl_->measure_segment(
                    current, result.font_family_, font_size, font_weight,
                    font_slant, letter_spacing, font_features);
                result.segments_.push_back(std::move(seg));
                current.clear();
            }
            ShapedSegment ws;
            ws.text = std::string(1, c);
            ws.width = impl_->measure_segment(
                ws.text, result.font_family_, font_size, font_weight,
                font_slant, letter_spacing, font_features);
            ws.is_whitespace = true;
            result.segments_.push_back(std::move(ws));
        } else {
            current += c;
        }
    }

    if (!current.empty()) {
        ShapedSegment seg;
        seg.text = current;
        seg.width = impl_->measure_segment(
            current, result.font_family_, font_size, font_weight,
            font_slant, letter_spacing, font_features);
        result.segments_.push_back(std::move(seg));
    }

    for (auto& segment : result.segments_) {
        segment.ascent = result.ascent_;
        segment.descent = result.descent_;
        segment.leading = result.leading_;
    }

    return result;
}

PreparedText TextShaper::prepare(
    const AttributedString& text,
    const std::vector<Canvas::FontFeature>& font_features) {
    detail_prepare_calls().fetch_add(1, std::memory_order_relaxed);
    // For attributed strings, prepare each span separately
    PreparedText result;
    if (text.empty()) return result;

    auto& first_span = text.spans()[0];
    result.font_family_ = first_span.font_family;
    result.font_size_ = first_span.font_size;
    result.font_weight_ = first_span.font_weight;
    float fallback_line_height = 0.0f;

    for (std::size_t span_index = 0; span_index < text.spans().size(); ++span_index) {
        const auto& span = text.spans()[span_index];
        // The span's own weight, not the run's first: a bold word inside a
        // regular sentence is exactly what an attributed string is for, and
        // measuring it at the paragraph's weight defeats the point.
        auto span_prepared = prepare(
            span.text, span.font_family, span.font_size, span.font_weight,
            span.font_slant != 0 ? span.font_slant : (span.italic ? 1 : 0),
            span.letter_spacing, font_features);
        // A mixed-size line needs the largest real metric on each side of the
        // shared baseline. Taking only the largest aggregate line height can
        // still clip (for example, a tall ascender in one face plus a deep
        // descender in another). Preserve the component maxima and derive the
        // common line box from them.
        result.ascent_ = std::max(result.ascent_, span_prepared.ascent_);
        result.descent_ = std::max(result.descent_, span_prepared.descent_);
        result.leading_ = std::max(result.leading_, span_prepared.leading_);
        result.typographic_ascent_ =
            std::max(result.typographic_ascent_, span_prepared.typographic_ascent_);
        result.typographic_descent_ =
            std::max(result.typographic_descent_, span_prepared.typographic_descent_);
        result.metrics_real_ = result.metrics_real_ || span_prepared.metrics_real_;
        fallback_line_height = std::max(fallback_line_height,
                                        span_prepared.line_height_);
        const bool joins_previous_word = !result.segments_.empty() &&
            !result.segments_.back().is_whitespace &&
            !result.segments_.back().is_newline &&
            !span_prepared.segments_.empty() &&
            !span_prepared.segments_.front().is_whitespace &&
            !span_prepared.segments_.front().is_newline;
        for (auto& segment : span_prepared.segments_)
            segment.style_index = static_cast<int>(span_index);
        if (joins_previous_word)
            span_prepared.segments_.front().joins_previous_word = true;
        result.segments_.insert(result.segments_.end(),
                               span_prepared.segments_.begin(),
                               span_prepared.segments_.end());
    }
    result.line_height_ = result.ascent_ + result.descent_ + result.leading_;
    if (result.line_height_ <= 0.0f)
        result.line_height_ = fallback_line_height;

    return result;
}

ShapedLayout TextShaper::layout(const PreparedText& prepared, float max_width,
                                 float line_height, int max_lines,
                                 BreakMode break_mode) const {
    return layout_from_segments(prepared.segments(), max_width, line_height,
                                prepared.line_height(), false, max_lines, break_mode);
}

ShapedLayout TextShaper::layout_with_lines(const PreparedText& prepared, float max_width,
                                            float line_height, int max_lines,
                                            BreakMode break_mode) const {
    return layout_from_segments(prepared.segments(), max_width, line_height,
                                prepared.line_height(), true, max_lines, break_mode);
}

float TextShaper::measure_height(const PreparedText& prepared, float max_width,
                                  float line_height) const {
    auto result = layout(prepared, max_width, line_height);
    return result.total_height;
}

int TextShaper::count_lines(const PreparedText& prepared, float max_width) const {
    auto result = layout(prepared, max_width);
    return result.line_count;
}

void TextShaper::clear_cache() {
    std::lock_guard<std::mutex> lock(impl_->cache_mutex);
    impl_->cache.clear();
}

bool TextShaper::uses_real_shaping() const {
    return impl_->has_real_shaping;
}

ParagraphTextExtents paragraph_text_extents(const std::vector<ParagraphLineExtent>& lines,
                                            float font_size) {
    ParagraphTextExtents extents;
    const float metric_limit = std::max(1.0f, font_size) * 10.0f;
    for (const auto& line : lines) {
        if (!std::isfinite(line.ascent) || !std::isfinite(line.descent) ||
            !std::isfinite(line.height) || line.ascent < 0.0f || line.descent < 0.0f ||
            line.ascent > metric_limit || line.descent > metric_limit || line.height <= 0.0f ||
            line.height > metric_limit) {
            continue;
        }
        extents.ascent = std::max(extents.ascent, line.ascent);
        extents.descent = std::max(extents.descent, line.descent);
        extents.leading =
            std::max(extents.leading, std::max(0.0f, line.height - line.ascent - line.descent));
        extents.real = true;
    }
    if (extents.line_height() <= 0.0f)
        extents.real = false;
    return extents;
}

TextShaper& global_text_shaper() {
    static TextShaper shaper;
    return shaper;
}

std::uint64_t text_shaper_prepare_call_count() {
    return detail_prepare_calls().load(std::memory_order_relaxed);
}

}  // namespace pulp::canvas
