// TextShaper — PreText-style measure-once, reflow-forever text layout.
//
// Two backends:
// 1. PULP_HAS_TEXT_SHAPING: Uses SkShaper/SkParagraph (real HarfBuzz shaping)
// 2. Fallback: Uses character-width estimation (same as before)
//
// The API is identical — only the measurement accuracy differs.

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
#include "modules/skshaper/include/SkShaper.h"
#include "include/core/SkFontMgr.h"

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

static ShapedLayout layout_from_segments(const std::vector<ShapedSegment>& segments,
                                          float max_width, float line_height, bool materialize) {
    ShapedLayout result;
    if (segments.empty()) return result;

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

        if (current_width + seg.width > max_width && current_width > 0) {
            // Wrap: break at last whitespace or force break here
            int break_at = (last_break > line_start) ? last_break : i;
            float break_width = (last_break > line_start) ? width_at_break : current_width;

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
            line_start = (last_break > line_start) ? last_break + 1 : i;
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

    // Lazily create a platform-appropriate font manager — mirrors the
    // helper in skia_canvas.cpp. Without this, SkFontMgr::RefEmpty()
    // returns no typefaces and `font.measureText()` reports near-zero
    // advance, collapsing Label::intrinsic_width() (pulp #945).
    static sk_sp<SkFontMgr> make_platform_font_mgr() {
#if defined(__APPLE__)
        return SkFontMgr_New_CoreText(nullptr);
#elif defined(_WIN32)
        return SkFontMgr_New_DirectWrite();
#elif defined(__ANDROID__)
        return SkFontMgr_New_Android(nullptr, SkFontScanner_Make_FreeType());
#elif defined(__linux__)
        return SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#else
        return nullptr;
#endif
    }

    Impl() {
        font_mgr = make_platform_font_mgr();
        if (!font_mgr) {
            font_mgr = SkFontMgr::RefEmpty();
        }
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

    float measure_segment(const std::string& text, const std::string& font_family, float font_size) {
        // Check cache first
        CacheKey key{font_family, font_size};
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
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
        if (font_mgr && font_mgr->countFamilies() > 0) {
            typeface = font_mgr->matchFamilyStyle(font_family.c_str(),
                                                  SkFontStyle::Normal());
            if (!typeface) {
                // Family wasn't found (e.g. "Inter" not installed) —
                // fall back to the platform default face so we still
                // get a real measurement instead of a phantom typeface.
                typeface = font_mgr->matchFamilyStyle(nullptr,
                                                      SkFontStyle::Normal());
            }
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
    result.line_height_ = font_size * 1.5f;

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
                                 float line_height) const {
    float lh = line_height > 0 ? line_height : prepared.line_height();
    return layout_from_segments(prepared.segments(), max_width, lh, false);
}

ShapedLayout TextShaper::layout_with_lines(const PreparedText& prepared, float max_width,
                                            float line_height) const {
    float lh = line_height > 0 ? line_height : prepared.line_height();
    return layout_from_segments(prepared.segments(), max_width, lh, true);
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
