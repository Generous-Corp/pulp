#pragma once

// Tracing badge — the always-visible "◉ TRACING" reminder the root View paints
// on every frame when the binary is built with PULP_TRACING=ON, so a developer
// can never forget that Perfetto tracing is compiled in while looking at the
// plugin UI. It complements the ship-guard, the `pulp status` line, and the
// startup log. The paint site (in View::paint_all) is behind
// `if constexpr (pulp::runtime::kTracingEnabled)`, so the default OFF build
// carries no branch, no draw, and no per-frame cost.

namespace pulp::view {

// True only when tracing is compiled in AND the badge has not been suppressed.
// Always defined (returns false in an OFF build) so callers and tests can query
// it in either build configuration.
bool tracing_badge_should_paint();

// Suppress or re-enable the badge at runtime. Default is visible-when-compiled-in
// (the whole point is that you can't miss it). Golden visual-regression harnesses
// call set_tracing_badge_visible(false) so an ON build's badge never pollutes a
// reference screenshot. No effect in an OFF build.
void set_tracing_badge_visible(bool visible);

// The badge's label, for an application that hides the root badge and draws
// the reminder in its own chrome instead.
inline constexpr const char* kTracingBadgeLabel = "\u25C9 TRACING";

// Where the root badge sits, in the root View's coordinates. The default is the
// top-right corner the badge has always used; an application whose header has
// a line of controls can move the pill onto that line (set `top` and `height`
// to the row's) instead of hiding it.
struct TracingBadgePlacement {
    float right = 8.0f;    ///< Gap between the pill and the root's right edge.
    float top = 8.0f;      ///< Pill top.
    float height = 0.0f;   ///< Pill height; 0 sizes it to the font plus padding.
    float font_px = 11.0f; ///< Label font size.
};

// Replace or read the process-wide placement. Safe to call from the UI thread
// while frames are painting; the next frame uses the new placement.
void set_tracing_badge_placement(const TracingBadgePlacement& placement);
TracingBadgePlacement tracing_badge_placement();

// The pill and label geometry for a root `root_width` wide, given the label's
// measured advance and its face's ascent and descent. Pure, so a test or an
// application can compute exactly where the root badge paints. The label is
// centred on its ink: equal space above the ascent and below the descent.
struct TracingBadgeLayout {
    float pill_x = 0.0f, pill_y = 0.0f, pill_width = 0.0f, pill_height = 0.0f;
    float text_x = 0.0f;
    float baseline_y = 0.0f;
};
TracingBadgeLayout tracing_badge_layout(float root_width, float text_width,
                                        float ascent, float descent,
                                        const TracingBadgePlacement& placement);

}  // namespace pulp::view
