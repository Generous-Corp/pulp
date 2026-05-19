// test_font_color_mode.cpp — Pulp #2163, font v2 Slice 3.1.
//
// ResolvedFont::supports_color_font / color_font_active behavior.
// Tests the predicates against the bundled non-color Inter face,
// the system color-emoji face if installed, and the ColorFontMode
// policy enum.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/font_resolver.hpp>
#include <pulp/canvas/font_options.hpp>

#ifdef PULP_HAS_SKIA
#include "include/core/SkTypeface.h"
#endif

using namespace pulp::canvas;

TEST_CASE("ColorFont: ResolvedFont carries color_font_mode from FontOptions",
          "[font][color][issue-2163]") {
    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;
    opts.color_font_mode = ColorFontMode::Bitmap;

    auto resolved = FontResolver::instance().resolve_family_list(opts);
    REQUIRE(resolved.color_font_mode == ColorFontMode::Bitmap);
}

TEST_CASE("ColorFont: Inter (non-color) reports supports_color_font=false",
          "[font][color][issue-2163]") {
    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;
    auto resolved = FontResolver::instance().resolve_family_list(opts);
    if (!resolved.has_typeface()) {
        WARN("Inter not resolved; skipping color-table check");
        return;
    }
    REQUIRE_FALSE(resolved.supports_color_font());
}

TEST_CASE("ColorFont: Apple Color Emoji on macOS reports true if installed",
          "[font][color][issue-2163]") {
#if defined(__APPLE__)
    FontOptions opts;
    opts.family_stack.push_back("Apple Color Emoji");
    opts.size = 14.0f;
    auto resolved = FontResolver::instance().resolve_family_list(opts);
    if (resolved.has_typeface()) {
        // Apple Color Emoji uses the `sbix` bitmap strikes table.
        REQUIRE(resolved.supports_color_font());
    } else {
        WARN("Apple Color Emoji not resolvable on this host");
    }
#else
    SUCCEED("non-Apple host; Apple Color Emoji unavailable");
#endif
}

TEST_CASE("ColorFont: color_font_active respects ForceMonochrome override",
          "[font][color][issue-2163]") {
#if defined(__APPLE__)
    FontOptions opts;
    opts.family_stack.push_back("Apple Color Emoji");
    opts.size = 14.0f;
    opts.color_font_mode = ColorFontMode::ForceMonochrome;
    auto resolved = FontResolver::instance().resolve_family_list(opts);
    if (resolved.has_typeface()) {
        REQUIRE(resolved.supports_color_font());
        REQUIRE_FALSE(resolved.color_font_active());  // forced mono
    } else {
        SUCCEED("Apple Color Emoji not resolvable; skipped");
    }
#else
    SUCCEED("non-Apple host; emoji-policy check skipped");
#endif
}

TEST_CASE("ColorFont: Auto mode delegates to font capability",
          "[font][color][issue-2163]") {
    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;
    opts.color_font_mode = ColorFontMode::Auto;
    auto resolved = FontResolver::instance().resolve_family_list(opts);
    if (!resolved.has_typeface()) {
        SUCCEED("Inter not resolved; skipped");
        return;
    }
    // Inter has no color tables → Auto means inactive.
    REQUIRE_FALSE(resolved.color_font_active());
}

TEST_CASE("ColorFont: no typeface => supports_color_font is false",
          "[font][color][issue-2163]") {
    ResolvedFont r;  // default-constructed, no typeface
    REQUIRE_FALSE(r.supports_color_font());
    REQUIRE_FALSE(r.color_font_active());
}

// pulp #2243 follow-up (Codex review P2): the explicit ColorFontMode
// values (Bitmap / COLR / SVG) request a SPECIFIC color format. The
// pre-fix `color_font_active()` returned true for any of those modes
// whenever `supports_color_font()` returned true — i.e. it treated
// every explicit mode like Auto, silently accepting "I have CBDT" as
// satisfying "I asked for COLR".
//
// On macOS, Apple Color Emoji uses the `sbix` (bitmap) table and
// does NOT carry COLR. Ask for ColorFontMode::COLR against it and
// assert color_font_active() is FALSE — the contract is "strict
// match", not "any color table will do".
TEST_CASE("ColorFont: explicit COLR mode rejects font without COLR table",
          "[font][color][issue-2243]") {
#if defined(__APPLE__) && defined(PULP_HAS_SKIA)
    FontOptions opts;
    opts.family_stack.push_back("Apple Color Emoji");
    opts.size = 14.0f;
    opts.color_font_mode = ColorFontMode::COLR;
    auto resolved = FontResolver::instance().resolve_family_list(opts);
    if (!resolved.has_typeface()) {
        SUCCEED("Apple Color Emoji not resolvable on this host; skipped");
        return;
    }
    // Apple Color Emoji uses sbix bitmaps, not COLR → supports_color_font
    // is still true (any color table) but color_font_active under an
    // explicit COLR request must be false (strict match).
    REQUIRE(resolved.supports_color_font());
    REQUIRE_FALSE(resolved.color_font_active());
#else
    SUCCEED("requires macOS + Skia for Apple Color Emoji probe");
#endif
}

// Companion check: Bitmap mode against a bitmap-strikes font must
// still activate. Apple Color Emoji on macOS uses sbix (bitmap), so
// asking for Bitmap mode must succeed.
TEST_CASE("ColorFont: explicit Bitmap mode accepts sbix-strikes font",
          "[font][color][issue-2243]") {
#if defined(__APPLE__) && defined(PULP_HAS_SKIA)
    FontOptions opts;
    opts.family_stack.push_back("Apple Color Emoji");
    opts.size = 14.0f;
    opts.color_font_mode = ColorFontMode::Bitmap;
    auto resolved = FontResolver::instance().resolve_family_list(opts);
    if (!resolved.has_typeface()) {
        SUCCEED("Apple Color Emoji not resolvable on this host; skipped");
        return;
    }
    REQUIRE(resolved.supports_color_font());
    REQUIRE(resolved.color_font_active());
#else
    SUCCEED("requires macOS + Skia for Apple Color Emoji probe");
#endif
}

// Companion check: SVG mode against a font without an SVG table must
// reject. Apple Color Emoji is sbix-only, no SVG.
TEST_CASE("ColorFont: explicit SVG mode rejects font without SVG table",
          "[font][color][issue-2243]") {
#if defined(__APPLE__) && defined(PULP_HAS_SKIA)
    FontOptions opts;
    opts.family_stack.push_back("Apple Color Emoji");
    opts.size = 14.0f;
    opts.color_font_mode = ColorFontMode::SVG;
    auto resolved = FontResolver::instance().resolve_family_list(opts);
    if (!resolved.has_typeface()) {
        SUCCEED("Apple Color Emoji not resolvable on this host; skipped");
        return;
    }
    REQUIRE(resolved.supports_color_font());
    REQUIRE_FALSE(resolved.color_font_active());
#else
    SUCCEED("requires macOS + Skia for Apple Color Emoji probe");
#endif
}
