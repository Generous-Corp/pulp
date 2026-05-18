// test_font_aa_hinting.cpp — Pulp #2163, font v2 Slice 3.2.
//
// Verifies that FontOptions.aa_mode / .hinting_mode are propagated by
// FontResolver onto the returned ResolvedFont, and that the helper
// translators on ResolvedFont (and the free `sk_edging_for` /
// `sk_hinting_for` functions) map to the documented Skia enums.
//
// Locks in the policy table from font_resolver.hpp so a stealth change
// (e.g. flipping AntiAliasMode::Default to kSubpixelAntiAlias) is caught
// by an automated test instead of by a goldens regression weeks later.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/font_options.hpp>
#include <pulp/canvas/font_resolver.hpp>

#ifdef PULP_HAS_SKIA
#include "include/core/SkFont.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkTypeface.h"
#endif

using namespace pulp::canvas;

#ifdef PULP_HAS_SKIA

TEST_CASE("AA mode translates: Default → kAntiAlias", "[font][aa][issue-2163]") {
    REQUIRE(sk_edging_for(AntiAliasMode::Default) == SkFont::Edging::kAntiAlias);
}

TEST_CASE("AA mode translates: LCD → kSubpixelAntiAlias", "[font][aa][issue-2163]") {
    REQUIRE(sk_edging_for(AntiAliasMode::LCD) == SkFont::Edging::kSubpixelAntiAlias);
}

TEST_CASE("AA mode translates: Grayscale → kAntiAlias", "[font][aa][issue-2163]") {
    REQUIRE(sk_edging_for(AntiAliasMode::Grayscale) == SkFont::Edging::kAntiAlias);
}

TEST_CASE("AA mode translates: NoAA → kAlias", "[font][aa][issue-2163]") {
    REQUIRE(sk_edging_for(AntiAliasMode::NoAA) == SkFont::Edging::kAlias);
}

TEST_CASE("Hinting mode translates: None → kNone", "[font][hinting][issue-2163]") {
    REQUIRE(sk_hinting_for(HintingMode::None) == SkFontHinting::kNone);
}

TEST_CASE("Hinting mode translates: Slight → kSlight", "[font][hinting][issue-2163]") {
    REQUIRE(sk_hinting_for(HintingMode::Slight) == SkFontHinting::kSlight);
}

TEST_CASE("Hinting mode translates: Normal → kNormal", "[font][hinting][issue-2163]") {
    REQUIRE(sk_hinting_for(HintingMode::Normal) == SkFontHinting::kNormal);
}

TEST_CASE("Hinting mode translates: Full → kFull", "[font][hinting][issue-2163]") {
    REQUIRE(sk_hinting_for(HintingMode::Full) == SkFontHinting::kFull);
}

TEST_CASE("Hinting mode translates: PlatformDefault → kNormal", "[font][hinting][issue-2163]") {
    // Documented contract: PlatformDefault resolves to Skia's documented
    // default (kNormal). When this changes per-platform, update both the
    // mapping in font_resolver.hpp AND this assertion in the same PR.
    REQUIRE(sk_hinting_for(HintingMode::PlatformDefault) == SkFontHinting::kNormal);
}

TEST_CASE("Resolver propagates aa_mode onto ResolvedFont", "[font][aa][issue-2163]") {
    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;
    opts.aa_mode = AntiAliasMode::LCD;

    auto resolved = FontResolver::instance().resolve_family_list(opts);
    REQUIRE(resolved.aa_mode == AntiAliasMode::LCD);
    REQUIRE(resolved.sk_edging() == SkFont::Edging::kSubpixelAntiAlias);
}

TEST_CASE("Resolver propagates hinting_mode onto ResolvedFont", "[font][hinting][issue-2163]") {
    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;
    opts.hinting_mode = HintingMode::Slight;

    auto resolved = FontResolver::instance().resolve_family_list(opts);
    REQUIRE(resolved.hinting_mode == HintingMode::Slight);
    REQUIRE(resolved.sk_hinting() == SkFontHinting::kSlight);
}

TEST_CASE("ResolvedFont accessors compose: NoAA + Full", "[font][aa][hinting][issue-2163]") {
    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;
    opts.aa_mode = AntiAliasMode::NoAA;
    opts.hinting_mode = HintingMode::Full;

    auto resolved = FontResolver::instance().resolve_family_list(opts);
    REQUIRE(resolved.sk_edging() == SkFont::Edging::kAlias);
    REQUIRE(resolved.sk_hinting() == SkFontHinting::kFull);
}

TEST_CASE("Default-constructed FontOptions maps to AA on, normal hinting",
          "[font][aa][hinting][issue-2163]") {
    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;

    auto resolved = FontResolver::instance().resolve_family_list(opts);
    // Defaults defined in font_options.hpp.
    REQUIRE(resolved.aa_mode == AntiAliasMode::Default);
    REQUIRE(resolved.hinting_mode == HintingMode::PlatformDefault);
    REQUIRE(resolved.sk_edging() == SkFont::Edging::kAntiAlias);
    REQUIRE(resolved.sk_hinting() == SkFontHinting::kNormal);
}

#else  // !PULP_HAS_SKIA

TEST_CASE("AA / hinting helpers skipped without Skia", "[font][aa][issue-2163]") {
    SUCCEED("PULP_HAS_SKIA not defined; sk_edging_for / sk_hinting_for unavailable");
}

#endif  // PULP_HAS_SKIA
