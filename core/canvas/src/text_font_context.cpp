// text_font_context.cpp — see header.

#include <pulp/canvas/text_font_context.hpp>

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/bundled_fonts.hpp>

#include "include/core/SkData.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontParameters.h"
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
    // Read the immutable family name outside the lock — sk_sp is a
    // local refbump and `getFamilyName` reads the immutable name table.
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
    // Invalidate downstream caches (skia_canvas typeface cache,
    // text_shaper segment cache).
    bump_font_registration_generation();
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

    // Bridge user-registered fonts into the SkParagraph font collection.
    // Without this, fonts registered via `register_font` /
    // `register_font_file` (e.g. an imported Figma design's
    // "Funnel Display" / "Clash Grotesk Variable") resolve only
    // on the Canvas2D fillText / FontResolver path; every Label renders
    // through SkParagraph, which previously only saw the platform
    // SkFontMgr and silently fell back to a system face. Register each
    // user typeface under the family alias it was registered with so
    // `TextStyle::setFontFamilies({"Funnel Display"})` matches.
    //
    // Variable fonts: SkParagraph does NOT instance a `wght` axis from the
    // requested SkFontStyle — it picks the closest STATIC face by
    // CSS-weight match. A variable font is registered once at its default
    // instance (Funnel Display defaults to wght 300 — "Light"), so a
    // request for 400/700 would land on the single 300 instance for every
    // weight. To honour distinct CSS weights we pre-bake one clone per
    // common weight step across the axis range and register each under the
    // same family alias; `TypefaceFontProvider::matchStyle` then selects
    // the closest baked instance for the requested weight. Cloning sets
    // the variation design position, and the clone reports the instanced
    // weight via `fontStyle()`, which is what the provider matches on.
    for (const auto& reg : registered_typefaces_snapshot()) {
        if (!reg.typeface || reg.family.empty()) continue;
        const SkString alias(reg.family.c_str());

        float wmin = 0, wmax = 0, wdef = 0;
        if (face_wght_axis(reg.typeface.get(), wmin, wmax, wdef)) {
            // Variable face: bake instances at the standard CSS weight
            // ladder, clamped to the axis range. Dedupe so an axis like
            // [300..800] doesn't register a 200 clone, and a narrow axis
            // doesn't register duplicates at its clamped endpoints.
            static constexpr float kWeights[] = {
                100, 200, 300, 400, 500, 600, 700, 800, 900};
            constexpr SkFourByteTag kWght = SkSetFourByteTag('w', 'g', 'h', 't');
            float last = -1.0f;
            bool any = false;
            for (float w : kWeights) {
                float v = w;
                if (v < wmin) v = wmin;
                if (v > wmax) v = wmax;
                if (v == last) continue;  // clamped duplicate
                last = v;
                SkFontArguments::VariationPosition::Coordinate coord{kWght, v};
                SkFontArguments args;
                SkFontArguments::VariationPosition pos{&coord, 1};
                args.setVariationDesignPosition(pos);
                if (auto clone = reg.typeface->makeClone(args)) {
                    provider->registerTypeface(clone, alias);
                    any = true;
                }
            }
            // Cloning unsupported by this Skia build — register the base
            // face so at least the default instance is reachable.
            if (!any) provider->registerTypeface(reg.typeface, alias);
        } else {
            // Static face — register as-is under its alias.
            provider->registerTypeface(reg.typeface, alias);
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
    // `set_emoji_typeface` invalidates downstream caches; no extra bump
    // needed here.
    TextFontContext::shared()->set_emoji_typeface(std::move(face));
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
