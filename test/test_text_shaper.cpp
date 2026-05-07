#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
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
