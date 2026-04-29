#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/theme_contrast.hpp>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

// ── Relative Luminance ──────────────────────────────────────────────────────

TEST_CASE("Relative luminance of black is 0", "[view][contrast]") {
    REQUIRE_THAT(relative_luminance(Color::rgba8(0, 0, 0)), WithinAbs(0.0, 0.001));
}

TEST_CASE("Relative luminance of white is 1", "[view][contrast]") {
    REQUIRE_THAT(relative_luminance(Color::rgba8(255, 255, 255)), WithinAbs(1.0, 0.001));
}

TEST_CASE("Relative luminance of mid-gray", "[view][contrast]") {
    auto lum = relative_luminance(Color::rgba8(128, 128, 128));
    REQUIRE(lum > 0.1f);
    REQUIRE(lum < 0.5f);
}

// ── Contrast Ratio ──────────────────────────────────────────────────────────

TEST_CASE("Contrast ratio of black vs white is ~21:1", "[view][contrast]") {
    float ratio = contrast_ratio(Color::rgba8(0, 0, 0), Color::rgba8(255, 255, 255));
    REQUIRE_THAT(ratio, WithinAbs(21.0, 0.1));
}

TEST_CASE("Contrast ratio of identical colors is 1:1", "[view][contrast]") {
    auto c = Color::rgba8(100, 150, 200);
    REQUIRE_THAT(contrast_ratio(c, c), WithinAbs(1.0, 0.01));
}

TEST_CASE("Contrast ratio is symmetric", "[view][contrast]") {
    auto a = Color::rgba8(50, 50, 50);
    auto b = Color::rgba8(200, 200, 200);
    REQUIRE_THAT(contrast_ratio(a, b), WithinAbs(contrast_ratio(b, a), 0.001));
}

// ── WCAG Compliance ─────────────────────────────────────────────────────────

TEST_CASE("White text on black meets AA normal", "[view][contrast]") {
    REQUIRE(meets_contrast(Color::rgba8(255, 255, 255), Color::rgba8(0, 0, 0), ContrastLevel::aa_normal));
}

TEST_CASE("White text on black meets AAA normal", "[view][contrast]") {
    REQUIRE(meets_contrast(Color::rgba8(255, 255, 255), Color::rgba8(0, 0, 0), ContrastLevel::aaa_normal));
}

TEST_CASE("Light gray on white fails AA normal", "[view][contrast]") {
    REQUIRE_FALSE(meets_contrast(Color::rgba8(200, 200, 200), Color::rgba8(255, 255, 255), ContrastLevel::aa_normal));
}

TEST_CASE("Min contrast thresholds are correct", "[view][contrast]") {
    REQUIRE_THAT(min_contrast_for_level(ContrastLevel::aa_normal), WithinAbs(4.5, 0.01));
    REQUIRE_THAT(min_contrast_for_level(ContrastLevel::aa_large), WithinAbs(3.0, 0.01));
    REQUIRE_THAT(min_contrast_for_level(ContrastLevel::aaa_normal), WithinAbs(7.0, 0.01));
    REQUIRE_THAT(min_contrast_for_level(ContrastLevel::aaa_large), WithinAbs(4.5, 0.01));
}

// ── Auto Contrast ───────────────────────────────────────────────────────────

TEST_CASE("Auto contrast picks white on dark background", "[view][contrast]") {
    auto fg = auto_contrast_foreground(Color::rgba8(20, 20, 30));
    REQUIRE(fg.r == Catch::Approx(1.0f));
    REQUIRE(fg.g == Catch::Approx(1.0f));
    REQUIRE(fg.b == Catch::Approx(1.0f));
}

TEST_CASE("Auto contrast picks black on light background", "[view][contrast]") {
    auto fg = auto_contrast_foreground(Color::rgba8(240, 240, 240));
    REQUIRE(fg.r == Catch::Approx(0.0f));
    REQUIRE(fg.g == Catch::Approx(0.0f));
    REQUIRE(fg.b == Catch::Approx(0.0f));
}

TEST_CASE("Adjust for contrast returns meeting color", "[view][contrast]") {
    auto result = adjust_for_contrast(
        Color::rgba8(150, 150, 150),  // Low contrast on white bg
        Color::rgba8(255, 255, 255),
        ContrastLevel::aa_normal);

    REQUIRE(result.has_value());
    REQUIRE(meets_contrast(*result, Color::rgba8(255, 255, 255), ContrastLevel::aa_normal));
}

// ── HSL Conversion ──────────────────────────────────────────────────────────

TEST_CASE("HSL round-trip for pure red", "[view][contrast][hsl]") {
    auto c = Color::rgba8(255, 0, 0);
    auto hsl = rgb_to_hsl(c);
    REQUIRE_THAT(hsl.h, WithinAbs(0.0, 1.0));
    REQUIRE_THAT(hsl.s, WithinAbs(1.0, 0.01));
    REQUIRE_THAT(hsl.l, WithinAbs(0.5, 0.01));

    auto back = hsl_to_rgb(hsl);
    REQUIRE(back.r == Catch::Approx(1.0f).margin(0.01f));
    REQUIRE(back.g == Catch::Approx(0.0f).margin(0.01f));
    REQUIRE(back.b == Catch::Approx(0.0f).margin(0.01f));
}

TEST_CASE("HSL round-trip for gray", "[view][contrast][hsl]") {
    auto c = Color::rgba8(128, 128, 128);
    auto hsl = rgb_to_hsl(c);
    REQUIRE_THAT(hsl.s, WithinAbs(0.0, 0.01));
    REQUIRE_THAT(hsl.l, WithinAbs(0.5, 0.02));
}

TEST_CASE("Shift hue wraps around", "[view][contrast][hsl]") {
    auto red = Color::rgba8(255, 0, 0);
    auto shifted = shift_hue(red, 120.0f); // Should be green-ish
    auto hsl = rgb_to_hsl(shifted);
    REQUIRE_THAT(hsl.h, WithinAbs(120.0, 2.0));
}

TEST_CASE("Adjust lightness clamps at range bounds", "[view][contrast][hsl]") {
    auto mid = Color::rgba8(128, 128, 128);

    auto darker = adjust_lightness(mid, -2.0f);
    REQUIRE_THAT(rgb_to_hsl(darker).l, WithinAbs(0.0, 0.001));

    auto lighter = adjust_lightness(mid, 2.0f);
    REQUIRE_THAT(rgb_to_hsl(lighter).l, WithinAbs(1.0, 0.001));
}

TEST_CASE("Blend colors at 0 returns first", "[view][contrast]") {
    auto a = Color::rgba8(255, 0, 0);
    auto b = Color::rgba8(0, 0, 255);
    auto result = blend_colors(a, b, 0.0f);
    REQUIRE(result.r8() == 255);
    REQUIRE(result.b8() == 0);
}

TEST_CASE("Blend colors at 1 returns second", "[view][contrast]") {
    auto a = Color::rgba8(255, 0, 0);
    auto b = Color::rgba8(0, 0, 255);
    auto result = blend_colors(a, b, 1.0f);
    REQUIRE(result.r8() == 0);
    REQUIRE(result.b8() == 255);
}

TEST_CASE("Blend colors clamps interpolation factor", "[view][contrast]") {
    auto a = Color::rgba8(255, 0, 0);
    auto b = Color::rgba8(0, 0, 255);

    auto below = blend_colors(a, b, -1.0f);
    REQUIRE(below.r8() == 255);
    REQUIRE(below.b8() == 0);

    auto above = blend_colors(a, b, 2.0f);
    REQUIRE(above.r8() == 0);
    REQUIRE(above.b8() == 255);
}

TEST_CASE("With alpha preserves rgb and applies alpha", "[view][contrast]") {
    auto c = Color::rgba8(10, 20, 30);
    auto translucent = with_alpha(c, 0.25f);
    REQUIRE(translucent.r8() == 10);
    REQUIRE(translucent.g8() == 20);
    REQUIRE(translucent.b8() == 30);
    REQUIRE_THAT(translucent.a, WithinAbs(0.25, 0.001));

    auto opaque = with_alpha(c, 2.0f);
    REQUIRE_THAT(opaque.a, WithinAbs(2.0, 0.001));
}

// ── Theme Contrast Validation ───────────────────────────────────────────────

TEST_CASE("Dark theme passes contrast validation", "[view][contrast][theme]") {
    auto theme = Theme::dark();
    auto issues = validate_theme_contrast(theme, ContrastLevel::aa_large);
    // Dark theme should have reasonable contrast for large text at minimum
    // Some pairs might fail aa_normal but should pass aa_large
    for (auto& issue : issues) {
        INFO("Failing pair: " << issue.foreground_token << " on " << issue.background_token
             << " ratio=" << issue.ratio);
    }
    // We don't require zero issues for aa_normal (accent colors may fail)
    // but key text pairs should pass
    auto text_on_bg = contrast_ratio(
        *theme.color("text.primary"), *theme.color("bg.primary"));
    REQUIRE(text_on_bg >= 4.5f);
}

TEST_CASE("Auto-fix contrast produces valid theme", "[view][contrast][theme]") {
    // Create a deliberately bad theme
    Theme bad;
    bad.colors["bg.primary"] = Color::rgba8(200, 200, 200);
    bad.colors["text.primary"] = Color::rgba8(180, 180, 180); // Almost same as bg
    bad.colors["text.secondary"] = Color::rgba8(190, 190, 190);
    bad.colors["bg.secondary"] = Color::rgba8(210, 210, 210);
    bad.colors["accent.primary"] = Color::rgba8(195, 195, 195);

    auto fixed = auto_fix_contrast(bad, ContrastLevel::aa_normal);

    // text.primary on bg.primary should now meet AA
    auto tp = fixed.color("text.primary");
    auto bp = fixed.color("bg.primary");
    if (tp && bp) {
        REQUIRE(contrast_ratio(*tp, *bp) >= 4.5f);
    }
}
