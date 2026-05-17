// text_font_context.hpp — owns the per-context typeface registry used by
// Canvas2D `fillText/strokeText` (via SkShaper fallback runs) and by the
// `TextShaper` measurement path (via SkParagraph FontCollection).
//
// Why this exists:
//   - HarfBuzz can only shape a multi-codepoint emoji cluster (ZWJ, flag,
//     keycap, modifier, tag) if the whole cluster lands on one font run.
//     `emoji_segmenter` pre-segments the text, and `TextFontContext`
//     supplies the typeface for each role (Default vs Emoji).
//   - A process-wide singleton would let one plugin overwrite another's
//     fallback registration in the same host process. Instead each
//     `AssetManager` / `Canvas` owns a context; a `shared()` default is
//     available for plugin authors who haven't wired their own.
//   - `SkParagraph::ParagraphBuilder` needs a `FontCollection` for label
//     measurement; we cache one per context so it isn't reconstructed
//     every call.
//
// This header is Skia-aware and intentionally only ships under
// `PULP_HAS_SKIA`. Non-Skia callers should use the API stubs in
// `font_registry_stubs.cpp`.

#pragma once

#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/emoji_segmenter.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef PULP_HAS_SKIA

#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTypeface.h"

namespace skia { namespace textlayout {
class FontCollection;
class TypefaceFontProvider;
} }

namespace pulp::canvas {

class TextFontContext {
public:
    TextFontContext();
    ~TextFontContext();

    TextFontContext(const TextFontContext&) = delete;
    TextFontContext& operator=(const TextFontContext&) = delete;

    /// Default, shared context. Plugins that haven't wired their own
    /// `AssetManager`-owned context fall back to this. It is constructed
    /// lazily on first use and lives until process exit.
    static std::shared_ptr<TextFontContext> shared();

    /// Platform `SkFontMgr` (CoreText / DirectWrite / fontconfig /
    /// Android). May be `nullptr` on platforms without one.
    sk_sp<SkFontMgr> font_manager() const { return font_mgr_; }

    /// `FontCollection` used by `ParagraphBuilder`. Built lazily; calls
    /// rebuild themselves when the typeface registry has advanced.
    sk_sp<skia::textlayout::FontCollection> font_collection() const;

    /// The family name used in `TextStyle::setFontFamilies({primary,
    /// emoji_family_name()})` so SkParagraph routes emoji clusters to
    /// the registered emoji typeface. Empty string if no emoji typeface
    /// has been registered.
    std::string emoji_family_name() const;

    /// Set / replace the color-emoji typeface. Pass `nullptr` to clear.
    /// Bumps `font_registration_generation()`.
    void set_emoji_typeface(sk_sp<SkTypeface> face);

    /// The currently-registered color-emoji typeface. May be `nullptr`.
    sk_sp<SkTypeface> emoji_typeface() const;

    /// Has an emoji typeface been registered (platform-discovered or
    /// user-provided)?
    bool has_emoji_typeface() const;

    /// Resolve a typeface for one segmenter-emitted run. For
    /// `FontRunRole::Emoji` we return the registered emoji typeface; if
    /// none is registered we return `primary` so the run still produces
    /// glyph advance (tofus, but doesn't crash). For `FontRunRole::
    /// Default` we return `primary` unchanged.
    sk_sp<SkTypeface> typeface_for_run(FontRunRole role,
                                       const sk_sp<SkTypeface>& primary) const;

private:
    sk_sp<SkFontMgr> font_mgr_;
    sk_sp<SkTypeface> emoji_typeface_;
    std::string emoji_family_name_;

    mutable std::mutex mutex_;
    mutable sk_sp<skia::textlayout::FontCollection> font_collection_;
    mutable sk_sp<skia::textlayout::TypefaceFontProvider> typeface_provider_;
    mutable std::uint64_t collection_generation_ = 0;
};

// ── Public registration API ────────────────────────────────────────────

/// Register a color-emoji typeface globally (writes into
/// `TextFontContext::shared()`). Bumps the font-registration generation
/// so all caches refresh. Pass `nullptr` to clear.
bool register_emoji_fallback(sk_sp<SkTypeface> face);

/// Convenience overload: look up `family` via the registered-fonts /
/// bundled-fonts / platform `SkFontMgr` cascade and register the
/// resolved typeface as the color-emoji fallback. Returns `true` on
/// success, `false` if no typeface for that family was found.
bool register_emoji_fallback(const std::string& family);

/// Discover and register the platform's built-in color-emoji typeface
/// ("Apple Color Emoji" on macOS, "Segoe UI Emoji" on Windows, system
/// "Noto Color Emoji" on Android). Idempotent. Returns `true` if a
/// typeface was registered, `false` otherwise — call
/// `register_bundled_noto_color_emoji()` next if you want a bundled
/// fallback for Linux / headless / CI.
bool register_platform_emoji_fallback();

/// Register the embedded Noto Color Emoji typeface that the build
/// compiled in via `PULP_BUNDLE_NOTO_COLOR_EMOJI=ON`. Returns `true` on
/// success, `false` if not bundled or Skia rejected the bytes.
/// Idempotent. Use this for deterministic CI goldens and Linux fallback.
bool register_bundled_noto_color_emoji();

/// Convenience: register the best-available emoji typeface. Tries
/// `register_platform_emoji_fallback()` first; falls back to the
/// bundled Noto. Returns `true` if any emoji typeface ended up
/// registered.
bool register_best_available_emoji_fallback();

} // namespace pulp::canvas

#else  // PULP_HAS_SKIA

namespace pulp::canvas {
// Non-Skia stubs so plugin startup code can call these unconditionally.
inline bool register_emoji_fallback(...) { return false; }
inline bool register_platform_emoji_fallback() { return false; }
inline bool register_bundled_noto_color_emoji() { return false; }
inline bool register_best_available_emoji_fallback() { return false; }
} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
