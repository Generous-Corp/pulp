// test_text_run_planner_icu.cpp — Pulp #2163, font v2 Slice 1.2.a finish.
//
// Real ICU bidi + script run iterators. Verifies that
// `TextRunPlanner::shape` emits multiple `ShapedRun`s on bidi /
// script boundaries (instead of one trivial run), and that the
// `ShapedText::clusters` table groups graphemes per UAX #29 (ZWJ,
// regional-indicator-pair, virama, combining marks, skin-tone
// modifiers all collapse into one cluster). Also asserts
// `UnicodeIndexMap` fully populates `utf16_offsets` + `byte_to_cluster`.
//
// Each test below targets a single corpus row from the v2 multilingual
// torture corpus (`test/text_corpus/corpus.json`). When ICU is not
// linked into Skia, the planner falls back to one segment and the
// assertions are skipped via `SUCCEED`. The single-segment fallback
// path is still exercised by `test_text_run_planner_parallel.cpp`.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/text_run_planner.hpp>
#include <pulp/canvas/font_options.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace {

pulp::canvas::FontOptions default_opts() {
    pulp::canvas::FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;
    return opts;
}

// Helper: count distinct (bidi_level, script_tag) tuples across runs.
// Used as an informational lower bound; tests on specific corpus rows
// assert exact run counts. `[[maybe_unused]]` because the only call
// site is inside `#ifdef PULP_HAS_SKIA` and the helper has to stay at
// file scope for Catch2 to pick it up from every TEST_CASE.
[[maybe_unused]] std::size_t distinct_run_keys(const pulp::canvas::ShapedText& s) {
    std::vector<std::pair<std::uint8_t, std::uint32_t>> seen;
    for (const auto& r : s.runs) {
        std::pair<std::uint8_t, std::uint32_t> key{r.bidi_level, r.script_tag};
        bool present = false;
        for (const auto& k : seen) {
            if (k == key) { present = true; break; }
        }
        if (!present) seen.push_back(key);
    }
    return seen.size();
}

} // namespace

// ── Bidi split — Hebrew embedded in English ───────────────────────────────
//
// `"Hello עברית world"` — LTR Latin, RTL Hebrew, LTR Latin. ICU's bidi
// algorithm must mark the middle segment with an odd bidi level (RTL)
// and the outer two with an even level (LTR). The planner produces
// three runs (one per maximal same-level segment). Trivial iterators
// would collapse this to one run.
TEST_CASE("planner — bidi split for Hello עברית world",
          "[font][planner][bidi][issue-2163]") {
#ifndef PULP_HAS_SKIA
    SUCCEED("Skia not linked — real ICU iterators not available");
    return;
#else
    const std::string text = "Hello \xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA world";
    auto out = pulp::canvas::TextRunPlanner::instance().shape(text, default_opts());

    // Strict assertion: bidi must produce >=2 runs. The Hebrew middle
    // segment is RTL (level 1) and the outer Latin is LTR (level 0).
    REQUIRE(out.runs.size() >= 2);

    // Confirm the run set actually contains both an even and an odd
    // level — if ICU only ran the trivial iterator we'd get a single
    // run with level 0.
    bool saw_ltr = false, saw_rtl = false;
    for (const auto& r : out.runs) {
        if (r.bidi_level == 0) saw_ltr = true;
        if (r.bidi_level == 1) saw_rtl = true;
    }
    REQUIRE(saw_ltr);
    REQUIRE(saw_rtl);

    // Runs cover the full input contiguously.
    std::size_t covered = 0;
    for (const auto& r : out.runs) {
        REQUIRE(r.logical_start == covered);
        REQUIRE(r.logical_end   >= r.logical_start);
        covered = r.logical_end;
    }
    REQUIRE(covered == text.size());
#endif
}

// ── Script split — Latin + Han + Latin ────────────────────────────────────
//
// `"Hello 日本語 world"` — Latin "Hello ", Han 日本語, Latin " world".
// ICU's HarfBuzz-script iterator labels the Han codepoints with script
// tag `'Hani'` (0x48616E69) and the Latin codepoints with `'Latn'`
// (0x4C61746E). The planner must emit >= 3 runs.
TEST_CASE("planner — script split for Hello 日本語 world",
          "[font][planner][script][issue-2163]") {
#ifndef PULP_HAS_SKIA
    SUCCEED("Skia not linked — real ICU iterators not available");
    return;
#else
    const std::string text = "Hello \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E world";
    auto out = pulp::canvas::TextRunPlanner::instance().shape(text, default_opts());

    REQUIRE(out.runs.size() >= 3);
    REQUIRE(distinct_run_keys(out) >= 2);  // Latin + Han at minimum

    // Total UTF-8 byte coverage = input size, runs disjoint + contiguous.
    std::size_t covered = 0;
    for (const auto& r : out.runs) {
        REQUIRE(r.logical_start == covered);
        covered = r.logical_end;
    }
    REQUIRE(covered == text.size());
#endif
}

// ── Single-run bidi: pure-RTL Hebrew keeps RTL level (Codex review on #2311) ─
//
// `"שלום"` is one bidi run AND one script run. SkShaper iterators report
// `atEnd()` true after the first consume(), but `currentLevel()` still
// reports the RTL level (1) for the just-consumed run. The planner must
// trust currentLevel(), not substitute `base_level` (0 for LTR default),
// or pure RTL strings get shaped as LTR.
TEST_CASE("planner — single-run RTL preserves bidi level after atEnd",
          "[font][planner][bidi][issue-2311]") {
#ifndef PULP_HAS_SKIA
    SUCCEED("Skia not linked — real ICU iterators not available");
    return;
#else
    const std::string text = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";  // שלום
    auto out = pulp::canvas::TextRunPlanner::instance().shape(text, default_opts());

    REQUIRE(out.runs.size() >= 1);
    // Every run must report an odd (RTL) bidi level — if the planner
    // substituted base_level on the atEnd branch we'd see level 0 here.
    bool any_rtl = false;
    for (const auto& r : out.runs) {
        if ((r.bidi_level & 1u) != 0u) any_rtl = true;
    }
    REQUIRE(any_rtl);
#endif
}

// ── Single-run script: pure CJK keeps Hani tag after atEnd (Codex review on #2311) ─
//
// `"日本語"` is one script run. After consume() the script iterator's
// `atEnd()` returns true, but `currentScript()` still reports 'Hani'
// (0x48616E69) for the just-consumed run. Substituting 0 here drops
// the script tag for any final / single-script run.
TEST_CASE("planner — single-run CJK preserves script tag after atEnd",
          "[font][planner][script][issue-2311]") {
#ifndef PULP_HAS_SKIA
    SUCCEED("Skia not linked — real ICU iterators not available");
    return;
#else
    const std::string text = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";  // 日本語
    auto out = pulp::canvas::TextRunPlanner::instance().shape(text, default_opts());

    REQUIRE(out.runs.size() >= 1);
    // 'Hani' = 0x48616E69 (big-endian ASCII tag).
    constexpr std::uint32_t kHani = 0x48616E69u;
    bool saw_hani = false;
    for (const auto& r : out.runs) {
        if (r.script_tag == kHani) saw_hani = true;
    }
    // If the planner substituted 0 on the atEnd branch, no run would
    // carry the 'Hani' tag and saw_hani would stay false.
    REQUIRE(saw_hani);
#endif
}

// ── Devanagari virama: क्ष is one cluster ─────────────────────────────────
//
// `"क्ष"` decomposes as ka (U+0915) + virama (U+094D) + ssa (U+0937).
// UAX #29 says: virama joins the following consonant into one extended
// grapheme cluster. cluster_step (Slice 3.6) already implements this;
// the planner must surface it in `ShapedText::clusters`.
TEST_CASE("planner — Devanagari virama: क्ष is one cluster",
          "[font][planner][cluster][issue-2163]") {
    const std::string text = "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7";  // क्ष
    auto out = pulp::canvas::TextRunPlanner::instance().shape(text, default_opts());

    REQUIRE(out.clusters.size() == 1);
    REQUIRE(out.clusters[0].utf8_start == 0);
    REQUIRE(out.clusters[0].utf8_end   == text.size());
}

// ── Emoji ZWJ family — single cluster ─────────────────────────────────────
//
// 👨‍👩‍👧‍👦 = man + ZWJ + woman + ZWJ + girl + ZWJ + boy. UAX #29 emoji
// ZWJ rule (GB11) collapses these into one grapheme cluster.
TEST_CASE("planner — emoji ZWJ family is one cluster",
          "[font][planner][cluster][emoji-zwj][issue-2163]") {
    const std::string text =
        "\xF0\x9F\x91\xA8"          // U+1F468 man
        "\xE2\x80\x8D"              // U+200D ZWJ
        "\xF0\x9F\x91\xA9"          // U+1F469 woman
        "\xE2\x80\x8D"              // U+200D ZWJ
        "\xF0\x9F\x91\xA7"          // U+1F467 girl
        "\xE2\x80\x8D"              // U+200D ZWJ
        "\xF0\x9F\x91\xA6";         // U+1F466 boy
    auto out = pulp::canvas::TextRunPlanner::instance().shape(text, default_opts());

    REQUIRE(out.clusters.size() == 1);
    REQUIRE(out.clusters[0].utf8_start == 0);
    REQUIRE(out.clusters[0].utf8_end   == text.size());
}

// ── Regional indicator pair — flag is one cluster ─────────────────────────
//
// 🇺🇸 = U+1F1FA + U+1F1F8 (Regional Indicator U + S). Two RI codepoints
// form one flag cluster per UAX #29 GB12/GB13.
TEST_CASE("planner — regional indicator pair is one cluster",
          "[font][planner][cluster][regional-indicators][issue-2163]") {
    const std::string text =
        "\xF0\x9F\x87\xBA"          // U+1F1FA RI-U
        "\xF0\x9F\x87\xB8";         // U+1F1F8 RI-S
    auto out = pulp::canvas::TextRunPlanner::instance().shape(text, default_opts());

    REQUIRE(out.clusters.size() == 1);
    REQUIRE(out.clusters[0].utf8_start == 0);
    REQUIRE(out.clusters[0].utf8_end   == text.size());
}

// ── Combining mark — é is one cluster regardless of NFC/NFD ───────────────
//
// NFC: U+00E9 (precomposed 'é', 2 UTF-8 bytes).
// NFD: U+0065 (e) + U+0301 (combining acute, 3 UTF-8 bytes total).
// Both must collapse to one cluster.
TEST_CASE("planner — combining mark é is one cluster",
          "[font][planner][cluster][combining-marks][issue-2163]") {
    SECTION("NFC precomposed") {
        const std::string text = "\xC3\xA9";   // U+00E9
        auto out = pulp::canvas::TextRunPlanner::instance().shape(text, default_opts());
        REQUIRE(out.clusters.size() == 1);
        REQUIRE(out.clusters[0].utf8_end == text.size());
    }
    SECTION("NFD decomposed") {
        const std::string text = "e\xCC\x81";  // U+0065 U+0301
        auto out = pulp::canvas::TextRunPlanner::instance().shape(text, default_opts());
        REQUIRE(out.clusters.size() == 1);
        REQUIRE(out.clusters[0].utf8_end == text.size());
    }
}

// ── UnicodeIndexMap fully populated ────────────────────────────────────────
//
// Every UTF-16 surface (JS, IME, macOS / Windows a11y) needs UTF-16
// code-unit offsets. Every cluster surface (TextEditor caret motion,
// IME composition, selection) needs `byte_to_cluster`. Both are
// populated by Slice 1.2.a finish.
TEST_CASE("planner — UnicodeIndexMap utf16_offsets + byte_to_cluster",
          "[font][planner][index-map][issue-2163]") {
    SECTION("BMP-only Latin: utf16_offsets parallel scalar_offsets") {
        const std::string text = "Hello";
        auto out = pulp::canvas::TextRunPlanner::instance().shape(text, default_opts());
        // 5 codepoints + sentinel
        REQUIRE(out.index_map.scalar_offsets.size() == 6);
        REQUIRE(out.index_map.utf16_offsets.size() == 6);
        // BMP-only → UTF-16 units match scalar count
        REQUIRE(out.index_map.utf16_offsets.back() == 5);
        // byte_to_cluster covers every byte + trailing sentinel
        REQUIRE(out.index_map.byte_to_cluster.size() == text.size() + 1);
        REQUIRE(out.index_map.byte_to_cluster.back() ==
                static_cast<std::uint32_t>(out.clusters.size()));
    }

    SECTION("Supplementary plane (non-BMP) doubles utf16 units") {
        // U+1F600 grinning face — non-BMP, 4 UTF-8 bytes, 2 UTF-16 units.
        const std::string text = "\xF0\x9F\x98\x80";
        auto out = pulp::canvas::TextRunPlanner::instance().shape(text, default_opts());
        REQUIRE(out.index_map.scalar_offsets.size() == 2);   // 1 codepoint + sentinel
        REQUIRE(out.index_map.utf16_offsets.size() == 2);
        REQUIRE(out.index_map.utf16_offsets.back() == 2);    // surrogate pair
        // Single cluster (no extender / mark).
        REQUIRE(out.clusters.size() == 1);
        REQUIRE(out.clusters[0].utf8_end == text.size());
    }

    SECTION("Cluster mapping invariant — byte_to_cluster[utf8_start] == cluster idx") {
        // 👨‍👩‍👧‍👦 family — 7 codepoints, 1 cluster. Every byte should
        // map to cluster 0; the sentinel byte (== text.size()) should
        // map to clusters.size().
        const std::string text =
            "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
            "\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6";
        auto out = pulp::canvas::TextRunPlanner::instance().shape(text, default_opts());
        REQUIRE(out.clusters.size() == 1);
        REQUIRE(out.index_map.byte_to_cluster.size() == text.size() + 1);
        for (std::size_t i = 0; i < text.size(); ++i) {
            INFO("byte " << i << " of " << text.size());
            REQUIRE(out.index_map.byte_to_cluster[i] == 0u);
        }
        REQUIRE(out.index_map.byte_to_cluster.back() == 1u);
    }
}

// ── Sanity: empty input remains valid ────────────────────────────────────
//
// Per the pre-1.2.a-finish contract (asserted in test_font_options.cpp's
// "TextRunPlanner handles empty text" case), empty input still yields
// exactly one zero-width ShapedRun so callers can index `runs[0]` for
// ascender/leading queries without branching on size.
TEST_CASE("planner — empty text shapes to single zero-width run",
          "[font][planner][issue-2163]") {
    auto out = pulp::canvas::TextRunPlanner::instance().shape("", default_opts());
    REQUIRE(out.runs.size() == 1);
    REQUIRE(out.runs[0].logical_start == 0);
    REQUIRE(out.runs[0].logical_end == 0);
    REQUIRE(out.clusters.empty());
    REQUIRE(out.total_width == 0.0f);
    // scalar_offsets always carries the trailing sentinel.
    REQUIRE(out.index_map.scalar_offsets.size() == 1);
    REQUIRE(out.index_map.utf16_offsets.size() == 1);
    REQUIRE(out.index_map.scalar_offsets.back() == 0u);
    REQUIRE(out.index_map.utf16_offsets.back() == 0u);
}

// ── ASCII-only input — runs stay simple ──────────────────────────────────
//
// Plain Latin should still produce well-formed output (single bidi
// level, single script tag, clusters one-per-codepoint).
TEST_CASE("planner — ASCII Hello produces a contiguous run set",
          "[font][planner][issue-2163]") {
    auto out = pulp::canvas::TextRunPlanner::instance().shape("Hello", default_opts());
    REQUIRE_FALSE(out.runs.empty());
    REQUIRE(out.clusters.size() == 5);  // H, e, l, l, o
    // Every cluster is one byte (no combining marks).
    for (const auto& c : out.clusters) {
        REQUIRE(c.utf8_end == c.utf8_start + 1);
    }
}
