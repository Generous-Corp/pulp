// test_cluster_step.cpp — Pulp #2163, font v2 Slice 3.6.
//
// UAX #29-lite cluster_step behavior: emoji ZWJ family steps as one,
// regional-indicator flag pairs step as one, Devanagari virama
// conjuncts step as one, Latin combining marks attach to the base.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/bundled_fonts.hpp>

#include <string>

using namespace pulp::canvas;

TEST_CASE("cluster_step: end-of-string is idempotent", "[font][cluster][issue-2163]") {
    std::string s = "abc";
    REQUIRE(cluster_step(s, s.size(), /*forward=*/true)  == s.size());
    REQUIRE(cluster_step(s, 0,        /*forward=*/false) == 0);
}

TEST_CASE("cluster_step: plain ASCII advances by one byte", "[font][cluster]") {
    std::string s = "hello";
    REQUIRE(cluster_step(s, 0, true) == 1u);
    REQUIRE(cluster_step(s, 1, true) == 2u);
    REQUIRE(cluster_step(s, 4, true) == 5u);
    REQUIRE(cluster_step(s, 5, false) == 4u);
    REQUIRE(cluster_step(s, 1, false) == 0u);
}

TEST_CASE("cluster_step: emoji ZWJ family is one cluster", "[font][cluster][emoji]") {
    // 👨‍👩‍👧‍👦 = 👨 ZWJ 👩 ZWJ 👧 ZWJ 👦
    // UTF-8 bytes: 4 + 3 + 4 + 3 + 4 + 3 + 4 = 25 bytes
    std::string s = "\xF0\x9F\x91\xA8"   // 👨
                    "\xE2\x80\x8D"        // ZWJ
                    "\xF0\x9F\x91\xA9"   // 👩
                    "\xE2\x80\x8D"        // ZWJ
                    "\xF0\x9F\x91\xA7"   // 👧
                    "\xE2\x80\x8D"        // ZWJ
                    "\xF0\x9F\x91\xA6";   // 👦
    REQUIRE(s.size() == 25u);
    REQUIRE(cluster_step(s, 0, true) == s.size());
    REQUIRE(cluster_step(s, s.size(), false) == 0u);
}

TEST_CASE("cluster_step: regional indicator pair (US flag) is one cluster",
          "[font][cluster][emoji]") {
    // 🇺🇸 = U+1F1FA U+1F1F8 (regional indicators U + S)
    std::string s = "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";  // 8 bytes
    REQUIRE(s.size() == 8u);
    REQUIRE(cluster_step(s, 0, true) == 8u);
    REQUIRE(cluster_step(s, 8, false) == 0u);
}

TEST_CASE("cluster_step: two consecutive flags are two clusters",
          "[font][cluster][emoji]") {
    // 🇺🇸🇯🇵 — US then JP, four RIs total
    std::string s = "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"   // 🇺🇸
                    "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5";  // 🇯🇵
    REQUIRE(s.size() == 16u);
    REQUIRE(cluster_step(s, 0, true) == 8u);   // first flag
    REQUIRE(cluster_step(s, 8, true) == 16u);  // second flag
}

TEST_CASE("cluster_step: emoji + skin-tone modifier is one cluster",
          "[font][cluster][emoji]") {
    // 👍🏽 = thumbs-up U+1F44D + medium skin tone U+1F3FD
    std::string s = "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD";
    REQUIRE(s.size() == 8u);
    REQUIRE(cluster_step(s, 0, true) == 8u);
}

TEST_CASE("cluster_step: Devanagari ka + virama + ssa (क्ष) is one cluster",
          "[font][cluster][devanagari]") {
    // U+0915 U+094D U+0937 — three codepoints, one visual cluster
    std::string s = "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7";
    REQUIRE(s.size() == 9u);
    REQUIRE(cluster_step(s, 0, true) == 9u);
}

TEST_CASE("cluster_step: Latin base + combining acute (é via NFD) is one cluster",
          "[font][cluster][combining]") {
    // 'e' (0x65) + COMBINING ACUTE ACCENT U+0301 (\xCC\x81)
    std::string s = "e\xCC\x81";
    REQUIRE(s.size() == 3u);
    REQUIRE(cluster_step(s, 0, true) == 3u);
    REQUIRE(cluster_step(s, 3, false) == 0u);
}

TEST_CASE("cluster_step: variation selector glues to base", "[font][cluster][emoji]") {
    // ☀️ = U+2600 (sun) + U+FE0F (emoji presentation VS-16)
    std::string s = "\xE2\x98\x80\xEF\xB8\x8F";
    REQUIRE(s.size() == 6u);
    REQUIRE(cluster_step(s, 0, true) == 6u);
}

TEST_CASE("cluster_step: offsets inside UTF-8 scalars snap to cluster boundary",
          "[font][cluster][utf8]") {
    // a + é (2 bytes) + 😀 (4 bytes) + b
    std::string s = "a\xC3\xA9\xF0\x9F\x98\x80"
                    "b";
    REQUIRE(s.size() == 8u);

    REQUIRE(cluster_step(s, 2, true) == 3u);  // inside é
    REQUIRE(cluster_step(s, 4, true) == 7u);  // inside 😀
    REQUIRE(cluster_step(s, 6, true) == 7u);  // inside 😀

    REQUIRE(cluster_step(s, 7, false) == 3u);
    REQUIRE(cluster_step(s, 6, false) == 3u);
    REQUIRE(cluster_step(s, 3, false) == 1u);
}

TEST_CASE("cluster_step: backward traversal returns compound-cluster starts",
          "[font][cluster][emoji][combining]") {
    std::string s = "A"
                    "e\xCC\x81"              // e + combining acute
                    "\xF0\x9F\x87\xBA"
                    "\xF0\x9F\x87\xB8"       // US flag
                    "\xF0\x9F\x91\xA8"
                    "\xE2\x80\x8D"
                    "\xF0\x9F\x91\xA9";      // man ZWJ woman
    REQUIRE(s.size() == 23u);

    REQUIRE(cluster_step(s, s.size(), false) == 12u);
    REQUIRE(cluster_step(s, 12, false) == 4u);
    REQUIRE(cluster_step(s, 4, false) == 1u);
    REQUIRE(cluster_step(s, 1, false) == 0u);
}

TEST_CASE("cluster_step: malformed UTF-8 always makes forward progress",
          "[font][cluster][utf8]") {
    std::string truncated = "A\xE2\x82";
    REQUIRE(truncated.size() == 3u);
    REQUIRE(cluster_step(truncated, 0, true) == 1u);
    REQUIRE(cluster_step(truncated, 1, true) == 2u);
    REQUIRE(cluster_step(truncated, 2, true) == 3u);

    std::string stray_continuation = "\x80"
                                     "B";
    REQUIRE(cluster_step(stray_continuation, 0, true) == 1u);
    REQUIRE(cluster_step(stray_continuation, 1, true) == 2u);
    REQUIRE(cluster_step(stray_continuation, 2, false) == 1u);
}

TEST_CASE("cluster_step: extended combining ranges attach to base",
          "[font][cluster][combining]") {
    // U+1AB0 combining mark.
    std::string extended_latin = "a\xE1\xAA\xB0";
    REQUIRE(cluster_step(extended_latin, 0, true) == extended_latin.size());
    REQUIRE(cluster_step(extended_latin, extended_latin.size(), false) == 0u);

    // U+20D0 combining mark.
    std::string symbol_mark = "x\xE2\x83\x90";
    REQUIRE(cluster_step(symbol_mark, 0, true) == symbol_mark.size());

    // U+E0100 variation selector, encoded as four UTF-8 bytes.
    std::string variation_selector = "z\xF3\xA0\x84\x80";
    REQUIRE(cluster_step(variation_selector, 0, true) == variation_selector.size());
}

TEST_CASE("cluster_step: regional indicator run preserves pair boundaries",
          "[font][cluster][emoji][coverage]") {
    // 🇺🇸🇯 — the third regional indicator starts the next flag pair.
    std::string s = "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"
                    "\xF0\x9F\x87\xAF";
    REQUIRE(s.size() == 12u);

    REQUIRE(cluster_step(s, 0, true) == 8u);
    REQUIRE(cluster_step(s, 4, true) == 8u);
    REQUIRE(cluster_step(s, 8, true) == 12u);

    REQUIRE(cluster_step(s, 12, false) == 8u);
    REQUIRE(cluster_step(s, 8, false) == 0u);
}

TEST_CASE("cluster_step: ZWJ only joins pictographic targets",
          "[font][cluster][coverage]") {
    std::string joined = "\xE2\x98\x80\xE2\x80\x8D"
                         "\xF0\x9F\x94\xA5"; // sun + ZWJ + fire
    REQUIRE(cluster_step(joined, 0, true) == joined.size());

    std::string not_joined = "a\xE2\x80\x8D"
                             "b";
    REQUIRE(cluster_step(not_joined, 0, true) == 4u);
    REQUIRE(cluster_step(not_joined, 4, true) == 5u);
}

TEST_CASE("cluster_step: malformed multi-byte prefixes fall back to one byte",
          "[font][cluster][utf8][coverage]") {
    std::string bad_two = "\xC2"
                          "A";
    REQUIRE(cluster_step(bad_two, 0, true) == 1u);
    REQUIRE(cluster_step(bad_two, 1, true) == 2u);

    std::string bad_three = "\xE2"
                            "AB";
    REQUIRE(cluster_step(bad_three, 0, true) == 1u);

    std::string bad_four = "\xF0\x9F"
                           "AB";
    REQUIRE(cluster_step(bad_four, 0, true) == 1u);
}
