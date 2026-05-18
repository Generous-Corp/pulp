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
