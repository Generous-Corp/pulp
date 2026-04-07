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

// ── HSL Conversion ──────────────────────────────────────────────────────────

HSL rgb_to_hsl(Color c) {
    float r = std::clamp(c.r, 0.0f, 1.0f);
    float g = std::clamp(c.g, 0.0f, 1.0f);
    float b = std::clamp(c.b, 0.0f, 1.0f);

    float mx = std::max({r, g, b});
    float mn = std::min({r, g, b});
    float d = mx - mn;

    HSL hsl;
    hsl.l = (mx + mn) / 2.0f;

    if (d < 1e-6f) {
        hsl.h = 0;
        hsl.s = 0;
        return hsl;
    }

    hsl.s = (hsl.l > 0.5f) ? d / (2.0f - mx - mn) : d / (mx + mn);

    if (mx == r)
        hsl.h = std::fmod((g - b) / d + 6.0f, 6.0f) * 60.0f;
    else if (mx == g)
        hsl.h = ((b - r) / d + 2.0f) * 60.0f;
    else
        hsl.h = ((r - g) / d + 4.0f) * 60.0f;

    return hsl;
}

static float hue_to_rgb(float p, float q, float t) {
    if (t < 0) t += 1.0f;
    if (t > 1) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

Color hsl_to_rgb(HSL hsl, float alpha) {
    if (hsl.s < 1e-6f) {
        float v = std::clamp(hsl.l, 0.0f, 1.0f);
        return Color::rgba(v, v, v, alpha);
    }

    float q = (hsl.l < 0.5f) ? hsl.l * (1.0f + hsl.s) : hsl.l + hsl.s - hsl.l * hsl.s;
    float p = 2.0f * hsl.l - q;
    float h = hsl.h / 360.0f;

    float r = hue_to_rgb(p, q, h + 1.0f / 3.0f);
    float g = hue_to_rgb(p, q, h);
    float b = hue_to_rgb(p, q, h - 1.0f / 3.0f);

    return Color::rgba(
        std::clamp(r, 0.0f, 1.0f),
        std::clamp(g, 0.0f, 1.0f),
        std::clamp(b, 0.0f, 1.0f),
        alpha);
}

Color shift_hue(Color c, float degrees) {
    auto hsl = rgb_to_hsl(c);
    hsl.h = std::fmod(hsl.h + degrees + 360.0f, 360.0f);
    return hsl_to_rgb(hsl, c.a);
}

Color adjust_lightness(Color c, float delta) {
    auto hsl = rgb_to_hsl(c);
    hsl.l = std::clamp(hsl.l + delta, 0.0f, 1.0f);
    return hsl_to_rgb(hsl, c.a);
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
