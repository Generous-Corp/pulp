// text_font_context.cpp — see header.

#include <pulp/canvas/text_font_context.hpp>

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/bundled_fonts.hpp>

#include "include/core/SkData.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"

#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"

#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#elif defined(__ANDROID__)
#include "include/ports/SkFontMgr_android.h"
#include "include/ports/SkFontScanner_FreeType.h"
#elif defined(__linux__)
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#endif

#include <mutex>
#include <vector>

namespace pulp::canvas {

namespace {

// Same platform-mgr matrix as `bundled_fonts.cpp::registration_font_manager`
// and `skia_canvas.cpp::get_font_manager`. We can't reach in to either TU's
// statics directly without leaking implementation, so it's mirrored here.
sk_sp<SkFontMgr> make_platform_font_mgr() {
#if defined(__APPLE__)
    return SkFontMgr_New_CoreText(nullptr);
#elif defined(_WIN32)
    return SkFontMgr_New_DirectWrite();
#elif defined(__ANDROID__)
    return SkFontMgr_New_Android(nullptr, SkFontScanner_Make_FreeType());
#elif defined(__linux__)
    return SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#else
    return nullptr;
#endif
}

// Platform-default color-emoji family for `register_platform_emoji_fallback`.
// Returns the family name to ask the platform font manager for.
const char* platform_emoji_family() {
#if defined(__APPLE__)
    return "Apple Color Emoji";
#elif defined(_WIN32)
    return "Segoe UI Emoji";
#elif defined(__ANDROID__)
    return "Noto Color Emoji";
#else
    return nullptr;
#endif
}

} // namespace

// ── TextFontContext ─────────────────────────────────────────────────────

TextFontContext::TextFontContext()
    : font_mgr_(make_platform_font_mgr()) {}

TextFontContext::~TextFontContext() = default;

std::shared_ptr<TextFontContext> TextFontContext::shared() {
    static std::shared_ptr<TextFontContext> instance =
        std::make_shared<TextFontContext>();
    return instance;
}

sk_sp<SkTypeface> TextFontContext::emoji_typeface() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return emoji_typeface_;
}

std::string TextFontContext::emoji_family_name() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return emoji_family_name_;
}

bool TextFontContext::has_emoji_typeface() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return static_cast<bool>(emoji_typeface_);
}

void TextFontContext::set_emoji_typeface(sk_sp<SkTypeface> face) {
    std::string family_name;
    if (face) {
        SkString sk_family;
        face->getFamilyName(&sk_family);
        family_name.assign(sk_family.c_str(), sk_family.size());
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        emoji_typeface_ = std::move(face);
        emoji_family_name_ = std::move(family_name);
        // Drop the cached FontCollection so the next `font_collection()`
        // call rebuilds it with the new asset manager wiring.
        font_collection_.reset();
        typeface_provider_.reset();
    }
    // Bump global generation so the static typeface cache in
    // skia_canvas.cpp and the measurement cache in text_shaper.cpp flush
    // their stale entries on the next lookup.
    (void)font_registration_generation();  // touch — see note below.
    // We can't write to the global counter from here without exposing a
    // setter; instead the public `register_emoji_fallback(...)` thunk
    // bumps it. set_emoji_typeface() is for callers who already manage
    // the generation themselves (e.g. AssetManager batching multiple
    // updates), so it deliberately doesn't auto-bump.
}

sk_sp<skia::textlayout::FontCollection> TextFontContext::font_collection() const {
    std::lock_guard<std::mutex> guard(mutex_);
    // Rebuild on first request OR when the global registration generation
    // has advanced (font_registration_generation may have moved due to a
    // register_font / register_emoji_fallback elsewhere).
    std::uint64_t now_gen = font_registration_generation();
    if (font_collection_ && collection_generation_ == now_gen) {
        return font_collection_;
    }

    auto provider = sk_make_sp<skia::textlayout::TypefaceFontProvider>();
    if (emoji_typeface_) {
        // Register under both the actual family name and a stable alias
        // ("Pulp Emoji") so callers can reference either string in their
        // `setFontFamilies` lists.
        provider->registerTypeface(emoji_typeface_);
        if (!emoji_family_name_.empty()) {
            provider->registerTypeface(emoji_typeface_,
                                       SkString("Pulp Emoji"));
        }
    }

    auto collection = sk_make_sp<skia::textlayout::FontCollection>();
    collection->setAssetFontManager(provider);
    if (font_mgr_) {
        // Tell SkParagraph what default families to consult for emoji
        // codepoints. The list is searched in order; we put the emoji
        // family first so `defaultEmojiFallback` picks our registered
        // typeface even when the platform mgr would have something.
        std::vector<SkString> defaults;
        if (!emoji_family_name_.empty()) {
            defaults.emplace_back(emoji_family_name_.c_str());
            defaults.emplace_back("Pulp Emoji");
        }
        if (defaults.empty()) {
            collection->setDefaultFontManager(font_mgr_);
        } else {
            collection->setDefaultFontManager(font_mgr_, defaults);
        }
    }
    collection->enableFontFallback();

    font_collection_ = collection;
    typeface_provider_ = provider;
    collection_generation_ = now_gen;
    return font_collection_;
}

sk_sp<SkTypeface> TextFontContext::typeface_for_run(FontRunRole role,
                                                    const sk_sp<SkTypeface>& primary) const {
    if (role == FontRunRole::Emoji) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (emoji_typeface_) return emoji_typeface_;
    }
    return primary;
}

// ── Public registration API ────────────────────────────────────────────

bool register_emoji_fallback(sk_sp<SkTypeface> face) {
    auto ctx = TextFontContext::shared();
    ctx->set_emoji_typeface(face);
    // Bump generation so downstream caches (skia_canvas.cpp typeface
    // cache, text_shaper.cpp segment cache, our own FontCollection
    // generation) all invalidate on the next use.
    bump_font_registration_generation();
    return true;
}

bool register_emoji_fallback(const std::string& family) {
    if (family.empty()) return false;
    SkFontStyle style = SkFontStyle::Normal();
    sk_sp<SkTypeface> face = match_registered_typeface(family, style);
    if (!face) {
        auto ctx = TextFontContext::shared();
        auto mgr = ctx->font_manager();
        if (mgr) {
            face = match_bundled_typeface(mgr.get(), family, style);
            if (!face) face = mgr->matchFamilyStyle(family.c_str(), style);
        }
    }
    if (!face) return false;
    return register_emoji_fallback(std::move(face));
}

bool register_platform_emoji_fallback() {
    const char* family = platform_emoji_family();
    if (!family) return false;
    auto ctx = TextFontContext::shared();
    if (ctx->has_emoji_typeface()) return true;
    auto mgr = ctx->font_manager();
    if (!mgr) return false;
    sk_sp<SkTypeface> face = mgr->matchFamilyStyle(family, SkFontStyle::Normal());
    if (!face) return false;
    return register_emoji_fallback(std::move(face));
}

// Forward-declared body for the embedded-Noto registrar. Defined in
// `noto_color_emoji.cpp` when PULP_BUNDLE_NOTO_COLOR_EMOJI is ON; falls
// back to a `false`-returning stub otherwise (see
// `noto_color_emoji_stub.cpp`).
bool register_bundled_noto_color_emoji();

bool register_best_available_emoji_fallback() {
    if (register_platform_emoji_fallback()) return true;
    return register_bundled_noto_color_emoji();
}

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
