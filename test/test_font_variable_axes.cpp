// test_font_variable_axes.cpp — Pulp #2163, font v2 Slice 2.3.
//
// Verifies that FontOptions.variation_axes flows through the
// FontResolver into SkTypeface::makeClone(SkFontArguments). Bundled
// Inter is non-variable, so the clone returns the base face — but
// the resolver MUST attempt the clone and must NOT corrupt the
// resolved typeface when axes are requested.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/font_resolver.hpp>
#include <pulp/canvas/font_options.hpp>

#ifdef PULP_HAS_SKIA
#include "include/core/SkTypeface.h"
#endif

using namespace pulp::canvas;

TEST_CASE("Variable axes: empty axis list resolves cleanly", "[font][axes][issue-2163]") {
#ifndef PULP_HAS_SKIA
    SUCCEED("Non-Skia builds use the resolver stub");
    return;
#endif

    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;
    REQUIRE(opts.variation_axes.empty());

    auto resolved = FontResolver::instance().resolve_family_list(opts);
#ifdef PULP_HAS_SKIA
    REQUIRE(resolved.has_typeface());
#else
    REQUIRE_FALSE(resolved.has_typeface());
    REQUIRE_FALSE(resolved.resolved());
    REQUIRE(resolved.origin == FallbackOrigin::NotFound);
#endif
}

TEST_CASE("Variable axes: axis on non-variable face still resolves", "[font][axes]") {
#ifndef PULP_HAS_SKIA
    SUCCEED("Non-Skia builds use the resolver stub");
    return;
#endif

    // Bundled Inter is static — requesting `wght=450` should NOT
    // crash, NOT return null, and the resolver should fall through
    // to the base face (a SynthesisTrace would document the miss
    // when that record is added in the FontFlightRecorder slice).
    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.weight = 450.0f;
    opts.size = 14.0f;
    opts.variation_axes.push_back({make_variation_axis_tag('w','g','h','t'), 450.0f});

    auto resolved = FontResolver::instance().resolve_family_list(opts);
#ifdef PULP_HAS_SKIA
    REQUIRE(resolved.has_typeface());
#else
    REQUIRE_FALSE(resolved.has_typeface());
    REQUIRE_FALSE(resolved.resolved());
    REQUIRE(resolved.origin == FallbackOrigin::NotFound);
#endif
}

TEST_CASE("Variable axes: distinct axis values cache distinctly", "[font][axes]") {
    FontOptions a;
    a.family_stack.push_back("Inter");
    a.size = 14.0f;
    a.variation_axes.push_back({make_variation_axis_tag('w','g','h','t'), 100.0f});

    FontOptions b = a;
    b.variation_axes.back().value = 900.0f;

    REQUIRE(a.hash() != b.hash());
}

TEST_CASE("Variable axes: tag construction is byte-stable", "[font][axes]") {
    REQUIRE(make_variation_axis_tag('w','g','h','t')
            == make_font_feature_tag('w','g','h','t'));
    // Spot-check the byte order matches OpenType convention: 'wght'
    // packs as 0x77 0x67 0x68 0x74 → 0x77676874.
    REQUIRE(make_variation_axis_tag('w','g','h','t') == 0x77676874u);
}

TEST_CASE("FontOptions hash includes render policy and fallback fields",
          "[font][options][codecov]") {
    FontOptions base;
    base.family_stack = {"Inter", "system"};
    base.size = 15.0f;
    base.locale = "en-US";

    auto changed = [&](auto mutate) {
        FontOptions copy = base;
        mutate(copy);
        REQUIRE(copy.hash() != base.hash());
    };

    changed([](FontOptions& o) { o.features.push_back({make_font_feature_tag('k','e','r','n'), 0}); });
    changed([](FontOptions& o) { o.letter_spacing = 0.5f; });
    changed([](FontOptions& o) { o.word_spacing = 1.0f; });
    changed([](FontOptions& o) { o.hinting_mode = HintingMode::Full; });
    changed([](FontOptions& o) { o.aa_mode = AntiAliasMode::NoAA; });
    changed([](FontOptions& o) { o.color_font_mode = ColorFontMode::ForceMonochrome; });
    changed([](FontOptions& o) { o.fallback_mode = FallbackMode::Deterministic; });
    changed([](FontOptions& o) { o.registry_generation = 7; });
}

TEST_CASE("FontOptions hash includes synthesis flags and scope ids",
          "[font][options][codecov]") {
    FontOptions base;
    base.family_stack = {"Inter"};

    FontOptions synth_weight = base;
    synth_weight.font_synthesis.weight = true;
    REQUIRE(synth_weight.hash() != base.hash());

    FontOptions synth_slant = base;
    synth_slant.font_synthesis.slant = true;
    REQUIRE(synth_slant.hash() != base.hash());

    FontOptions synth_width = base;
    synth_width.font_synthesis.width = true;
    REQUIRE(synth_width.hash() != base.hash());

    FontOptions plugin_scope = base;
    plugin_scope.scope = FontScopeId::plugin(42);
    REQUIRE(plugin_scope.hash() != base.hash());
    REQUIRE(std::hash<FontScopeId>{}(FontScopeId::plugin(42))
            != std::hash<FontScopeId>{}(FontScopeId::view(42)));

    FontOptions view_scope = base;
    view_scope.scope = FontScopeId::view(42);
    REQUIRE(view_scope.hash() != plugin_scope.hash());
    REQUIRE(FontScopeId::global() == FontScopeId{});
}
