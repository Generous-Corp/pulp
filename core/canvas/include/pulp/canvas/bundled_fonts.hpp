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
#include <future>
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
/// process-global font state. Downstream caches sample this and flush.
std::uint64_t font_registration_generation() noexcept;

/// Force-bump the registration generation. Called from
/// `register_emoji_fallback(...)` and other entry points that mutate
/// process-global font state outside `register_font(...)`.
void bump_font_registration_generation() noexcept;

// ── Phase 2 skeleton — async lifecycle + font security ──────────────────
// pulp #2163 — font v2 Slices 2.1 / 2.8 surface declarations.

/// Async font lifecycle state (Slice 2.1).
enum class FontState : std::uint8_t {
    Pending,
    Loaded,
    Failed,
    Substituted,
};

/// Validate font bytes before handing them to Skia (Slice 2.8). Skeleton
/// confirms Skia can parse the bytes; full sanitizer is the impl slice.
bool validate_font_bytes(const std::uint8_t* data, std::size_t size);

/// Slice 2.1 — async font lifecycle. Schedule a font registration from a
/// URL or filesystem path and return a future that resolves to the
/// final `FontState`. Supported URL forms:
///   * `file:///abs/path/to/font.ttf` — decoded to an absolute path and
///     dispatched to `register_font_file` via `std::async`.
///   * `/abs/path/to/font.ttf` — treated as a local path, same dispatch.
///   * `http://…` / `https://…` — resolved immediately to `Failed`
///     until the in-tree HTTP fetcher is wired through Slice 2.1.b.
///     Callers that need network fetches today should pre-download
///     and call this entry point with a `file://` URL.
///
/// The returned future never blocks the caller. Resolves to `Loaded` on
/// success, `Failed` on any error (unreadable file, Skia rejection, or
/// unsupported scheme). The async font cache lives at process scope so
/// the future is safe to detach.
std::future<FontState> register_font_url(const std::string& url,
                                         const std::string& family_override = "");

// ── Phase 3 skeletons — color fonts, WOFF2, TextEditor cluster APIs ────
// pulp #2163 — font v2 Slices 3.1, 3.5, 3.6 surface declarations. The
// bodies arrive in the Phase 3 implementation slices; declarations live
// here so Phase 3 callers can target a stable API now.

/// Slice 3.5 — register a WOFF2-compressed font at runtime. Gated on
/// the Phase 2 sanitizer (validate_font_bytes); rejects malformed
/// payloads cleanly.
///
/// Behaviour today (security-gated, detection-only):
///   * Null / empty input → false.
///   * Bytes whose first 4 bytes are not the WOFF2 magic (`wOF2`,
///     0x774F4632 big-endian) → false. This is the structural reject
///     path and works on every build.
///   * Bytes WITH valid WOFF2 magic, when no Brotli/woff2 decoder is
///     linked into the build → false. Callers can probe this case
///     ahead of time via `woff2_decoder_available()` and route
///     through `register_font(...)` with a pre-decoded TTF/OTF
///     payload instead.
///   * Bytes WITH valid WOFF2 magic when a decoder IS linked → the
///     payload is decompressed, validated via `validate_font_bytes`,
///     and forwarded to `register_font(...)`.
///
/// Vendoring an in-tree woff2/Brotli decoder is intentionally deferred:
/// Pulp's MIT release stays free of large third-party blobs until a
/// real workload demands one. See `planning/2026-05-18-font-hardening-
/// phase23-execution-plan.md` Slice 3.5.
bool register_font_woff2(const std::uint8_t* woff2_data, std::size_t size,
                         const std::string& family_override = "");

/// Slice 3.5 — runtime check for WOFF2 decoder availability. Returns
/// `false` on builds where no Brotli/woff2 implementation is linked,
/// meaning `register_font_woff2(...)` cannot succeed regardless of
/// input. Callers can use this to fall back to a pre-decoded TTF/OTF
/// payload registered via `register_font(...)` instead.
///
/// Available on every build (Skia or no Skia), so plugin startup can
/// branch on it without `#ifdef`s leaking into user code.
bool woff2_decoder_available() noexcept;

/// Slice 3.6 — grapheme-aware cursor step for TextEditor. Returns the
/// UTF-8 byte offset of the cluster boundary one step forward (or
/// backward, when forward=false) from `byte_offset`. Skeleton uses a
/// naive single-codepoint step; the implementation slice consults the
/// Phase 1 UnicodeIndexMap for proper UAX #29 cluster boundaries (so
/// 👨‍👩‍👧‍👦 moves in one step, not 7).
std::size_t cluster_step(const std::string& text, std::size_t byte_offset,
                         bool forward = true);

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
