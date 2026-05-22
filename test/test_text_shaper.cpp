#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/text_run_planner.hpp>
#include <pulp/canvas/text_layout.hpp>
#include <pulp/canvas/text_shaper.hpp>

using namespace pulp::canvas;
using Catch::Matchers::WithinAbs;

// ── Prepare ─────────────────────────────────────────────────────────────

TEST_CASE("TextShaper prepare empty string", "[canvas][text_shaper]") {
    TextShaper shaper;
    auto prepared = shaper.prepare("", "system", 14);
    REQUIRE(prepared.empty());
}

TEST_CASE("TextShaper prepare single word", "[canvas][text_shaper]") {
    TextShaper shaper;
    auto prepared = shaper.prepare("Hello", "system", 14);
    REQUIRE_FALSE(prepared.empty());
    REQUIRE(prepared.segments().size() == 1);
    REQUIRE(prepared.segments()[0].text == "Hello");
    REQUIRE(prepared.segments()[0].width > 0);
}

TEST_CASE("TextShaper prepare splits on spaces", "[canvas][text_shaper]") {
    TextShaper shaper;
    auto prepared = shaper.prepare("Hello World", "system", 14);
    // "Hello" + " " + "World" = 3 segments
    REQUIRE(prepared.segments().size() == 3);
    REQUIRE(prepared.segments()[0].text == "Hello");
    REQUIRE(prepared.segments()[1].is_whitespace);
    REQUIRE(prepared.segments()[2].text == "World");
}

TEST_CASE("TextShaper prepare handles newlines", "[canvas][text_shaper]") {
    TextShaper shaper;
    auto prepared = shaper.prepare("Line1\nLine2", "system", 14);
    // "Line1" + "\n" + "Line2"
    REQUIRE(prepared.segments().size() == 3);
    REQUIRE(prepared.segments()[1].is_newline);
}

TEST_CASE("TextShaper prepare caches measurements", "[canvas][text_shaper]") {
    TextShaper shaper;
    auto p1 = shaper.prepare("Hello World", "system", 14);
    auto p2 = shaper.prepare("Hello World", "system", 14);

    // Same segments, same widths (from cache)
    REQUIRE(p1.segments().size() == p2.segments().size());
    for (size_t i = 0; i < p1.segments().size(); ++i)
        REQUIRE(p1.segments()[i].width == p2.segments()[i].width);
}

// ── Layout (the cheap path — just arithmetic) ───────────────────────────

TEST_CASE("TextShaper layout single line", "[canvas][text_shaper]") {
    TextShaper shaper;
    auto prepared = shaper.prepare("Short", "system", 14);
    auto layout = shaper.layout(prepared, 1000.0f);

    REQUIRE(layout.line_count == 1);
    REQUIRE(layout.total_height > 0);
}

TEST_CASE("TextShaper layout wraps long text", "[canvas][text_shaper]") {
    TextShaper shaper;
    auto prepared = shaper.prepare(
        "This is a long sentence that should wrap to multiple lines", "system", 14);

    auto layout = shaper.layout(prepared, 100.0f);
    REQUIRE(layout.line_count > 1);
}

TEST_CASE("TextShaper layout respects newlines", "[canvas][text_shaper]") {
    TextShaper shaper;
    auto prepared = shaper.prepare("Line1\nLine2\nLine3", "system", 14);

    auto layout = shaper.layout(prepared, 1000.0f);
    REQUIRE(layout.line_count == 3);
}

TEST_CASE("TextShaper layout_with_lines materializes text", "[canvas][text_shaper]") {
    TextShaper shaper;
    auto prepared = shaper.prepare("Hello World", "system", 14);

    auto layout = shaper.layout_with_lines(prepared, 1000.0f);
    REQUIRE(layout.line_count == 1);
    REQUIRE(layout.lines[0].text.find("Hello") != std::string::npos);
}

// pulp #1410 — `white-space: nowrap` consumers pass max_lines=1 to
// force a single-line layout regardless of `max_width`. Otherwise the
// wrapping path fires before #1407's ellipsis truncation can engage.
TEST_CASE("TextShaper layout with max_lines=1 forces single-line layout",
          "[canvas][text_shaper][issue-1410]") {
    TextShaper shaper;
    auto prepared = shaper.prepare(
        "This is a long sentence that should wrap to multiple lines", "system", 14);

    auto wrapped = shaper.layout(prepared, 100.0f);
    REQUIRE(wrapped.line_count > 1);

    auto nowrap = shaper.layout(prepared, 100.0f, 0, /*max_lines=*/1);
    REQUIRE(nowrap.line_count == 1);
    REQUIRE(nowrap.total_width >= wrapped.lines.front().width);
}

TEST_CASE("TextShaper layout_with_lines max_lines=1 materializes the full string",
          "[canvas][text_shaper][issue-1410]") {
    TextShaper shaper;
    auto prepared = shaper.prepare("alpha beta gamma delta", "system", 14);

    auto nowrap = shaper.layout_with_lines(prepared, 50.0f, 0, /*max_lines=*/1);
    REQUIRE(nowrap.line_count == 1);
    REQUIRE(nowrap.lines[0].text.find("alpha") != std::string::npos);
    REQUIRE(nowrap.lines[0].text.find("beta") != std::string::npos);
    REQUIRE(nowrap.lines[0].text.find("gamma") != std::string::npos);
    REQUIRE(nowrap.lines[0].text.find("delta") != std::string::npos);
}

// pulp #1410 (Codex pre-merge sweep) — CSS `white-space: nowrap`
// collapses hard line breaks into a single space, both in the
// materialized line text and in the width sum. Otherwise an input
// like "alpha\nbeta" would measure as alpha + beta with no inter-word
// advance, mis-reporting the intrinsic width that #1407's ellipsis
// truncation needs to detect overflow.
TEST_CASE("TextShaper layout_with_lines max_lines=1 collapses hard breaks to spaces",
          "[canvas][text_shaper][issue-1410]") {
    TextShaper shaper;
    auto prepared = shaper.prepare("alpha\nbeta\ngamma", "system", 14);

    auto out = shaper.layout_with_lines(prepared, 1000.0f, 0, /*max_lines=*/1);
    REQUIRE(out.line_count == 1);
    // Materialized line replaces newlines with spaces (CSS nowrap rule).
    REQUIRE(out.lines[0].text.find('\n') == std::string::npos);
    REQUIRE(out.lines[0].text.find("alpha") != std::string::npos);
    REQUIRE(out.lines[0].text.find("beta") != std::string::npos);
    REQUIRE(out.lines[0].text.find("gamma") != std::string::npos);

    // Width must include some advance for each collapsed newline —
    // not strictly equal to a real space (segmentation differs between
    // "alpha\n" and "alpha "), but strictly greater than the no-space
    // baseline of just summing word widths.
    auto no_breaks = shaper.prepare("alphabetagamma", "system", 14);
    auto compact = shaper.layout(no_breaks, 1000.0f);
    REQUIRE(out.total_width > compact.lines[0].width);
}

TEST_CASE("TextShaper layout with max_lines=2 caps line count",
          "[canvas][text_shaper][issue-1410]") {
    TextShaper shaper;
    auto prepared = shaper.prepare(
        "alpha beta gamma delta epsilon zeta eta theta iota kappa", "system", 14);

    auto unbounded = shaper.layout(prepared, 50.0f);
    REQUIRE(unbounded.line_count > 2);

    auto capped = shaper.layout(prepared, 50.0f, 0, /*max_lines=*/2);
    REQUIRE(capped.line_count == 2);
}

TEST_CASE("TextShaper measure_height", "[canvas][text_shaper]") {
    TextShaper shaper;
    auto prepared = shaper.prepare("Line1\nLine2", "system", 14);

    float height = shaper.measure_height(prepared, 1000.0f, 20.0f);
    REQUIRE_THAT(height, WithinAbs(40.0f, 1.0f));  // 2 lines * 20px
}

TEST_CASE("TextShaper count_lines", "[canvas][text_shaper]") {
    TextShaper shaper;
    auto prepared = shaper.prepare("A\nB\nC", "system", 14);
    REQUIRE(shaper.count_lines(prepared, 1000.0f) == 3);
}

TEST_CASE("TextShaper reflow is cheap (same prepared, different width)", "[canvas][text_shaper]") {
    TextShaper shaper;
    auto prepared = shaper.prepare(
        "This text should reflow at different widths without re-shaping", "system", 14);

    auto wide = shaper.layout(prepared, 500.0f);
    auto narrow = shaper.layout(prepared, 100.0f);

    // Narrow layout should have more lines
    REQUIRE(narrow.line_count > wide.line_count);
    // But both use the same segments (no re-measurement)
}

TEST_CASE("TextShaper clear_cache", "[canvas][text_shaper]") {
    TextShaper shaper;
    shaper.prepare("test", "system", 14);
    shaper.clear_cache();
    // Should still work after clearing cache (just re-measures)
    auto p = shaper.prepare("test", "system", 14);
    REQUIRE_FALSE(p.empty());
}

TEST_CASE("TextShaper uses_real_shaping reports backend", "[canvas][text_shaper]") {
    TextShaper shaper;
    // In test builds without GPU, this returns false (estimation fallback)
    // In GPU builds with Skia, this returns true
    // Either way, the API works identically
    bool has_shaping = shaper.uses_real_shaping();
    (void)has_shaping;  // Don't assert — depends on build config
}

TEST_CASE("TextShaper attributed string", "[canvas][text_shaper]") {
    TextShaper shaper;
    AttributedString text;
    text.append("Bold ");
    text.append("Normal");

    auto prepared = shaper.prepare(text);
    REQUIRE_FALSE(prepared.empty());

    auto layout = shaper.layout(prepared, 1000.0f);
    REQUIRE(layout.line_count >= 1);
}

TEST_CASE("Global text shaper singleton", "[canvas][text_shaper]") {
    auto& s1 = global_text_shaper();
    auto& s2 = global_text_shaper();
    REQUIRE(&s1 == &s2);
}

// ── GlyphArrangement / paragraph layout helpers ────────────────────────

TEST_CASE("layout_paragraph handles empty text and newline-only text",
          "[canvas][text_layout]") {
    FontSpec font;
    font.size = 10.0f;

    auto empty = layout_paragraph("", font);
    REQUIRE(empty.line_count() == 0);
    REQUIRE_THAT(empty.total_width(), WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(empty.total_height(), WithinAbs(0.0f, 1e-5f));
    REQUIRE(empty.hit_test(5.0f, 5.0f) == 0);
    REQUIRE_THAT(empty.position_for_index(4), WithinAbs(0.0f, 1e-5f));

    auto newline = layout_paragraph("\n", font);
    REQUIRE(newline.line_count() == 1);
    REQUIRE(newline.lines()[0].glyphs.empty());
    REQUIRE_THAT(newline.lines()[0].width, WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(newline.total_height(), WithinAbs(10.0f, 1e-5f));
    REQUIRE(newline.hit_test(100.0f, 100.0f) == 0);
}

TEST_CASE("layout_paragraph applies letter spacing before wrapping",
          "[canvas][text_layout]") {
    FontSpec font;
    font.size = 10.0f;
    font.letter_spacing = 2.0f;

    auto unwrapped = layout_paragraph("abc", font);
    REQUIRE(unwrapped.line_count() == 1);
    REQUIRE(unwrapped.lines()[0].glyphs.size() == 3);
    REQUIRE_THAT(unwrapped.lines()[0].glyphs[0].advance, WithinAbs(8.0f, 1e-5f));
    REQUIRE_THAT(unwrapped.lines()[0].glyphs[1].x, WithinAbs(8.0f, 1e-5f));
    REQUIRE_THAT(unwrapped.total_width(), WithinAbs(24.0f, 1e-5f));

    auto wrapped = layout_paragraph("abc", font, 16.0f);
    REQUIRE(wrapped.line_count() == 2);
    REQUIRE(wrapped.lines()[0].glyphs.size() == 2);
    REQUIRE(wrapped.lines()[1].glyphs.size() == 1);
    REQUIRE_THAT(wrapped.lines()[0].width, WithinAbs(16.0f, 1e-5f));
    REQUIRE_THAT(wrapped.lines()[1].width, WithinAbs(8.0f, 1e-5f));
    REQUIRE_THAT(wrapped.total_width(), WithinAbs(16.0f, 1e-5f));
}

TEST_CASE("GlyphArrangement hit testing and index positioning cover line edges",
          "[canvas][text_layout]") {
    FontSpec font;
    font.size = 10.0f;

    auto layout = layout_paragraph("ab\ncd", font);
    REQUIRE(layout.line_count() == 2);

    REQUIRE(layout.hit_test(2.9f, 0.0f) == 0);
    REQUIRE(layout.hit_test(3.1f, 0.0f) == 1);
    REQUIRE(layout.hit_test(99.0f, 0.0f) == 2);
    REQUIRE(layout.hit_test(0.0f, 11.0f) == 3);
    REQUIRE(layout.hit_test(99.0f, 11.0f) == 5);

    REQUIRE_THAT(layout.position_for_index(1), WithinAbs(6.0f, 1e-5f));
    REQUIRE_THAT(layout.position_for_index(3), WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(layout.position_for_index(99), WithinAbs(12.0f, 1e-5f));
}

TEST_CASE("Parallelogram contains accepts interior and edge points",
          "[canvas][text_layout]") {
    auto para = Parallelogram::from_rect_shear(10.0f, 20.0f, 30.0f, 10.0f, 5.0f);

    REQUIRE(para.contains(20.0f, 25.0f));
    REQUIRE(para.contains(15.0f, 20.0f));
    REQUIRE_FALSE(para.contains(9.0f, 25.0f));
    REQUIRE_FALSE(para.contains(46.0f, 20.0f));
}

TEST_CASE("GlyphArrangement aggregates manual lines and clears layout state",
          "[canvas][text_layout][codecov]") {
    TextLine first;
    first.width = 20.0f;
    first.ascent = 6.0f;
    first.descent = 2.0f;
    first.baseline_y = 8.0f;
    first.glyphs.push_back({0.0f, 0.0f, 8.0f, 'a', 0});
    first.glyphs.push_back({8.0f, 0.0f, 12.0f, 'b', 1});

    TextLine second;
    second.width = 12.0f;
    second.ascent = 6.0f;
    second.descent = 4.0f;
    second.baseline_y = 22.0f;
    second.glyphs.push_back({0.0f, 0.0f, 6.0f, 'c', 2});
    second.glyphs.push_back({6.0f, 0.0f, 6.0f, 'd', 3});

    GlyphArrangement arrangement;
    arrangement.add_line(first);
    arrangement.add_line(second);

    REQUIRE(arrangement.line_count() == 2);
    REQUIRE(arrangement.lines().size() == 2);
    REQUIRE_THAT(arrangement.total_width(), WithinAbs(20.0f, 1e-5f));
    REQUIRE_THAT(arrangement.total_height(), WithinAbs(26.0f, 1e-5f));
    REQUIRE(arrangement.hit_test(1.0f, 1.0f) == 0);
    REQUIRE(arrangement.hit_test(11.0f, 1.0f) == 1);
    REQUIRE(arrangement.hit_test(99.0f, 1.0f) == 2);
    REQUIRE(arrangement.hit_test(4.0f, 17.0f) == 3);
    REQUIRE_THAT(arrangement.position_for_index(0), WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(arrangement.position_for_index(3), WithinAbs(6.0f, 1e-5f));
    REQUIRE_THAT(arrangement.position_for_index(99), WithinAbs(12.0f, 1e-5f));

    arrangement.clear();
    REQUIRE(arrangement.line_count() == 0);
    REQUIRE_THAT(arrangement.total_width(), WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(arrangement.total_height(), WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("layout_paragraph preserves hard breaks while wrapping later glyphs",
          "[canvas][text_layout][codecov]") {
    FontSpec font;
    font.size = 10.0f;

    auto layout = layout_paragraph("ab\ncde", font, 12.0f);
    REQUIRE(layout.line_count() == 3);

    REQUIRE(layout.lines()[0].glyphs[0].text_index == 0);
    REQUIRE(layout.lines()[0].glyphs[1].text_index == 1);
    REQUIRE_THAT(layout.lines()[0].width, WithinAbs(12.0f, 1e-5f));
    REQUIRE_THAT(layout.lines()[0].baseline_y, WithinAbs(8.0f, 1e-5f));

    REQUIRE(layout.lines()[1].glyphs[0].text_index == 3);
    REQUIRE(layout.lines()[1].glyphs[1].text_index == 4);
    REQUIRE_THAT(layout.lines()[1].width, WithinAbs(12.0f, 1e-5f));
    REQUIRE_THAT(layout.lines()[1].baseline_y, WithinAbs(18.0f, 1e-5f));

    REQUIRE(layout.lines()[2].glyphs.size() == 1);
    REQUIRE(layout.lines()[2].glyphs[0].text_index == 5);
    REQUIRE_THAT(layout.lines()[2].width, WithinAbs(6.0f, 1e-5f));
    REQUIRE_THAT(layout.lines()[2].baseline_y, WithinAbs(28.0f, 1e-5f));
    REQUIRE_THAT(layout.total_height(), WithinAbs(30.0f, 1e-5f));

    REQUIRE(layout.hit_test(2.0f, 27.0f) == 5);
    REQUIRE_THAT(layout.position_for_index(5), WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("layout_paragraph treats non-positive width as unbounded and narrow width as per-glyph wrap",
          "[canvas][text_layout][codecov]") {
    FontSpec font;
    font.size = 10.0f;

    auto zero_width = layout_paragraph("abc", font, 0.0f);
    REQUIRE(zero_width.line_count() == 1);
    REQUIRE_THAT(zero_width.total_width(), WithinAbs(18.0f, 1e-5f));

    auto negative_width = layout_paragraph("abc", font, -1.0f);
    REQUIRE(negative_width.line_count() == 1);
    REQUIRE_THAT(negative_width.total_width(), WithinAbs(18.0f, 1e-5f));

    auto narrow = layout_paragraph("abc", font, 1.0f);
    REQUIRE(narrow.line_count() == 3);
    REQUIRE(narrow.lines()[0].glyphs[0].text_index == 0);
    REQUIRE(narrow.lines()[1].glyphs[0].text_index == 1);
    REQUIRE(narrow.lines()[2].glyphs[0].text_index == 2);
    REQUIRE_THAT(narrow.lines()[0].width, WithinAbs(6.0f, 1e-5f));
    REQUIRE_THAT(narrow.lines()[1].width, WithinAbs(6.0f, 1e-5f));
    REQUIRE_THAT(narrow.lines()[2].width, WithinAbs(6.0f, 1e-5f));
}

TEST_CASE("Parallelogram contains accepts reverse winding and rejects exterior points",
          "[canvas][text_layout][codecov]") {
    Parallelogram clockwise{0.0f, 0.0f, 0.0f, 10.0f, 10.0f, 10.0f, 10.0f, 0.0f};

    REQUIRE(clockwise.contains(5.0f, 5.0f));
    REQUIRE(clockwise.contains(0.0f, 5.0f));
    REQUIRE(clockwise.contains(10.0f, 10.0f));
    REQUIRE_FALSE(clockwise.contains(-0.1f, 5.0f));
    REQUIRE_FALSE(clockwise.contains(5.0f, 10.1f));
    REQUIRE_FALSE(clockwise.contains(11.0f, 5.0f));
}

// ── pulp #1737 — CSS overflow-wrap / word-break BreakMode ───────────────
// PR-1 of 2 in the css/overflowWrap roadmap slice. PR-1 ships the
// TextShaper API for honoring break-word + anywhere break opportunities;
// PR-2 wires Label::paint to call this through View::word_break_ and
// flips the catalog. These tests anchor the contract for PR-2's
// integration step.
//
// Approach: proportional in-segment split (`seg.width / utf8_codepoints`
// per codepoint) at the codepoint boundary that fits before max_width.
// CSS Text Module Level 3 §6.1 does not require pixel-perfect break
// positions for soft-wrap — the contract is "do not overflow when a
// break opportunity exists." Re-shaping individual fragments would be
// more accurate but defeats PreText's measure-once-reflow-forever
// invariant. Browsers themselves use simplified heuristics for this case.

TEST_CASE("TextShaper BreakMode::normal preserves legacy whole-word overflow",
          "[canvas][text_shaper][issue-1737]") {
    TextShaper shaper;
    // A single long unbroken word wider than max_width must overflow on
    // one line under `normal` mode. This pins the legacy behavior so
    // the BreakMode plumbing is purely additive — existing consumers
    // see zero change at the API boundary.
    auto prepared = shaper.prepare("Supercalifragilisticexpialidocious", "system", 14);
    auto layout = shaper.layout(prepared, 30.0f, 0, 0, BreakMode::normal);
    REQUIRE(layout.line_count == 1);
    REQUIRE(layout.lines[0].width > 30.0f);
}

TEST_CASE("TextShaper BreakMode::break_word splits inside an over-wide word",
          "[canvas][text_shaper][issue-1737]") {
    TextShaper shaper;
    auto prepared = shaper.prepare("Supercalifragilisticexpialidocious", "system", 14);
    auto layout = shaper.layout_with_lines(prepared, 30.0f, 0, 0, BreakMode::break_word);
    // Must produce at least two lines (one would mean overflow, which
    // is exactly what break-word is supposed to prevent).
    REQUIRE(layout.line_count >= 2);
    // Each emitted line must be at most max_width wide (within the
    // proportional-split rounding tolerance — one codepoint of slop).
    for (const auto& line : layout.lines) {
        REQUIRE(line.width <= 30.0f + 5.0f);  // 5px slop = ~one wide char
    }
    // First line's text plus all subsequent lines' text must reconstruct
    // the original. The shaper materializes line text exactly; no chars
    // dropped, no chars duplicated.
    std::string reconstructed;
    for (const auto& line : layout.lines) reconstructed += line.text;
    REQUIRE(reconstructed == "Supercalifragilisticexpialidocious");
}

TEST_CASE("TextShaper BreakMode::break_word still prefers whitespace breaks",
          "[canvas][text_shaper][issue-1737]") {
    TextShaper shaper;
    // Probe text chosen so each individual word fits comfortably within
    // max_width — break-word only diverges from `normal` when a word is
    // ALSO over-wide (CSS Text Module §6.1: prefer the best break
    // opportunity; inside-word breaks only fire on actual overflow with
    // no whitespace break available).
    //
    // "ab cd ef gh" with 14px estimate: each 2-char word ≈ 17px. With
    // max_width=40, "ab cd" fits (~39px) then "ef gh" wraps to a new
    // line. NEITHER mode should split a word.
    auto prepared = shaper.prepare("ab cd ef gh", "system", 14);
    auto wide_layout = shaper.layout_with_lines(prepared, 1000.0f, 0, 0, BreakMode::break_word);
    REQUIRE(wide_layout.line_count == 1);

    auto narrow_layout = shaper.layout_with_lines(prepared, 40.0f, 0, 0, BreakMode::break_word);
    // The break_word verdict here MUST match `normal`'s verdict —
    // whitespace breaks only.
    auto narrow_normal = shaper.layout_with_lines(prepared, 40.0f, 0, 0, BreakMode::normal);
    REQUIRE(narrow_layout.line_count == narrow_normal.line_count);
    for (size_t k = 0; k < narrow_layout.lines.size(); ++k) {
        REQUIRE(narrow_layout.lines[k].text == narrow_normal.lines[k].text);
    }
}

TEST_CASE("TextShaper BreakMode::anywhere splits at codepoint boundaries",
          "[canvas][text_shaper][issue-1737]") {
    TextShaper shaper;
    // anywhere is a stricter version of break_word: the contract still
    // applies (don't overflow when a break opportunity exists), but
    // ALL codepoints are break candidates. In our implementation,
    // break_word and anywhere coincide on the overflow branch — this
    // test pins that they both round-trip the same way for an
    // over-wide single word.
    auto prepared = shaper.prepare("Antidisestablishmentarianism", "system", 14);
    auto layout = shaper.layout_with_lines(prepared, 25.0f, 0, 0, BreakMode::anywhere);
    REQUIRE(layout.line_count >= 2);
    std::string reconstructed;
    for (const auto& line : layout.lines) reconstructed += line.text;
    REQUIRE(reconstructed == "Antidisestablishmentarianism");
}

TEST_CASE("TextShaper BreakMode never breaks inside a UTF-8 codepoint",
          "[canvas][text_shaper][issue-1737]") {
    TextShaper shaper;
    // Mixed ASCII + multibyte UTF-8 (here: U+00E9 LATIN SMALL LETTER E
    // WITH ACUTE, encoded as 0xC3 0xA9 — 2 bytes, 1 codepoint).
    // Splitting inside a multibyte sequence would corrupt the output;
    // utf8_byte_offset_for_codepoints must always land on a codepoint
    // boundary. Verify by checking each emitted line's bytes are
    // valid UTF-8 (no orphan continuation bytes at start/end).
    // U+00E9 = 0xC3 0xA9. Use string-literal concatenation so each \x
    // escape doesn't greedily consume subsequent hex characters.
    auto prepared = shaper.prepare("aaaaa" "\xc3\xa9" "\xc3\xa9" "\xc3\xa9" "aaaaa",
                                    "system", 14);
    auto layout = shaper.layout_with_lines(prepared, 20.0f, 0, 0, BreakMode::break_word);
    for (const auto& line : layout.lines) {
        if (line.text.empty()) continue;
        // First byte must NOT be a continuation byte (0x80-0xBF)
        const unsigned char first = static_cast<unsigned char>(line.text.front());
        REQUIRE((first & 0xC0) != 0x80);
        // Last byte either ASCII (top bit 0) or a complete sequence —
        // walk the string and verify the final codepoint terminates
        // before end-of-string.
        size_t i = 0;
        while (i < line.text.size()) {
            unsigned char b = static_cast<unsigned char>(line.text[i]);
            int len = 1;
            if      ((b & 0x80) == 0)    len = 1;
            else if ((b & 0xE0) == 0xC0) len = 2;
            else if ((b & 0xF0) == 0xE0) len = 3;
            else if ((b & 0xF8) == 0xF0) len = 4;
            REQUIRE(i + len <= line.text.size());
            i += len;
        }
    }
}

TEST_CASE("TextShaper BreakMode::normal default-arg matches the no-arg overload",
          "[canvas][text_shaper][issue-1737]") {
    TextShaper shaper;
    auto prepared = shaper.prepare("hello world how are you", "system", 14);
    auto without_mode = shaper.layout(prepared, 50.0f);
    auto with_normal  = shaper.layout(prepared, 50.0f, 0, 0, BreakMode::normal);
    // The default value of BreakMode must be `normal`. If anyone changes
    // the default in the header, this test fires before any consumer
    // notices a behavior change.
    REQUIRE(without_mode.line_count == with_normal.line_count);
    for (size_t k = 0; k < without_mode.lines.size(); ++k) {
        REQUIRE_THAT(without_mode.lines[k].width,
                     WithinAbs(with_normal.lines[k].width, 0.001f));
    }
}

TEST_CASE("TextShaper BreakMode preserves remnant when the over-wide segment is followed by more text",
          "[canvas][text_shaper][issue-1737]") {
    TextShaper shaper;
    // Codex P1 on PR #1795: when break_word splits an over-wide segment
    // mid-segment AND that segment is NOT the last one, the remainder
    // characters were silently dropped. The fix emits the remnant as
    // its own line unconditionally so materialization downstream sees
    // every character. Probe: a long unbroken token, then whitespace,
    // then a normal word — verify reconstruction is bit-exact.
    auto prepared = shaper.prepare(
        "Antidisestablishmentarianism follows", "system", 14);
    auto layout = shaper.layout_with_lines(prepared, 30.0f, 0, 0, BreakMode::break_word);

    std::string reconstructed;
    for (const auto& line : layout.lines) reconstructed += line.text;
    // Whitespace handling: the legacy break-at-whitespace code path
    // skips the whitespace segment when emitting (line_start advances
    // past it), so the reconstructed string drops the inter-word space.
    // Accept the canonical "no spaces" reconstruction OR the spaced one
    // — what matters is that NO non-whitespace characters are lost.
    std::string canonical = "Antidisestablishmentarianismfollows";
    std::string with_space = "Antidisestablishmentarianism follows";
    bool ok = (reconstructed == canonical) || (reconstructed == with_space);
    INFO("reconstructed='" << reconstructed << "'");
    REQUIRE(ok);
}

TEST_CASE("TextRunPlanner emits UTF-8 scalar offsets and line breaks",
          "[canvas][text_run_planner][codecov]") {
    FontOptions options;
    options.family_stack = {"system"};
    options.size = 14.0f;

    auto& planner = TextRunPlanner::instance();
    planner.clear_cache();
    auto shaped = planner.shape("A " "\xc3\xa9" "\nB", options);

    REQUIRE_FALSE(shaped.empty());
    REQUIRE(shaped.text == "A " "\xc3\xa9" "\nB");
    REQUIRE(shaped.runs.size() == 1);
    REQUIRE(shaped.runs[0].logical_start == 0);
    REQUIRE(shaped.runs[0].logical_end == shaped.text.size());
    REQUIRE(shaped.runs[0].bidi_level == 0);
    REQUIRE(shaped.total_width >= 0.0f);

    REQUIRE(shaped.index_map.scalar_count() == 5);
    REQUIRE(shaped.index_map.scalar_offsets == std::vector<std::uint32_t>{0, 1, 2, 4, 5, 6});

    REQUIRE(shaped.line_breaks.size() == 2);
    REQUIRE(shaped.line_breaks[0].utf8_offset == 2);
    REQUIRE(shaped.line_breaks[0].kind == LineBreakOpportunity::Kind::Soft);
    REQUIRE(shaped.line_breaks[1].utf8_offset == 4);
    REQUIRE(shaped.line_breaks[1].kind == LineBreakOpportunity::Kind::Hard);
}

TEST_CASE("TextRunPlanner cache clear preserves shaped output contracts",
          "[canvas][text_run_planner][codecov]") {
    FontOptions options;
    options.family_stack = {"system"};
    options.size = 18.0f;
    options.direction = BaseDirection::RTL;

    auto& planner = TextRunPlanner::instance();
    planner.clear_cache();
    auto first = planner.shape("abc\tdef", options);
    auto cached = planner.shape("abc\tdef", options);
    planner.clear_cache();
    auto after_clear = planner.shape("abc\tdef", options);

    REQUIRE(first.text == cached.text);
    REQUIRE(first.text == after_clear.text);
    REQUIRE_FALSE(first.runs.empty());
    REQUIRE(cached.runs.size() == first.runs.size());
    REQUIRE(after_clear.runs.size() == first.runs.size());
    for (std::size_t i = 0; i < first.runs.size(); ++i) {
        REQUIRE(cached.runs[i].logical_start == first.runs[i].logical_start);
        REQUIRE(cached.runs[i].logical_end == first.runs[i].logical_end);
        REQUIRE(cached.runs[i].bidi_level == first.runs[i].bidi_level);
        REQUIRE(cached.runs[i].script_tag == first.runs[i].script_tag);
        REQUIRE(after_clear.runs[i].logical_start == first.runs[i].logical_start);
        REQUIRE(after_clear.runs[i].logical_end == first.runs[i].logical_end);
        REQUIRE(after_clear.runs[i].bidi_level == first.runs[i].bidi_level);
        REQUIRE(after_clear.runs[i].script_tag == first.runs[i].script_tag);
    }
    REQUIRE(first.line_breaks.size() == 1);
    REQUIRE(first.line_breaks[0].utf8_offset == 4);
    REQUIRE(first.line_breaks[0].kind == LineBreakOpportunity::Kind::Soft);
    REQUIRE(first.index_map.scalar_offsets == after_clear.index_map.scalar_offsets);
    REQUIRE_THAT(first.total_width, WithinAbs(after_clear.total_width, 0.001f));
}
