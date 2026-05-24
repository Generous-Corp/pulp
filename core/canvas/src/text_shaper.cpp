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
#include <algorithm>
#include <cmath>
#include <sstream>
#include <numeric>

#ifdef PULP_HAS_TEXT_SHAPING
// SkParagraph headers — available when Skia is built with text shaping
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "skia_unicode.hpp"
#include "modules/skshaper/include/SkShaper.h"
#include "include/core/SkFontMgr.h"

#include <pulp/canvas/text_font_context.hpp>

// Platform font managers — must mirror what SkiaCanvas uses, otherwise
// `mgr->matchFamilyStyle("Inter", ...)` returns null and SkFont falls
// back to a typeface-less default whose `measureText()` advance is
// effectively zero. That collapses Label::intrinsic_width() and Yoga
// reserves no horizontal space for the label, which paints into the
// (now-tiny) box and clips. See pulp #945 (regression of #935 / #928).
#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#elif defined(__ANDROID__)
#include "include/ports/SkFontMgr_android.h"
#include "include/ports/SkFontScanner_FreeType.h"
#elif defined(__linux__)
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#endif
#endif

namespace pulp::canvas {

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

static ShapedLayout layout_from_segments(const std::vector<ShapedSegment>& segments,
                                          float max_width, float line_height, bool materialize,
                                          int max_lines = 0,
                                          BreakMode break_mode = BreakMode::normal) {
    ShapedLayout result;
    if (segments.empty()) return result;

    // pulp #1410 — `white-space: nowrap` path. Force a single line that
    // includes every segment (including hard newlines flattened to
    // spaces, matching CSS nowrap behavior) so the caller sees the full
    // intrinsic width and can decide to truncate via #1407's ellipsis.
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
        if (whitespace_width == 0.0f && line_height > 0)
            whitespace_width = line_height * 0.18f;

        ShapedLayout::Line line;
        line.first_segment = 0;
        line.segment_count = static_cast<int>(segments.size());
        line.y = 0;
        float w = 0;
        for (const auto& seg : segments) {
            if (seg.is_newline) {
                // CSS `white-space: nowrap` collapses hard breaks into
                // a single space — both visually (materialized text)
                // and in width (so #1407's overflow detection sees the
                // collapsed advance).
                w += whitespace_width;
                if (materialize) line.text += ' ';
            } else {
                w += seg.width;
                if (materialize) line.text += seg.text;
            }
        }
        line.width = w;
        result.lines.push_back(std::move(line));
        result.total_width = w;
        result.total_height = line_height;
        result.line_count = 1;
        return result;
    }

    float current_width = 0;
    int line_start = 0;
    float y = 0;
    int last_break = -1;
    float width_at_break = 0;
    float max_line_width = 0;

    for (int i = 0; i < static_cast<int>(segments.size()); ++i) {
        auto& seg = segments[i];

        if (seg.is_newline) {
            // Hard line break
            ShapedLayout::Line line;
            line.width = current_width;
            line.y = y;
            line.first_segment = line_start;
            line.segment_count = i - line_start;
            if (materialize) {
                for (int j = line_start; j < i; ++j)
                    line.text += segments[j].text;
            }
            result.lines.push_back(std::move(line));
            max_line_width = std::max(max_line_width, current_width);

            y += line_height;
            current_width = 0;
            line_start = i + 1;
            last_break = -1;
            continue;
        }

        if (seg.is_whitespace) {
            last_break = i;
            width_at_break = current_width;
        }

        // pulp #1737 — break-word / anywhere also need to fire when the
        // over-wide segment is the FIRST on an empty line (current_width
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
            // pulp #1737 — overflow-wrap / word-break decision point.
            // CSS Text Module Level 3 §6.1:
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
            const bool has_ws_break = (last_break > line_start);
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
                    line.y = y;
                    line.first_segment = line_start;
                    line.segment_count = i - line_start;  // segments BEFORE this one stay grouped
                    if (materialize) {
                        for (int j = line_start; j < i; ++j)
                            line.text += segments[j].text;
                        line.text += head;
                    }
                    result.lines.push_back(std::move(line));
                    max_line_width = std::max(max_line_width, current_width + head_w);

                    y += line_height;

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
                        chunk_line.y = y;
                        chunk_line.first_segment = i;
                        chunk_line.segment_count = 1;
                        if (materialize) chunk_line.text = chunk;
                        result.lines.push_back(std::move(chunk_line));
                        max_line_width = std::max(max_line_width, chunk_w);
                        y += line_height;
                        remaining_text = remaining_text.substr(chunk_cut);
                        remaining_w -= chunk_w;
                        ++safety;
                    }

                    // Final remnant of this segment must be emitted as
                    // its own line right now — `current_width` would
                    // preserve the numeric width into the next iteration
                    // but the textual content (remaining_text) belongs
                    // to segment `i`, not to any of segments[i+1..end).
                    // The materialization loops downstream only walk
                    // `segments[j].text`, so leaving the remnant in
                    // current_width would silently drop the characters
                    // when followed by ANY further segments (Codex P1
                    // on #1795 / pulp #1737: "Preserve split-word
                    // remainder before subsequent segments"). The
                    // tradeoff: the remnant gets its own line instead
                    // of combining with the following segment on the
                    // same visual line — minor cosmetic vs. data loss.
                    //
                    // Also handles the last-segment case correctly: an
                    // unconditional emit means we always materialize
                    // the remnant, regardless of whether more segments
                    // follow.
                    {
                        ShapedLayout::Line tail_line;
                        tail_line.width = remaining_w;
                        tail_line.y = y;
                        tail_line.first_segment = i;
                        tail_line.segment_count = 1;
                        if (materialize) tail_line.text = remaining_text;
                        result.lines.push_back(std::move(tail_line));
                        max_line_width = std::max(max_line_width, remaining_w);
                        y += line_height;
                    }
                    line_start = i + 1;
                    current_width = 0;
                    last_break = -1;
                    continue;
                }
                // cps == 0 falls through to legacy whole-segment path
            }

            // Legacy `normal`-mode behavior: break at last whitespace or
            // segment boundary (no inside-word breaks). Same path as
            // pre-#1737.
            int break_at = has_ws_break ? last_break : i;
            float break_width = has_ws_break ? width_at_break : current_width;

            ShapedLayout::Line line;
            line.width = break_width;
            line.y = y;
            line.first_segment = line_start;
            line.segment_count = break_at - line_start;
            if (materialize) {
                for (int j = line_start; j < break_at; ++j)
                    line.text += segments[j].text;
            }
            result.lines.push_back(std::move(line));
            max_line_width = std::max(max_line_width, break_width);

            y += line_height;

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
    if (line_start < static_cast<int>(segments.size())) {
        ShapedLayout::Line line;
        line.width = current_width;
        line.y = y;
        line.first_segment = line_start;
        line.segment_count = static_cast<int>(segments.size()) - line_start;
        if (materialize) {
            for (int j = line_start; j < static_cast<int>(segments.size()); ++j)
                line.text += segments[j].text;
        }
        result.lines.push_back(std::move(line));
        max_line_width = std::max(max_line_width, current_width);
        y += line_height;
    }

    // pulp #1410 — clamp to max_lines (>1) by dropping trailing lines.
    // max_lines=1 already short-circuits at the top of the function.
    if (max_lines > 1 && static_cast<int>(result.lines.size()) > max_lines) {
        result.lines.resize(static_cast<std::size_t>(max_lines));
        max_line_width = 0;
        for (const auto& line : result.lines)
            max_line_width = std::max(max_line_width, line.width);
        y = max_lines * line_height;
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

#ifdef PULP_HAS_TEXT_SHAPING
    sk_sp<SkFontMgr> font_mgr;
    sk_sp<skia::textlayout::FontCollection> font_collection;

    // pulp #2163 / font v2 Slice 1.1.a — platform font manager comes from
    // the single canonical helper in bundled_fonts.cpp (exported via
    // bundled_fonts.hpp). Without a manager, SkFontMgr::RefEmpty()
    // returns no typefaces and `font.measureText()` reports near-zero
    // advance, collapsing Label::intrinsic_width() (pulp #945).

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
    struct CacheKey {
        std::string font_family;
        float font_size;
        bool operator==(const CacheKey& o) const {
            return font_family == o.font_family && font_size == o.font_size;
        }
    };
    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            return std::hash<std::string>{}(k.font_family) ^
                   (std::hash<float>{}(k.font_size) << 16);
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

    // pulp #2163 — metrics cache keyed by (font_family, font_size). Stores
    // SkFontMetrics-derived ascent/descent/leading once per typeface so
    // every measure_metrics call after the first is pure cache hit. Same
    // PreText "measure once, reuse forever" model as the segment cache.
    struct LineBox {
        float ascent;   // positive distance above baseline (Skia stores it negative)
        float descent;  // positive distance below baseline
        float leading;  // extra inter-line gap
        float line_height;  // ascent + descent + leading
        bool real;      // true if derived from SkFontMetrics, false for the heuristic fallback
    };
    std::unordered_map<CacheKey, LineBox, CacheKeyHash> metrics_cache;
    std::mutex metrics_mutex;

#ifdef PULP_HAS_TEXT_SHAPING
    // pulp #2163 / font v2 Slice 1.1.a (caller migration) — typeface
    // resolution now routes through FontResolver. Comma-list parsing
    // happens once inside the resolver; the registered → bundled →
    // platform cascade is shared with skia_canvas. Returns null when
    // no family matched and there's no platform fallback (non-Skia
    // build, RefEmpty mgr, etc.).
    sk_sp<SkTypeface> resolve_typeface(const std::string& font_family) {
        FontOptions opts;
        // Mirror skia_canvas.cpp's split_font_family_list so comma-
        // separated CSS family stacks are walked correctly. Strip
        // whitespace + matching outer quotes.
        size_t pos = 0;
        while (pos < font_family.size()) {
            size_t comma = font_family.find(',', pos);
            std::string seg = font_family.substr(
                pos, (comma == std::string::npos ? font_family.size() : comma) - pos);
            pos = (comma == std::string::npos) ? font_family.size() : comma + 1;
            // Strip outer whitespace.
            size_t a = seg.find_first_not_of(" \t");
            size_t b = seg.find_last_not_of(" \t");
            if (a == std::string::npos) continue;
            seg = seg.substr(a, b - a + 1);
            // Strip matching outer quotes.
            if (seg.size() >= 2
                && (seg.front() == '"' || seg.front() == '\'')
                && seg.back() == seg.front()) {
                seg = seg.substr(1, seg.size() - 2);
            }
            if (!seg.empty()) opts.family_stack.push_back(std::move(seg));
        }
        auto resolved = FontResolver::instance().resolve_family_list(opts);
        return resolved.typeface;
    }
#endif

    LineBox measure_metrics(const std::string& font_family, float font_size) {
        CacheKey key{font_family, font_size};
        {
            std::lock_guard<std::mutex> lock(metrics_mutex);
            auto it = metrics_cache.find(key);
            if (it != metrics_cache.end()) return it->second;
        }

        LineBox box{};
        box.line_height = font_size * 1.5f;  // fallback if no real metrics
        box.real = false;

#ifdef PULP_HAS_TEXT_SHAPING
        SkFont font;
        sk_sp<SkTypeface> tf = resolve_typeface(font_family);
        if (tf) font.setTypeface(std::move(tf));
        font.setSize(font_size);
        if (font.getTypeface()) {
            SkFontMetrics m;
            font.getMetrics(&m);
            // pulp #2163 — use fTop / fBottom (the WORST-CASE glyph
            // bbox extents) rather than fAscent / fDescent (the
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
            box.leading =  m.fLeading > 0 ? m.fLeading : 0;
            // pulp #2163 — Phase 1 exit (font v2 roadmap). Historical
            // context: commit 2371479c3 added an empirical
            // `0.5 * font_size` line-height margin on top of fTop/fBottom
            // to fix small-font clipping in Chainer (CROSSOVER /
            // MID / SIDE WIDTH titles at fontSize 7). The margin was a
            // stopgap — the real cause was the y-semantics drift
            // documented in v2 tar pit #9.
            //
            // After Slices 1.1.a (FontResolver) + 1.1.b (Yoga baseline
            // channel) + 1.2.b (TextAnchor + paint_at) + 1.3 (parity
            // harness asserting TextShaper ≡ SkFont::measureText within
            // 0.5 px), the structural cause is fixed and the margin is
            // unnecessary. Default behavior is now NO margin (raw
            // fTop/fBottom). Opt back in to the legacy margin with
            // `PULP_FONT_LEGACY_SAFETY_MARGIN=1` for bisection or A/B
            // regression triage; the opt-in goes away once the full
            // multilingual torture-corpus harness ships in a follow-up
            // and proves we've never needed it.
            const bool legacy_margin =
                std::getenv("PULP_FONT_LEGACY_SAFETY_MARGIN") != nullptr;
            const float safety = legacy_margin ? font_size * 0.5f : 0.0f;
            box.line_height = box.ascent + box.descent + box.leading + safety;
            box.real = true;
        }
#endif

        if (std::getenv("PULP_METRICS_TRACE")) {
            std::fprintf(stderr, "[metrics] family='%s' size=%.1f ascent=%.2f descent=%.2f leading=%.2f line_height=%.2f real=%d\n",
                         font_family.c_str(), font_size, box.ascent, box.descent,
                         box.leading, box.line_height, box.real ? 1 : 0);
        }
        {
            std::lock_guard<std::mutex> lock(metrics_mutex);
            metrics_cache[key] = box;
        }
        return box;
    }

    float measure_segment(const std::string& text, const std::string& font_family, float font_size) {
        std::uint64_t current_gen = font_registration_generation();

        // Check cache first
        CacheKey key{font_family, font_size};
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

#ifdef PULP_HAS_TEXT_SHAPING
        // Real measurement via SkFont. Use the platform font manager
        // (CoreText/DirectWrite/fontconfig/Android) — RefEmpty() returns
        // no typefaces and silently produces ~0 advance widths, which
        // is what regressed Label measurement in pulp #945.
        SkFont font;
        sk_sp<SkTypeface> typeface;

        // pulp #2163 — CSS font-family can be a comma-separated list
        // ("'IBM Plex Mono', monospace"). Walk the list and return the
        // first family that resolves to a real typeface. Before this
        // fix, the whole string was used as a single family lookup,
        // which always failed and fell through to the platform default
        // → label measurements no longer matched the painter's actual
        // typeface and Yoga reserved the wrong width.
        auto try_family = [&](const std::string& fam) -> sk_sp<SkTypeface> {
            // Strip outer quotes (CSS allows 'Family Name' / "Family Name").
            std::string clean = fam;
            // Trim whitespace
            size_t a = clean.find_first_not_of(" \t");
            size_t b = clean.find_last_not_of(" \t");
            if (a == std::string::npos) return nullptr;
            clean = clean.substr(a, b - a + 1);
            if (clean.size() >= 2
                && (clean.front() == '"' || clean.front() == '\'')
                && clean.back() == clean.front()) {
                clean = clean.substr(1, clean.size() - 2);
            }
            if (clean.empty()) return nullptr;

            // pulp #1150 — plugin-registered typefaces win over the platform
            // font manager so measurement matches paint (skia_canvas.cpp's
            // get_cached_typeface honours the same precedence).
            auto tf = match_registered_typeface(clean, SkFontStyle::Normal());
            if (tf) return tf;
            if (font_mgr && font_mgr->countFamilies() > 0) {
                tf = font_mgr->matchFamilyStyle(clean.c_str(),
                                                SkFontStyle::Normal());
                // Verify the matcher didn't silently substitute the
                // platform default. Compare names case-insensitively;
                // accept if the actual name contains the requested
                // family or vice versa.
                if (tf) {
                    SkString actual;
                    tf->getFamilyName(&actual);
                    std::string a_str(actual.c_str(), actual.size());
                    auto lower = [](std::string s) {
                        for (auto& c : s)
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        return s;
                    };
                    std::string al = lower(a_str), cl = lower(clean);
                    if (al == cl
                        || al.find(cl) != std::string::npos
                        || cl.find(al) != std::string::npos) {
                        return tf;
                    }
                    // The matcher returned a fallback that doesn't match
                    // the requested name — keep walking the comma list.
                }
            }
            return nullptr;
        };

        if (font_family.find(',') == std::string::npos) {
            typeface = try_family(font_family);
        } else {
            size_t pos = 0;
            while (pos < font_family.size()) {
                size_t comma = font_family.find(',', pos);
                std::string segment = font_family.substr(
                    pos, (comma == std::string::npos ? font_family.size() : comma) - pos);
                pos = (comma == std::string::npos) ? font_family.size() : comma + 1;
                typeface = try_family(segment);
                if (typeface) break;
            }
        }

        // Final fallback — platform default. Used only when every
        // family in the list missed (or no list was provided).
        if (!typeface && font_mgr && font_mgr->countFamilies() > 0) {
            typeface = font_mgr->matchFamilyStyle(nullptr, SkFontStyle::Normal());
        }
        if (typeface) font.setTypeface(std::move(typeface));
        font.setSize(font_size);
        font.setEdging(SkFont::Edging::kSubpixelAntiAlias);

        if (font.getTypeface()) {
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
                    pstyle.setTextStyle(tstyle);
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
        } else {
            // No platform font manager (or all matchers failed) — fall
            // back to the same character-width estimator the non-Skia
            // build uses, so callers still get a sane positive width.
            width = static_cast<float>(text.size()) * font_size * 0.6f;
        }
#else
        // Fallback: character-width estimation
        width = static_cast<float>(text.size()) * font_size * 0.6f;
#endif

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
            cache[key][text] = width;
        }

        return width;
    }
};

// ── TextShaper ──────────────────────────────────────────────────────────

TextShaper::TextShaper() : impl_(std::make_unique<Impl>()) {}
TextShaper::~TextShaper() = default;

PreparedText TextShaper::prepare(std::string_view text, std::string_view font_family,
                                  float font_size) {
    PreparedText result;
    result.font_family_ = std::string(font_family);
    result.font_size_ = font_size;
    // pulp #2163 — ask the impl for real SkFontMetrics-derived line
    // height. Falls back to font_size * 1.5 only when there's no
    // resolvable typeface (non-Skia build, empty font manager, etc.).
    // Cached per (family, size) so repeated layout calls hit pure
    // arithmetic — same PreText "measure once" guarantee that already
    // drives the segment width cache.
    auto box = impl_->measure_metrics(result.font_family_, font_size);
    result.line_height_ = box.line_height;
    result.ascent_       = box.ascent;
    result.descent_      = box.descent;
    result.leading_      = box.leading;
    result.metrics_real_ = box.real;

    // Segment the text: split on whitespace and newlines
    std::string current;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];

        if (c == '\n') {
            if (!current.empty()) {
                ShapedSegment seg;
                seg.text = current;
                seg.width = impl_->measure_segment(current, result.font_family_, font_size);
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
                seg.width = impl_->measure_segment(current, result.font_family_, font_size);
                result.segments_.push_back(std::move(seg));
                current.clear();
            }
            ShapedSegment ws;
            ws.text = std::string(1, c);
            ws.width = impl_->measure_segment(ws.text, result.font_family_, font_size);
            ws.is_whitespace = true;
            result.segments_.push_back(std::move(ws));
        } else {
            current += c;
        }
    }

    if (!current.empty()) {
        ShapedSegment seg;
        seg.text = current;
        seg.width = impl_->measure_segment(current, result.font_family_, font_size);
        result.segments_.push_back(std::move(seg));
    }

    return result;
}

PreparedText TextShaper::prepare(const AttributedString& text) {
    // For attributed strings, prepare each span separately
    PreparedText result;
    if (text.empty()) return result;

    auto& first_span = text.spans()[0];
    result.font_family_ = first_span.font_family;
    result.font_size_ = first_span.font_size;
    result.line_height_ = first_span.font_size * 1.5f;

    for (auto& span : text.spans()) {
        // Use the largest font's line height so mixed-size text doesn't overlap
        float span_lh = span.font_size * 1.5f;
        if (span_lh > result.line_height_)
            result.line_height_ = span_lh;

        auto span_prepared = prepare(span.text, span.font_family, span.font_size);
        result.segments_.insert(result.segments_.end(),
                               span_prepared.segments_.begin(),
                               span_prepared.segments_.end());
    }

    return result;
}

ShapedLayout TextShaper::layout(const PreparedText& prepared, float max_width,
                                 float line_height, int max_lines,
                                 BreakMode break_mode) const {
    float lh = line_height > 0 ? line_height : prepared.line_height();
    return layout_from_segments(prepared.segments(), max_width, lh, false, max_lines, break_mode);
}

ShapedLayout TextShaper::layout_with_lines(const PreparedText& prepared, float max_width,
                                            float line_height, int max_lines,
                                            BreakMode break_mode) const {
    float lh = line_height > 0 ? line_height : prepared.line_height();
    return layout_from_segments(prepared.segments(), max_width, lh, true, max_lines, break_mode);
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

TextShaper& global_text_shaper() {
    static TextShaper shaper;
    return shaper;
}

}  // namespace pulp::canvas
