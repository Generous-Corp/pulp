#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/attributed_string.hpp>
#include <pulp/canvas/text_layout.hpp>

using namespace pulp::canvas;
using Catch::Matchers::WithinAbs;

// ── AttributedString basics ─────────────────────────────────────────────

TEST_CASE("AttributedString empty", "[canvas][text]") {
    AttributedString str;
    REQUIRE(str.empty());
    REQUIRE(str.length() == 0);
}

TEST_CASE("AttributedString append text", "[canvas][text]") {
    AttributedString str;
    str.append("Hello");
    str.append(" World");

    REQUIRE(str.plain_text() == "Hello World");
    REQUIRE(str.length() == 11);
    REQUIRE(str.spans().size() == 2);
}

TEST_CASE("AttributedString from string", "[canvas][text]") {
    AttributedString str("Direct text");
    REQUIRE(str.plain_text() == "Direct text");
    REQUIRE(str.spans().size() == 1);
}

TEST_CASE("AttributedString styled spans", "[canvas][text]") {
    AttributedString str;
    str.append("normal text");
    str.append("red text", Color::rgba(255, 0, 0));
    str.append("big text", 24.0f, Color::rgba(0, 255, 0));

    REQUIRE(str.spans().size() == 3);
    REQUIRE(str.spans()[1].color.r == 255);
    REQUIRE(str.spans()[2].font_size == 24.0f);
}

TEST_CASE("AttributedString append full span preserves style fields", "[canvas][text]") {
    AttributedString str;

    TextSpan span;
    span.text = "styled";
    span.font_family = "serif";
    span.font_size = 19.0f;
    span.font_weight = 700;
    span.italic = true;
    span.color = Color::rgba(10, 20, 30);
    span.decoration = TextDecoration::underline;
    span.decoration_color = Color::rgba(40, 50, 60);
    span.letter_spacing = 1.5f;

    str.append(span);

    REQUIRE(str.plain_text() == "styled");
    REQUIRE(str.length() == 6);
    REQUIRE(str.spans().size() == 1);
    REQUIRE(str.spans()[0].font_family == "serif");
    REQUIRE(str.spans()[0].font_size == 19.0f);
    REQUIRE(str.spans()[0].font_weight == 700);
    REQUIRE(str.spans()[0].italic);
    REQUIRE(str.spans()[0].color.r == 10);
    REQUIRE(str.spans()[0].color.g == 20);
    REQUIRE(str.spans()[0].color.b == 30);
    REQUIRE(str.spans()[0].decoration == TextDecoration::underline);
    REQUIRE(str.spans()[0].decoration_color.r == 40);
    REQUIRE(str.spans()[0].decoration_color.g == 50);
    REQUIRE(str.spans()[0].decoration_color.b == 60);
    REQUIRE(str.spans()[0].letter_spacing == 1.5f);
}

TEST_CASE("AttributedString set_font applies to all spans", "[canvas][text]") {
    AttributedString str;
    str.append("a");
    str.append("b");
    str.set_font("monospace", 18.0f);

    for (auto& span : str.spans()) {
        REQUIRE(span.font_family == "monospace");
        REQUIRE(span.font_size == 18.0f);
    }
}

TEST_CASE("AttributedString clear", "[canvas][text]") {
    AttributedString str("some text");
    str.clear();
    REQUIRE(str.empty());
}

TEST_CASE("AttributedString set_color applies to all spans", "[canvas][text]") {
    AttributedString str;
    str.append("a");
    str.append("b", Color::rgba(1, 2, 3));

    str.set_color(Color::rgba(11, 22, 33));

    for (auto& span : str.spans()) {
        REQUIRE(span.color.r == 11);
        REQUIRE(span.color.g == 22);
        REQUIRE(span.color.b == 33);
    }
}

TEST_CASE("AttributedString empty span has zero text length but remains a span", "[canvas][text]") {
    AttributedString str;
    str.append("");

    REQUIRE_FALSE(str.empty());
    REQUIRE(str.length() == 0);
    REQUIRE(str.plain_text().empty());
    REQUIRE(str.spans().size() == 1);
}

// ── TextLayout word wrapping ────────────────────────────────────────────

TEST_CASE("TextLayout empty string", "[canvas][layout]") {
    AttributedString str;
    auto layout = layout_attributed_string(str, 200.0f);
    REQUIRE(layout.lines.empty());
    REQUIRE(layout.total_height == 0);
}

TEST_CASE("TextLayout rejects non-positive max width", "[canvas][layout]") {
    AttributedString str("text");

    auto zero_width = layout_attributed_string(str, 0.0f);
    auto negative_width = layout_attributed_string(str, -1.0f);

    REQUIRE(zero_width.lines.empty());
    REQUIRE(zero_width.total_width == 0);
    REQUIRE(zero_width.total_height == 0);
    REQUIRE(negative_width.lines.empty());
    REQUIRE(negative_width.total_width == 0);
    REQUIRE(negative_width.total_height == 0);
}

TEST_CASE("TextLayout single short line", "[canvas][layout]") {
    AttributedString str("Short");
    auto layout = layout_attributed_string(str, 200.0f);

    REQUIRE(layout.lines.size() == 1);
    REQUIRE(layout.lines[0].spans.size() == 1);
    REQUIRE(layout.lines[0].spans[0].text == "Short");
    REQUIRE(layout.total_height > 0);
}

TEST_CASE("TextLayout wraps long text", "[canvas][layout]") {
    // With default 14pt font, char width ~8.4px
    // 200px / 8.4 ~= 23 chars per line
    AttributedString str("This is a sentence that should definitely wrap to multiple lines because it is quite long");
    auto layout = layout_attributed_string(str, 200.0f);

    REQUIRE(layout.lines.size() > 1);
    // Each line should not exceed max_width
    for (auto& line : layout.lines) {
        REQUIRE(line.width <= 210.0f);  // Slight tolerance
    }
}

TEST_CASE("TextLayout width boundary stays on one line when text fits", "[canvas][layout]") {
    AttributedString str("abcde");
    auto layout = layout_attributed_string(str, 43.0f);  // 5 chars at default 14pt

    REQUIRE(layout.lines.size() == 1);
    REQUIRE(layout.lines[0].spans[0].text == "abcde");
    REQUIRE_THAT(layout.lines[0].width, WithinAbs(42.0f, 1e-5));
    REQUIRE_THAT(layout.total_width, WithinAbs(42.0f, 1e-5));
}

TEST_CASE("TextLayout below single character width forces one character rows", "[canvas][layout]") {
    AttributedString str("abc");
    auto layout = layout_attributed_string(str, 0.5f);

    REQUIRE(layout.lines.size() == 3);
    REQUIRE(layout.lines[0].spans[0].text == "a");
    REQUIRE(layout.lines[1].spans[0].text == "b");
    REQUIRE(layout.lines[2].spans[0].text == "c");
}

TEST_CASE("TextLayout uses tab as word break delimiter", "[canvas][layout]") {
    AttributedString str("alpha\tbeta");
    auto layout = layout_attributed_string(str, 50.0f);

    REQUIRE(layout.lines.size() == 2);
    REQUIRE(layout.lines[0].spans[0].text == "alpha");
    REQUIRE(layout.lines[1].spans[0].text == "beta");
}

TEST_CASE("TextLayout respects newlines", "[canvas][layout]") {
    AttributedString str("Line 1\nLine 2\nLine 3");
    auto layout = layout_attributed_string(str, 1000.0f);  // Wide enough for no wrapping

    REQUIRE(layout.lines.size() == 3);
    REQUIRE(layout.lines[0].spans[0].text == "Line 1");
    REQUIRE(layout.lines[1].spans[0].text == "Line 2");
    REQUIRE(layout.lines[2].spans[0].text == "Line 3");
}

TEST_CASE("TextLayout line height", "[canvas][layout]") {
    AttributedString str("Line 1\nLine 2");
    auto layout = layout_attributed_string(str, 1000.0f, 30.0f);

    REQUIRE(layout.lines.size() == 2);
    REQUIRE_THAT(layout.lines[0].height, WithinAbs(30.0f, 1e-5));
    REQUIRE_THAT(layout.total_height, WithinAbs(60.0f, 1e-5));
}

TEST_CASE("TextLayout empty span creates blank line", "[canvas][layout]") {
    AttributedString str;
    str.append("");

    auto layout = layout_attributed_string(str, 100.0f, 12.0f);

    REQUIRE(layout.lines.size() == 1);
    REQUIRE(layout.lines[0].spans.empty());
    REQUIRE(layout.lines[0].width == 0);
    REQUIRE_THAT(layout.lines[0].height, WithinAbs(12.0f, 1e-5));
    REQUIRE_THAT(layout.total_height, WithinAbs(12.0f, 1e-5));
}

TEST_CASE("TextLayout narrow width forces word break", "[canvas][layout]") {
    AttributedString str("Supercalifragilisticexpialidocious");
    auto layout = layout_attributed_string(str, 50.0f);  // Very narrow

    REQUIRE(layout.lines.size() > 1);  // Must break
}

TEST_CASE("TextLayout multiple spans", "[canvas][layout]") {
    AttributedString str;
    str.append("First span ");
    str.append("Second span");

    auto layout = layout_attributed_string(str, 1000.0f);

    // Two spans become two layout lines (each span on its own line in current impl)
    REQUIRE(layout.lines.size() == 2);
}

TEST_CASE("GlyphArrangement hit testing and cursor positions", "[canvas][layout]") {
    FontSpec font;
    font.size = 10.0f;
    font.letter_spacing = 1.0f;

    auto layout = layout_paragraph("abcd", font, 15.0f);

    REQUIRE(layout.line_count() == 2);
    REQUIRE(layout.lines()[0].glyphs.size() == 2);
    REQUIRE(layout.lines()[1].glyphs.size() == 2);
    REQUIRE_THAT(layout.lines()[0].width, WithinAbs(14.0f, 1e-5f));
    REQUIRE_THAT(layout.lines()[1].width, WithinAbs(14.0f, 1e-5f));

    REQUIRE(layout.hit_test(0.0f, 0.0f) == 0);
    REQUIRE(layout.hit_test(4.0f, 0.0f) == 1);
    REQUIRE(layout.hit_test(0.0f, 12.0f) == 2);
    REQUIRE(layout.hit_test(100.0f, 12.0f) == 4);

    REQUIRE_THAT(layout.position_for_index(1), WithinAbs(7.0f, 1e-5f));
    REQUIRE_THAT(layout.position_for_index(99), WithinAbs(14.0f, 1e-5f));

    layout.clear();
    REQUIRE(layout.line_count() == 0);
    REQUIRE(layout.hit_test(0.0f, 0.0f) == 0);
    REQUIRE_THAT(layout.position_for_index(0), WithinAbs(0.0f, 1e-5f));
}
