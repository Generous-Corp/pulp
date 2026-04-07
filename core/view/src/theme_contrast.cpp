#include <pulp/view/theme_contrast.hpp>
#include <algorithm>
#include <cmath>

namespace pulp::view {

// ── Relative Luminance (WCAG 2.1) ──────────────────────────────────────────

static float linearize(float s) {
    s = std::clamp(s, 0.0f, 1.0f);
    return (s <= 0.04045f) ? (s / 12.92f) : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

float relative_luminance(Color c) {
    return 0.2126f * linearize(c.r) + 0.7152f * linearize(c.g) + 0.0722f * linearize(c.b);
}

float contrast_ratio(Color a, Color b) {
    float la = relative_luminance(a);
    float lb = relative_luminance(b);
    float lighter = std::max(la, lb);
    float darker = std::min(la, lb);
    return (lighter + 0.05f) / (darker + 0.05f);
}

// ── Contrast Level Thresholds ───────────────────────────────────────────────

float min_contrast_for_level(ContrastLevel level) {
    switch (level) {
        case ContrastLevel::aa_normal:  return 4.5f;
        case ContrastLevel::aa_large:   return 3.0f;
        case ContrastLevel::aaa_normal: return 7.0f;
        case ContrastLevel::aaa_large:  return 4.5f;
    }
    return 4.5f;
}

bool meets_contrast(Color foreground, Color background, ContrastLevel level) {
    return contrast_ratio(foreground, background) >= min_contrast_for_level(level);
}

// ── Color utilities (delegate to Color methods) ────────────────────────────

Color shift_hue(Color c, float degrees) {
    auto hsl = c.to_hsl();
    hsl.h = std::fmod(hsl.h + degrees + 360.0f, 360.0f);
    return Color::from_hsl(hsl, c.a);
}

Color adjust_lightness(Color c, float delta) {
    auto hsl = c.to_hsl();
    hsl.l = std::clamp(hsl.l + delta, 0.0f, 1.0f);
    return Color::from_hsl(hsl, c.a);
}

Color blend_colors(Color a, Color b, float t) {
    return a.interpolate(b, std::clamp(t, 0.0f, 1.0f));
}

Color with_alpha(Color c, float alpha) {
    return c.with_alpha(alpha);
}

// ── Auto-Contrast ───────────────────────────────────────────────────────────

Color auto_contrast_foreground(Color background, ContrastLevel level) {
    float threshold = min_contrast_for_level(level);
    Color white = Color::rgba(1.0f, 1.0f, 1.0f);
    Color black = Color::rgba(0.0f, 0.0f, 0.0f);

    if (contrast_ratio(white, background) >= threshold)
        return white;
    if (contrast_ratio(black, background) >= threshold)
        return black;

    // Edge case: neither pure white nor black meets threshold (very unlikely)
    // Return whichever has higher contrast
    return (contrast_ratio(white, background) > contrast_ratio(black, background))
        ? white : black;
}

std::optional<Color> adjust_for_contrast(Color foreground, Color background, ContrastLevel level) {
    float threshold = min_contrast_for_level(level);

    // Already meets requirement
    if (contrast_ratio(foreground, background) >= threshold)
        return foreground;

    auto hsl = rgb_to_hsl(foreground);
    float bg_lum = relative_luminance(background);

    // Determine direction: lighten if bg is dark, darken if bg is light
    float direction = (bg_lum < 0.5f) ? 0.01f : -0.01f;

    for (int i = 0; i < 100; ++i) {
        hsl.l = std::clamp(hsl.l + direction, 0.0f, 1.0f);
        Color candidate = hsl_to_rgb(hsl, foreground.a);
        if (contrast_ratio(candidate, background) >= threshold)
            return candidate;

        // Hit the boundary without meeting threshold
        if (hsl.l <= 0.0f || hsl.l >= 1.0f)
            break;
    }

    return std::nullopt;
}

// ── Theme Contrast Validation ───────────────────────────────────────────────

// Standard token pairs that should meet contrast requirements
static const std::pair<const char*, const char*> kTokenPairs[] = {
    {"text.primary",   "bg.primary"},
    {"text.primary",   "bg.secondary"},
    {"text.secondary", "bg.primary"},
    {"text.secondary", "bg.secondary"},
    {"text.primary",   "bg.surface"},
    {"text.primary",   "bg.elevated"},
    {"accent.primary", "bg.primary"},
    {"accent.error",   "bg.primary"},
    {"accent.success", "bg.primary"},
    {"accent.warning", "bg.primary"},
};

std::vector<ContrastIssue> validate_theme_contrast(const Theme& theme, ContrastLevel level) {
    std::vector<ContrastIssue> issues;
    float threshold = min_contrast_for_level(level);

    for (auto& [fg_token, bg_token] : kTokenPairs) {
        auto fg = theme.color(fg_token);
        auto bg = theme.color(bg_token);
        if (!fg || !bg) continue;

        float ratio = contrast_ratio(*fg, *bg);
        if (ratio < threshold) {
            issues.push_back({fg_token, bg_token, ratio, level});
        }
    }

    return issues;
}

Theme auto_fix_contrast(const Theme& theme, ContrastLevel level) {
    Theme fixed = theme;
    auto issues = validate_theme_contrast(theme, level);

    for (auto& issue : issues) {
        auto fg = fixed.color(issue.foreground_token);
        auto bg = fixed.color(issue.background_token);
        if (!fg || !bg) continue;

        auto adjusted = adjust_for_contrast(*fg, *bg, level);
        if (adjusted) {
            fixed.colors[issue.foreground_token] = *adjusted;
        }
    }

    return fixed;
}

} // namespace pulp::view
