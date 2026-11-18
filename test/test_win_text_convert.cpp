// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/win/text_convert.hpp>

#include <string>

using namespace pulp::platform::win_text;

TEST_CASE("utf16_to_utf8 returns exactly the converted length", "[win-text]") {
    // The Windows dialog backend once sized the result one byte short of the
    // buffer it handed to the converter. The portable converter stores no
    // terminator, so size() is the content length and nothing lies beyond it.
    const std::u16string ascii = u"abc";
    const std::string out = utf16_to_utf8(ascii.c_str());
    REQUIRE(out == "abc");
    REQUIRE(out.size() == 3);
    REQUIRE(utf16_to_utf8(u"", 0).empty());
    REQUIRE(utf16_to_utf8(static_cast<const char16_t*>(nullptr)).empty());
}

TEST_CASE("utf16_to_utf8 handles multi-byte and surrogate pairs", "[win-text]") {
    REQUIRE(utf16_to_utf8(u"\u00e9") == "\xC3\xA9");
    REQUIRE(utf16_to_utf8(u"\u4e2d") == "\xE4\xB8\xAD");
    REQUIRE(utf16_to_utf8(u"\U0001F3B5") == "\xF0\x9F\x8E\xB5");
}

TEST_CASE("utf16_to_utf8 replaces unpaired surrogates", "[win-text]") {
    const char16_t lone_high[] = {0xD83C, u'x'};
    const char16_t lone_low[] = {0xDC00};
    const char16_t trailing_high[] = {u'a', 0xD83C};
    REQUIRE(utf16_to_utf8(lone_high, 2) == "\xEF\xBF\xBD" "x");
    REQUIRE(utf16_to_utf8(lone_low, 1) == "\xEF\xBF\xBD");
    REQUIRE(utf16_to_utf8(trailing_high, 2) == "a\xEF\xBF\xBD");
}

TEST_CASE("utf8_to_utf16 round-trips non-ASCII paths and arguments", "[win-text]") {
    for (const char* s : {"", "plain", "C:\\Users\\Ren\xC3\xA9\\\xE4\xB8\xAD\xE6\x96\x87.wav",
                          "\xF0\x9F\x8E\xB5 song"}) {
        const std::u16string wide = utf8_to_utf16(s);
        REQUIRE(utf16_to_utf8(wide.data(), wide.size()) == s);
    }
    // Non-ASCII input must not collapse to code-page '?' substitutes.
    REQUIRE(utf8_to_utf16("\xC3\xA9") == u"\u00e9");
    REQUIRE(utf8_to_utf16("\xF0\x9F\x8E\xB5") == u"\U0001F3B5");
}

TEST_CASE("utf8_to_utf16 replaces invalid UTF-8", "[win-text]") {
    REQUIRE(utf8_to_utf16("\xFF") == u"\uFFFD");
    REQUIRE(utf8_to_utf16("a\xC3") == u"a\uFFFD");             // truncated sequence
    REQUIRE(utf8_to_utf16("\xC0\x80") == u"\uFFFD\uFFFD");     // overlong
    REQUIRE(utf8_to_utf16("\xED\xA0\x80") == u"\uFFFD\uFFFD\uFFFD");  // encoded surrogate
}

TEST_CASE("sta_init_usable refuses a thread already in another apartment", "[win-text]") {
    REQUIRE(sta_init_usable(0));                    // S_OK
    REQUIRE(sta_init_usable(1));                    // S_FALSE: already an STA
    REQUIRE_FALSE(sta_init_usable(kComChangedMode));  // RPC_E_CHANGED_MODE
    REQUIRE_FALSE(sta_init_usable(static_cast<std::int32_t>(0x80004005u)));  // E_FAIL
}
