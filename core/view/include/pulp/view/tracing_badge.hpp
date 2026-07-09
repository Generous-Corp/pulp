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

}  // namespace pulp::view
