// font_resolver.hpp
//
// Pulp #2163 follow-up — Phase 1 / Slice 1.1.a of the font-subsystem-
// hardening v2 roadmap.
//
// `FontResolver` is the canonical resolution path: one parser, one
// cascade, one set of fallback semantics. Every text-rendering or text-
// measurement caller in the SDK (SkiaCanvas, TextShaper, bundled_fonts,
// sdf_atlas, examples/ui-preview, the JS web-compat layer) talks to
// this resolver. The five separate parsers/cascades that existed in
// `feature/jsx-instrument-rebased@2371479c3` are eliminated.
//
// The resolver returns a `ResolvedFont` describing not just the chosen
// typeface but also *how* it was chosen: which scope owned it, which
// step of the cascade matched, whether faux-synthesis was applied. That
// trace data feeds the `FontFlightRecorder` (Slice 1.3) and the
// import-missing-font-advisor (companion doc).
//
// This header forward-declares `SkTypeface`; the implementation pulls
// in Skia. Non-GPU translation units can include this header to obtain
// a `ResolvedFont` value without dragging Skia in — the typeface field
// is opaque via `ResolvedFont::has_typeface()`.

#pragma once

#include "pulp/canvas/font_options.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef PULP_HAS_SKIA
#include "include/core/SkRefCnt.h"
class SkTypeface;
#endif

namespace pulp::canvas {

// ── Trace types ──────────────────────────────────────────────────────────

/// Which step of the cascade produced the resolved typeface. Mirrored
/// into `FallbackTrace` records on the `FontFlightRecorder`.
enum class FallbackOrigin : std::uint8_t {
    ScopeView,    ///< Resolved inside a View-scoped registration.
    ScopePlugin,  ///< Resolved inside a Plugin-scoped registration.
    ScopeGlobal,  ///< Resolved inside the Global scope.
    Bundled,      ///< Resolved by `match_bundled_typeface` (external/fonts).
    Platform,     ///< Resolved by `SkFontMgr::matchFamilyStyle`.
    PlatformChar, ///< Resolved by `SkFontMgr::matchFamilyStyleCharacter`
                  ///< (character-fallback path; only emitted by
                  ///< `resolve_character_fallback`).
    Synthetic,    ///< No matching face; resolver applied faux-synthesis.
    NotFound,     ///< Nothing matched and synthesis was disabled.
};

const char* to_string(FallbackOrigin) noexcept;

struct FallbackTraceStep {
    std::string    requested_family;  ///< Family name as listed in the cascade.
    FallbackOrigin origin;            ///< Cascade step attempted at this position.
    bool           succeeded;         ///< Whether this step produced a usable face.
    std::string    selected_family;   ///< Actual family name on the chosen face.
    std::string    note;              ///< Free-text annotation (e.g. "platform default fallback rejected").
};

struct SynthesisTrace {
    bool        faux_bold   = false;
    bool        faux_italic = false;
    bool        faux_width  = false;
    std::string source_family;  ///< Family the synthesis was applied to.
};

// ── ResolvedFont ─────────────────────────────────────────────────────────

/// The output of a resolver call. Carries the resolved typeface (or
/// nullptr if `origin == NotFound`), the trace, and the generation
/// value baked into the cache key so consumers can verify their copy
/// hasn't gone stale.
struct ResolvedFont {
#ifdef PULP_HAS_SKIA
    sk_sp<SkTypeface> typeface;
#endif

    std::string    actual_family;      ///< Family name reported by the chosen face.
    FallbackOrigin origin = FallbackOrigin::NotFound;
    FontScopeId    scope;
    std::uint64_t  generation = 0;     ///< `merged_generation_for(options.scope)` at resolve time.

    std::vector<FallbackTraceStep> trace;
    SynthesisTrace                 synthesis;

    bool has_typeface() const noexcept {
#ifdef PULP_HAS_SKIA
        return static_cast<bool>(typeface);
#else
        return false;
#endif
    }

    bool resolved() const noexcept {
        return origin != FallbackOrigin::NotFound;
    }
};

// ── FontResolver ─────────────────────────────────────────────────────────

class FontResolver {
public:
    /// Process-wide singleton. Thread-safe.
    static FontResolver& instance();

    /// Resolve the primary face for a `FontOptions` blob. Walks the
    /// family stack front-to-back through the scope cascade (View →
    /// Plugin → Global → Bundled → Platform). Synthesizes faux bold /
    /// italic only if `options.font_synthesis` allows it; otherwise
    /// emits a `SynthesisTrace` documenting what would have been
    /// synthesized and returns the closest-style real face.
    ResolvedFont resolve_family_list(const FontOptions& options);

    /// Character-level fallback. Called by the run planner (Slice 1.2)
    /// when a cluster's primary face has no glyph for one of its
    /// codepoints. Tries each face in the family stack first, then
    /// `SkFontMgr::matchFamilyStyleCharacter` (i.e. honors the platform
    /// font manager's per-codepoint fallback heuristics in
    /// `Native`/`Hybrid` modes; refuses heuristic fallback in
    /// `Deterministic` mode).
    ResolvedFont resolve_character_fallback(const FontOptions& options,
                                            const ResolvedFont& primary,
                                            std::uint32_t codepoint);

    /// Test-only: discard the internal cache. Production code never
    /// calls this — invalidation happens through scope generation
    /// bumps baked into cache keys.
    void clear_cache();

private:
    FontResolver();
    ~FontResolver();
    FontResolver(const FontResolver&) = delete;
    FontResolver& operator=(const FontResolver&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::canvas
