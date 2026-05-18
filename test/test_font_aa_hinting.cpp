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

// pulp #2163 — review-sweep update: `Default` / `PlatformDefault`
// no longer hard-map to a specific Skia enum (Codex P2 review on
// PR #2186). They resolve to `std::nullopt`, signalling "caller
// preserves the existing per-context heuristic". Explicit modes
// still resolve to the fixed enum.

TEST_CASE("AA mode: Default returns nullopt (caller decides)", "[font][aa][issue-2163]") {
    REQUIRE(sk_edging_for(AntiAliasMode::Default) == std::nullopt);
}

TEST_CASE("AA mode translates: LCD → kSubpixelAntiAlias", "[font][aa][issue-2163]") {
    REQUIRE(sk_edging_for(AntiAliasMode::LCD)
            == std::optional<SkFont::Edging>{SkFont::Edging::kSubpixelAntiAlias});
}

TEST_CASE("AA mode translates: Grayscale → kAntiAlias", "[font][aa][issue-2163]") {
    REQUIRE(sk_edging_for(AntiAliasMode::Grayscale)
            == std::optional<SkFont::Edging>{SkFont::Edging::kAntiAlias});
}

TEST_CASE("AA mode translates: NoAA → kAlias", "[font][aa][issue-2163]") {
    REQUIRE(sk_edging_for(AntiAliasMode::NoAA)
            == std::optional<SkFont::Edging>{SkFont::Edging::kAlias});
}

TEST_CASE("Hinting mode translates: None → kNone", "[font][hinting][issue-2163]") {
    REQUIRE(sk_hinting_for(HintingMode::None)
            == std::optional<SkFontHinting>{SkFontHinting::kNone});
}

TEST_CASE("Hinting mode translates: Slight → kSlight", "[font][hinting][issue-2163]") {
    REQUIRE(sk_hinting_for(HintingMode::Slight)
            == std::optional<SkFontHinting>{SkFontHinting::kSlight});
}

TEST_CASE("Hinting mode translates: Normal → kNormal", "[font][hinting][issue-2163]") {
    REQUIRE(sk_hinting_for(HintingMode::Normal)
            == std::optional<SkFontHinting>{SkFontHinting::kNormal});
}

TEST_CASE("Hinting mode translates: Full → kFull", "[font][hinting][issue-2163]") {
    REQUIRE(sk_hinting_for(HintingMode::Full)
            == std::optional<SkFontHinting>{SkFontHinting::kFull});
}

TEST_CASE("Hinting mode: PlatformDefault returns nullopt (caller decides)",
          "[font][hinting][issue-2163]") {
    REQUIRE(sk_hinting_for(HintingMode::PlatformDefault) == std::nullopt);
}

TEST_CASE("Resolver propagates aa_mode onto ResolvedFont", "[font][aa][issue-2163]") {
    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;
    opts.aa_mode = AntiAliasMode::LCD;

    auto resolved = FontResolver::instance().resolve_family_list(opts);
    REQUIRE(resolved.aa_mode == AntiAliasMode::LCD);
    REQUIRE(resolved.sk_edging() == std::optional<SkFont::Edging>{SkFont::Edging::kSubpixelAntiAlias});
}

TEST_CASE("Resolver propagates hinting_mode onto ResolvedFont", "[font][hinting][issue-2163]") {
    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;
    opts.hinting_mode = HintingMode::Slight;

    auto resolved = FontResolver::instance().resolve_family_list(opts);
    REQUIRE(resolved.hinting_mode == HintingMode::Slight);
    REQUIRE(resolved.sk_hinting() == std::optional<SkFontHinting>{SkFontHinting::kSlight});
}

TEST_CASE("ResolvedFont accessors compose: NoAA + Full", "[font][aa][hinting][issue-2163]") {
    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;
    opts.aa_mode = AntiAliasMode::NoAA;
    opts.hinting_mode = HintingMode::Full;

    auto resolved = FontResolver::instance().resolve_family_list(opts);
    REQUIRE(resolved.sk_edging() == std::optional<SkFont::Edging>{SkFont::Edging::kAlias});
    REQUIRE(resolved.sk_hinting() == std::optional<SkFontHinting>{SkFontHinting::kFull});
}

TEST_CASE("Default-constructed FontOptions defers AA + hinting decisions",
          "[font][aa][hinting][issue-2163]") {
    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;

    auto resolved = FontResolver::instance().resolve_family_list(opts);
    REQUIRE(resolved.aa_mode == AntiAliasMode::Default);
    REQUIRE(resolved.hinting_mode == HintingMode::PlatformDefault);
    REQUIRE(resolved.sk_edging() == std::nullopt);
    REQUIRE(resolved.sk_hinting() == std::nullopt);
}

#else  // !PULP_HAS_SKIA

TEST_CASE("AA / hinting helpers skipped without Skia", "[font][aa][issue-2163]") {
    SUCCEED("PULP_HAS_SKIA not defined; sk_edging_for / sk_hinting_for unavailable");
}

#endif  // PULP_HAS_SKIA
