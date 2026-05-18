// font_resolver.cpp — Pulp #2163 follow-up, Phase 1 / Slice 1.1.a.
//
// First-cut implementation of the canonical resolver. For Slice 1.1.a
// the resolver is wired in as the single entry point but its body
// delegates to the existing match cascade in `bundled_fonts.cpp` /
// `skia_canvas.cpp`. Subsequent slices replace those delegations with
// scope-aware lookups and emit richer fallback traces.
//
// The header is Skia-free for non-GPU consumers; this .cpp is the
// translation unit that bridges to Skia under `PULP_HAS_SKIA`.

#include "pulp/canvas/font_resolver.hpp"
#include "pulp/canvas/font_scope.hpp"
#include "pulp/canvas/bundled_fonts.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#ifdef PULP_HAS_SKIA
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkFontArguments.h"
#endif

namespace pulp::canvas {

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

struct FontResolver::Impl {
    mutable std::mutex mtx;
    // Cache keyed on the full FontOptions hash; the value carries its
    // own generation so a stale entry can be detected on lookup.
    std::unordered_map<std::size_t, ResolvedFont> cache;
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

// Bridge to legacy cascade (skia_canvas.cpp `get_cached_typeface_single`)
// is not yet symbol-visible from here. For Slice 1.1.a we use the public
// `match_registered_typeface` + `match_bundled_typeface` + SkFontMgr
// path directly so this TU compiles standalone. The migration of
// `skia_canvas.cpp` to call THIS resolver instead happens in Slice 1.1.a
// part 2.

ResolvedFont resolve_one_family(const std::string& family,
                                const FontOptions& opts,
                                SkFontStyle sk_style,
                                sk_sp<SkFontMgr> mgr,
                                std::vector<FallbackTraceStep>& trace) {
    ResolvedFont r;
    r.scope = opts.scope;
    r.generation = merged_generation_for(opts.scope);
    // Slice 3.2: AA / hinting policy travels alongside the resolved face.
    r.aa_mode = opts.aa_mode;
    r.hinting_mode = opts.hinting_mode;

    // 1) Registered (scoped). Phase 1.1.a only consults the global
    //    registered map; per-scope storage arrives in 1.1.b.
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
    if (mgr) {
        if (auto tf = mgr->matchFamilyStyle(family.c_str(), sk_style)) {
            SkString actual;
            tf->getFamilyName(&actual);

            // Honor v1's "did we get a generic fallback?" check: if the
            // platform returned a default face whose name doesn't relate
            // to the requested family, mark it as failed so the caller
            // can try the next family in the stack.
            std::string lo_a(actual.c_str(), actual.size());
            for (auto& c : lo_a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::string lo_f = family;
            for (auto& c : lo_f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool name_overlap = (lo_a == lo_f
                                 || lo_a.find(lo_f) != std::string::npos
                                 || lo_f.find(lo_a) != std::string::npos);

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
                trace.push_back({family, FallbackOrigin::Platform, true,
                                 r.actual_family,
                                 name_overlap ? "" : "platform default (name does not overlap)"});
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

ResolvedFont FontResolver::resolve_family_list(const FontOptions& options) {
    // Cache lookup. Generation check happens at use site (callers compare
    // `resolved.generation` against `merged_generation_for(scope)`).
    const std::size_t key = options.hash();
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        auto it = impl_->cache.find(key);
        if (it != impl_->cache.end()
            && it->second.generation == merged_generation_for(options.scope)) {
            return it->second;
        }
    }

    SkFontStyle sk_style = to_sk_style(options);
    sk_sp<SkFontMgr> mgr = platform_font_manager();

    std::vector<FallbackTraceStep> trace;
    ResolvedFont resolved;
    resolved.scope = options.scope;
    resolved.generation = merged_generation_for(options.scope);
    // pulp #2163 — font v2 Slice 3.2. AA / hinting policy carried straight
    // out of FontOptions onto the ResolvedFont so paint paths derive Skia
    // flags from one canonical source. See `sk_edging_for` / `sk_hinting_for`
    // in font_resolver.hpp for the enum translation.
    resolved.aa_mode = options.aa_mode;
    resolved.hinting_mode = options.hinting_mode;

    // pulp #2163 — font v2 Slice 2.3. After a face resolves, if the
    // caller requested variation axes (`font-variation-settings`),
    // clone the typeface with those axes applied so the cache holds
    // one entry per distinct axis instance (the FontOptions hash
    // already keys on variation_axes — this just applies them).
    auto apply_variation_axes = [&](sk_sp<SkTypeface> face) -> sk_sp<SkTypeface> {
        if (!face || options.variation_axes.empty()) return face;
        std::vector<SkFontArguments::VariationPosition::Coordinate> coords;
        coords.reserve(options.variation_axes.size());
        for (const auto& axis : options.variation_axes) {
            coords.push_back({static_cast<SkFourByteTag>(axis.tag), axis.value});
        }
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
            std::lock_guard<std::mutex> lock(impl_->mtx);
            impl_->cache[key] = resolved;
            return resolved;
        }
    }

    // Empty stack or nothing matched: last-resort platform default.
    if (mgr) {
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

    resolved.trace = std::move(trace);
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->cache[key] = resolved;
    return resolved;
}

ResolvedFont FontResolver::resolve_character_fallback(const FontOptions& options,
                                                     const ResolvedFont& primary,
                                                     std::uint32_t codepoint) {
    // Slice 1.1.a stub: ask the platform font manager for a face that
    // covers `codepoint`. Slice 1.2 enriches this with cluster-aware
    // selection and locale-influenced ranking.
    ResolvedFont r;
    r.scope = options.scope;
    r.generation = merged_generation_for(options.scope);
    // Slice 3.2: char-fallback faces carry the same AA / hinting policy
    // as the primary so paint paths stay consistent across the run.
    r.aa_mode = options.aa_mode;
    r.hinting_mode = options.hinting_mode;

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
    ResolvedFont r;
    r.scope = options.scope;
    r.generation = merged_generation_for(options.scope);
    r.aa_mode = options.aa_mode;
    r.hinting_mode = options.hinting_mode;
    r.origin = FallbackOrigin::NotFound;
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
    r.origin = FallbackOrigin::NotFound;
    return r;
}

#endif // PULP_HAS_SKIA

} // namespace pulp::canvas
