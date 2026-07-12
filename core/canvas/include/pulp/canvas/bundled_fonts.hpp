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
// The embedded families are materialized through Skia on first lookup so
// `canvas.set_font("JetBrains Mono", ...)` succeeds even on machines that
// do not have JetBrains Mono installed system-wide.
//
// This header is only meaningful when `PULP_HAS_SKIA` is defined; without
// Skia, bundled-font registration is a no-op.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

#ifdef PULP_HAS_SKIA
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
class SkFontMgr;
class SkTypeface;
#endif

namespace pulp::canvas {

// ── Public font-registration API ─────────────────────────────────────────
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
/// Calling `register_font` repeatedly for one family is safe: each distinct
/// Skia typeface is retained so Regular/Bold/Italic faces can share the same
/// family key, while duplicate Skia identities are ignored. Successful
/// registrations bump the font-generation counter so downstream caches flush
/// on the next lookup. There is no `unregister_font` today; the expected
/// lifetime is "process".
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

// ── Async lifecycle + font validation ────────────────────────────────────

/// Async font lifecycle state.
enum class FontState : std::uint8_t {
    Pending,
    Loaded,
    Failed,
    Substituted,
};

/// Validate font bytes before handing them to Skia. Skia builds perform
/// structural sfnt validation of the header, table directory, required tables,
/// and critical table lengths; non-Skia builds accept any non-empty buffer
/// because there is no parser available on that lane.
bool validate_font_bytes(const std::uint8_t* data, std::size_t size);

/// Schedule a font registration from a URL or filesystem path and return a
/// future that resolves to the final `FontState`. Supported URL forms:
///   * `file:///abs/path/to/font.ttf` — decoded to an absolute path and
///     dispatched to `register_font_file` on a background worker.
///   * `/abs/path/to/font.ttf` — treated as a local path, same background
///     dispatch.
///   * `http://…` / `https://…` — resolved immediately to `Failed`
///     because this API does not fetch network resources. Callers that need
///     network fetches today should pre-download and call this entry point
///     with a `file://` URL.
///
/// The returned future never blocks the caller. Resolves to `Loaded` on
/// success, `Failed` on any error (unreadable file, Skia rejection, or
/// unsupported scheme). Dropping the future is safe; the worker reports its
/// result through a detached promise.
std::future<FontState> register_font_url(const std::string& url,
                                         const std::string& family_override = "");

// ── Compressed fonts + text boundary helpers ─────────────────────────────

/// Register a WOFF2-compressed font at runtime. Rejects malformed payloads
/// cleanly and validates the decompressed sfnt bytes before handing them to
/// Skia.
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
/// Pulp's MIT release stays free of large third-party blobs until a real
/// workload demands one.
bool register_font_woff2(const std::uint8_t* woff2_data, std::size_t size,
                         const std::string& family_override = "");

/// Runtime check for WOFF2 decoder availability. Returns `false` on builds
/// where no Brotli/woff2 implementation is linked, meaning
/// `register_font_woff2(...)` cannot succeed regardless of input. Callers can
/// use this to fall back to a pre-decoded TTF/OTF payload registered via
/// `register_font(...)` instead.
///
/// Available on every build (Skia or no Skia), so plugin startup can
/// branch on it without `#ifdef`s leaking into user code.
bool woff2_decoder_available() noexcept;

/// Grapheme-aware cursor step for TextEditor. Returns the UTF-8 byte offset
/// of the cluster boundary one step forward (or backward, when
/// `forward=false`) from `byte_offset`. Always available and implemented as a
/// UAX #29-lite UTF-8 walker, so common emoji ZWJ sequences, regional
/// indicator pairs, combining marks, variation selectors, virama chains,
/// skin-tone modifiers, and Hangul trailing jamo move as one cluster.
std::size_t cluster_step(const std::string& text, std::size_t byte_offset,
                         bool forward = true);

/// Locale-aware word break iterator. Returns the UTF-8 byte offset of the next
/// word boundary forward from `byte_offset` (or backward when
/// `forward=false`). `locale` is a BCP-47 tag ("en-US", "ja-JP", ...). Empty
/// locale falls back to ICU's root locale. When ICU's public BreakIterator
/// headers are not available, the implementation returns the fallback
/// `cluster_step()` result so callers can rely on the surface universally.
std::size_t word_break_step(const std::string& text,
                            std::size_t byte_offset,
                            const std::string& locale = "",
                            bool forward = true);

/// Find line-break opportunities in `text`. Returns UTF-8 byte offsets where
/// the renderer may wrap and always includes the trailing offset ==
/// text.size(). With ICU BreakIterator support this follows ICU's line-break
/// iterator; otherwise it returns ASCII space/tab boundaries plus the trailing
/// offset so the API still functions degraded.
std::vector<std::size_t> line_break_opportunities(const std::string& text,
                                                  const std::string& locale = "");

#ifdef PULP_HAS_SKIA

/// Process-wide platform font manager. Returns the CoreText / DirectWrite /
/// Android / FontConfig manager appropriate for the current OS, or nullptr
/// on platforms where no manager is available. Lazily constructed; the
/// same instance is returned for every call.
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
/// The bundle currently ships only Regular/Upright faces. Off-style requests
/// return `nullptr` so the caller can continue to registered or platform font
/// fallback for bold, italic, or otherwise unavailable variants.
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

/// Reports whether `face` exposes an OpenType `wght` (weight) variation
/// axis, and if so writes the axis min / max / default design values.
/// A static (non-variable) face, or a variable face with no `wght`
/// axis, returns `false` and leaves the out-params untouched.
///
/// The resolver uses this to instantiate a registered variable font at
/// the caller's requested CSS `font-weight` (via `SkTypeface::makeClone`)
/// instead of dropping to a heavier system fallback when the requested
/// weight is outside the static-face matching tolerance. Generalized:
/// any imported / registered variable font (e.g. Funnel Display,
/// Clash Grotesk Variable) honours `font-weight` across its full axis.
bool face_wght_axis(const SkTypeface* face,
                    float& out_min, float& out_max, float& out_default);

/// A single user-registered typeface paired with the family name it was
/// registered under (the `family_override` passed to `register_font`).
struct RegisteredTypeface {
    std::string       family;
    sk_sp<SkTypeface> typeface;
};

/// Snapshot of every typeface registered via the public `register_font` /
/// `register_font_file` API, keyed by registered family name. Taken under
/// the registry lock and returned by value so callers (notably
/// `TextFontContext::font_collection()`) can wire user fonts into the
/// SkParagraph `TypefaceFontProvider` WITHOUT holding the registry lock
/// while they touch Skia — avoiding lock-order coupling between the font
/// registry mutex and Skia's internal locks.
///
/// Why this exists: `register_font` populates a private registry that the
/// `FontResolver` / `make_font` (Canvas2D fillText) path consults, but the
/// SkParagraph label-rendering path resolves fonts through its own
/// `FontCollection`. Without bridging the two, an imported design's
/// registered fonts render only on the fillText path and silently fall
/// back to a system face for every Label (which routes through
/// SkParagraph). This snapshot is the bridge.
std::vector<RegisteredTypeface> registered_typefaces_snapshot();

/// Snapshot of every BUNDLED typeface (the .ttfs compiled into the binary),
/// keyed by the family name the face reports. Same bridging purpose as
/// `registered_typefaces_snapshot()`, but for the built-in bundle: without it
/// the SkParagraph `FontCollection` can only see the platform font manager,
/// which on a browser build (custom-empty manager, no system font DB) knows
/// no families at all and renders every Label with zero glyphs.
std::vector<RegisteredTypeface> bundled_typefaces_snapshot();

/// True when the platform font manager exposes a usable system font database —
/// defined as "its DEFAULT face can draw a Latin glyph".
///
/// This is FALSE on Emscripten. `SkFontMgr_New_Custom_Empty()` reports one
/// family holding one typeface and hands that non-null typeface back from
/// `matchFamilyStyle(...)` — but the face has NO GLYPHS, so every string
/// measures at 0.0 advance and paints nothing. Neither a null check nor a
/// family count can tell that apart from a real font DB (verified in a browser:
/// `countFamilies() == 1`, family 0 `count() == 1`); asking the face for a
/// glyph can. Cascades must skip the platform rung entirely — and fall back to
/// the bundle — when this returns false.
bool platform_font_db_usable();

/// Last-resort bundled face for the default cascade (empty family stack, or a
/// generic family like `sans-serif` that nothing in the bundle advertises).
/// Prefers "Inter"; otherwise returns the first bundled face. Style is
/// deliberately ignored — a Regular face with glyphs beats a perfectly-styled
/// face with none. Returns nullptr when no font manager is available or the
/// bundle is empty.
sk_sp<SkTypeface> bundled_fallback_typeface();

/// Number of embedded bundled fonts compiled in. Useful for tests that want
/// to assert "the build actually pulled in some .ttfs". Always returns the
/// embedded count regardless of whether `match_bundled_typeface` has been
/// called.
std::size_t bundled_font_count();

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
