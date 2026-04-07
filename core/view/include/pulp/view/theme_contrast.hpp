#pragma once

#include <pulp/view/theme.hpp>
#include <string>
#include <vector>
#include <optional>

namespace pulp::view {

// ── WCAG Contrast Utilities ─────────────────────────────────────────────────
// Implements WCAG 2.1 contrast ratio calculations for accessible theme design.

/// Compute relative luminance of a color per WCAG 2.1 definition.
/// Returns a value in [0, 1] where 0 = black and 1 = white.
float relative_luminance(Color c);

/// Compute WCAG contrast ratio between two colors.
/// Returns a value in [1, 21] where 1 = identical and 21 = black vs white.
float contrast_ratio(Color a, Color b);

/// WCAG AA compliance levels
enum class ContrastLevel {
    aa_normal,    // 4.5:1 for normal text (< 18pt or < 14pt bold)
    aa_large,     // 3.0:1 for large text (>= 18pt or >= 14pt bold)
    aaa_normal,   // 7.0:1 for AAA normal text
    aaa_large     // 4.5:1 for AAA large text
};

/// Get the minimum contrast ratio required for a given level.
float min_contrast_for_level(ContrastLevel level);

/// Check if a foreground/background pair meets a given contrast level.
bool meets_contrast(Color foreground, Color background, ContrastLevel level = ContrastLevel::aa_normal);

/// Given a background color, compute an accessible foreground color.
/// Tries white first; if that fails the contrast threshold, uses black.
/// For more nuanced results, adjusts lightness of the provided hint color.
Color auto_contrast_foreground(Color background, ContrastLevel level = ContrastLevel::aa_normal);

/// Given a background color and a desired foreground, adjust the foreground
/// lightness until it meets the required contrast level. Returns the adjusted
/// color, or std::nullopt if no adjustment can achieve the target.
std::optional<Color> adjust_for_contrast(Color foreground, Color background,
                                          ContrastLevel level = ContrastLevel::aa_normal);

// ── HSL Conversion Utilities ────────────────────────────────────────────────
// Delegates to Color::to_hsl() / Color::from_hsl() — these wrappers exist
// for backward compatibility with code that uses the free-function API.

using HSL = canvas::Color::HSL;

inline HSL rgb_to_hsl(Color c) { return c.to_hsl(); }
inline Color hsl_to_rgb(HSL hsl, float alpha = 1.0f) { return Color::from_hsl(hsl, alpha); }

/// Shift hue by degrees (wraps around 360).
Color shift_hue(Color c, float degrees);

/// Adjust lightness by a delta (clamped to [0,1]).
Color adjust_lightness(Color c, float delta);

/// Blend two colors by factor t in [0,1]. t=0 returns a, t=1 returns b.
Color blend_colors(Color a, Color b, float t);

/// Apply alpha to a color [0.0-1.0].
Color with_alpha(Color c, float alpha);

// ── Theme Contrast Validation ───────────────────────────────────────────────

struct ContrastIssue {
    std::string foreground_token;
    std::string background_token;
    float ratio;
    ContrastLevel required_level;
};

/// Validate a theme for contrast issues. Checks standard token pairs:
/// text.primary on bg.primary, text.secondary on bg.secondary, etc.
/// Returns a list of pairs that fail the required contrast level.
std::vector<ContrastIssue> validate_theme_contrast(const Theme& theme,
                                                    ContrastLevel level = ContrastLevel::aa_normal);

/// Auto-fix contrast issues in a theme by adjusting foreground colors.
/// Returns a new theme with adjusted colors. Original is not modified.
Theme auto_fix_contrast(const Theme& theme, ContrastLevel level = ContrastLevel::aa_normal);

} // namespace pulp::view
