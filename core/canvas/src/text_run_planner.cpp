// text_run_planner.cpp — Pulp #2163 follow-up, Phase 1 / Slice 1.2.a.
//
// Skeleton implementation of `TextRunPlanner`. Wraps the existing
// TextShaper / SkShaper path behind a single-run `ShapedText` so
// downstream consumers can target the canonical-artifact API today
// without forcing the bidi/script/cluster correctness work in the
// same commit. Slice 1.2.a finish replaces the trivial single-run
// path with real ICU/HarfBuzz iterators emitting multiple runs;
// the API surface is stable across that swap.

#include "pulp/canvas/text_run_planner.hpp"
#include "pulp/canvas/text_shaper.hpp"
#include "pulp/canvas/font_resolver.hpp"
#include "pulp/canvas/font_scope.hpp"

#ifdef PULP_HAS_SKIA
#include "include/core/SkTypeface.h"
#endif

#include <mutex>
#include <unordered_map>

namespace pulp::canvas {

struct TextRunPlanner::Impl {
    mutable std::mutex mtx;

    struct CacheKey {
        std::string text;
        std::size_t options_hash = 0;

        bool operator==(const CacheKey& other) const {
            return options_hash == other.options_hash && text == other.text;
        }
    };
    struct CacheKeyHash {
        std::size_t operator()(const CacheKey& k) const noexcept {
            return std::hash<std::string>{}(k.text) ^ (k.options_hash * 0x9e3779b97f4a7c15ull);
        }
    };

    std::unordered_map<CacheKey, ShapedText, CacheKeyHash> cache;
};

TextRunPlanner::TextRunPlanner() : impl_(std::make_unique<Impl>()) {}
TextRunPlanner::~TextRunPlanner() = default;

TextRunPlanner& TextRunPlanner::instance() {
    static TextRunPlanner inst;
    return inst;
}

void TextRunPlanner::clear_cache() {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->cache.clear();
}

namespace {

// Slice 1.2.a skeleton: build a per-codepoint cluster table by
// scanning UTF-8 byte by byte. Each codepoint becomes its own
// cluster. Slice 1.2.a finish replaces this with UAX #29 grapheme
// segmentation (ZWJ + RI-pair + combining-mark grouping).
void populate_index_map(std::string_view text, UnicodeIndexMap& map) {
    map.scalar_offsets.clear();
    map.scalar_offsets.reserve(text.size() + 1);
    for (std::size_t i = 0; i < text.size();) {
        map.scalar_offsets.push_back(static_cast<std::uint32_t>(i));
        unsigned char c = static_cast<unsigned char>(text[i]);
        if      (c < 0x80) ++i;             // 1-byte
        else if ((c & 0xE0) == 0xC0) i += 2; // 2-byte
        else if ((c & 0xF0) == 0xE0) i += 3; // 3-byte
        else if ((c & 0xF8) == 0xF0) i += 4; // 4-byte
        else ++i;                            // invalid continuation; skip
    }
    map.scalar_offsets.push_back(static_cast<std::uint32_t>(text.size()));
}

void populate_line_breaks(std::string_view text,
                          std::vector<LineBreakOpportunity>& out) {
    out.clear();
    // Skeleton: ASCII whitespace + hard newline. ICU-driven locale-
    // aware break opportunities arrive in Phase 2 / Slice 2.4.
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\n') {
            out.push_back({static_cast<std::uint32_t>(i),
                           LineBreakOpportunity::Kind::Hard});
        } else if (c == ' ' || c == '\t') {
            out.push_back({static_cast<std::uint32_t>(i + 1),
                           LineBreakOpportunity::Kind::Soft});
        }
    }
}

} // namespace

ShapedText TextRunPlanner::shape(std::string_view text,
                                 const FontOptions& opts_in) {
    FontOptions opts = opts_in;
    if (opts.registry_generation == 0) {
        opts.registry_generation = merged_generation_for(opts.scope);
    }

    Impl::CacheKey key{std::string(text), opts.hash()};
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        auto it = impl_->cache.find(key);
        if (it != impl_->cache.end()) return it->second;
    }

    ShapedText out;
    out.text = std::string(text);
    out.options = opts;

    populate_index_map(text, out.index_map);
    populate_line_breaks(text, out.line_breaks);

    // Resolve the typeface via FontResolver (same path
    // SkiaCanvas/TextShaper use post-Slice-1.1.a). The resolved
    // ResolvedFont carries the trace + scope + generation we record
    // on the run.
    ResolvedFont resolved = FontResolver::instance().resolve_family_list(opts);

    // Skeleton: one ShapedRun spanning the full input. Shaping data
    // (glyph_ids, advances, offsets) is populated from TextShaper's
    // prepared-text path so width matches what the painter draws.
    // Slice 1.2.a finish swaps in real SkShaper output with per-run
    // bidi/script split.
    ShapedRun run;
    run.font = std::move(resolved);
    run.logical_start = 0;
    run.logical_end = text.size();
    run.bidi_level = (opts.direction == BaseDirection::RTL) ? 1 : 0;

    auto& shaper = global_text_shaper();
    // Build the family string for the shaper using the resolved
    // typeface's actual family — the shaper will hit the resolver's
    // cache as well, so this is consistent.
    std::string family_str = run.font.actual_family;
    if (family_str.empty() && !opts.family_stack.empty()) {
        family_str = opts.family_stack.front();
    }
    auto prepared = shaper.prepare(text, family_str, opts.size);
    run.advance_total = prepared.total_width();
    run.metrics.ascent  = prepared.ascent();
    run.metrics.descent = prepared.descent();
    run.metrics.leading = prepared.leading();

    out.total_width = run.advance_total;
    out.overall_metrics = run.metrics;
    out.runs.push_back(std::move(run));

    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->cache[key] = out;
    }
    return out;
}

} // namespace pulp::canvas
