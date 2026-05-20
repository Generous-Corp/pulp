// skia_canvas_text.cpp — Canvas2D text shaping + text-paint methods,
// extracted from skia_canvas.cpp in the 2026-05 Phase 4 (R2-3) refactor.
//
// The SkParagraph-based text surface: cluster-aware shaping with
// HarfBuzz + ICU + color-emoji fallback, and every SkiaCanvas text
// method that routes through it — set_text_align, fill_text,
// fill_text_anchored, fill_text_with_max_width, stroke_text,
// fill_text_sdf, measure_text, and the file-local shape helpers
// (shape_with_glyph_fallback, active_typeface_covers_text).
//
// Definitions only; declarations stay in skia_canvas.hpp. Relocated so
// text-shaping work no longer recompiles the whole 2.9k-line
// skia_canvas.cpp. The include block mirrors skia_canvas.cpp's so the
// SkParagraph / SkShaper / font headers are all in scope.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

#ifdef PULP_HAS_SKIA
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/utils/SkParsePath.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTextBlob.h"
#include "modules/skshaper/include/SkShaper.h"
#include "include/core/SkRRect.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"
#include "include/core/SkSurface.h"
#include "include/core/SkSamplingOptions.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkColorMatrix.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkBlendMode.h"
#include "include/effects/SkImageFilters.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"
#include "webgpu/webgpu_cpp.h"
#endif  // PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/emoji_segmenter.hpp>
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/canvas/font_options.hpp>
#include <pulp/canvas/text_shaper.hpp>
#ifdef PULP_HAS_SKIA
#include <pulp/canvas/text_font_context.hpp>
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#endif

#ifdef PULP_HAS_SKIA

#include "runtime_effect_cache.hpp"
#include "include/effects/SkImageFilters.h"
#include "include/core/SkColorFilter.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"
#include "webgpu/webgpu_cpp.h"

// Platform font manager
#ifdef __APPLE__
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#elif defined(__ANDROID__)
// Android: use the built-in Android font manager with FreeType scanner
#include "include/ports/SkFontMgr_android.h"
#include "include/ports/SkFontScanner_FreeType.h"
#elif defined(__linux__)
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#endif

#include "skia_canvas_internal.hpp"

namespace pulp::canvas {

// ── SkParagraph-based shape helper ─────────────────────────────────────
// Canvas2D text routes through `SkParagraph` for cluster-aware shaping
// + color-emoji fallback. SkParagraph uses HarfBuzz + ICU + the
// `TextFontContext`'s FontCollection (which has the registered emoji
// typeface in its default-family list) so ZWJ families, regional flag
// pairs, keycaps, and skin-tone modifiers all shape as single grapheme
// clusters routed to the emoji font.
namespace {

struct PreparedParagraph {
    std::unique_ptr<skia::textlayout::Paragraph> paragraph;
    float advance = 0;
    float alphabetic_baseline = 0;
};

PreparedParagraph make_paragraph(const std::string& text,
                                  const std::string& family,
                                  float size,
                                  int weight,
                                  int slant,
                                  float letter_spacing,
                                  bool ltr,
                                  std::optional<SkPaint> foreground_paint,
                                  const std::vector<Canvas::FontFeature>& features = {}) {
    PreparedParagraph result;
    if (text.empty()) return result;
    auto ctx = pulp::canvas::TextFontContext::shared();
    auto fc = ctx->font_collection();
    if (!fc) return result;

    skia::textlayout::ParagraphStyle pstyle;
    pstyle.setTextDirection(ltr ? skia::textlayout::TextDirection::kLtr
                                : skia::textlayout::TextDirection::kRtl);
    skia::textlayout::TextStyle tstyle;
    std::vector<SkString> families;
    // Codex P2 (PR #2157): split CSS font-family lists. `family` can be a
    // comma-separated CSS fallback list ("Inter, sans-serif"). Passing the
    // whole string as one SkString makes SkParagraph treat it as a single
    // literal family name and miss the fallback resolution. Split on commas
    // and strip surrounding whitespace + optional quote marks so each entry
    // resolves independently in the font collection.
    if (!family.empty()) {
        size_t cursor = 0;
        while (cursor < family.size()) {
            size_t comma = family.find(',', cursor);
            std::string entry = family.substr(cursor,
                comma == std::string::npos ? std::string::npos : comma - cursor);
            // Trim ASCII whitespace.
            size_t lo = entry.find_first_not_of(" \t\n\r\f\v");
            size_t hi = entry.find_last_not_of(" \t\n\r\f\v");
            if (lo != std::string::npos && hi != std::string::npos) {
                entry = entry.substr(lo, hi - lo + 1);
                // Strip a single matching pair of single or double quotes.
                if (entry.size() >= 2
                    && (entry.front() == '"' || entry.front() == '\'')
                    && entry.front() == entry.back()) {
                    entry = entry.substr(1, entry.size() - 2);
                }
                if (!entry.empty()) {
                    families.emplace_back(entry.c_str());
                }
            }
            if (comma == std::string::npos) break;
            cursor = comma + 1;
        }
    }
    const std::string emoji_family = ctx->emoji_family_name();
    if (!emoji_family.empty()) {
        families.emplace_back(emoji_family.c_str());
        families.emplace_back("Pulp Emoji");
    }
    if (families.empty()) families.emplace_back("");
    tstyle.setFontFamilies(families);
    tstyle.setFontSize(size > 0 ? size : 14.0f);
    SkFontStyle sk_style{weight,
                         SkFontStyle::kNormal_Width,
                         slant ? SkFontStyle::kItalic_Slant
                               : SkFontStyle::kUpright_Slant};
    tstyle.setFontStyle(sk_style);
    if (letter_spacing != 0.0f) tstyle.setLetterSpacing(letter_spacing);
    if (foreground_paint.has_value()) {
        tstyle.setForegroundPaint(*foreground_paint);
    }
    for (const auto& f : features) {
        // Unpack the FourByteTag back into a 4-character SkString:
        // tag = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3.
        char tag_chars[4] = {
            static_cast<char>((f.tag >> 24) & 0xFF),
            static_cast<char>((f.tag >> 16) & 0xFF),
            static_cast<char>((f.tag >> 8) & 0xFF),
            static_cast<char>(f.tag & 0xFF),
        };
        tstyle.addFontFeature(SkString(tag_chars, 4),
                              static_cast<int>(f.value));
    }
    pstyle.setTextStyle(tstyle);

    auto pb = skia::textlayout::ParagraphBuilder::make(pstyle, fc);
    if (!pb) return result;
    pb->addText(text.c_str(), text.size());
    auto paragraph = pb->Build();
    if (!paragraph) return result;
    paragraph->layout(SK_ScalarInfinity);
    // With an infinite layout width SkParagraph produces a single line;
    // `getLongestLine()` is therefore the paint advance. Intrinsic width
    // can under-report mixed emoji/default runs on newer Skia builds,
    // which breaks Canvas2D textAlign because paint still uses the full
    // line width.
    result.advance = paragraph->getLongestLine();
    if (result.advance <= 0.0f) {
        result.advance = paragraph->getMaxIntrinsicWidth();
    }
    result.alphabetic_baseline = paragraph->getAlphabeticBaseline();
    result.paragraph = std::move(paragraph);
    return result;
}

bool contains_variation_selector(std::string_view text) {
    // U+FE0E forces text presentation and U+FE0F forces emoji
    // presentation. Both affect shaping, so measurement must follow the
    // SkParagraph path that fill_text uses even when FE0E demotes an
    // emoji-default codepoint to the default font run.
    constexpr std::string_view kTextPresentation = "\xEF\xB8\x8E";
    constexpr std::string_view kEmojiPresentation = "\xEF\xB8\x8F";
    return text.find(kTextPresentation) != std::string_view::npos
        || text.find(kEmojiPresentation) != std::string_view::npos;
}

bool needs_paragraph_for_text_metrics(
    std::string_view text,
    const std::vector<Canvas::FontFeature>& features,
    float letter_spacing) {
    return !features.empty()
        || letter_spacing != 0.0f
        || pulp::canvas::contains_emoji(text)
        || contains_variation_selector(text);
}

} // namespace


// pulp #2163 — minimal UTF-8 decoder. Skia's SkUTF helper isn't in our
// public include set, but the format is small and well-defined; inline
// here so we can check codepoint coverage without pulling skia/private.
// Returns U+FFFD on malformed input and always advances at least one
// byte so the caller cannot infinite-loop.
static SkUnichar next_utf8(const char* s, const char* end, int* advance) {
    if (s >= end) { *advance = 0; return 0; }
    unsigned char c = static_cast<unsigned char>(*s);
    if (c < 0x80) { *advance = 1; return c; }
    int extra;
    SkUnichar uc;
    if      ((c & 0xE0) == 0xC0) { extra = 1; uc = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; uc = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; uc = c & 0x07; }
    else                          { *advance = 1; return 0xFFFD; }
    if (s + 1 + extra > end)      { *advance = 1; return 0xFFFD; }
    for (int i = 0; i < extra; ++i) {
        unsigned char cc = static_cast<unsigned char>(s[1 + i]);
        if ((cc & 0xC0) != 0x80)  { *advance = 1; return 0xFFFD; }
        uc = (uc << 6) | (cc & 0x3F);
    }
    *advance = 1 + extra;
    return uc;
}

// pulp #2163 — per-glyph font fallback for fill_text. Walks the codepoints
// using `active` as the preferred typeface; for any codepoint missing
// from `active`, asks SkFontMgr::matchFamilyStyleCharacter for a
// fallback typeface that contains the codepoint. Builds contiguous
// runs sharing one typeface, shapes each run with SkShaper, then
// concatenates the resulting blobs.
//
// Motivated by JSX imports (e.g. Chainer) that request fonts the host
// machine doesn't have installed. The fallback typeface that
// matchFamilyStyle resolves can lack common Unicode characters like
// → (U+2192), ↑ (U+2191), em-dashes, etc. — single-typeface SkShaper
// renders those as the typeface's .notdef "tofu" box, which is
// visually broken. Per-glyph fallback fixes this without requiring
// every plugin author to ship a Unicode-complete bundled font.
//
// The fast path (ASCII-only text OR the active typeface covers every
// codepoint) skips this entirely — the caller checks before routing
// here.
static void shape_with_glyph_fallback(SkCanvas* canvas,
                                      const std::string& text,
                                      float x, float y,
                                      const SkFont& base_font,
                                      const SkPaint& paint,
                                      const std::string& font_family,
                                      int font_weight,
                                      int font_slant,
                                      bool ltr,
                                      TextAlign text_align,
                                      sk_sp<SkFontMgr> font_mgr) {
    auto* base_tf = base_font.getTypeface();
    if (!base_tf) return;

    SkFontStyle style{font_weight, SkFontStyle::kNormal_Width,
                      font_slant ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant};

    struct Run { std::string text; sk_sp<SkTypeface> tf; };
    std::vector<Run> runs;
    Run current;
    current.tf = sk_ref_sp(base_tf);

    // Cache fallback typefaces per missing codepoint within this call.
    // Same codepoint missing twice → resolve once.
    std::unordered_map<SkUnichar, sk_sp<SkTypeface>> fallback_cache;

    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        int adv = 0;
        SkUnichar cp = next_utf8(p, end, &adv);
        if (adv == 0) break;
        const char* cp_bytes = p;
        p += adv;

        // Pick a typeface for this codepoint.
        sk_sp<SkTypeface> chosen;
        // Control characters (newlines etc.) always go through the base
        // typeface — no point routing them to fallback.
        if (cp >= 0x20 && base_tf->unicharToGlyph(cp) == 0) {
            auto it = fallback_cache.find(cp);
            if (it != fallback_cache.end()) {
                chosen = it->second;
            } else if (font_mgr) {
                chosen = font_mgr->matchFamilyStyleCharacter(
                    font_family.empty() ? nullptr : font_family.c_str(),
                    style, nullptr, 0, cp);
                if (chosen && chosen->unicharToGlyph(cp) == 0) chosen.reset();
                fallback_cache[cp] = chosen;
            }
        }
        if (!chosen) chosen = sk_ref_sp(base_tf);

        if (chosen.get() != current.tf.get()) {
            if (!current.text.empty()) runs.push_back(std::move(current));
            current = Run{};
            current.tf = chosen;
        }
        current.text.append(cp_bytes, adv);
    }
    if (!current.text.empty()) runs.push_back(std::move(current));

    if (runs.empty()) return;

    // Shape each run with its own typeface.
    struct ShapedRun { sk_sp<SkTextBlob> blob; float width; };
    std::vector<ShapedRun> shaped;
    shaped.reserve(runs.size());
    float total_w = 0;

    auto shaper = SkShaper::Make();
    if (!shaper) return;

    for (const auto& r : runs) {
        SkFont rf = base_font;
        rf.setTypeface(r.tf);

        SkTextBlobBuilderRunHandler handler(r.text.c_str(), {0, 0});
        shaper->shape(r.text.c_str(), r.text.size(), rf, ltr,
                      SK_ScalarInfinity, &handler);
        const float w = handler.endPoint().x();
        shaped.push_back({handler.makeBlob(), w});
        total_w += w;
    }

    float draw_x = x;
    if (text_align == TextAlign::center) draw_x -= total_w * 0.5f;
    else if (text_align == TextAlign::right) draw_x -= total_w;

    for (const auto& sr : shaped) {
        if (sr.blob) canvas->drawTextBlob(sr.blob, draw_x, y, paint);
        draw_x += sr.width;
    }
}

// Quick pre-flight: does `tf` contain a glyph for every non-ASCII
// codepoint in `text`? ASCII bytes (< 0x80) are assumed covered by any
// sane Latin typeface and skipped — this keeps the hot path allocation-
// free. Returns true if the single-typeface SkShaper path is safe;
// false when at least one codepoint would render as .notdef.
static bool active_typeface_covers_text(SkTypeface* tf, const std::string& text) {
    if (!tf) return true;  // no font → caller will early-return anyway
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        unsigned char b = static_cast<unsigned char>(*p);
        if (b < 0x80) { ++p; continue; }
        int adv = 0;
        SkUnichar cp = next_utf8(p, end, &adv);
        if (adv == 0) break;
        if (cp >= 0x20 && tf->unicharToGlyph(cp) == 0) return false;
        p += adv;
    }
    return true;
}

void SkiaCanvas::set_text_align(TextAlign align) {
    text_align_ = align;
}

void SkiaCanvas::fill_text_anchored(const std::string& text,
                                    float x, float y, TextAnchor anchor) {
    GUARD_CANVAS;
    if (text.empty()) return;
    // pulp #2163 / font v2 Slice 1.2.b — translate the anchor's y
    // reference into a baseline-y, then delegate to fill_text. The
    // worst-case-glyph metrics (SkFontMetrics::fTop / fBottom flipped
    // positive) come from TextShaper, which pulls them from the same
    // resolved typeface the painter will use — guarantees that the
    // anchor-y → baseline-y math matches what the painter actually
    // does, slice-1.3 parity harness asserts pixel-equal output.
    if (anchor == TextAnchor::Baseline) {
        fill_text(text, x, y);
        return;
    }

    auto& shaper = global_text_shaper();
    auto prepared = shaper.prepare(text, font_family_, font_size_);
    float ascent  = prepared.ascent();    // distance above baseline
    float descent = prepared.descent();   // distance below baseline
    if (ascent <= 0.0f)  ascent  = font_size_ * 0.85f;  // fallback
    if (descent <= 0.0f) descent = font_size_ * 0.2f;   // fallback

    float baseline_y = y;
    switch (anchor) {
        case TextAnchor::Baseline:
            // Already handled.
            break;
        case TextAnchor::GlyphTop:
            // y is glyph-top. Baseline sits `ascent` below.
            baseline_y = y + ascent;
            break;
        case TextAnchor::GlyphCenter:
            // y is glyph vertical center. Baseline sits half the glyph
            // box height below the center, offset by the asymmetry
            // between ascent and descent.
            baseline_y = y + (ascent - descent) * 0.5f;
            break;
        case TextAnchor::EmBoxTop:
            // y is em-box top. Baseline is at `font_size * baseline_ratio`
            // — for most Latin fonts ascent/font_size ≈ 0.8, so use that
            // as the em-box-to-baseline distance directly.
            baseline_y = y + font_size_ * 0.8f;
            break;
    }
    fill_text(text, x, baseline_y);
}

void SkiaCanvas::fill_text(const std::string& text, float x, float y) {
    GUARD_CANVAS;
    if (text.empty()) return;
    // pulp #2163 — `PULP_FILL_TEXT_TRACE=1` env var prints the (text, x, y)
    // arguments reaching the Skia path for the labels named below.
    // Useful for triage during the font-hardening rollout — pair with
    // PULP_LABEL_DEBUG_BOX to compare Label::paint's computed baseline
    // against the y argument fill_text actually receives.
    if (std::getenv("PULP_FILL_TEXT_TRACE")) {
        if (text.find("CROSSOVER") != std::string::npos
         || text.find("MID / SIDE") != std::string::npos
         || text.find("MULTIBAND") != std::string::npos
         || text == "LO" || text == "HI") {
            std::fprintf(stderr, "[fill_text] text='%s' x=%g y=%g family='%s' size=%g letter_sp=%g\n",
                         text.c_str(), x, y, font_family_.c_str(), font_size_, letter_spacing_);
        }
    }

    // pulp #1899 (gap #3) — when any currently-open save_layer carries
    // alpha < 1, Skia's LCD subpixel AA degrades on the non-opaque
    // destination and glyphs render faint. make_font() falls back to
    // greyscale AA in that case (browser parity).
    SkFont font = make_font(font_family_, font_size_, font_weight_, font_slant_,
                            inside_non_opaque_layer());
    if (!font.getTypeface()) return;

    // pulp Wave 3 c2d.6 — gradient (and pattern) fillStyle on text. Route
    // through current_fill_paint() so any active gradient_shader_ flows
    // onto the glyph paint. Without this, ctx.fillStyle =
    // createLinearGradient(...); ctx.fillText('Hi', x, y) silently
    // degraded to the first stop colour. The shader's geometry maps in
    // device space, so the gradient stretches across the rendered glyphs
    // exactly like Blink / WebKit. current_fill_paint() also folds in
    // the sticky Canvas2D shadow* state and the CSS filter chain (issue-
    // 1434 batch 7 / pulp #1520) so text honors `ctx.shadowBlur` and
    // `ctx.filter` in the same call.
    auto paint = current_fill_paint();

#ifdef PULP_HAS_TEXT_SHAPING
    // pulp #2163 — per-glyph font fallback. If the resolved typeface
    // lacks a glyph for any codepoint in `text` (common when JSX
    // imports request a host-uninstalled font like "IBM Plex Mono"
    // and Pulp falls back to the system default which lacks Unicode
    // arrows/dashes/etc.), partition the text into runs by typeface
    // and shape each separately. ASCII text bypasses the scan; it's
    // virtually always covered by any Latin typeface.
    //
    const bool needs_paragraph =
        needs_paragraph_for_text_metrics(text, font_features_, letter_spacing_);

    // pulp #2163 — for plain letter_spacing_ == 0 text only, route
    // missing-glyph text through SkShaper-based per-run fallback for
    // best kerning / ligature quality. Emoji, variation selectors,
    // font features, and tracked text must use SkParagraph instead:
    // that path preserves cluster shaping and reports the advance used
    // for Canvas2D textAlign.
    if (letter_spacing_ == 0.0f
        && !needs_paragraph
        && !active_typeface_covers_text(font.getTypeface(), text)) {
        const bool ltr = (direction_ != TextDirection::rtl);
        shape_with_glyph_fallback(canvas_, text, x, y, font, paint,
                                  font_family_, font_weight_, font_slant_,
                                  ltr, text_align_, get_font_manager());
        return;
    }

    // pulp #2163 — if letter_spacing_ != 0 OR letter_spacing_ == 0
    // path-1 (SkShaper) gave up (above), but the active typeface
    // covers the text, fall through to the unified per-glyph builder.
    // For letter-spaced text with missing glyphs, the per-glyph builder
    // handles fallback inline so we never reach this point with .notdef
    // boxes — the bullet ● in '● READY' now renders via system fallback
    // even when letter_spacing_ > 0.
    //
    // For letter_spacing_ == 0 + fully covered text the path-1 SkShaper
    // already returned above, so we won't double-render.

    // SkShaper path: full OpenType kerning + ligatures via HarfBuzz.
    //
    // The RunHandler origin MUST be {0,0}. The draw position is passed
    // exclusively to drawTextBlob() to avoid double-offset in nested
    // save/translate/clip contexts (issue #75). The bug: if the handler
    // is seeded with {x,y} AND drawTextBlob also receives {x,y}, glyph
    // positions are offset twice — once inside the blob and once by the
    // draw call — producing a ghost/double image that worsens with each
    // nesting level in the widget paint pipeline.
    //
    // SkParagraph handles cluster-aware emoji fallback, CSS letter-
    // spacing, OpenType font features, and bidi direction.
    //
    // Codex P1 (PR #2157): previously this branch was gated on
    // `needs_paragraph` (features/letter-spacing/rtl/emoji) and routed
    // plain LTR ASCII text into the per-glyph SkTextBlob fallback as a
    // hot-path optimization. That fallback has NO kerning or ligatures,
    // so common strings like "AV" / "ffi" / "fl" rendered with the
    // wrong advances and bare-glyph spacing — a visual + width-sensitive
    // regression vs. the pre-emoji code, which always shaped through
    // SkParagraph. The per-glyph blob path is now only reached when
    // shaping is compile-time disabled (`PULP_HAS_TEXT_SHAPING` off) or
    // SkParagraph fails to build a paragraph at runtime.
    {
        const bool ltr = (direction_ != TextDirection::rtl);
        auto prepared = make_paragraph(text, font_family_, font_size_,
                                        font_weight_, font_slant_,
                                        letter_spacing_, ltr, paint,
                                        font_features_);
        if (prepared.paragraph) {
            float draw_x = x;
            if (text_align_ == TextAlign::center) draw_x -= prepared.advance * 0.5f;
            else if (text_align_ == TextAlign::right) draw_x -= prepared.advance;
            // SkParagraph paint(canvas, x, y) places the paragraph TOP
            // at (x, y); Canvas2D fillText puts the alphabetic baseline
            // at y. Translate so the baseline lands on `y`.
            prepared.paragraph->paint(canvas_, draw_x,
                                       y - prepared.alphabetic_baseline);
            return;
        }
    }
#endif

    // pulp #2163 — unified per-glyph fallback path with per-codepoint
    // font fallback AND letter-spacing support. Reached when
    // PULP_HAS_TEXT_SHAPING is compile-time disabled OR (post-PR #2157)
    // when make_paragraph fails to build a paragraph. One code path so
    // every fill_text call ends up at the same baseline placement,
    // regardless of whether the text has missing glyphs or letter-spacing.
    // Replaces the prior split where letter-spaced text rendered missing
    // glyphs as .notdef boxes while non-letter-spaced text routed through
    // shape_with_glyph_fallback — the divergent baselines between paths
    // showed up as visibly stacked labels (pulp #2163 #31 iMX8/READY
    // regression).
    //
    // Algorithm:
    //  1. Walk UTF-8 codepoints.
    //  2. For each cp, prefer the active typeface; if it lacks the
    //     glyph, ask SkFontMgr::matchFamilyStyleCharacter for a
    //     fallback. Cache fallbacks per cp inside this call.
    //  3. Group consecutive codepoints with the same typeface into
    //     runs (SkTextBlobBuilder needs one allocRunPosH per font).
    //  4. Measure advances per glyph via SkFont::getWidths on the
    //     run's font.
    //  5. Lay out glyph positions with cumulative cursor + per-pair
    //     letter_spacing.
    //  6. Apply text_align after total width is known.
    //
    // SkShaper's kerning + ligature quality is lost on this path,
    // but it's only entered for letter-spaced or non-fully-covered
    // text — both cases that already preclude useful kerning. The
    // best-quality SkParagraph path (#2157) is still chosen for the
    // common case at the top of fill_text above.
    auto font_mgr_for_fallback = get_font_manager();
    SkFontStyle style{font_weight_, SkFontStyle::kNormal_Width,
                      font_slant_ ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant};
    std::unordered_map<SkUnichar, sk_sp<SkTypeface>> fallback_cache;
    auto* active_tf = font.getTypeface();

    struct GlyphEntry {
        sk_sp<SkTypeface> tf;  // typeface for this glyph
        SkGlyphID glyph;       // resolved glyph id (always non-zero when tf has the glyph)
        SkScalar width;        // advance for this glyph
    };
    std::vector<GlyphEntry> entries;
    entries.reserve(text.size());

    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        int adv = 0;
        SkUnichar cp = next_utf8(p, end, &adv);
        if (adv == 0) break;
        p += adv;

        // Pick typeface for this codepoint: active → fallback.
        sk_sp<SkTypeface> chosen;
        SkGlyphID gid = 0;
        if (active_tf) {
            gid = active_tf->unicharToGlyph(cp);
            if (gid != 0) chosen = sk_ref_sp(active_tf);
        }
        if (!chosen && cp >= 0x20 && font_mgr_for_fallback) {
            auto it = fallback_cache.find(cp);
            if (it != fallback_cache.end()) chosen = it->second;
            else {
                chosen = font_mgr_for_fallback->matchFamilyStyleCharacter(
                    font_family_.empty() ? nullptr : font_family_.c_str(),
                    style, nullptr, 0, cp);
                if (chosen) {
                    gid = chosen->unicharToGlyph(cp);
                    if (gid == 0) chosen.reset();  // matcher lied; treat as no fallback
                }
                fallback_cache[cp] = chosen;
            }
            if (chosen && gid == 0) gid = chosen->unicharToGlyph(cp);
        }
        // Final fallback: emit .notdef in active typeface (preserves
        // current behavior when fallback path can't reach a font with
        // the glyph — at least the text doesn't disappear entirely).
        if (!chosen) {
            chosen = sk_ref_sp(active_tf);
            gid = 0;  // .notdef
        }
        if (!chosen) continue;  // shouldn't happen — active_tf was checked above

        // Measure advance for this single glyph in its typeface's font.
        SkFont glyph_font = font;
        glyph_font.setTypeface(chosen);
        SkScalar w = 0;
        glyph_font.getWidths(SkSpan<const SkGlyphID>(&gid, 1),
                             SkSpan<SkScalar>(&w, 1));
        entries.push_back({std::move(chosen), gid, w});
    }

    if (entries.empty()) return;

    // Total advance: sum of glyph widths + (N-1) * letter_spacing.
    float total_w = 0;
    for (const auto& e : entries) total_w += e.width;
    if (entries.size() > 1) {
        total_w += letter_spacing_ * static_cast<float>(entries.size() - 1);
    }

    float draw_x = x;
    if (text_align_ == TextAlign::center) draw_x -= total_w * 0.5f;
    else if (text_align_ == TextAlign::right) draw_x -= total_w;

    // Build runs grouped by typeface identity. SkTextBlobBuilder requires
    // one allocRunPosH per (font, count) pair, so we walk entries and
    // open a new run whenever the typeface changes.
    SkTextBlobBuilder builder;
    float cursor = draw_x;
    size_t i = 0;
    while (i < entries.size()) {
        SkTypeface* run_tf = entries[i].tf.get();
        size_t j = i + 1;
        while (j < entries.size() && entries[j].tf.get() == run_tf) ++j;
        const int run_n = static_cast<int>(j - i);

        SkFont run_font = font;
        run_font.setTypeface(entries[i].tf);
        const auto& run = builder.allocRunPosH(run_font, run_n, y);
        for (int k = 0; k < run_n; ++k) {
            run.glyphs[k] = entries[i + k].glyph;
            run.pos[k] = cursor;
            cursor += entries[i + k].width;
            // Letter-spacing applies between every pair of glyphs in
            // the source text, including across run boundaries — same
            // rule browsers use when CSS letter-spacing crosses an
            // <em> / fallback-font boundary.
            if (i + k + 1 < entries.size()) cursor += letter_spacing_;
        }
        i = j;
    }

    canvas_->drawTextBlob(builder.make(), 0, 0, paint);
}

void SkiaCanvas::fill_text_with_max_width(const std::string& text,
                                           float x, float y, float max_width) {
    // pulp #1525 — Canvas2D `fillText(text, x, y, maxWidth)`. When the
    // measured advance exceeds `max_width` the spec requires the user
    // agent to either pick a narrower font OR scale the text horizontally
    // so the resulting run is exactly `max_width` px wide. We take the
    // scaling path: it preserves the active typeface (no fallback font
    // surprises), preserves vertical metrics, and matches HarfBuzz's
    // per-cluster shape / draw model — each glyph cluster shrinks as a
    // rigid unit, keeping cluster boundaries spec-compliant.
    //
    // Sentinel: `max_width <= 0` means "no constraint" — fall through
    // to the unconstrained `fill_text` path bit-for-bit.
    if (max_width <= 0.0f || text.empty()) {
        fill_text(text, x, y);
        return;
    }
    GUARD_CANVAS;
    const float measured = measure_text(text);
    if (measured <= max_width || measured <= 0.0f) {
        // Already fits (or zero-width edge case) — no scale needed.
        fill_text(text, x, y);
        return;
    }
    const float scale = max_width / measured;
    // Scale around the text origin (x, y). Save/restore so the caller's
    // device matrix is unaffected — the spec says only the rendering of
    // this single fillText is squeezed; subsequent draws revert to
    // natural metrics.
    canvas_->save();
    canvas_->translate(x, y);
    canvas_->scale(scale, 1.0f);
    canvas_->translate(-x, -y);
    fill_text(text, x, y);
    canvas_->restore();
}

void SkiaCanvas::stroke_text(const std::string& text, float x, float y,
                              float max_width) {
    // pulp #1525 — true stroked-glyph rendering. Build a paint with
    // SkPaint::kStroke_Style so each glyph outline is honoured at the
    // active line width / stroke colour, rather than the pre-#1525
    // approximation that re-routed through fillText with strokeStyle as
    // the fill colour. HarfBuzz / SkShaper still handles cluster shaping
    // — we only swap the paint's style flag.
    GUARD_CANVAS;
    if (text.empty()) return;
    // pulp #1899 (gap #3) — see fill_text comment. Mirror the edging
    // policy so stroked glyphs inside an opacity layer track the
    // greyscale-AA path that fill_text uses.
    SkFont font = make_font(font_family_, font_size_, font_weight_, font_slant_,
                            inside_non_opaque_layer());
    if (!font.getTypeface()) return;

    auto stroke_paint = make_stroke_paint(stroke_color_, line_width_);
    stroke_paint.setAntiAlias(true);
    // Codex P2 (PR #1555): propagate sticky stroke state — lineJoin,
    // lineCap, miterLimit, and any stroke pattern shader — onto the
    // text-stroke paint. Without this, ctx.lineJoin / ctx.lineCap /
    // ctx.miterLimit / ctx.strokeStyle=createPattern(...) are silently
    // dropped on strokeText, even though every other stroke primitive
    // (stroke_rect, stroke_path, stroke_circle, …) honours them.
    apply_stroke_state(stroke_paint);
    apply_shadow_filter(stroke_paint);

    // pulp #1525 — apply maxWidth squeeze around (x, y) before drawing.
    bool needs_restore = false;
    if (max_width > 0.0f) {
        const float measured = measure_text(text);
        if (measured > max_width && measured > 0.0f) {
            const float scale = max_width / measured;
            canvas_->save();
            canvas_->translate(x, y);
            canvas_->scale(scale, 1.0f);
            canvas_->translate(-x, -y);
            needs_restore = true;
        }
    }

#ifdef PULP_HAS_TEXT_SHAPING
    // Same SkParagraph path as fill_text; ASCII / no-features / no-
    // tracking strokes skip it and use the per-glyph blob below. Color
    // emoji typefaces typically have CBDT/COLR bitmaps with no outline
    // tables, so strokeText effectively leaves them unchanged (CSS
    // behavior). Latin text in mixed-emoji runs still gets the stroke.
    const bool needs_paragraph =
        needs_paragraph_for_text_metrics(text, font_features_, letter_spacing_);
    if (needs_paragraph) {
        auto prepared = make_paragraph(text, font_family_, font_size_,
                                        font_weight_, font_slant_,
                                        letter_spacing_, /*ltr=*/true,
                                        stroke_paint, font_features_);
        if (prepared.paragraph) {
            float draw_x = x;
            if (text_align_ == TextAlign::center) draw_x -= prepared.advance * 0.5f;
            else if (text_align_ == TextAlign::right) draw_x -= prepared.advance;
            prepared.paragraph->paint(canvas_, draw_x,
                                       y - prepared.alphabetic_baseline);
            if (needs_restore) canvas_->restore();
            return;
        }
    }
#endif

    // Fallback: per-glyph blob, mirrors fill_text's non-shaper path so
    // the stroke pass tracks the fill pass exactly.
    int glyph_count = static_cast<int>(font.countText(text.c_str(), text.size(), SkTextEncoding::kUTF8));
    if (glyph_count <= 0) {
        if (needs_restore) canvas_->restore();
        return;
    }

    std::vector<SkGlyphID> glyphs(glyph_count);
    font.textToGlyphs(text.c_str(), text.size(), SkTextEncoding::kUTF8,
                      SkSpan<SkGlyphID>(glyphs.data(), glyph_count));

    std::vector<SkScalar> widths(glyph_count);
    font.getWidths(SkSpan<const SkGlyphID>(glyphs.data(), glyph_count),
                   SkSpan<SkScalar>(widths.data(), glyph_count));

    float total_w = 0;
    for (int i = 0; i < glyph_count; ++i) total_w += widths[i];
    if (glyph_count > 1) total_w += letter_spacing_ * static_cast<float>(glyph_count - 1);

    float draw_x = x;
    if (text_align_ == TextAlign::center) draw_x -= total_w * 0.5f;
    else if (text_align_ == TextAlign::right) draw_x -= total_w;

    SkTextBlobBuilder builder;
    const auto& run = builder.allocRunPosH(font, glyph_count, y);
    float cursor = draw_x;
    for (int i = 0; i < glyph_count; ++i) {
        run.glyphs[i] = glyphs[i];
        run.pos[i] = cursor;
        cursor += widths[i];
        if (i + 1 < glyph_count) cursor += letter_spacing_;
    }

    canvas_->drawTextBlob(builder.make(), 0, 0, stroke_paint);
    if (needs_restore) canvas_->restore();
}

void SkiaCanvas::fill_text_sdf(const std::string& text, float x, float y,
                               const SdfAtlas& atlas) {
    GUARD_CANVAS;
    if (text.empty()) return;

    // Scale factor: how the current font_size_ relates to the atlas's base_size.
    float scale = font_size_ / static_cast<float>(atlas.base_size());
    int atlas_w = atlas.width();
    int atlas_h = atlas.height();
    if (atlas_w <= 0 || atlas_h <= 0) { fill_text(text, x, y); return; }

    // Create atlas texture as an SkImage (uploaded to GPU on first use).
    // The atlas is single-channel (R8), so we use kAlpha_8_SkColorType
    // which maps the byte to the alpha channel — the shader reads it.
    auto image_data = SkData::MakeWithoutCopy(atlas.pixels(), atlas_w * atlas_h);
    SkImageInfo info = SkImageInfo::Make(atlas_w, atlas_h,
                                         kAlpha_8_SkColorType, kPremul_SkAlphaType);
    auto atlas_image = SkImages::RasterFromData(info, image_data, atlas_w);
    if (!atlas_image) { fill_text(text, x, y); return; }

    // Compute glyph positions and total advance for alignment.
    float total_advance = 0;
    struct GlyphDraw { const SdfGlyph* g; float x_offset; };
    std::vector<GlyphDraw> draws;

    // UTF-8 → codepoint iteration with bounds checking.
    // Falls back to fill_text() if any glyph is missing from the atlas
    // (partial atlas should not produce invisible characters).
    bool has_missing_glyph = false;
    size_t i = 0;
    while (i < text.size()) {
        char32_t cp;
        uint8_t c = static_cast<uint8_t>(text[i]);
        if (c < 0x80) {
            cp = c; i += 1;
        } else if (c < 0xE0) {
            if (i + 1 >= text.size()) break;  // truncated
            cp = (c & 0x1F) << 6 | (text[i+1] & 0x3F); i += 2;
        } else if (c < 0xF0) {
            if (i + 2 >= text.size()) break;
            cp = (c & 0x0F) << 12 | (text[i+1] & 0x3F) << 6 | (text[i+2] & 0x3F); i += 3;
        } else {
            if (i + 3 >= text.size()) break;
            cp = (c & 0x07) << 18 | (text[i+1] & 0x3F) << 12 | (text[i+2] & 0x3F) << 6 | (text[i+3] & 0x3F); i += 4;
        }

        const SdfGlyph* g = atlas.glyph(cp);
        if (!g) { has_missing_glyph = true; break; }
        draws.push_back({g, total_advance});
        total_advance += g->advance * scale;
    }

    // If any glyph is missing, fall back to standard text rendering
    // rather than producing incomplete/misaligned output.
    if (has_missing_glyph || draws.empty()) { fill_text(text, x, y); return; }

    // Apply text alignment.
    float draw_x = x;
    if (text_align_ == TextAlign::center) draw_x -= total_advance * 0.5f;
    else if (text_align_ == TextAlign::right) draw_x -= total_advance;

    // Draw each glyph as a textured quad with the SDF alpha channel.
    // The smoothstep is applied per-pixel by Skia's shader pipeline
    // when we use kAlpha_8 — we just draw with the fill color's paint.
    //
    // pulp Wave 3 c2d.6 — gradient/pattern fillStyle on SDF-drawn text.
    // Mirror the fill_text() update: route through current_fill_paint()
    // so an active gradient_shader_ tints the glyph quad consistently
    // with the shape-fill paths. The SDF channel is sampled out of the
    // alpha-only atlas image; the paint shader supplies the colour, so
    // gradients composite identically to the Skia-shaped text path.
    auto paint = current_fill_paint();
    for (auto& [g, x_off] : draws) {
        float gx = draw_x + x_off + g->bearing_x * scale;
        float gy = y - g->bearing_y * scale;
        float gw = g->width * scale;
        float gh = g->height * scale;

        SkRect src = SkRect::MakeXYWH(g->atlas_x, g->atlas_y, g->width, g->height);
        SkRect dst = SkRect::MakeXYWH(gx, gy, gw, gh);

        canvas_->drawImageRect(atlas_image, src, dst,
                               SkSamplingOptions(SkFilterMode::kLinear),
                               &paint, SkCanvas::kStrict_SrcRectConstraint);
    }
}

float SkiaCanvas::measure_text(const std::string& text) {
    SkFont font = make_font(font_family_, font_size_, font_weight_, font_slant_);
    if (!font.getTypeface()) return font_size_ * text.size() * 0.5f;

#ifdef PULP_HAS_TEXT_SHAPING
    // SkParagraph is the only working shape path in our Skia prebuilt
    // (legacy SkShaper APIs return zero-width). Hot-path labels (plain
    // ASCII, no features, no tracking) skip it to avoid the per-call
    // `ParagraphBuilder + Build + layout` cost — they're consistent
    // with the per-glyph fallback below that fill_text also takes on
    // the same input. Anything emoji-bearing or feature-tagged still
    // goes through SkParagraph so the measured width matches what
    // fill_text actually draws.
    const bool needs_paragraph =
        needs_paragraph_for_text_metrics(text, font_features_, letter_spacing_);
    if (needs_paragraph) {
        auto prepared = make_paragraph(text, font_family_, font_size_,
                                        font_weight_, font_slant_,
                                        letter_spacing_, /*ltr=*/true,
                                        std::nullopt, font_features_);
        if (prepared.paragraph) {
            return prepared.advance;
        }
    }
#endif

    // Fallback: per-glyph advances (no kerning/ligatures)
    int glyph_count = static_cast<int>(font.countText(text.c_str(), text.size(), SkTextEncoding::kUTF8));
    if (glyph_count <= 0) return 0;

    std::vector<SkGlyphID> glyphs(glyph_count);
    font.textToGlyphs(text.c_str(), text.size(), SkTextEncoding::kUTF8,
                      SkSpan<SkGlyphID>(glyphs.data(), glyph_count));

    std::vector<SkScalar> widths(glyph_count);
    font.getWidths(SkSpan<const SkGlyphID>(glyphs.data(), glyph_count),
                   SkSpan<SkScalar>(widths.data(), glyph_count));

    float total = 0;
    for (int i = 0; i < glyph_count; ++i) total += widths[i];
    // pulp #927: include CSS letter-spacing in measurement so layout code
    // (e.g. ellipsis truncation in Label::paint) reasons over the same
    // total advance the renderer will draw. Legacy fallback path —
    // glyph-pair tracking, not cluster-aware. Hit only when the
    // shaper / text-shaping module is unavailable.
    if (glyph_count > 1) total += letter_spacing_ * static_cast<float>(glyph_count - 1);
    return total;
}

Canvas::TextMetrics SkiaCanvas::measure_text_full(const std::string& text) {
    SkFont font = make_font(font_family_, font_size_, font_weight_, font_slant_);
    if (!font.getTypeface()) {
        // Fallback estimates — keep all fields populated so JS callers can
        // still e.g. center text against actual_bounding_box_left/right.
        TextMetrics fallback;
        fallback.width = font_size_ * text.size() * 0.5f;
        fallback.ascent = font_size_;
        fallback.descent = font_size_ * 0.25f;
        fallback.line_height = font_size_ * 1.2f;
        fallback.actual_bounding_box_ascent = fallback.ascent;
        fallback.actual_bounding_box_descent = fallback.descent;
        fallback.actual_bounding_box_left = 0;
        fallback.actual_bounding_box_right = fallback.width;
        return fallback;
    }

    SkFontMetrics sk_metrics;
    font.getMetrics(&sk_metrics);

    SkRect bounds;
    // SkFont::measureText returns the advance width and stores the
    // tight rendering bbox in `bounds`. The advance is what
    // CanvasRenderingContext2D.measureText.width returns; the bbox
    // gives us the actualBoundingBoxLeft/Right/Ascent/Descent fields
    // (issue-916).
    SkScalar advance = font.measureText(
        text.c_str(), text.size(), SkTextEncoding::kUTF8, &bounds);

#ifdef PULP_HAS_TEXT_SHAPING
    // `font.measureText` counts emoji codepoints as tofu (.notdef
    // advance); re-measure via SkParagraph when the text contains
    // emoji, font-features, or letter-spacing so `width` matches what
    // fill_text actually draws. ASCII / no-feature labels keep the
    // SkFont measurement above — same width fill_text emits via the
    // per-glyph fallback path.
    const bool needs_paragraph =
        needs_paragraph_for_text_metrics(text, font_features_, letter_spacing_);
    if (needs_paragraph) {
        auto prepared = make_paragraph(text, font_family_, font_size_,
                                        font_weight_, font_slant_,
                                        letter_spacing_, /*ltr=*/true,
                                        std::nullopt, font_features_);
        if (prepared.paragraph) {
            advance = prepared.advance;
        }
    }
#endif

    TextMetrics m;
    m.width = advance;
    m.ascent = -sk_metrics.fAscent;   // Skia ascent is negative, we return positive
    m.descent = sk_metrics.fDescent;  // Skia descent is positive
    m.line_height = -sk_metrics.fAscent + sk_metrics.fDescent + sk_metrics.fLeading;

    // HTML5 TextMetrics — bounds is in glyph-local coordinates with the
    // text origin at (0, 0). Negate fLeft so positive values mean
    // "extends left of origin" per the spec.
    m.actual_bounding_box_left = -bounds.fLeft;
    m.actual_bounding_box_right = bounds.fRight;
    m.actual_bounding_box_ascent = -bounds.fTop;
    m.actual_bounding_box_descent = bounds.fBottom;
    return m;
}

Canvas::TextMetrics SkiaCanvas::measure_text_with_font(
    const std::string& family, float size, const std::string& text) {
    SkFont font = make_font(family, size);
    Canvas::TextMetrics m;
    if (!font.getTypeface()) {
        m.width = static_cast<float>(text.size()) * size * 0.5f;
        m.ascent = size;
        m.descent = size * 0.25f;
        m.line_height = size * 1.2f;
        m.actual_bounding_box_left = 0;
        m.actual_bounding_box_right = m.width;
        m.actual_bounding_box_ascent = m.ascent;
        m.actual_bounding_box_descent = m.descent;
        return m;
    }
    SkFontMetrics fm;
    font.getMetrics(&fm);
    SkRect bounds;
    SkScalar advance = font.measureText(
        text.c_str(), text.size(), SkTextEncoding::kUTF8, &bounds);
    m.width = advance;
    m.ascent = -fm.fAscent;
    m.descent = fm.fDescent;
    m.line_height = -fm.fAscent + fm.fDescent + fm.fLeading;
    m.actual_bounding_box_left = -bounds.fLeft;
    m.actual_bounding_box_right = bounds.fRight;
    m.actual_bounding_box_ascent = -bounds.fTop;
    m.actual_bounding_box_descent = bounds.fBottom;
    return m;
}

bool SkiaCanvas::read_pixels(int x, int y, int width, int height, uint8_t* out) {
    if (!canvas_ || !out || width <= 0 || height <= 0) return false;

    auto* surface = canvas_->getSurface();
    if (!surface) return false;

    // Read in unpremultiplied RGBA — matches HTML5 ImageData.data layout.
    SkImageInfo info = SkImageInfo::Make(
        width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    return surface->readPixels(info, out,
                               static_cast<size_t>(width) * 4u,
                               x, y);
}

bool SkiaCanvas::write_pixels(const uint8_t* data, int width, int height,
                               int dx, int dy) {
    if (!canvas_ || !data || width <= 0 || height <= 0) return false;

    SkImageInfo info = SkImageInfo::Make(
        width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    SkBitmap bitmap;
    if (!bitmap.installPixels(info, const_cast<uint8_t*>(data),
                              static_cast<size_t>(width) * 4u)) {
        return false;
    }
    auto image = bitmap.asImage();
    if (!image) return false;

    // CanvasRenderingContext2D.putImageData ignores the current transform
    // and global compositing — bypass them by saving/restoring around
    // a copy-mode draw.
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);
    canvas_->drawImage(image, static_cast<SkScalar>(dx), static_cast<SkScalar>(dy),
                       SkSamplingOptions(), &paint);
    return true;
}


} // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
