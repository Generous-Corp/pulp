// bundled_fonts.hpp
//
// Pulp ships a small set of curated fonts under `external/fonts/` so plugin UIs
// can render deterministically without depending on what happens to be
// installed system-wide. Those .ttf files are baked into `pulp-canvas` at
// build time (see core/canvas/CMakeLists.txt → pulp_add_binary_data) and this
// module makes them visible to Skia's font lookup path.
//
// The Skia prebuilt binaries shipped via skia-builder are linked without
// FreeType/Fontations, which means `SkFontMgr_New_Custom_Data` is not exported
// from libskia.a. Instead we go one rung lower: use the *platform* font
// manager (CoreText / DirectWrite / fontconfig / Android) to materialise the
// bundled .ttfs into `SkTypeface`s via `SkFontMgr::makeFromData`, and cache
// those typefaces by family name. Callers can then ask for the bundled face
// before falling back to a `matchFamilyStyle` query that depends on the host
// system.
//
// Issue: pulp #932 — register external/fonts/*.ttf with SkFontMgr at startup
// so that `canvas.set_font("JetBrains Mono", ...)` succeeds on a stock macOS
// machine that doesn't have JetBrains Mono installed system-wide.
//
// This header is only meaningful when `PULP_HAS_SKIA` is defined; without
// Skia, bundled-font registration is a no-op.

#pragma once

#include <string>

#ifdef PULP_HAS_SKIA
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
class SkFontMgr;
class SkTypeface;
#endif

namespace pulp::canvas {

#ifdef PULP_HAS_SKIA

/// Look up a bundled typeface by family name (e.g. "Inter",
/// "JetBrains Mono"). The first call for a given process eagerly materialises
/// every embedded .ttf into an `SkTypeface` via the supplied font manager and
/// caches the results keyed by `getFamilyName()`. Subsequent calls are O(1)
/// and never touch the embedded bytes again.
///
/// Returns `nullptr` if:
///   * `mgr` is null (font manager not available on this platform), or
///   * No bundled font advertises the requested family name.
///
/// `style` is ignored for now — the bundle currently ships a single weight
/// per family — but the caller should still pass the requested style so that
/// future bundle expansions (e.g. JetBrainsMono-Bold) can match without an
/// API break.
sk_sp<SkTypeface> match_bundled_typeface(SkFontMgr* mgr,
                                         const std::string& family,
                                         SkFontStyle style);

/// Number of embedded bundled fonts compiled in. Useful for tests that want
/// to assert "the build actually pulled in some .ttfs". Always returns the
/// embedded count regardless of whether `match_bundled_typeface` has been
/// called.
std::size_t bundled_font_count();

#endif // PULP_HAS_SKIA

} // namespace pulp::canvas
