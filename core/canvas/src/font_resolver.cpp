// font_resolver.cpp
//
// Canonical resolver implementation. It is the single entry point for
// font-family resolution and delegates to the registered-font map, bundled
// fonts, and the platform font manager while emitting fallback traces.
//
// The header is Skia-free for non-GPU consumers; this .cpp is the
// translation unit that bridges to Skia under `PULP_HAS_SKIA`.

#include "pulp/canvas/font_resolver.hpp"
#include "pulp/canvas/font_scope.hpp"
#include "pulp/canvas/bundled_fonts.hpp"
#include "pulp/canvas/font_flight_recorder.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <iterator>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#ifdef PULP_HAS_SKIA
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkFontArguments.h"
#if defined(__APPLE__)
#include <CoreText/CoreText.h>
#include "include/ports/SkTypeface_mac.h"
#endif
#endif

namespace pulp::canvas {

namespace {
// Generic-keyword mappings a test stood in; see set_css_generic_family_override.
std::atomic<bool> g_generic_overrides_present{false};
std::mutex g_generic_overrides_mutex;
std::unordered_map<std::string, std::string>& generic_overrides() {
    static std::unordered_map<std::string, std::string> overrides;
    return overrides;
}
} // namespace

void set_css_generic_family_override(const std::string& keyword, const std::string& family) {
    std::string lower = keyword;
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::lock_guard<std::mutex> lock(g_generic_overrides_mutex);
    auto& overrides = generic_overrides();
    if (family.empty())
        overrides.erase(lower);
    else
        overrides[lower] = family;
    g_generic_overrides_present.store(!overrides.empty(), std::memory_order_release);
}

std::string platform_family_for_css_generic(const std::string& css_family) {
    if (g_generic_overrides_present.load(std::memory_order_acquire)) {
        std::string lower = css_family;
        for (auto& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::lock_guard<std::mutex> lock(g_generic_overrides_mutex);
        const auto& overrides = generic_overrides();
        if (auto it = overrides.find(lower); it != overrides.end())
            return it->second;
    }
#if defined(__APPLE__)
    // Chromium's macOS defaults for each generic family, as reported by
    // CSS.getPlatformFontsForNode. Keywords Chromium does not support on this
    // platform (ui-monospace, ui-sans-serif, -apple-system, ...) are left
    // unmapped so the list walk moves on to the next entry, as it does there.
    struct Entry {
        const char* keyword;
        const char* family;
    };
    static constexpr Entry kGeneric[] = {
        {"system-ui", ".AppleSystemUIFont"},
        {"blinkmacsystemfont", ".AppleSystemUIFont"},
        {"sans-serif", "Helvetica"},
        {"serif", "Times"},
        {"monospace", "Menlo"},
        {"cursive", "Apple Chancery"},
        {"fantasy", "Papyrus"},
        {"math", "STIX Two Math"},
    };
    std::string lower = css_family;
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& entry : kGeneric)
        if (lower == entry.keyword)
            return entry.family;
#else
    (void)css_family;
#endif
    return {};
}

namespace {
std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

CssFontFamily parse_css_font_family(std::string_view entry) {
    constexpr std::string_view kSpace = " \t\n\r\f\v";
    CssFontFamily out;
    const size_t lo = entry.find_first_not_of(kSpace);
    if (lo == std::string_view::npos)
        return out;
    const size_t hi = entry.find_last_not_of(kSpace);
    entry = entry.substr(lo, hi - lo + 1);
    if (entry.size() >= 2 && (entry.front() == '"' || entry.front() == '\'') &&
        entry.back() == entry.front()) {
        out.quoted = true;
        entry = entry.substr(1, entry.size() - 2);
    }
    out.name = std::string(entry);
    return out;
}

std::vector<std::string> split_css_font_family_list(std::string_view list) {
    constexpr std::string_view kSpace = " \t\n\r\f\v";
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= list.size()) {
        const size_t comma = list.find(',', pos);
        std::string_view entry =
            list.substr(pos, (comma == std::string_view::npos ? list.size() : comma) - pos);
        const size_t lo = entry.find_first_not_of(kSpace);
        if (lo != std::string_view::npos) {
            const size_t hi = entry.find_last_not_of(kSpace);
            entry = entry.substr(lo, hi - lo + 1);
            if (!parse_css_font_family(entry).name.empty())
                out.emplace_back(entry);
        }
        if (comma == std::string_view::npos)
            break;
        pos = comma + 1;
    }
    return out;
}

std::string platform_family_for_css_family(std::string_view entry) {
    CssFontFamily family = parse_css_font_family(entry);
    if (!family.quoted) {
        if (auto generic = platform_family_for_css_generic(family.name); !generic.empty())
            return generic;
    }
    return std::move(family.name);
}

bool platform_face_is_family(std::string_view platform_family, std::string_view actual_family) {
    if (platform_family.empty() || actual_family.empty())
        return false;
    const std::string want = lower_ascii(platform_family);
    const std::string got = lower_ascii(actual_family);
    if (want.find(got) != std::string::npos || got.find(want) != std::string::npos)
        return true;
    return want.front() == '.' && got.front() == '.';
}

const char* to_string(FallbackOrigin o) noexcept {
    switch (o) {
        case FallbackOrigin::ScopeView:    return "scope-view";
        case FallbackOrigin::ScopePlugin:  return "scope-plugin";
        case FallbackOrigin::ScopeGlobal:  return "scope-global";
        case FallbackOrigin::Bundled:      return "bundled";
        case FallbackOrigin::Platform:     return "platform";
        case FallbackOrigin::PlatformChar: return "platform-char";
        case FallbackOrigin::Synthetic:    return "synthetic";
        case FallbackOrigin::NotFound:     return "not-found";
    }
    return "?";
}

// ── FontResolver::Impl ───────────────────────────────────────────────────

// The resolver cache is LRU-with-cap so variable-font animations
// (60fps `wght` interpolation produces 60
// distinct keys/sec) don't grow unbounded. Default cap is 256 entries
// — a small static UI has ~10-30 cached faces, animations spike then
// settle, and the LRU eviction keeps the working set in cache.
struct FontResolver::Impl {
    mutable std::mutex mtx;
    // hash → cache entry. The list iterator points at this entry's
    // position in the LRU order list; hit -> move-to-back, evict ->
    // pop-front. `value.first` mirrors the map key for fast eviction.
    struct Entry {
        ResolvedFont                                                resolved;
        std::list<std::size_t>::iterator                            lru_pos;
    };
    std::unordered_map<std::size_t, Entry> cache;
    std::list<std::size_t>                 lru_order;  // oldest at front
    std::size_t                            capacity = 256;
};

FontResolver::FontResolver() : impl_(std::make_unique<Impl>()) {}
FontResolver::~FontResolver() = default;

FontResolver& FontResolver::instance() {
    static FontResolver inst;
    return inst;
}

void FontResolver::clear_cache() {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->cache.clear();
    impl_->lru_order.clear();
}

void FontResolver::set_cache_capacity(std::size_t entries) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->capacity = entries;
    if (impl_->capacity == 0) return;  // 0 disables cap (legacy mode)
    while (impl_->cache.size() > impl_->capacity) {
        std::size_t oldest = impl_->lru_order.front();
        impl_->lru_order.pop_front();
        impl_->cache.erase(oldest);
    }
}

std::size_t FontResolver::cache_capacity() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->capacity;
}

std::size_t FontResolver::cache_size() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->cache.size();
}

#ifdef PULP_HAS_SKIA

namespace {

// `platform_font_manager()` lives in bundled_fonts.cpp and is exported via
// bundled_fonts.hpp — same instance everywhere in `pulp::canvas`.

SkFontStyle to_sk_style(const FontOptions& opts) {
    int sk_weight = static_cast<int>(opts.weight);
    int sk_width  = SkFontStyle::kNormal_Width;
    if      (opts.width <= 56.25f) sk_width = SkFontStyle::kUltraCondensed_Width;
    else if (opts.width <= 68.75f) sk_width = SkFontStyle::kExtraCondensed_Width;
    else if (opts.width <= 81.25f) sk_width = SkFontStyle::kCondensed_Width;
    else if (opts.width <= 93.75f) sk_width = SkFontStyle::kSemiCondensed_Width;
    else if (opts.width <= 106.25f) sk_width = SkFontStyle::kNormal_Width;
    else if (opts.width <= 118.75f) sk_width = SkFontStyle::kSemiExpanded_Width;
    else if (opts.width <= 137.5f)  sk_width = SkFontStyle::kExpanded_Width;
    else if (opts.width <= 175.0f)  sk_width = SkFontStyle::kExtraExpanded_Width;
    else                            sk_width = SkFontStyle::kUltraExpanded_Width;

    SkFontStyle::Slant sk_slant = SkFontStyle::kUpright_Slant;
    if (opts.slant == FontSlant::Italic)  sk_slant = SkFontStyle::kItalic_Slant;
    if (opts.slant == FontSlant::Oblique) sk_slant = SkFontStyle::kOblique_Slant;

    return SkFontStyle(sk_weight, sk_width, sk_slant);
}

#if defined(__APPLE__)
// Whether `css_family` is a keyword for the platform's UI font.
bool names_system_ui_font(const std::string& css_family) {
    std::string lower = css_family;
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower == "system-ui" || lower == "blinkmacsystemfont";
}

// CoreText's weight trait for a CSS weight: Apple's NSFontWeight values at
// each hundred, interpolated between them, as Skia's own mapping does.
CGFloat ct_weight_for_css_weight(int css_weight) {
    static constexpr CGFloat kAtHundreds[] = {-1.00, -0.80, -0.60, -0.40, 0.00, 0.23,
                                              0.30,  0.40,  0.56,  0.62,  1.00};
    const int clamped = std::clamp(css_weight, 0, 1000);
    const int lo = clamped / 100;
    if (lo >= 10)
        return kAtHundreds[10];
    const CGFloat t = static_cast<CGFloat>(clamped - lo * 100) / 100.0;
    return kAtHundreds[lo] + (kAtHundreds[lo + 1] - kAtHundreds[lo]) * t;
}

// The system UI font at `style`. Its family name (".AppleSystemUIFont") is
// private, and CoreText does not reliably honour a lookup by it: after a
// request at a weight between its named instances, later lookups by the name
// in the same process return nothing. CTFontCreateUIFontForLanguage is the
// supported way in, and its faces report the same `.SFNS-*` names the
// browser does.
sk_sp<SkTypeface> system_ui_typeface(SkFontStyle style) {
    CTFontRef base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 0.0, nullptr);
    if (!base)
        return nullptr;
    CTFontDescriptorRef base_descriptor = CTFontCopyFontDescriptor(base);
    CFRelease(base);
    if (!base_descriptor)
        return nullptr;

    const CGFloat weight = ct_weight_for_css_weight(style.weight());
    CFNumberRef weight_number = CFNumberCreate(nullptr, kCFNumberCGFloatType, &weight);
    const CTFontSymbolicTraits symbolic =
        style.slant() == SkFontStyle::kUpright_Slant ? 0 : kCTFontItalicTrait;
    CFNumberRef symbolic_number = CFNumberCreate(nullptr, kCFNumberSInt32Type, &symbolic);
    const void* trait_keys[] = {kCTFontWeightTrait, kCTFontSymbolicTrait};
    const void* trait_values[] = {weight_number, symbolic_number};
    CFDictionaryRef traits =
        CFDictionaryCreate(nullptr, trait_keys, trait_values, 2, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    const void* attribute_keys[] = {kCTFontTraitsAttribute};
    const void* attribute_values[] = {traits};
    CFDictionaryRef attributes =
        CFDictionaryCreate(nullptr, attribute_keys, attribute_values, 1,
                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CTFontDescriptorRef descriptor =
        CTFontDescriptorCreateCopyWithAttributes(base_descriptor, attributes);
    CFRelease(attributes);
    CFRelease(traits);
    CFRelease(symbolic_number);
    CFRelease(weight_number);
    CFRelease(base_descriptor);
    if (!descriptor)
        return nullptr;

    CTFontRef font = CTFontCreateWithFontDescriptor(descriptor, 0.0, nullptr);
    CFRelease(descriptor);
    if (!font)
        return nullptr;
    sk_sp<SkTypeface> typeface = SkMakeTypefaceFromCTFont(font);
    CFRelease(font);
    return typeface;
}
#endif

// Keep this TU self-contained by using the public registered-font,
// bundled-font, and SkFontMgr paths directly.

ResolvedFont resolve_one_family(const std::string& entry, const FontOptions& opts,
                                SkFontStyle sk_style, sk_sp<SkFontMgr> mgr,
                                std::vector<FallbackTraceStep>& trace) {
    // A family_stack entry may carry its CSS quotes; a quoted entry is a
    // literal family name and never maps as a generic keyword.
    const CssFontFamily css = parse_css_font_family(entry);
    const std::string& family = css.name;
    ResolvedFont r;
    r.scope = opts.scope;
    r.generation = merged_generation_for(opts.scope);
    // AA / hinting policy travels alongside the resolved face.
    r.aa_mode = opts.aa_mode;
    r.hinting_mode = opts.hinting_mode;
    // Color-font policy travels alongside the resolved face.
    r.color_font_mode = opts.color_font_mode;

    // 1) Registered. Current storage consults the global registered map.
    if (auto tf = match_registered_typeface(family, sk_style)) {
        SkString actual;
        tf->getFamilyName(&actual);
        r.typeface = std::move(tf);
        r.actual_family = std::string(actual.c_str(), actual.size());
        r.origin = FallbackOrigin::ScopeGlobal;
        trace.push_back({family, FallbackOrigin::ScopeGlobal, true,
                         r.actual_family, ""});
        return r;
    }
    trace.push_back({family, FallbackOrigin::ScopeGlobal, false, "", "no match"});

    // 2) Bundled.
    if (mgr) {
        if (auto tf = match_bundled_typeface(mgr.get(), family, sk_style)) {
            SkString actual;
            tf->getFamilyName(&actual);
            r.typeface = std::move(tf);
            r.actual_family = std::string(actual.c_str(), actual.size());
            r.origin = FallbackOrigin::Bundled;
            trace.push_back({family, FallbackOrigin::Bundled, true,
                             r.actual_family, ""});
            return r;
        }
        trace.push_back({family, FallbackOrigin::Bundled, false, "", "no match"});
    }

    // 3) Platform.
    //
    // Skipped entirely when the platform font manager exposes no real font
    // database (browser builds use SkFontMgr_New_Custom_Empty). That manager
    // still returns a NON-NULL default typeface from `matchFamilyStyle` — but
    // one with no glyphs, which measures every string at 0.0 advance and paints
    // nothing. Trusting it here is what made every Label in the browser demo
    // invisible; the miss must fall through to the bundled last-resort below.
    if (mgr && platform_font_db_usable()) {
        // A generic keyword names a platform family rather than being one.
        const std::string generic =
            css.quoted ? std::string{} : platform_family_for_css_generic(family);
        const std::string& platform_family = generic.empty() ? family : generic;
        sk_sp<SkTypeface> tf;
        bool system_ui = false;
#if defined(__APPLE__)
        // Widths other than normal keep the name lookup, which maps them.
        if (!css.quoted && names_system_ui_font(family) &&
            sk_style.width() == SkFontStyle::kNormal_Width) {
            tf = system_ui_typeface(sk_style);
            system_ui = tf != nullptr;
        }
#endif
        if (!tf)
            tf = mgr->matchFamilyStyle(platform_family.c_str(), sk_style);
        if (tf) {
            SkString actual;
            tf->getFamilyName(&actual);

            // Honor v1's "did we get a generic fallback?" check: if the
            // platform returned a default face whose name doesn't relate
            // to the requested family, mark it as failed so the caller
            // can try the next family in the stack. A generic keyword is
            // held to the family it maps to: when that family is missing
            // the face is the matcher's default, not the generic's.
            const bool name_overlap =
                system_ui || platform_face_is_family(platform_family,
                                                     std::string(actual.c_str(), actual.size()));

            // In Deterministic mode we refuse any platform face whose
            // name doesn't overlap the requested family — bug-compatible
            // with the legacy split_font_family_list walker.
            if (opts.fallback_mode == FallbackMode::Deterministic && !name_overlap) {
                trace.push_back({family, FallbackOrigin::Platform, false,
                                 std::string(actual.c_str(), actual.size()),
                                 "deterministic: rejected platform default fallback"});
            } else {
                r.typeface = std::move(tf);
                r.actual_family = std::string(actual.c_str(), actual.size());
                r.origin = FallbackOrigin::Platform;
                trace.push_back({family, FallbackOrigin::Platform, true, r.actual_family,
                                 system_ui      ? "system UI font"
                                 : name_overlap ? ""
                                                : "platform default (name does not overlap)"});
                return r;
            }
        } else {
            trace.push_back({family, FallbackOrigin::Platform, false, "", "no match"});
        }
    }

    r.origin = FallbackOrigin::NotFound;
    return r;
}

} // namespace

// Insert `resolved` into the LRU cache under `key`. Promotes existing
// entries to the back. Evicts the oldest entry when the cap is exceeded.
// Caller must hold `impl.mtx`.
static void cache_put_locked(FontResolver::Impl& impl,
                             std::size_t key,
                             const ResolvedFont& resolved) {
    auto it = impl.cache.find(key);
    if (it != impl.cache.end()) {
        impl.lru_order.splice(impl.lru_order.end(), impl.lru_order,
                              it->second.lru_pos);
        it->second.resolved = resolved;
        return;
    }
    impl.lru_order.push_back(key);
    auto pos = std::prev(impl.lru_order.end());
    impl.cache.emplace(key, FontResolver::Impl::Entry{resolved, pos});
    if (impl.capacity > 0 && impl.cache.size() > impl.capacity) {
        std::size_t oldest = impl.lru_order.front();
        impl.lru_order.pop_front();
        impl.cache.erase(oldest);
    }
}

ResolvedFont FontResolver::resolve_family_list(const FontOptions& options) {
    // Record every successful resolve path, not just the family-stack branch.
    auto record_event = [](const std::string& requested, const ResolvedFont& r) {
        if (!r.resolved()) return;
        FontFlightRecorder::instance().record_fallback({
            /*requested_family*/ requested,
            /*selected_family */ r.actual_family,
            /*origin*/   static_cast<std::uint8_t>(r.origin),
            /*generation*/ r.generation,
            /*sequence*/ 0,
        });
    };
    const std::string primary_requested = options.family_stack.empty()
        ? std::string("<default>") : options.family_stack.front();

    // Cache lookup. Generation check happens at use site (callers compare
    // `resolved.generation` against `merged_generation_for(scope)`).
    const std::size_t key = options.hash();
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        auto it = impl_->cache.find(key);
        if (it != impl_->cache.end()
            && it->second.resolved.generation == merged_generation_for(options.scope)) {
            // LRU hit — promote to back.
            impl_->lru_order.splice(impl_->lru_order.end(),
                                     impl_->lru_order,
                                     it->second.lru_pos);
            record_event(primary_requested, it->second.resolved);
            return it->second.resolved;
        }
    }

    SkFontStyle sk_style = to_sk_style(options);
    sk_sp<SkFontMgr> mgr = platform_font_manager();

    std::vector<FallbackTraceStep> trace;
    ResolvedFont resolved;
    resolved.scope = options.scope;
    resolved.generation = merged_generation_for(options.scope);
    // AA / hinting policy is carried straight out of FontOptions onto the
    // ResolvedFont so paint paths derive Skia flags from one canonical source.
    // See `sk_edging_for` / `sk_hinting_for` in font_resolver.hpp for the enum
    // translation.
    resolved.aa_mode = options.aa_mode;
    resolved.hinting_mode = options.hinting_mode;
    // Color-font policy travels onto the ResolvedFont so paint paths can
    // branch on color_font_active().
    resolved.color_font_mode = options.color_font_mode;

    // After a face resolves, if the caller requested variation axes
    // (`font-variation-settings`), clone the typeface with those axes applied
    // so the cache holds one entry per distinct axis instance (the FontOptions
    // hash already keys on variation_axes — this just applies them).
    constexpr SkFourByteTag kWghtTag = SkSetFourByteTag('w', 'g', 'h', 't');
    auto apply_variation_axes = [&](sk_sp<SkTypeface> face) -> sk_sp<SkTypeface> {
        if (!face) return face;

        std::vector<SkFontArguments::VariationPosition::Coordinate> coords;
        bool caller_set_wght = false;
        coords.reserve(options.variation_axes.size() + 1);
        for (const auto& axis : options.variation_axes) {
            coords.push_back({static_cast<SkFourByteTag>(axis.tag), axis.value});
            if (static_cast<SkFourByteTag>(axis.tag) == kWghtTag)
                caller_set_wght = true;
        }

        // A variable font registered/resolved at its default instance
        // (e.g. Funnel Display @ 400) must honor `font-weight: 700` by
        // instancing its `wght` axis, not by falling back to a heavier
        // system face. Only synthesize when:
        //   * the face actually exposes a `wght` axis,
        //   * the caller didn't already pin `wght` via
        //     `font-variation-settings` (explicit axes win), and
        //   * the requested weight differs from the axis default (no-op
        //     clone otherwise).
        // The requested weight is clamped to the axis range so an
        // out-of-range request renders at the nearest real instance.
        float wmin = 0, wmax = 0, wdef = 0;
        if (!caller_set_wght && options.weight > 0.0f &&
            face_wght_axis(face.get(), wmin, wmax, wdef)) {
            float want = options.weight;
            if (want < wmin) want = wmin;
            if (want > wmax) want = wmax;
            // A face the platform already matched at the requested weight
            // (a named instance such as SF's Bold) needs no clone; cloning it
            // renames it to a synthetic instance the browser never reports.
            const bool already_at_weight =
                std::abs(static_cast<float>(face->fontStyle().weight()) - want) < 0.5f;
            if (want != wdef && !already_at_weight) {
                coords.push_back({kWghtTag, want});
            }
        }

        if (coords.empty()) return face;
        SkFontArguments args;
        SkFontArguments::VariationPosition pos{
            coords.data(), static_cast<int>(coords.size())
        };
        args.setVariationDesignPosition(pos);
        if (auto clone = face->makeClone(args)) return clone;
        // Face has no variation axes — return the base typeface;
        // a SynthesisTrace entry is emitted by the caller below.
        return face;
    };

    for (const auto& family : options.family_stack) {
        ResolvedFont r = resolve_one_family(family, options, sk_style, mgr, trace);
        if (r.resolved() && r.has_typeface()) {
            r.typeface = apply_variation_axes(std::move(r.typeface));
            r.trace = std::move(trace);
            resolved = std::move(r);
            record_event(family, resolved);
            std::lock_guard<std::mutex> lock(impl_->mtx);
            cache_put_locked(*impl_, key, resolved);
            return resolved;
        }
    }

    // Empty stack or nothing matched: last-resort platform default.
    if (mgr && platform_font_db_usable()) {
        if (auto tf = mgr->matchFamilyStyle(nullptr, sk_style)) {
            SkString actual;
            tf->getFamilyName(&actual);
            resolved.typeface = apply_variation_axes(std::move(tf));
            resolved.actual_family = std::string(actual.c_str(), actual.size());
            resolved.origin = FallbackOrigin::Platform;
            trace.push_back({"<default>", FallbackOrigin::Platform, true,
                             resolved.actual_family, "last-resort default"});
        }
    }

    // No system font database (browser): the default cascade — an empty family
    // stack, or a generic family like `sans-serif` that nothing in the bundle
    // advertises — has nowhere left to go. Fall back to a BUNDLED face rather
    // than to the empty manager's glyph-less default, which would silently
    // measure every string at zero width and draw nothing at all.
    if (!resolved.has_typeface()) {
        if (auto tf = bundled_fallback_typeface()) {
            SkString actual;
            tf->getFamilyName(&actual);
            resolved.typeface = apply_variation_axes(std::move(tf));
            resolved.actual_family = std::string(actual.c_str(), actual.size());
            resolved.origin = FallbackOrigin::Bundled;
            trace.push_back({"<default>", FallbackOrigin::Bundled, true,
                             resolved.actual_family,
                             "last-resort bundled default (no platform font db)"});
        }
    }

    resolved.trace = std::move(trace);
    record_event(primary_requested, resolved);
    std::lock_guard<std::mutex> lock(impl_->mtx);
    cache_put_locked(*impl_, key, resolved);
    return resolved;
}

ResolvedFont FontResolver::resolve_character_fallback(const FontOptions& options,
                                                     const ResolvedFont& primary,
                                                     std::uint32_t codepoint) {
    // Ask the platform font manager for a face that covers `codepoint`.
    // Locale is passed through when available so platform fallback can
    // influence ranking.
    ResolvedFont r;
    r.scope = options.scope;
    r.generation = merged_generation_for(options.scope);
    // Char-fallback faces carry the same AA / hinting policy as the primary
    // so paint paths stay consistent across the run.
    r.aa_mode = options.aa_mode;
    r.hinting_mode = options.hinting_mode;
    // Char-fallback also inherits color-font policy.
    r.color_font_mode = options.color_font_mode;

    sk_sp<SkFontMgr> mgr = platform_font_manager();
    if (!mgr) {
        r.origin = FallbackOrigin::NotFound;
        return r;
    }

    SkFontStyle sk_style = to_sk_style(options);
    const char* bcp47[] = { options.locale.empty() ? nullptr : options.locale.c_str() };
    int bcp47_count = options.locale.empty() ? 0 : 1;

    const char* base_family = primary.actual_family.empty()
                              ? nullptr
                              : primary.actual_family.c_str();

    if (auto tf = mgr->matchFamilyStyleCharacter(base_family, sk_style,
                                                 bcp47, bcp47_count,
                                                 static_cast<SkUnichar>(codepoint))) {
        SkString actual;
        tf->getFamilyName(&actual);
        r.typeface = std::move(tf);
        r.actual_family = std::string(actual.c_str(), actual.size());
        r.origin = FallbackOrigin::PlatformChar;
        r.trace.push_back({primary.actual_family, FallbackOrigin::PlatformChar,
                           true, r.actual_family, "char fallback"});
        // Record char-fallback resolves too.
        FontFlightRecorder::instance().record_fallback({
            primary.actual_family.empty() ? std::string("<char-fallback>")
                                          : primary.actual_family,
            r.actual_family,
            static_cast<std::uint8_t>(r.origin),
            r.generation, 0,
        });
    } else {
        r.origin = FallbackOrigin::NotFound;
        r.trace.push_back({primary.actual_family, FallbackOrigin::PlatformChar,
                           false, "", "no face covers codepoint"});
    }

    return r;
}

#else // !PULP_HAS_SKIA

// Non-Skia builds: every resolver call returns a NotFound result. The
// callers that pull this in are expected to fall back to their own
// non-rendering paths.

ResolvedFont FontResolver::resolve_family_list(const FontOptions& options) {
    const std::size_t key = options.hash();
    const auto generation = merged_generation_for(options.scope);
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        auto it = impl_->cache.find(key);
        if (it != impl_->cache.end()
            && it->second.resolved.generation == generation) {
            impl_->lru_order.splice(impl_->lru_order.end(),
                                    impl_->lru_order,
                                    it->second.lru_pos);
            return it->second.resolved;
        }
    }

    ResolvedFont r;
    r.scope = options.scope;
    r.generation = generation;
    r.aa_mode = options.aa_mode;
    r.hinting_mode = options.hinting_mode;
    r.color_font_mode = options.color_font_mode;
    r.origin = FallbackOrigin::NotFound;

    std::lock_guard<std::mutex> lock(impl_->mtx);
    auto it = impl_->cache.find(key);
    if (it != impl_->cache.end()) {
        impl_->lru_order.splice(impl_->lru_order.end(), impl_->lru_order,
                                it->second.lru_pos);
        it->second.resolved = r;
        return r;
    }

    impl_->lru_order.push_back(key);
    auto pos = std::prev(impl_->lru_order.end());
    impl_->cache.emplace(key, FontResolver::Impl::Entry{r, pos});
    if (impl_->capacity > 0 && impl_->cache.size() > impl_->capacity) {
        std::size_t oldest = impl_->lru_order.front();
        impl_->lru_order.pop_front();
        impl_->cache.erase(oldest);
    }
    return r;
}

ResolvedFont FontResolver::resolve_character_fallback(const FontOptions& options,
                                                     const ResolvedFont& /*primary*/,
                                                     std::uint32_t /*codepoint*/) {
    ResolvedFont r;
    r.scope = options.scope;
    r.generation = merged_generation_for(options.scope);
    r.aa_mode = options.aa_mode;
    r.hinting_mode = options.hinting_mode;
    r.color_font_mode = options.color_font_mode;
    r.origin = FallbackOrigin::NotFound;
    return r;
}

#endif // PULP_HAS_SKIA

// Color-font predicates — Skia-conditional method bodies that compile
// in both Skia and non-Skia builds.

#ifdef PULP_HAS_SKIA
namespace {

// Enumerate which specific color tables a typeface carries so
// `color_font_active()` can honor explicit `ColorFontMode` requests strictly
// (Bitmap / COLR / SVG must match the requested table, not just *some* color
// table).
//
// Tags packed big-endian:
constexpr SkFontTableTag kCOLR = 0x434F4C52u;
constexpr SkFontTableTag kCPAL = 0x4350414Cu;
constexpr SkFontTableTag kCBDT = 0x43424454u;
constexpr SkFontTableTag kCBLC = 0x43424C43u;
constexpr SkFontTableTag kSBIX = 0x73626978u;
constexpr SkFontTableTag kSVG  = 0x53564720u;  // 'SVG '

struct ColorTablePresence {
    bool any_color = false;  // Any of the six tags below.
    bool colr = false;       // COLR or CPAL.
    bool bitmap = false;     // CBDT/CBLC or sbix.
    bool svg = false;        // SVG table.
};

ColorTablePresence scan_color_tables(const SkTypeface* typeface) {
    ColorTablePresence p;
    if (!typeface) return p;
    const int count = typeface->countTables();
    if (count <= 0) return p;
    std::vector<SkFontTableTag> tags(static_cast<std::size_t>(count));
    // `readTableTags` is the canonical span-based API in this Skia
    // version; `getTableTags(tags[])` is gated behind a legacy macro.
    typeface->readTableTags({tags.data(), tags.size()});
    for (SkFontTableTag t : tags) {
        if (t == kCOLR || t == kCPAL) { p.colr = true; p.any_color = true; }
        else if (t == kCBDT || t == kCBLC || t == kSBIX) {
            p.bitmap = true; p.any_color = true;
        }
        else if (t == kSVG) { p.svg = true; p.any_color = true; }
    }
    return p;
}

} // namespace
#endif // PULP_HAS_SKIA

bool ResolvedFont::supports_color_font() const noexcept {
#ifdef PULP_HAS_SKIA
    return scan_color_tables(typeface.get()).any_color;
#else
    return false;
#endif
}

bool ResolvedFont::color_font_active() const noexcept {
#ifdef PULP_HAS_SKIA
    if (color_font_mode == ColorFontMode::ForceMonochrome) return false;
    if (!typeface) return false;
    const ColorTablePresence p = scan_color_tables(typeface.get());
    // Explicit modes are strict: they request a specific color format, so the
    // typeface must carry that specific table. Treating explicit modes as
    // Auto-equivalent silently degrades to "any color table will do", which
    // contradicts the ColorFontMode contract.
    switch (color_font_mode) {
        case ColorFontMode::Auto:           return p.any_color;
        case ColorFontMode::Bitmap:         return p.bitmap;
        case ColorFontMode::COLR:           return p.colr;
        case ColorFontMode::SVG:            return p.svg;
        case ColorFontMode::ForceMonochrome: return false;
    }
    return false;
#else
    return false;
#endif
}

std::string resolved_face_identity(const std::string& css_font_family,
                                   float weight, FontSlant slant) {
#ifdef PULP_HAS_SKIA
    FontOptions options;
    options.weight = weight;
    options.slant = slant;
    // Same comma walk the shaper uses; entries keep their quotes.
    options.family_stack = split_css_font_family_list(css_font_family);
    const auto resolved = FontResolver::instance().resolve_family_list(options);
    if (!resolved.typeface) return {};
    SkString name;
    resolved.typeface->getPostScriptName(&name);
    if (name.isEmpty()) {
        // Not every face carries one; fall back to the family so the answer is
        // still an identity rather than silently empty (which would read as
        // "unknown" and disable a cache that is in fact valid).
        return resolved.actual_family;
    }
    return std::string(name.c_str(), name.size());
#else
    (void)css_font_family;
    (void)weight;
    return {};
#endif
}

} // namespace pulp::canvas
