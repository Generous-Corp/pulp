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
