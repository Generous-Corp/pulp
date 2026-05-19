// text_run_planner.cpp — Pulp #2163 follow-up, Phase 1 / Slice 1.2.a.
//
// Slice 1.2.a finish: real ICU/HarfBuzz iterators replace the trivial
// single-run skeleton. Bidi level + script tag are computed per run via
// `SkShaper::MakeIcuBiDiRunIterator` + `SkShaper::MakeHbIcuScriptRunIterator`
// (available because `external/skia-build/build/mac-gpu/lib/Release/libskshaper.a`
// statically links the ICU-backed builders — verified with `nm` at
// implementation time). When Skia is not linked (PULP_HAS_SKIA undefined),
// the planner falls back to a single run with bidi level / script tag
// inferred from `FontOptions::direction`. The data shape produced is the
// same in both paths; only the run count varies.
//
// Cluster construction: the public `cluster_step()` walker
// (font_registry_stubs.cpp) is UAX #29-lite — it knows ZWJ, regional-
// indicator parity, virama+consonant, combining marks, skin-tone
// modifiers, and Hangul T jamo. The planner walks it forward through
// every run-spanning region to build `ShapedText::clusters` and the
// `byte_to_cluster` index map. This is the same walker `TextEditor`'s
// caret-motion uses (Slice 3.6) so the cluster boundaries the painter
// and editor see are identical.
//
// UnicodeIndexMap: scalar_offsets (per-codepoint UTF-8 byte offsets +
// trailing sentinel), utf16_offsets (per-codepoint UTF-16 code unit
// offsets — single-unit for BMP, surrogate-pair for supplementary
// planes), and byte_to_cluster (per-UTF-8-byte cluster index). The
// trailing sentinel is included in each array so cluster_count =
// scalar_offsets.size() - 1, and byte_to_cluster[text.size()] equals
// clusters.size() — every downstream consumer (a11y selection ranges,
// IME composition, locale shaping) can subtract two entries without a
// bounds check.

#include "pulp/canvas/text_run_planner.hpp"
#include "pulp/canvas/text_shaper.hpp"
#include "pulp/canvas/font_resolver.hpp"
#include "pulp/canvas/font_scope.hpp"
#include "pulp/canvas/bundled_fonts.hpp"   // cluster_step()

#include <algorithm>
#include <future>
#include <thread>

#ifdef PULP_HAS_SKIA
// The bundled Skia archive ships `libskshaper.a` + `libskunicode_icu.a`
// with the ICU-backed bidi + HarfBuzz/ICU-backed script run-iterator
// builders linked in (verified at impl time with
// `nm libskshaper.a | grep MakeIcuBiDi`). The corresponding
// declarations in `SkShaper.h` are gated by
// `SK_SHAPER_UNICODE_AVAILABLE` / `SK_SHAPER_HARFBUZZ_AVAILABLE`, which
// upstream's prebuilt distribution doesn't pre-define for downstream
// consumers. We define both *before* including the header so the
// declarations are visible and the call sites compile — the link step
// then resolves them against the static libs as expected.
#  ifndef SK_SHAPER_UNICODE_AVAILABLE
#    define SK_SHAPER_UNICODE_AVAILABLE 1
#  endif
#  ifndef SK_SHAPER_HARFBUZZ_AVAILABLE
#    define SK_SHAPER_HARFBUZZ_AVAILABLE 1
#  endif
#include "include/core/SkTypeface.h"
#include "modules/skshaper/include/SkShaper.h"
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

// Decode the next UTF-8 codepoint at `text[i]`. Returns
// {codepoint, byte_length}; malformed sequences return {0, 1} so the
// walker always makes progress. Local to this TU to keep
// font_registry_stubs.cpp's identical helper at internal linkage.
struct Utf8Cp { std::uint32_t cp; std::size_t bytes; };

inline bool utf8_cont(unsigned char b) noexcept {
    return (b & 0xC0u) == 0x80u;
}

inline Utf8Cp decode_utf8(std::string_view s, std::size_t i) noexcept {
    if (i >= s.size()) return {0, 0};
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return {c, 1};
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        if (!utf8_cont(b1)) return {0, 1};
        return {static_cast<std::uint32_t>(((c & 0x1Fu) << 6) | (b1 & 0x3Fu)), 2};
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
        if (!utf8_cont(b1) || !utf8_cont(b2)) return {0, 1};
        return {static_cast<std::uint32_t>(((c & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu)), 3};
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
        unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
        unsigned char b3 = static_cast<unsigned char>(s[i + 3]);
        if (!utf8_cont(b1) || !utf8_cont(b2) || !utf8_cont(b3)) return {0, 1};
        return {static_cast<std::uint32_t>(((c & 0x07u) << 18) | ((b1 & 0x3Fu) << 12)
                                         | ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu)), 4};
    }
    return {0, 1};
}

// UTF-16 code-unit length of a Unicode scalar value: 2 for non-BMP
// (supplementary plane, U+10000..U+10FFFF, surrogate pair), 1
// otherwise. ICU's UText API and HarfBuzz both use the same rule.
inline std::uint32_t utf16_units(std::uint32_t cp) noexcept {
    return cp >= 0x10000u ? 2u : 1u;
}

// Build per-codepoint UTF-8 offsets + UTF-16 offsets. Both arrays end
// with a trailing sentinel equal to the total UTF-8 byte size /
// UTF-16 code unit count so callers can compute scalar widths with a
// single subtraction.
void populate_codepoint_offsets(std::string_view text, UnicodeIndexMap& map) {
    map.scalar_offsets.clear();
    map.utf16_offsets.clear();
    map.scalar_offsets.reserve(text.size() + 1);
    map.utf16_offsets.reserve(text.size() + 1);

    std::uint32_t utf16_idx = 0;
    for (std::size_t i = 0; i < text.size();) {
        Utf8Cp cp = decode_utf8(text, i);
        map.scalar_offsets.push_back(static_cast<std::uint32_t>(i));
        map.utf16_offsets.push_back(utf16_idx);
        utf16_idx += utf16_units(cp.cp);
        i += cp.bytes;
    }
    map.scalar_offsets.push_back(static_cast<std::uint32_t>(text.size()));
    map.utf16_offsets.push_back(utf16_idx);
}

// Walk the public UAX #29 `cluster_step` (Slice 3.6) forward through
// `text`, emitting one `ClusterEntry` per grapheme cluster. The walker
// honors ZWJ, RI parity, virama+consonant, combining marks, skin-tone
// modifiers, Hangul T jamo, and variation selectors. Each cluster
// stores its UTF-8 byte range plus the run index it belongs to; glyph
// counts and starts are intentionally left at 0 in this slice — the
// painter does not consume them yet, and populating them requires
// driving SkShaper end-to-end which is the Phase-2 follow-on. The
// run index is computed by binary-searching `run_starts`.
void populate_clusters(std::string_view text,
                       const std::vector<std::size_t>& run_starts,
                       std::vector<ClusterEntry>& clusters,
                       std::vector<std::uint32_t>& byte_to_cluster) {
    clusters.clear();
    byte_to_cluster.assign(text.size() + 1, 0);

    // Promote the string_view to a std::string once — the public
    // `cluster_step` takes `const std::string&`. The cost is one copy
    // per shape() call, which we already pay for `ShapedText::text`.
    const std::string owned(text);

    std::size_t cursor = 0;
    while (cursor < owned.size()) {
        std::size_t next = cluster_step(owned, cursor, /*forward=*/true);
        if (next <= cursor) next = cursor + 1;   // safety: forward progress
        if (next > owned.size()) next = owned.size();

        ClusterEntry e;
        e.utf8_start = static_cast<std::uint32_t>(cursor);
        e.utf8_end   = static_cast<std::uint32_t>(next);

        // Find which run this cluster's *start* falls into. Runs are
        // contiguous and disjoint, so a linear search over the sorted
        // run_starts is fine — paragraph-sized inputs cap run_count
        // in single digits in practice.
        std::uint32_t run_idx = 0;
        for (std::size_t r = 0; r < run_starts.size(); ++r) {
            if (run_starts[r] <= cursor) run_idx = static_cast<std::uint32_t>(r);
            else break;
        }
        e.run_index = run_idx;

        const std::uint32_t cluster_idx = static_cast<std::uint32_t>(clusters.size());
        for (std::size_t b = cursor; b < next && b < byte_to_cluster.size(); ++b) {
            byte_to_cluster[b] = cluster_idx;
        }
        clusters.push_back(e);
        cursor = next;
    }
    // Trailing sentinel: byte_to_cluster[text.size()] == clusters.size()
    // so the half-open interval [byte_to_cluster[i], byte_to_cluster[j])
    // gives the cluster span for any UTF-8 byte slice [i, j].
    if (!byte_to_cluster.empty()) {
        byte_to_cluster.back() = static_cast<std::uint32_t>(clusters.size());
    }
}

void populate_line_breaks(std::string_view text,
                          std::vector<LineBreakOpportunity>& out) {
    out.clear();
    // Slice 1.2.a finish keeps the heuristic break opportunities; the
    // ICU BreakIterator-driven path is Slice 2.4 (locale-aware line
    // breaking) and consumes the same out array.
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

// A bidi+script segment of the input. Half-open UTF-8 byte range
// [start, end). Always non-empty.
struct BidiScriptSegment {
    std::size_t  start;
    std::size_t  end;
    std::uint8_t bidi_level;
    std::uint32_t script_tag;
};

#ifdef PULP_HAS_SKIA
// Walk both ICU iterators in lockstep, emitting one segment per
// (bidi, script) tuple. Modelled on Skia's own `RunIteratorQueue`
// (SkShaper_harfbuzz.cpp): at each step, find the iterator with the
// earlier `endOfCurrentRun()`, read both currently-active states,
// advance any iterator whose run ends at the chosen endpoint. This
// produces a sequence of maximal segments where bidi level and
// script tag are both constant — exactly the shaping units the
// HarfBuzz / SkShaper engine wants when called with explicit
// iterators.
//
// Returns an empty vector if either iterator construction fails
// (e.g. ICU not actually linked at runtime), letting the caller fall
// back to a single segment for the whole input.
std::vector<BidiScriptSegment>
segment_with_icu(std::string_view text, std::uint8_t base_level) {
    std::vector<BidiScriptSegment> out;
    if (text.empty()) return out;

    auto bidi = SkShaper::MakeIcuBiDiRunIterator(text.data(), text.size(), base_level);
    auto scr  = SkShaper::MakeHbIcuScriptRunIterator(text.data(), text.size());
    if (!bidi || !scr) return out;   // caller will degrade gracefully

    // SkShaper RunIterator contract: post-construction, the FIRST
    // consume() advances state INTO the first run. After consume(),
    // `endOfCurrentRun()` is the end offset of the just-consumed run
    // and `current*()` reports its properties. Subsequent consume()
    // calls advance to the next run. `atEnd()` becomes true only
    // after consuming the LAST run.
    bidi->consume();
    scr->consume();

    std::size_t cursor = 0;
    // Loop invariant at the top of each iteration:
    //   * cursor is the start offset of the next segment to emit.
    //   * Both iterators have consumed at least one run AND their
    //     endOfCurrentRun() is strictly > cursor (or atEnd() is true).
    constexpr std::size_t kMaxSegments = 1 << 16;
    while (cursor < text.size() && out.size() < kMaxSegments) {
        const bool be = bidi->atEnd();
        const bool se = scr->atEnd();
        const std::size_t bidi_end = be ? text.size() : bidi->endOfCurrentRun();
        const std::size_t scr_end  = se ? text.size() : scr->endOfCurrentRun();
        const std::size_t end      = std::min(bidi_end, scr_end);
        if (end <= cursor) break;   // safety: forward progress

        BidiScriptSegment seg;
        seg.start      = cursor;
        seg.end        = end;
        // SkShaper RunIterator contract: after the final consume() on
        // a one-run input (e.g. pure Hebrew "שלום", pure CJK "日本語"),
        // atEnd() reports true but currentLevel() / currentScript()
        // remain valid for the just-consumed run. Substituting
        // base_level / 0 here would shape pure-RTL text as LTR and
        // drop the script tag for any final or single-script run.
        // See Codex review on PR #2311.
        seg.bidi_level = bidi->currentLevel();
        seg.script_tag = scr->currentScript();

        // Merge with the previous segment when both properties match,
        // which happens at seams where one iterator's run ended and
        // the other didn't — without the merge those would be two
        // identical-property segments back-to-back.
        if (!out.empty()
            && out.back().end        == seg.start
            && out.back().bidi_level == seg.bidi_level
            && out.back().script_tag == seg.script_tag) {
            out.back().end = seg.end;
        } else {
            out.push_back(seg);
        }

        cursor = end;
        // Advance any iterator whose current run ended exactly at
        // `cursor`. Both can advance in the same step if their runs
        // line up; one will advance while the other waits if they
        // don't.
        if (!be && bidi->endOfCurrentRun() <= cursor && !bidi->atEnd()) {
            bidi->consume();
        }
        if (!se && scr->endOfCurrentRun() <= cursor && !scr->atEnd()) {
            scr->consume();
        }
    }
    return out;
}
#endif  // PULP_HAS_SKIA

// SkShaper iterator endpoints aren't guaranteed to land on cluster
// boundaries — for the input we care about they overwhelmingly do
// (Han / Latin / Arabic are all script-pure, bidi flips at script
// boundaries that already correspond to grapheme boundaries), but the
// API contract is "UTF-8 byte offsets" not "cluster offsets". This
// helper snaps any run boundary to the nearest enclosing cluster
// boundary by walking `cluster_step` from byte 0, which is O(n) per
// shape() call — acceptable for label-sized inputs.
void snap_runs_to_cluster_boundaries(const std::string& text,
                                     std::vector<BidiScriptSegment>& segs) {
    if (segs.empty()) return;

    // Build a sorted vector of cluster boundary offsets.
    std::vector<std::size_t> boundaries;
    boundaries.reserve(text.size() / 4 + 2);
    boundaries.push_back(0);
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        std::size_t next = cluster_step(text, cursor, /*forward=*/true);
        if (next <= cursor) next = cursor + 1;
        if (next > text.size()) next = text.size();
        boundaries.push_back(next);
        cursor = next;
    }

    auto snap_to_le_boundary = [&boundaries](std::size_t off) -> std::size_t {
        // Largest boundary <= off.
        auto it = std::upper_bound(boundaries.begin(), boundaries.end(), off);
        if (it == boundaries.begin()) return 0;
        --it;
        return *it;
    };
    auto snap_to_ge_boundary = [&boundaries](std::size_t off) -> std::size_t {
        auto it = std::lower_bound(boundaries.begin(), boundaries.end(), off);
        if (it == boundaries.end()) return boundaries.back();
        return *it;
    };

    // First pass: snap each segment's end to a boundary >= its end so
    // we never cleave a cluster, then re-thread the starts.
    for (auto& s : segs) {
        s.end = snap_to_ge_boundary(s.end);
    }
    segs.front().start = 0;
    for (std::size_t i = 1; i < segs.size(); ++i) {
        segs[i].start = segs[i - 1].end;
    }
    // Drop empties (can happen if two iterator boundaries collapsed
    // onto the same cluster).
    segs.erase(std::remove_if(segs.begin(), segs.end(),
                              [](const BidiScriptSegment& s) { return s.end <= s.start; }),
               segs.end());
    // Coalesce adjacent same-property segments after the snap.
    std::vector<BidiScriptSegment> merged;
    merged.reserve(segs.size());
    for (const auto& s : segs) {
        if (!merged.empty()
            && merged.back().end == s.start
            && merged.back().bidi_level == s.bidi_level
            && merged.back().script_tag == s.script_tag) {
            merged.back().end = s.end;
        } else {
            merged.push_back(s);
        }
    }
    segs.swap(merged);
    (void)snap_to_le_boundary;   // reserved for the inverse direction
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

    populate_codepoint_offsets(text, out.index_map);
    populate_line_breaks(text, out.line_breaks);

    // Resolve the typeface via FontResolver (same path
    // SkiaCanvas/TextShaper use post-Slice-1.1.a). The resolved
    // ResolvedFont carries the trace + scope + generation we record
    // on every run we emit.
    ResolvedFont resolved = FontResolver::instance().resolve_family_list(opts);

    if (text.empty()) {
        // Preserve the pre-1.2.a-finish contract: even empty input
        // yields exactly one zero-width run so callers can rely on
        // `out.runs[0]` existing for ascender/leading queries on an
        // empty label without branching on size. The test target
        // pulp-test-font-options (TextRunPlanner handles empty text)
        // asserts this shape.
        ShapedRun run;
        run.font          = std::move(resolved);
        run.logical_start = 0;
        run.logical_end   = 0;
        run.bidi_level    = (opts.direction == BaseDirection::RTL) ? 1u : 0u;
        out.runs.push_back(std::move(run));
        // byte_to_cluster keeps its single sentinel entry (== 0 clusters).
        out.index_map.byte_to_cluster.assign(1, 0u);

        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->cache[key] = out;
        return out;
    }

    const std::uint8_t base_level =
        (opts.direction == BaseDirection::RTL) ? 1u : 0u;

    // ── Build bidi+script segments ────────────────────────────────────────
    std::vector<BidiScriptSegment> segments;
#ifdef PULP_HAS_SKIA
    segments = segment_with_icu(text, base_level);
#endif
    if (segments.empty()) {
        // Degraded path (non-Skia or ICU iterator construction failed):
        // single segment, level / script inferred from FontOptions.
        BidiScriptSegment seg;
        seg.start      = 0;
        seg.end        = text.size();
        seg.bidi_level = base_level;
        seg.script_tag = 0;
        segments.push_back(seg);
    } else {
        // Ensure full coverage. ICU iterators sometimes stop short on
        // malformed UTF-8; we cap the last segment at text.size() and
        // synthesize a trailing run if any bytes weren't covered.
        if (segments.back().end < text.size()) {
            BidiScriptSegment tail;
            tail.start      = segments.back().end;
            tail.end        = text.size();
            tail.bidi_level = segments.back().bidi_level;
            tail.script_tag = segments.back().script_tag;
            segments.push_back(tail);
        }
        // Snap to UAX #29 cluster boundaries so a single grapheme is
        // never split across runs (e.g. an emoji ZWJ family at the
        // bidi-script boundary stays inside one run).
        snap_runs_to_cluster_boundaries(out.text, segments);
    }

    // ── Emit one ShapedRun per segment ────────────────────────────────────
    out.runs.reserve(segments.size());
    auto& shaper = global_text_shaper();
    std::string family_str = resolved.actual_family;
    if (family_str.empty() && !opts.family_stack.empty()) {
        family_str = opts.family_stack.front();
    }

    float ascent_max  = 0.0f;
    float descent_max = 0.0f;
    float leading_max = 0.0f;
    float width_total = 0.0f;

    std::vector<std::size_t> run_starts;
    run_starts.reserve(segments.size());

    for (const auto& seg : segments) {
        ShapedRun run;
        run.font          = resolved;   // same resolved face for all runs in v1
        run.logical_start = seg.start;
        run.logical_end   = seg.end;
        run.bidi_level    = seg.bidi_level;
        run.script_tag    = seg.script_tag;

        std::string_view segment_view = text.substr(seg.start, seg.end - seg.start);

        // Measurement uses the existing TextShaper path — the SkShaper
        // glyph buffers + per-glyph advances are downstream consumers'
        // job (Phase 2 / Slice 2.4 locale-aware line breaking exercises
        // them). What matters here is that each run reports its own
        // sub-string width so `total_width` is the sum of run widths,
        // not a re-measurement of the whole input.
        auto prepared = shaper.prepare(segment_view, family_str, opts.size);
        run.advance_total  = prepared.total_width();
        run.metrics.ascent  = prepared.ascent();
        run.metrics.descent = prepared.descent();
        run.metrics.leading = prepared.leading();

        ascent_max  = std::max(ascent_max,  run.metrics.ascent);
        descent_max = std::max(descent_max, run.metrics.descent);
        leading_max = std::max(leading_max, run.metrics.leading);
        width_total += run.advance_total;

        run_starts.push_back(seg.start);
        out.runs.push_back(std::move(run));
    }

    // ── Cluster table + byte_to_cluster index map ────────────────────────
    populate_clusters(text, run_starts, out.clusters, out.index_map.byte_to_cluster);

    out.total_width            = width_total;
    out.overall_metrics.ascent  = ascent_max;
    out.overall_metrics.descent = descent_max;
    out.overall_metrics.leading = leading_max;

    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->cache[key] = out;
    }
    return out;
}

// pulp #2163 / font v2 Slice 3.7 — parallel shaping.
// Fans shape() calls out via std::async(launch::async). Resolver,
// FontFlightRecorder, and the per-planner cache are all thread-safe
// (internal mutexes); ShapedText is owned-by-value so moving it out
// of a future is safe. Concurrency is bounded by hardware_concurrency
// to avoid kernel-thread oversubscription on big batches across many
// UI surfaces.
std::vector<ShapedText> TextRunPlanner::shape_batch(
    const std::vector<std::pair<std::string, FontOptions>>& inputs) {
    if (inputs.empty()) return {};
    if (inputs.size() == 1) {
        std::vector<ShapedText> out;
        out.reserve(1);
        out.push_back(shape(inputs.front().first, inputs.front().second));
        return out;
    }

    const unsigned hint = std::thread::hardware_concurrency();
    const std::size_t conc = std::min<std::size_t>(
        inputs.size(), hint == 0 ? 4u : static_cast<std::size_t>(hint));

    std::vector<ShapedText> results(inputs.size());

    std::size_t i = 0;
    while (i < inputs.size()) {
        const std::size_t batch = std::min(conc, inputs.size() - i);
        std::vector<std::future<ShapedText>> pending;
        pending.reserve(batch);
        for (std::size_t j = 0; j < batch; ++j) {
            const auto& in = inputs[i + j];
            pending.push_back(std::async(std::launch::async,
                [this, &in]() {
                    return shape(in.first, in.second);
                }));
        }
        for (std::size_t j = 0; j < batch; ++j) {
            results[i + j] = pending[j].get();
        }
        i += batch;
    }
    return results;
}

} // namespace pulp::canvas
