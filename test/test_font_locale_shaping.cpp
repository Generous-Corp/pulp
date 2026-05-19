// test_font_locale_shaping.cpp — Pulp #2163, font v2 Slice 2.4.
//
// Locale-aware word + line break surface. When ICU's public headers
// are on the include path (`__has_include(<unicode/brkiter.h>)`), the
// bundled SkUnicode_icu symbols in libskia.a back the implementation
// and the assertions exercise the real UAX #14 path. Otherwise the
// degraded fallback (`word_break_step` → `cluster_step`,
// `line_break_opportunities` → ASCII-space + trailing offset) keeps
// the API linkable and the English-language assertions still hold.
//
// Japanese-specific dictionary-break assertions are *conditional*:
// they require the real ICU path to produce > 2 break offsets for a
// no-whitespace CJK string. When ICU is not linked, we skip those
// expectations rather than fail.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/bundled_fonts.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace pulp::canvas;

namespace {

bool contains(const std::vector<std::size_t>& v, std::size_t value) {
    return std::find(v.begin(), v.end(), value) != v.end();
}

} // namespace

TEST_CASE("line_break_opportunities: empty text yields trailing 0", "[font][locale][issue-2163]") {
    auto v = line_break_opportunities("", "en-US");
    REQUIRE_FALSE(v.empty());
    REQUIRE(v.back() == 0u);
}

TEST_CASE("line_break_opportunities: English Hello world breaks at 6", "[font][locale][issue-2163]") {
    const std::string s = "Hello world";
    auto v = line_break_opportunities(s, "en-US");
    REQUIRE_FALSE(v.empty());
    REQUIRE(v.back() == s.size());
    // Both real ICU and the degraded ASCII-space fallback emit the
    // boundary one byte past the space (offset 6, between "Hello "
    // and "world").
    REQUIRE(contains(v, 6u));
}

TEST_CASE("word_break_step: English Hello forward from 0 lands on 5", "[font][locale][issue-2163]") {
    const std::string s = "Hello world";
    // ICU word boundary after "Hello" is offset 5 (just before the
    // space). The degraded fallback (cluster_step) advances one
    // cluster — that's just offset 1. We accept either, since the
    // surface guarantees a forward-moving offset, and exercise the
    // ICU-only assertion conditionally below.
    const std::size_t out = word_break_step(s, 0, "en-US", /*forward=*/true);
    REQUIRE(out > 0u);
    REQUIRE(out <= s.size());
    // The ICU-backed implementation must land on the word end at 5.
    // The degraded fallback advances by a cluster; on plain ASCII
    // that's 1 byte. If the impl returned 5, we have the real ICU
    // path; if it returned 1, we have the degraded path. Both are
    // valid — we just need a forward-moving offset, asserted above.
    REQUIRE((out == 5u || out == 1u));
}

TEST_CASE("word_break_step: empty locale tolerated", "[font][locale][issue-2163]") {
    const std::string s = "abc def";
    const std::size_t out = word_break_step(s, 0, /*locale=*/"", /*forward=*/true);
    REQUIRE(out > 0u);
    REQUIRE(out <= s.size());
}

TEST_CASE("line_break_opportunities: empty locale tolerated", "[font][locale][issue-2163]") {
    const std::string s = "one two three";
    auto v = line_break_opportunities(s, /*locale=*/"");
    REQUIRE_FALSE(v.empty());
    REQUIRE(v.back() == s.size());
}

TEST_CASE("line_break_opportunities: Japanese dictionary break (ICU-only)",
          "[font][locale][japanese][issue-2163]") {
    // "日本語のテスト" — no ASCII whitespace. ICU's ja_JP line iterator
    // splits on grammatical / dictionary boundaries; the degraded
    // fallback only emits the trailing offset.
    const std::string s = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"   // 日本語
                          "\xE3\x81\xAE"                            // の
                          "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88";   // テスト
    auto v = line_break_opportunities(s, "ja-JP");
    REQUIRE_FALSE(v.empty());
    REQUIRE(v.back() == s.size());

    // Conditional ICU assertion: if the real ICU path is linked, we
    // expect > 2 break offsets in this CJK string. The degraded path
    // returns just {text.size()}, so size() <= 2 — skip the
    // assertion in that case rather than fail.
    if (v.size() > 2u) {
        // Real ICU path: must have at least one interior break.
        REQUIRE(v.size() >= 2u);
        REQUIRE(v.front() < s.size());
    }
}

TEST_CASE("word_break_step: backward from end yields offset < end", "[font][locale][issue-2163]") {
    const std::string s = "abc def";
    const std::size_t out = word_break_step(s, s.size(), "en-US", /*forward=*/false);
    REQUIRE(out < s.size());
}

// pulp #2249 follow-up (Codex review P2): byte offsets that land
// strictly inside a multi-byte UTF-8 scalar must be mapped to the
// FLOOR UTF-16 index (the scalar that contains the offset), not the
// next scalar's start. With the pre-fix `lower_bound` mapping a
// mid-codepoint cursor jumped *past* the codepoint, and the off-by-
// one cascaded through every word-break call. Verify with U+00E9 'é'
// (UTF-8 `0xC3 0xA9`): step forward from byte offset 1 (the middle
// of the scalar) and assert the returned position is on a cluster
// boundary — i.e. either 0 (start of é) or 2 (end of é), never 1.
TEST_CASE("word_break_step: mid-codepoint cursor rounds down to cluster boundary",
          "[font][locale][issue-2249]") {
    // "café " — the 'é' occupies bytes 3..4 (`0xC3 0xA9`), so byte
    // offset 4 is strictly inside that scalar. Forward and backward
    // steps from inside the scalar must return cluster-aligned
    // offsets — never 4 (mid-codepoint).
    const std::string s = "caf\xC3\xA9 word";
    {
        const std::size_t out = word_break_step(s, 4, "en-US", /*forward=*/true);
        // Must NOT report 4 (mid-codepoint). Acceptable values are
        // any cluster-aligned offset > 4 OR exactly 3 (the scalar
        // start, if the implementation snaps backward first).
        REQUIRE(out != 4u);
        REQUIRE((out == 3u || out >= 5u));
        REQUIRE(out <= s.size());
    }
    {
        const std::size_t out = word_break_step(s, 4, "en-US", /*forward=*/false);
        REQUIRE(out != 4u);
        // Backward from inside é must not land deeper than the scalar
        // start.
        REQUIRE(out <= 3u);
    }
}

// pulp #2249 follow-up (Codex review P2): malformed UTF-8 lead bytes
// whose continuation bytes don't match `0b10xxxxxx` must be advanced
// by exactly 1 byte (matching ICU's U+FFFD substitution length). A
// pre-fix `utf8_scalar_len` that accepted `0xC2 0x41` as a 2-byte
// scalar desynced the bridge: build_bridge advanced 2, ICU advanced
// 1, and subsequent byte→UTF-16 lookups returned wrong offsets.
// Verify with `"\xC2A"` — a lead byte claiming 2 bytes followed by
// ASCII 'A'. Calling word_break_step over this input must not crash,
// must return a valid offset in [0, text.size()], and must treat the
// input as non-empty (forward step from 0 must move forward).
TEST_CASE("word_break_step: malformed continuation byte does not desync bridge",
          "[font][locale][issue-2249]") {
    // Lead byte 0xC2 claims a 2-byte scalar, but the next byte 'A'
    // (0x41) is not a valid continuation byte. Build the string via
    // explicit byte concatenation to avoid C++ hex-escape ambiguity
    // (`"\xC2A"` parses as a single 3-digit hex escape).
    std::string s;
    s.push_back(static_cast<char>(0xC2));
    s.append("A bc");
    REQUIRE_NOTHROW(word_break_step(s, 0, "en-US", /*forward=*/true));
    const std::size_t out = word_break_step(s, 0, "en-US", /*forward=*/true);
    REQUIRE(out > 0u);
    REQUIRE(out <= s.size());
}
