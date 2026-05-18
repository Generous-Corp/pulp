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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#ifdef PULP_HAS_SKIA
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
class SkFontMgr;
class SkTypeface;
#endif

namespace pulp::canvas {

// ── Public font-registration API (pulp #1150) ───────────────────────────────
// Plugin authors bundle their own .ttf/.otf files (e.g. via the CMake
// `pulp_register_font(target NAME path/to/font.ttf [FAMILY "..."])` macro
// or by hand-rolling pulp_add_binary_data) and call `register_font(...)`
// during plugin startup so subsequent `canvas.set_font("My Family", ...)`
// and `setFontFamily(label, "My Family")` calls resolve to the supplied
// face instead of silently falling back to the platform font manager.
//
// These entry points are available regardless of `PULP_HAS_SKIA`. Without
// Skia they degrade to a no-op that returns `false`, which lets plugin
// startup code call them unconditionally.

/// Register a font from raw TTF/OTF bytes. The supplied buffer is copied
/// into a long-lived `SkData` so the caller may free `data` immediately.
///
/// If `family_override` is empty, the family name reported by the font's
/// own name table (via Skia's `SkTypeface::getFamilyName`) is used. Pass
/// a non-empty override when you want subsequent `setFontFamily("Foo")`
/// calls to match a name that doesn't appear in the font file itself.
///
/// Returns `true` if Skia parsed the bytes into a usable `SkTypeface` and
/// the registration was stored; `false` if Skia rejected the bytes, no
/// platform font manager was available, the family name was empty (and
/// no override was supplied), or `PULP_HAS_SKIA` is not defined.
///
/// Idempotent: calling `register_font` twice with the same family is
/// safe — the second call replaces the first, but the typeface cache is
/// invalidated so callers that already resolved the family see the new
/// face on the next lookup. There is no `unregister_font` today; the
/// expected lifetime is "process".
bool register_font(const std::uint8_t* data, std::size_t size,
                   const std::string& family_override = "");

/// Convenience overload: register a font from a file on disk. Reads the
/// entire file into memory, then forwards to the bytes-based overload.
/// Returns `false` if the file cannot be opened, is empty, or Skia
/// rejects the bytes.
bool register_font_file(const std::string& path,
                        const std::string& family_override = "");

/// Returns true iff the requested family name was previously registered
/// via `register_font` / `register_font_file`. Bundled families (Inter,
/// JetBrains Mono) and platform-installed families are NOT covered by
/// this query — use `is_font_family_available` for the cascading check.
///
/// Comparison is exact-string (case-sensitive) against the registered
/// name (override-or-table-derived).
bool is_font_registered(const std::string& family);

/// Monotonic counter that bumps every time a typeface registration mutates
/// process-global font state (`register_font`, `register_font_file`,
/// `register_emoji_fallback`). Downstream caches that key on typeface
/// identity (the static `SkTypeface` cache in `skia_canvas.cpp`, the
/// `(family,size)->text->width` cache in `text_shaper.cpp`) sample this
/// before every lookup and flush themselves when it has advanced.
///
/// Without this, re-registering "MyBrand" with a different .ttf — or
/// swapping the emoji fallback mid-process — would still resolve to the
/// previously-cached `SkTypeface`, contradicting `register_font`'s
/// documented "idempotent, invalidates the cache" contract.
std::uint64_t font_registration_generation() noexcept;

/// Force-bump the registration generation. Called from
/// `register_emoji_fallback(...)` (defined in `text_font_context.cpp`)
/// and other entry points that mutate process-global font state outside
/// `register_font(...)`. Avoid calling from regular code — the standard
/// registration entry points handle it for you.
void bump_font_registration_generation() noexcept;

#ifdef PULP_HAS_SKIA

/// Process-wide platform font manager. Returns the CoreText / DirectWrite /
/// Android / FontConfig manager appropriate for the current OS, or nullptr
/// on platforms where no manager is available. Lazily constructed; the
/// same instance is returned for every call.
///
/// pulp #2163 / font v2 Slice 1.1.a — consolidates the five identical
/// `SkFontMgr_New_*` switch blocks that previously lived inside
/// `skia_canvas.cpp`, `text_shaper.cpp`, `sdf_atlas.cpp`,
/// `bundled_fonts.cpp` (×2), and the seed copy in `font_resolver.cpp`.
sk_sp<SkFontMgr> platform_font_manager();

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

/// Look up a plugin-registered typeface by family name (registered via the
/// public `register_font` API). Returns `nullptr` if no matching family
/// has been registered or the registered face does not satisfy `style`
/// (a Regular face will not be returned for a Bold request, mirroring
/// `match_bundled_typeface`).
///
/// Skia-aware variant; `core/view/` callers should prefer `is_font_registered`
/// to avoid pulling Skia headers in.
sk_sp<SkTypeface> match_registered_typeface(const std::string& family,
                                            SkFontStyle style);

/// Number of embedded bundled fonts compiled in. Useful for tests that want
/// to assert "the build actually pulled in some .ttfs". Always returns the
/// embedded count regardless of whether `match_bundled_typeface` has been
/// called.
std::size_t bundled_font_count();

/// pulp #2163 — programmatic glyph-coverage probe.
///
/// Resolves `family`+`weight`+`slant` through the registered → bundled →
/// platform-font-manager cascade exactly as a real fill_text call would,
/// then reports whether the resulting typeface contains a glyph for the
/// given codepoint. Returns the actual family name the cascade landed
/// on so the caller can detect silent fallbacks (e.g. "asked for IBM
/// Plex Mono, got Helvetica").
///
/// All output fields are plain types — no Skia headers required at the
/// call site — so tools and `examples/ui-preview` can use this for
/// non-visual import validation.
struct FontProbe {
    std::string family;            ///< the family that was requested
    std::uint32_t codepoint = 0;   ///< the codepoint that was probed
    bool family_resolved = false;  ///< true iff a typeface was returned at all
    bool glyph_present = false;    ///< true iff the typeface has a glyph for `codepoint`
    std::string resolved_family;   ///< actual family name of the resolved typeface (empty if not resolved)
};

FontProbe probe_font_glyph(const std::string& family,
                           int weight, int slant,
                           std::uint32_t codepoint);

#endif // PULP_HAS_SKIA

} // namespace pulp::canvas
