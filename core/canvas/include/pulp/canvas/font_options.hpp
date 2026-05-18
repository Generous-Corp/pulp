// font_options.hpp
//
// Pulp #2163 follow-up — Phase 1 / Slice 1.1.a of the font-subsystem-hardening
// v2 roadmap (see planning/2026-05-17-font-subsystem-hardening-v2.md).
//
// `FontOptions` is the typed, hashable blob that replaces today's
// `(family_string, size, weight, slant)` tuple wherever the SDK needs to
// describe what font to use. Every cache in the font subsystem must key on
// the FULL blob, never on a subset — this single rule eliminates an entire
// class of "the cache returned the wrong thing because the key was too
// narrow" bugs (lifecycle invalidation, axis-instance cohabitation,
// per-feature cache splits, scope isolation).
//
// Deliberately NOT included in FontOptions:
//   * Paint position (x, y) — that's an argument to ShapedText::paint_at.
//   * TextAnchor — that's an argument to ShapedText::paint_at.
// Folding either into FontOptions would force every cache miss to re-shape
// on an anchor or position change. The Slice 1.2 `paint_at` API takes them
// separately so the same `ShapedText` artifact can paint at any anchor
// without re-shaping.
//
// This header is intentionally Skia-free so non-GPU translation units
// (e.g. `core/view/` widgets) can construct FontOptions without dragging
// the Skia headers in. The resolver layer (font_resolver.hpp) is the seam
// where Skia types are introduced.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pulp::canvas {

// ── Enums ────────────────────────────────────────────────────────────────

/// CSS-style slant. Oblique carries a non-zero `oblique_angle` in
/// FontOptions; Normal/Italic use `oblique_angle = 0`.
enum class FontSlant : std::uint8_t {
    Normal,
    Italic,
    Oblique,
};

/// Logical text direction. `Auto` defers to bidi analysis in the
/// `TextRunPlanner` (Slice 1.2); `LTR`/`RTL` force a paragraph-level
/// base direction.
enum class BaseDirection : std::uint8_t {
    LTR,
    RTL,
    Auto,
};

/// Three fallback policies for the resolver cascade (Slice 1.1.a). Plugins
/// pick at registration time:
///   * `Deterministic` — registered → bundled → platform-`nullptr`-fallback.
///     Strictly identical across machines; what an audio plugin wants when
///     shipping a fixed visual. Ignores Core Text / DirectWrite / fontconfig
///     heuristic ranking.
///   * `Native` — defers to the platform font manager for cascade ranking
///     after the explicit family stack misses. Matches what a user would
///     see in their browser, but means two machines may render differently.
///   * `Hybrid` (default) — registered → bundled → platform, then
///     platform-heuristic-ranked fallback for character-fallback only. Best
///     for app-style UIs that want native feel for emoji / CJK while
///     keeping plugin-bundled families deterministic.
enum class FallbackMode : std::uint8_t {
    Deterministic,
    Native,
    Hybrid,
};

/// CSS `font-smoothing` analogue. `PlatformDefault` lets the platform pick;
/// the others are explicit overrides.
enum class HintingMode : std::uint8_t {
    None,
    Slight,
    Normal,
    Full,
    PlatformDefault,
};

/// `Default` follows the existing `inside_non_opaque_layer()` heuristic
/// (pulp #1899). The other modes force a specific AA path; observable in
/// `MeasurementTrace` so designer-grade goldens (Phase 3 / Slice 3.2)
/// catch unintended changes.
enum class AntiAliasMode : std::uint8_t {
    Default,
    LCD,
    Grayscale,
    NoAA,
};

/// Color-font policy. `Auto` picks the best available format per glyph;
/// the explicit modes force a single format (or monochrome) — useful when
/// a designer needs deterministic output across backends with uneven
/// COLR/CPAL support (see Phase 3 / Slice 3.1 tar pit).
enum class ColorFontMode : std::uint8_t {
    Auto,
    Bitmap,
    COLR,
    SVG,
    ForceMonochrome,
};

// ── Tag types ────────────────────────────────────────────────────────────

/// OpenType four-byte tag (e.g. `'k','e','r','n'`). Use
/// `make_font_feature_tag()` to construct from a four-char literal.
using FontFeatureTag = std::uint32_t;

constexpr FontFeatureTag make_font_feature_tag(char a, char b, char c, char d) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24)
         | (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16)
         | (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) <<  8)
         | (static_cast<std::uint32_t>(static_cast<unsigned char>(d)));
}

/// OpenType feature record: tag + value. `value=0` disables, `value>0`
/// enables (most features are boolean; stylistic alternates can use
/// value=N to pick a specific variant).
struct FontFeature {
    FontFeatureTag tag = 0;
    std::int32_t value = 0;

    constexpr bool operator==(const FontFeature& other) const noexcept {
        return tag == other.tag && value == other.value;
    }
};

/// Variation-axis tag + value. Tags are conventionally `wght` (weight),
/// `wdth` (width), `opsz` (optical size), `slnt` (slant), `ital` (italic),
/// plus any custom axes the font declares.
using VariationAxisTag = std::uint32_t;

constexpr VariationAxisTag make_variation_axis_tag(char a, char b, char c, char d) {
    return make_font_feature_tag(a, b, c, d);
}

struct VariationAxis {
    VariationAxisTag tag = 0;
    float value = 0.0f;

    bool operator==(const VariationAxis& other) const noexcept {
        return tag == other.tag && value == other.value;
    }
};

// ── Synthesis policy ─────────────────────────────────────────────────────

/// Explicit policy for faux-bold / faux-italic / faux-width. Default is
/// all-false: synthesis is opt-in, never accidental. When synthesis IS
/// allowed and the resolver applies it, a `SynthesisTrace` is emitted to
/// the `FontFlightRecorder` so the developer can see *what* was
/// synthesized and from *what source face*.
struct FontSynthesisPolicy {
    bool weight = false;
    bool slant  = false;
    bool width  = false;

    bool operator==(const FontSynthesisPolicy&) const = default;
};

// ── Scope ────────────────────────────────────────────────────────────────

/// Opaque identifier for a `FontScope` (defined in font_scope.hpp).
/// Carried inside `FontOptions` so cache keys include the scope, which
/// in turn means a Plugin(A) registration cannot pollute Plugin(B)
/// resolutions (and vice versa). The `View` kind powers hot-reload in
/// the design-import workflow (Phase 2 / Slice 2.x).
struct FontScopeId {
    enum class Kind : std::uint8_t { Global, Plugin, View };

    Kind kind = Kind::Global;
    std::uint64_t id = 0;

    static constexpr FontScopeId global() noexcept { return {Kind::Global, 0}; }
    static constexpr FontScopeId plugin(std::uint64_t plugin_id) noexcept {
        return {Kind::Plugin, plugin_id};
    }
    static constexpr FontScopeId view(std::uint64_t view_id) noexcept {
        return {Kind::View, view_id};
    }

    bool operator==(const FontScopeId&) const = default;
};

// ── FontOptions blob ─────────────────────────────────────────────────────

/// The typed, hashable contract that replaces `(family_string, size)`
/// throughout the font subsystem. Every resolver, shaper, line-layout,
/// and paint cache keys on this whole structure.
struct FontOptions {
    // ── Family + style ──────────────────────────────────────────────────
    /// Full CSS-style cascade. Order is significant; the resolver walks
    /// front-to-back. Empty vector falls through to platform default.
    std::vector<std::string> family_stack;

    /// CSS scalar 100..900 (or variable-axis value when the resolved face
    /// has a `wght` axis). Defaults to 400 (Regular).
    float weight = 400.0f;

    /// CSS `font-stretch` scalar 50..200, also used as `wdth` axis value
    /// when present. Defaults to 100 (Normal).
    float width = 100.0f;

    FontSlant slant = FontSlant::Normal;
    /// Slant angle in degrees. Used when `slant == Oblique`; ignored
    /// otherwise.
    float oblique_angle = 0.0f;

    /// Pixel size at the canvas's current device scale. Always in
    /// device-independent pixels (px); the canvas is responsible for
    /// any DPR/transform application.
    float size = 14.0f;

    // ── OpenType features ───────────────────────────────────────────────
    std::vector<FontFeature> features;

    // ── Variable-font axes ──────────────────────────────────────────────
    /// Each entry: (axis tag, value). The resolver consults these for
    /// axis instancing (Phase 2 / Slice 2.3). Ordering is irrelevant for
    /// resolution but preserved for trace fidelity.
    std::vector<VariationAxis> variation_axes;

    // ── Locale + direction ──────────────────────────────────────────────
    /// BCP-47 language tag (`""`, `"en"`, `"ja-JP"`, `"zh-Hans"`).
    /// Drives ICU locale-aware shaping and line-break (Phase 2 / 2.4).
    std::string locale;
    BaseDirection direction = BaseDirection::Auto;


    // ── Spacing ─────────────────────────────────────────────────────────
    /// Extra letter spacing in px (post-shaping, between clusters).
    float letter_spacing = 0.0f;
    /// Extra word spacing in px (applied at whitespace cluster boundaries).
    float word_spacing = 0.0f;

    // ── Render policy ───────────────────────────────────────────────────
    HintingMode    hinting_mode    = HintingMode::PlatformDefault;
    AntiAliasMode  aa_mode         = AntiAliasMode::Default;
    ColorFontMode  color_font_mode = ColorFontMode::Auto;

    // ── Synthesis policy ────────────────────────────────────────────────
    FontSynthesisPolicy font_synthesis;

    // ── Resolver behavior ───────────────────────────────────────────────
    FallbackMode fallback_mode = FallbackMode::Hybrid;
    FontScopeId  scope         = FontScopeId::global();

    /// Set by the resolver before returning a `ResolvedFont`. Baked into
    /// every downstream cache key so stale resolutions become eligible
    /// for collection on the next lookup after a registration. Never
    /// set by the caller — exposed as a public field so caches that
    /// don't have access to the resolver (e.g. SkiaCanvas typeface
    /// cache) can still observe and bake it in.
    std::uint64_t registry_generation = 0;

    bool operator==(const FontOptions&) const = default;

    /// Hash combining all fields. Slow-path acceptable; cache keys take
    /// the hash once and reuse it for the lifetime of the lookup. Stable
    /// across process runs for the same input.
    std::size_t hash() const noexcept;
};

} // namespace pulp::canvas

namespace std {
template <>
struct hash<pulp::canvas::FontOptions> {
    std::size_t operator()(const pulp::canvas::FontOptions& opts) const noexcept {
        return opts.hash();
    }
};
template <>
struct hash<pulp::canvas::FontScopeId> {
    std::size_t operator()(const pulp::canvas::FontScopeId& id) const noexcept {
        return (std::hash<std::uint64_t>{}(id.id) << 2)
             ^ std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(id.kind));
    }
};
} // namespace std
