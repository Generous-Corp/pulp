// SPDX-License-Identifier: MIT
//
// pulp #1407 — focused unit tests for the truncate_to_width helper that
// translates CSS `text-overflow: ellipsis` into a UTF-8-safe truncated
// string. Pairs with the integration tests in test_widgets.cpp and
// test_gui_components.cpp that drive Label/TextButton paint paths.

#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/text_overflow.hpp>

using namespace pulp::canvas;
using namespace pulp::view;

// RecordingCanvas::measure_text returns 7 px per byte. ASCII chars are
// 1 byte each, so plain ASCII strings cost 7 px per character. We reuse
// pulp::view::kEllipsis (UTF-8 0xE2 0x80 0xA6) from the helper header.

TEST_CASE("truncate_to_width returns input verbatim when it already fits",
          "[view][text-overflow][issue-1407]") {
    RecordingCanvas canvas;
    REQUIRE(truncate_to_width(canvas, "ok", 100.0f) == "ok");
    REQUIRE(truncate_to_width(canvas, "", 100.0f) == "");
}

TEST_CASE("truncate_to_width appends U+2026 when text overflows",
          "[view][text-overflow][issue-1407]") {
    RecordingCanvas canvas;
    auto out = truncate_to_width(canvas, "Mid-band attenuation with high-shelf compensation", 100.0f);
    REQUIRE(out.size() >= 3);
    REQUIRE(out.compare(out.size() - 3, 3, kEllipsis) == 0);
    REQUIRE(canvas.measure_text(out) <= 100.0f);
    // Should not be the unmodified input.
    REQUIRE(out != "Mid-band attenuation with high-shelf compensation");
}

TEST_CASE("truncate_to_width never splits a multibyte UTF-8 codepoint",
          "[view][text-overflow][issue-1407]") {
    RecordingCanvas canvas;
    // "ñoño" has two 2-byte codepoints (ñ = 0xC3 0xB1) plus two 1-byte
    // codepoints, total 6 bytes. Mix with ASCII so the truncation point
    // lands inside a multibyte sequence if the helper were buggy.
    std::string input = "\xc3\xb1o\xc3\xb1o is fine";  // "ñoño is fine"
    auto out = truncate_to_width(canvas, input, 35.0f);  // ~5 ASCII chars + ellipsis
    REQUIRE(out.size() >= 3);
    REQUIRE(out.compare(out.size() - 3, 3, kEllipsis) == 0);
    // Walk the UTF-8 leading bytes and ensure no continuation byte is a
    // standalone trailing byte before the ellipsis (i.e. no split).
    std::size_t i = 0;
    while (i + 3 < out.size()) {
        unsigned char c = static_cast<unsigned char>(out[i]);
        std::size_t step = 1;
        if (c < 0x80)      step = 1;
        else if (c < 0xC0) {  // stray continuation byte → bug
            FAIL("truncated string contains stray UTF-8 continuation byte");
        } else if (c < 0xE0) step = 2;
        else if (c < 0xF0) step = 3;
        else                step = 4;
        REQUIRE(i + step <= out.size() - 3);  // doesn't split into the ellipsis
        i += step;
    }
}

TEST_CASE("truncate_to_width returns lone ellipsis when ellipsis itself doesn't fit",
          "[view][text-overflow][issue-1407]") {
    RecordingCanvas canvas;
    // RecordingCanvas measures the 3-byte ellipsis at 21 px. With
    // available_width = 5 px, even the ellipsis alone overflows; the
    // helper still returns "…" so the widget can clip via its bounds.
    auto out = truncate_to_width(canvas, "abcdef", 5.0f);
    REQUIRE(out == kEllipsis);
}

TEST_CASE("truncate_to_width returns ellipsis when available_width is non-positive",
          "[view][text-overflow][issue-1407]") {
    // Codex post-merge sweep: a TextButton with width <= 16 px passes 0
    // (or negative) here after subtracting its 8 px horizontal padding.
    // Returning the original string would visibly leak the unwrapped
    // label past the button's rounded background — exactly the symptom
    // pulp #1407 is meant to eliminate.
    RecordingCanvas canvas;
    REQUIRE(truncate_to_width(canvas, "Mid-band attenuation", 0.0f) == kEllipsis);
    REQUIRE(truncate_to_width(canvas, "anything", -3.0f) == kEllipsis);
}

TEST_CASE("utf8 helpers count and advance over leading bytes correctly",
          "[view][text-overflow][issue-1407]") {
    REQUIRE(utf8_codepoint_count("hello") == 5);
    REQUIRE(utf8_codepoint_count("\xc3\xb1o") == 2);  // "ño"
    REQUIRE(utf8_codepoint_count("") == 0);

    REQUIRE(utf8_advance("hello", 3) == 3);
    REQUIRE(utf8_advance("\xc3\xb1o", 1) == 2);  // skip past 2-byte ñ
    REQUIRE(utf8_advance("hello", 99) == 5);     // clamps to size
}

TEST_CASE("utf8 helpers skip stray continuation bytes without spending codepoint budget",
          "[view][text-overflow][issue-1407][coverage][phase3]") {
    const std::string leading = std::string("\x80", 1) + "abc";
    const std::string doubled = std::string("\x80\x81", 2) + "abc";
    const std::string middle = std::string("a") + std::string("\x80", 1) + "bc";
    const std::string trailing = std::string("abc") + std::string("\x80", 1);
    const std::string only = std::string("\x80\x81\x82", 3);
    const std::string mixed = std::string("\x80", 1) + "\xc3\xb1" +
                              std::string("\x81", 1) + "o";

    REQUIRE(utf8_codepoint_count(leading) == 3);
    REQUIRE(utf8_codepoint_count(doubled) == 3);
    REQUIRE(utf8_codepoint_count(middle) == 3);
    REQUIRE(utf8_codepoint_count(trailing) == 3);
    REQUIRE(utf8_codepoint_count(only) == 0);
    REQUIRE(utf8_codepoint_count(mixed) == 2);

    REQUIRE(utf8_advance(leading, 0) == 0);
    REQUIRE(utf8_advance(leading, 1) == 2);
    REQUIRE(utf8_advance(leading, 2) == 3);
    REQUIRE(utf8_advance(leading, 3) == 4);
    REQUIRE(utf8_advance(leading, 4) == 4);

    REQUIRE(utf8_advance(doubled, 1) == 3);
    REQUIRE(utf8_advance(doubled, 2) == 4);
    REQUIRE(utf8_advance(doubled, 3) == 5);
    REQUIRE(utf8_advance(doubled, 99) == 5);

    REQUIRE(utf8_advance(middle, 1) == 1);
    REQUIRE(utf8_advance(middle, 2) == 3);
    REQUIRE(utf8_advance(middle, 3) == 4);
    REQUIRE(utf8_advance(middle, 99) == 4);

    REQUIRE(utf8_advance(trailing, 1) == 1);
    REQUIRE(utf8_advance(trailing, 2) == 2);
    REQUIRE(utf8_advance(trailing, 3) == 3);
    REQUIRE(utf8_advance(trailing, 4) == 4);

    REQUIRE(utf8_advance(only, 1) == 3);
    REQUIRE(utf8_advance(only, 99) == 3);

    REQUIRE(utf8_advance(mixed, 1) == 3);
    REQUIRE(utf8_advance(mixed, 2) == 5);
    REQUIRE(utf8_advance(mixed, 3) == 5);
}

TEST_CASE("truncate_to_width ignores stray continuation bytes when choosing prefix",
          "[view][text-overflow][issue-1407][coverage][phase3]") {
    RecordingCanvas canvas;
    const std::string leading = std::string("\x80", 1) + "abcdef";
    const std::string doubled = std::string("\x80\x81", 2) + "abcdef";

    auto leading_out = truncate_to_width(canvas, leading, 49.0f);
    REQUIRE(leading_out.compare(leading_out.size() - 3, 3, kEllipsis) == 0);
    REQUIRE(leading_out.find('\x80') != std::string::npos);
    REQUIRE(leading_out.find("abc") != std::string::npos);
    REQUIRE(leading_out.find("def") == std::string::npos);
    REQUIRE(canvas.measure_text(leading_out) <= 49.0f);

    auto doubled_out = truncate_to_width(canvas, doubled, 56.0f);
    REQUIRE(doubled_out.compare(doubled_out.size() - 3, 3, kEllipsis) == 0);
    REQUIRE(doubled_out.find('\x80') != std::string::npos);
    REQUIRE(doubled_out.find('\x81') != std::string::npos);
    REQUIRE(doubled_out.find("abc") != std::string::npos);
    REQUIRE(doubled_out.find("def") == std::string::npos);
    REQUIRE(canvas.measure_text(doubled_out) <= 56.0f);
}

TEST_CASE("utf8 helpers handle continuation three-byte and four-byte paths",
          "[view][text-overflow][coverage][phase3]") {
    const std::string stray_continuation("\x80" "abc", 4);
    REQUIRE(utf8_codepoint_count(stray_continuation) == 3);
    REQUIRE(utf8_advance(stray_continuation, 1) == 2);
    REQUIRE(utf8_advance(stray_continuation, 2) == 3);

    const std::string euro_then_a("\xe2\x82\xac" "a", 4);
    REQUIRE(utf8_codepoint_count(euro_then_a) == 2);
    REQUIRE(utf8_advance(euro_then_a, 1) == 3);
    REQUIRE(utf8_advance(euro_then_a, 2) == 4);

    const std::string emoji_then_z("\xf0\x9f\x98\x80" "z", 5);
    REQUIRE(utf8_codepoint_count(emoji_then_z) == 2);
    REQUIRE(utf8_advance(emoji_then_z, 1) == 4);
    REQUIRE(utf8_advance(emoji_then_z, 2) == 5);
}

TEST_CASE("utf8 helpers clamp truncated multibyte sequences",
          "[view][text-overflow][coverage][phase3]") {
    const std::string truncated_two_byte("\xc3", 1);
    REQUIRE(utf8_codepoint_count(truncated_two_byte) == 1);
    REQUIRE(utf8_advance(truncated_two_byte, 1) == 1);
    REQUIRE(utf8_advance(truncated_two_byte, 9) == 1);

    const std::string truncated_three_byte("\xe2\x82", 2);
    REQUIRE(utf8_codepoint_count(truncated_three_byte) == 1);
    REQUIRE(utf8_advance(truncated_three_byte, 1) == 2);

    const std::string truncated_four_byte("\xf0\x9f\x98", 3);
    REQUIRE(utf8_codepoint_count(truncated_four_byte) == 1);
    REQUIRE(utf8_advance(truncated_four_byte, 1) == 3);
}

TEST_CASE("truncate_to_width respects exact text and ellipsis boundaries",
          "[view][text-overflow][coverage][phase3]") {
    RecordingCanvas canvas;

    REQUIRE(truncate_to_width(canvas, "abc", 21.0f) == "abc");
    REQUIRE(truncate_to_width(canvas, "abc", 20.0f) == kEllipsis);
    REQUIRE(truncate_to_width(canvas, "abc", 21.0f).size() == 3);

    const auto one_char = truncate_to_width(canvas, "abcde", 28.0f);
    REQUIRE(one_char == std::string("a") + kEllipsis);
    REQUIRE(canvas.measure_text(one_char) == 28.0f);

    const auto two_chars = truncate_to_width(canvas, "abcdef", 35.0f);
    REQUIRE(two_chars == std::string("ab") + kEllipsis);
    REQUIRE(canvas.measure_text(two_chars) == 35.0f);
}

TEST_CASE("truncate_to_width handles empty text in collapsed boxes",
          "[view][text-overflow][coverage][phase3]") {
    RecordingCanvas canvas;

    REQUIRE(truncate_to_width(canvas, "", 0.0f) == kEllipsis);
    REQUIRE(truncate_to_width(canvas, "", -1.0f) == kEllipsis);
    REQUIRE(truncate_to_width(canvas, "", 1.0f) == "");
    REQUIRE(truncate_to_width(canvas, "", 21.0f) == "");
}

TEST_CASE("truncate_to_width preserves full multibyte prefixes",
          "[view][text-overflow][coverage][phase3]") {
    RecordingCanvas canvas;

    const std::string input = "\xe2\x82\xac" "\xf0\x9f\x98\x80" "abcd";
    const auto euro_only = truncate_to_width(canvas, input, 42.0f);
    REQUIRE(euro_only == std::string("\xe2\x82\xac") + kEllipsis);
    REQUIRE(canvas.measure_text(euro_only) == 42.0f);

    const auto euro_and_emoji = truncate_to_width(canvas, input, 70.0f);
    REQUIRE(euro_and_emoji == std::string("\xe2\x82\xac" "\xf0\x9f\x98\x80") + kEllipsis);
    REQUIRE(canvas.measure_text(euro_and_emoji) == 70.0f);

    REQUIRE(utf8_codepoint_count(input) == 6);
    REQUIRE(utf8_advance(input, 2) == 7);
}

TEST_CASE("truncate_to_width keeps exact multibyte fits unmodified",
          "[view][text-overflow][coverage][phase3]") {
    RecordingCanvas canvas;

    const std::string text = "\xe2\x82\xac" "\xf0\x9f\x98\x80";
    REQUIRE(canvas.measure_text(text) == 49.0f);
    REQUIRE(truncate_to_width(canvas, text, 49.0f) == text);
    REQUIRE(truncate_to_width(canvas, text, 48.0f) == std::string("\xe2\x82\xac") + kEllipsis);
    REQUIRE(utf8_codepoint_count(text) == 2);
    REQUIRE(utf8_advance(text, 99) == text.size());
}
