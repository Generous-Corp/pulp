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
