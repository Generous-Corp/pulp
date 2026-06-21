// Exercises core/canvas::BidiAnalyzer (SheenBidi-backed paragraph
// Unicode Bidirectional Algorithm) and verifies that the
// TextRunPlanner non-ICU fallback path emits correct bidi runs for
// Arabic / Hebrew / mixed-direction text.
//
// Coverage layers:
//   1. BidiAnalyzer directly: pure-Arabic, pure-Hebrew, Latin+Arabic,
//      Latin+Hebrew, and "Latin Arabic Latin" mixed strings. Assertions
//      cover (a) run count >= expected, (b) RTL run wraps the RTL
//      bytes, (c) base level is 1 for pure RTL and 0 for pure LTR.
//   2. visual_order(): asserts a single RTL run inside an LTR
//      paragraph is positioned correctly (rule L2 simplified).
//   3. TextRunPlanner fallback: under non-Skia builds, asserts the
//      planner emits multiple runs for mixed-direction input rather
//      than one trivial run. Under PULP_HAS_SKIA the planner uses
//      ICU iterators, so we only assert "more than one run" without
//      pinning exact counts.
//
// The tests are deterministic: input strings are hex-encoded UTF-8
// literals so they survive editors that re-normalize Hebrew / Arabic
// glyph orientation when viewing the file.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/bidi.hpp>
#include <pulp/canvas/text_run_planner.hpp>
#include <pulp/canvas/font_options.hpp>

#include <algorithm>
#include <string>
#include <string_view>

using pulp::canvas::BidiAnalyzer;
using pulp::canvas::BidiBaseDirection;
using pulp::canvas::BidiRun;

namespace {

// "Hello " in ASCII (6 bytes), all level 0.
constexpr std::string_view kLatinHello = "Hello ";

// "שלום" (Shalom) — Hebrew, 4 codepoints, 8 UTF-8 bytes. Each char is
// 2 bytes in UTF-8.
constexpr std::string_view kHebrewShalom = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";

// "مرحبا" (Marhaba — "hello") — Arabic, 5 codepoints, 10 UTF-8 bytes.
constexpr std::string_view kArabicMarhaba = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7";

// " world" in ASCII (6 bytes), all level 0.
constexpr std::string_view kLatinWorld = " world";

std::string concat3(std::string_view a, std::string_view b, std::string_view c) {
    std::string out;
    out.reserve(a.size() + b.size() + c.size());
    out.append(a).append(b).append(c);
    return out;
}

}  // namespace

TEST_CASE("BidiAnalyzer empty input is empty", "[bidi]") {
    auto bp = BidiAnalyzer::analyze("", BidiBaseDirection::Auto);
    REQUIRE(bp.runs.empty());
}

TEST_CASE("BidiAnalyzer pure-Latin is one LTR run", "[bidi]") {
    auto bp = BidiAnalyzer::analyze("Hello, world!", BidiBaseDirection::Auto);
    REQUIRE(bp.runs.size() == 1);
    REQUIRE(bp.runs[0].level == 0);
    REQUIRE(bp.runs[0].is_rtl() == false);
    REQUIRE(bp.base_level == 0);
}

TEST_CASE("BidiAnalyzer pure-Hebrew has base level 1 and an RTL run",
          "[bidi]") {
    auto bp = BidiAnalyzer::analyze(kHebrewShalom, BidiBaseDirection::Auto);
    REQUIRE_FALSE(bp.runs.empty());

    if (BidiAnalyzer::has_sheenbidi()) {
        // SheenBidi: rule P2 detects Hebrew as the first strong char →
        // base level 1.
        REQUIRE(bp.base_level == 1);
        // Every emitted run covers RTL text → odd level.
        for (const auto& r : bp.runs) {
            REQUIRE(r.is_rtl());
        }
    } else {
        // Pass-through stub honours the requested base direction
        // (Auto → LTR). Production builds always have SheenBidi
        // linked; this branch only fires in degraded reduced-deps
        // sandboxes that compile the stub.
        SUCCEED("SheenBidi not linked — pass-through stub returns one LTR run");
    }
}

TEST_CASE("BidiAnalyzer pure-Arabic has base level 1 and an RTL run",
          "[bidi]") {
    auto bp = BidiAnalyzer::analyze(kArabicMarhaba, BidiBaseDirection::Auto);
    REQUIRE_FALSE(bp.runs.empty());

    if (BidiAnalyzer::has_sheenbidi()) {
        REQUIRE(bp.base_level == 1);
        for (const auto& r : bp.runs) {
            REQUIRE(r.is_rtl());
        }
    } else {
        SUCCEED("SheenBidi not linked — pass-through stub returns one LTR run");
    }
}

TEST_CASE("BidiAnalyzer mixed Latin + Arabic emits multiple runs",
          "[bidi]") {
    // "Hello مرحبا world" — Latin prefix, Arabic middle, Latin suffix.
    const std::string mixed = concat3(kLatinHello, kArabicMarhaba, kLatinWorld);
    auto bp = BidiAnalyzer::analyze(mixed, BidiBaseDirection::Auto);

    REQUIRE_FALSE(bp.runs.empty());

    if (BidiAnalyzer::has_sheenbidi()) {
        // Base level should resolve to LTR (first strong char is 'H').
        REQUIRE(bp.base_level == 0);
        // Expect at least three runs: LTR / RTL / LTR.
        REQUIRE(bp.runs.size() >= 3);

        // The Arabic span should appear in the runs and carry an
        // odd embedding level.
        bool found_rtl_run = false;
        for (const auto& r : bp.runs) {
            if (r.is_rtl()) {
                found_rtl_run = true;
                // The RTL run should fall within the Arabic byte range:
                // [kLatinHello.size(), kLatinHello.size() + kArabicMarhaba.size()).
                REQUIRE(r.start >= static_cast<std::uint32_t>(kLatinHello.size()));
                REQUIRE(r.end()  <= static_cast<std::uint32_t>(kLatinHello.size() + kArabicMarhaba.size()));
            }
        }
        REQUIRE(found_rtl_run);

        // analyze() returns logical order — runs must be sorted by start.
        for (std::size_t i = 1; i < bp.runs.size(); ++i) {
            REQUIRE(bp.runs[i].start >= bp.runs[i - 1].start);
        }

        // Coverage: union of runs must equal the input length.
        std::uint32_t total_covered = 0;
        for (const auto& r : bp.runs) {
            total_covered += r.length;
        }
        REQUIRE(total_covered == static_cast<std::uint32_t>(mixed.size()));
    } else {
        SUCCEED("SheenBidi not linked — pass-through stub returns one LTR run");
    }
}

TEST_CASE("BidiAnalyzer Latin + Hebrew emits multiple runs",
          "[bidi]") {
    const std::string mixed = concat3(kLatinHello, kHebrewShalom, kLatinWorld);
    auto bp = BidiAnalyzer::analyze(mixed, BidiBaseDirection::Auto);

    REQUIRE_FALSE(bp.runs.empty());

    if (BidiAnalyzer::has_sheenbidi()) {
        REQUIRE(bp.base_level == 0);
        REQUIRE(bp.runs.size() >= 3);

        bool found_rtl_run = false;
        for (const auto& r : bp.runs) {
            if (r.is_rtl()) {
                found_rtl_run = true;
                REQUIRE(r.start >= static_cast<std::uint32_t>(kLatinHello.size()));
                REQUIRE(r.end()  <= static_cast<std::uint32_t>(kLatinHello.size() + kHebrewShalom.size()));
            }
        }
        REQUIRE(found_rtl_run);
    } else {
        SUCCEED("SheenBidi not linked — pass-through stub returns one LTR run");
    }
}

TEST_CASE("BidiAnalyzer respects forced base direction", "[bidi]") {
    // Forcing RTL on a Latin-only string still gives base_level = 1
    // even though all individual runs are LTR (level 0). This is the
    // UBA rule P3 surface — useful for an editor where the user picked
    // RTL paragraph direction.
    auto bp = BidiAnalyzer::analyze("Hello", BidiBaseDirection::RTL);
    REQUIRE_FALSE(bp.runs.empty());

    if (BidiAnalyzer::has_sheenbidi()) {
        REQUIRE(bp.base_level == 1);
        // Runs are still LTR (level 0 / 2) because the text is Latin.
        // The base level only affects display order at line ends, not
        // the level of pure-LTR text.
    } else {
        // Pass-through stub honours base direction directly.
        REQUIRE(bp.base_level == 1);
    }
}

TEST_CASE("BidiAnalyzer visual_order reverses RTL-only paragraph",
          "[bidi]") {
    // Three runs at level 1 (all RTL) — visual order should reverse
    // them so the rightmost logical run paints first.
    std::vector<BidiRun> runs = {
        {0,  4, 1},
        {4,  6, 1},
        {10, 2, 1},
    };
    auto visual = BidiAnalyzer::visual_order(runs);
    REQUIRE(visual.size() == 3);
    REQUIRE(visual[0].start == 10);
    REQUIRE(visual[1].start == 4);
    REQUIRE(visual[2].start == 0);
}

TEST_CASE("BidiAnalyzer visual_order leaves LTR runs alone",
          "[bidi]") {
    std::vector<BidiRun> runs = {
        {0,  4, 0},
        {4,  6, 0},
    };
    auto visual = BidiAnalyzer::visual_order(runs);
    REQUIRE(visual.size() == 2);
    REQUIRE(visual[0].start == 0);
    REQUIRE(visual[1].start == 4);
}

TEST_CASE("BidiAnalyzer visual_order reverses RTL island inside LTR",
          "[bidi]") {
    // Mixed Latin-Hebrew-Latin: logical order is L0, L1, L0; visual
    // order keeps L0 runs in place but the L1 run stays in the middle
    // (single RTL run reverses trivially to itself). The interesting
    // case is two adjacent RTL runs which should swap.
    std::vector<BidiRun> runs = {
        {0, 6, 0},     // "Hello "
        {6, 4, 1},     // RTL chunk A
        {10, 4, 1},    // RTL chunk B
        {14, 6, 0},    // " world"
    };
    auto visual = BidiAnalyzer::visual_order(runs);
    REQUIRE(visual.size() == 4);
    REQUIRE(visual[0].start == 0);   // "Hello " stays first
    REQUIRE(visual[1].start == 10);  // RTL chunk B paints before A
    REQUIRE(visual[2].start == 6);   // RTL chunk A
    REQUIRE(visual[3].start == 14);  // " world" stays last
}

TEST_CASE("TextRunPlanner emits multiple runs for mixed Latin+Arabic",
          "[bidi][planner]") {
    // Even on builds without Skia (non-ICU path), the new SheenBidi
    // fallback should let the planner emit more than one run for
    // mixed-direction text. Under PULP_HAS_SKIA the ICU path also
    // produces multiple runs.
    pulp::canvas::FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;

    const std::string mixed = concat3(kLatinHello, kArabicMarhaba, kLatinWorld);
    auto shaped = pulp::canvas::TextRunPlanner::instance().shape(mixed, opts);

    REQUIRE_FALSE(shaped.runs.empty());

    if (BidiAnalyzer::has_sheenbidi()) {
        REQUIRE(shaped.runs.size() >= 3);

        // At least one run should be RTL (odd bidi_level).
        bool found_rtl = false;
        for (const auto& r : shaped.runs) {
            if ((r.bidi_level & 1u) == 1u) {
                found_rtl = true;
                break;
            }
        }
        REQUIRE(found_rtl);
    } else {
        // Pass-through fallback collapses to one run; the planner
        // emits exactly one ShapedRun covering the whole input.
        // This branch only fires in unusual reduced-deps configs.
        SUCCEED("SheenBidi not linked — planner emits trivial single-run output");
    }
}
