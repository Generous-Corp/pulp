#include <catch2/catch_test_macros.hpp>

#include "pulp/canvas/bundled_fonts.hpp"
#include "pulp/canvas/font_options.hpp"
#include "pulp/canvas/font_resolver.hpp"
#include "pulp/canvas/shaped_text.hpp"
#include "pulp/canvas/font_scope.hpp"
#include "pulp/canvas/text_run_planner.hpp"

#include <functional>

using namespace pulp::canvas;

namespace {

FontOptions rich_options() {
    FontOptions opts;
    opts.family_stack = {"Inter", "Noto Sans"};
    opts.weight = 700.0f;
    opts.width = 112.5f;
    opts.slant = FontSlant::Oblique;
    opts.oblique_angle = 12.0f;
    opts.size = 18.0f;
    opts.features = {
        {make_font_feature_tag('k', 'e', 'r', 'n'), 1},
        {make_font_feature_tag('t', 'n', 'u', 'm'), 0},
    };
    opts.variation_axes = {
        {make_variation_axis_tag('w', 'g', 'h', 't'), 650.0f},
        {make_variation_axis_tag('o', 'p', 's', 'z'), 14.0f},
    };
    opts.locale = "ja-JP";
    opts.direction = BaseDirection::RTL;
    opts.letter_spacing = 1.25f;
    opts.word_spacing = 2.5f;
    opts.hinting_mode = HintingMode::Full;
    opts.aa_mode = AntiAliasMode::Grayscale;
    opts.color_font_mode = ColorFontMode::ForceMonochrome;
    opts.font_synthesis = {true, false, true};
    opts.fallback_mode = FallbackMode::Deterministic;
    opts.scope = FontScopeId::plugin(42);
    opts.registry_generation = 99;
    return opts;
}

} // namespace

TEST_CASE("FontOptions hash includes full resolver cache key",
          "[canvas][font][options][coverage]") {
    const FontOptions base = rich_options();
    REQUIRE(base == rich_options());
    REQUIRE(std::hash<FontOptions>{}(base) == std::hash<FontOptions>{}(rich_options()));

    auto changed = base;
    changed.family_stack = {"Noto Sans", "Inter"};
    REQUIRE(changed != base);
    REQUIRE(changed.hash() != base.hash());

    changed = base;
    changed.features[1].value = 1;
    REQUIRE(changed != base);
    REQUIRE(changed.hash() != base.hash());

    changed = base;
    changed.variation_axes[0].value = 651.0f;
    REQUIRE(changed != base);
    REQUIRE(changed.hash() != base.hash());

    changed = base;
    changed.locale = "en-US";
    REQUIRE(changed != base);
    REQUIRE(changed.hash() != base.hash());

    changed = base;
    changed.scope = FontScopeId::view(42);
    REQUIRE(changed != base);
    REQUIRE(changed.hash() != base.hash());

    changed = base;
    changed.registry_generation += 1;
    REQUIRE(changed != base);
    REQUIRE(changed.hash() != base.hash());
}

TEST_CASE("Font tag helpers pack OpenType tags in byte order",
          "[canvas][font][options][coverage]") {
    REQUIRE(make_font_feature_tag('k', 'e', 'r', 'n') == 0x6B65726Eu);
    REQUIRE(make_font_feature_tag('t', 'n', 'u', 'm') == 0x746E756Du);
    REQUIRE(make_variation_axis_tag('w', 'g', 'h', 't') == 0x77676874u);
    REQUIRE(make_variation_axis_tag('s', 'l', 'n', 't')
            == make_font_feature_tag('s', 'l', 'n', 't'));
}

TEST_CASE("FontScope factories and generations isolate plugin and view scopes",
          "[canvas][font][scope][coverage]") {
    auto plugin_id = 728042u;
    auto view_id = 728043u;

    auto& plugin = plugin_scope(plugin_id);
    auto& view = view_scope(view_id);

    REQUIRE(plugin.id() == FontScopeId::plugin(plugin_id));
    REQUIRE(view.id() == FontScopeId::view(view_id));
    REQUIRE(global_scope().id() == FontScopeId::global());

    const auto plugin_before = plugin.generation();
    const auto view_before = view.generation();
    const auto merged_plugin_before = merged_generation_for(plugin.id());
    const auto merged_view_before = merged_generation_for(view.id());

    plugin.bump_generation();
    REQUIRE(plugin.generation() == plugin_before + 1);
    REQUIRE(merged_generation_for(plugin.id()) == merged_plugin_before + 1);
    REQUIRE(view.generation() == view_before);
    REQUIRE(merged_generation_for(view.id()) == merged_view_before);

    view.set_memory_budget(4096);
    REQUIRE(view.memory_budget() == 4096);
    view.bump_generation();
    REQUIRE(view.generation() == view_before + 1);

    release_view_scope(view_id);
    auto& fresh_view = view_scope(view_id);
    REQUIRE(fresh_view.id() == FontScopeId::view(view_id));
    REQUIRE(fresh_view.generation() == 0);
    REQUIRE(fresh_view.memory_budget() == 0);
}

TEST_CASE("Font resolver trace names cover every fallback origin",
          "[canvas][font][resolver][coverage]") {
    REQUIRE(std::string(to_string(FallbackOrigin::ScopeView)) == "scope-view");
    REQUIRE(std::string(to_string(FallbackOrigin::ScopePlugin)) == "scope-plugin");
    REQUIRE(std::string(to_string(FallbackOrigin::ScopeGlobal)) == "scope-global");
    REQUIRE(std::string(to_string(FallbackOrigin::Bundled)) == "bundled");
    REQUIRE(std::string(to_string(FallbackOrigin::Platform)) == "platform");
    REQUIRE(std::string(to_string(FallbackOrigin::PlatformChar)) == "platform-char");
    REQUIRE(std::string(to_string(FallbackOrigin::Synthetic)) == "synthetic");
    REQUIRE(std::string(to_string(FallbackOrigin::NotFound)) == "not-found");
}

TEST_CASE("ResolvedFont default state is unresolved without a typeface",
          "[canvas][font][resolver][coverage]") {
    ResolvedFont font;
    REQUIRE_FALSE(font.has_typeface());
    REQUIRE_FALSE(font.resolved());
    REQUIRE(font.origin == FallbackOrigin::NotFound);
    REQUIRE(font.generation == 0);
    REQUIRE(font.scope == FontScopeId::global());
    REQUIRE(font.trace.empty());
    REQUIRE_FALSE(font.synthesis.faux_bold);
    REQUIRE_FALSE(font.synthesis.faux_italic);
    REQUIRE_FALSE(font.synthesis.faux_width);
}

#ifndef PULP_HAS_SKIA
TEST_CASE("FontResolver non-Skia path returns scoped NotFound results",
          "[canvas][font][resolver][coverage]") {
    auto& resolver = FontResolver::instance();
    resolver.clear_cache();

    constexpr std::uint64_t plugin_id = 728245;
    auto& scope = plugin_scope(plugin_id);
    scope.bump_generation();

    FontOptions opts;
    opts.family_stack = {"Inter", "Missing Family"};
    opts.scope = scope.id();
    opts.locale = "en-US";

    auto primary = resolver.resolve_family_list(opts);
    REQUIRE_FALSE(primary.resolved());
    REQUIRE_FALSE(primary.has_typeface());
    REQUIRE(primary.origin == FallbackOrigin::NotFound);
    REQUIRE(primary.scope == opts.scope);
    REQUIRE(primary.generation == merged_generation_for(opts.scope));
    REQUIRE(primary.trace.empty());

    auto fallback = resolver.resolve_character_fallback(opts, primary, 0x1F642);
    REQUIRE_FALSE(fallback.resolved());
    REQUIRE_FALSE(fallback.has_typeface());
    REQUIRE(fallback.origin == FallbackOrigin::NotFound);
    REQUIRE(fallback.scope == opts.scope);
    REQUIRE(fallback.generation == merged_generation_for(opts.scope));
}

TEST_CASE("Font registry non-Skia stubs reject registrations but validate bytes",
          "[canvas][font][registry][coverage]") {
    const std::uint8_t bytes[] = {0x00, 0x01, 0x02, 0x03};

    REQUIRE_FALSE(register_font(bytes, sizeof(bytes), "StubFont"));
    REQUIRE_FALSE(register_font_file("/tmp/definitely-missing-font.ttf", "StubFont"));
    REQUIRE_FALSE(register_font_woff2(bytes, sizeof(bytes), "StubFont"));
    REQUIRE_FALSE(is_font_registered("StubFont"));
    REQUIRE_FALSE(is_font_registered(""));

    REQUIRE_FALSE(validate_font_bytes(nullptr, 4));
    REQUIRE_FALSE(validate_font_bytes(bytes, 0));
    REQUIRE(validate_font_bytes(bytes, sizeof(bytes)));

    REQUIRE(font_registration_generation() == 0);
    bump_font_registration_generation();
    REQUIRE(font_registration_generation() == 0);
}

TEST_CASE("Font cluster_step non-Skia skeleton skips UTF-8 continuation bytes",
          "[canvas][font][registry][coverage]") {
    const std::string text = std::string("A") + "\xC3\xA9"
                           + "\xF0\x9F\x99\x82" + "Z";

    REQUIRE(cluster_step(text, 0, true) == 1);
    REQUIRE(cluster_step(text, 1, true) == 3);
    REQUIRE(cluster_step(text, 3, true) == 7);
    REQUIRE(cluster_step(text, 7, true) == 8);
    REQUIRE(cluster_step(text, text.size(), true) == text.size());

    REQUIRE(cluster_step(text, 8, false) == 7);
    REQUIRE(cluster_step(text, 7, false) == 3);
    REQUIRE(cluster_step(text, 3, false) == 1);
    REQUIRE(cluster_step(text, 1, false) == 0);
    REQUIRE(cluster_step(text, 0, false) == 0);
}
#endif

TEST_CASE("TextRunPlanner skeleton maps UTF-8 scalars and line breaks",
          "[canvas][font][planner][coverage]") {
    auto& planner = TextRunPlanner::instance();
    planner.clear_cache();

    FontOptions opts;
    opts.family_stack = {"Inter"};
    opts.size = 16.0f;
    opts.direction = BaseDirection::LTR;

    const std::string text = std::string("A") + "\xC3\xA9" + "\xF0\x9F\x99\x82"
                           + "\nB\tC";
    auto shaped = planner.shape(text, opts);

    REQUIRE(shaped.text == text);
    REQUIRE_FALSE(shaped.empty());
    REQUIRE(shaped.runs.size() == 1);
    REQUIRE(shaped.runs[0].logical_start == 0);
    REQUIRE(shaped.runs[0].logical_end == text.size());
    REQUIRE(shaped.runs[0].bidi_level == 0);

    REQUIRE(shaped.index_map.scalar_count() == 7);
    REQUIRE(shaped.index_map.scalar_offsets
            == std::vector<std::uint32_t>{0, 1, 3, 7, 8, 9, 10, 11});

    REQUIRE(shaped.line_breaks.size() == 2);
    REQUIRE(shaped.line_breaks[0].utf8_offset == 7);
    REQUIRE(shaped.line_breaks[0].kind == LineBreakOpportunity::Kind::Hard);
    REQUIRE(shaped.line_breaks[1].utf8_offset == 10);
    REQUIRE(shaped.line_breaks[1].kind == LineBreakOpportunity::Kind::Soft);
}

TEST_CASE("TextRunPlanner captures scope generation and RTL base direction",
          "[canvas][font][planner][coverage]") {
    auto& planner = TextRunPlanner::instance();
    planner.clear_cache();

    constexpr std::uint64_t plugin_id = 728144;
    auto& scope = plugin_scope(plugin_id);
    scope.bump_generation();

    FontOptions opts;
    opts.family_stack = {"Inter"};
    opts.scope = scope.id();
    opts.direction = BaseDirection::RTL;

    const auto expected_generation = merged_generation_for(opts.scope);
    auto shaped = planner.shape("abc", opts);

    REQUIRE(shaped.options.scope == opts.scope);
    REQUIRE(shaped.options.registry_generation == expected_generation);
    REQUIRE(shaped.runs.size() == 1);
    REQUIRE(shaped.runs[0].bidi_level == 1);
    REQUIRE(shaped.runs[0].font.scope == opts.scope);
    REQUIRE(shaped.runs[0].font.generation == expected_generation);

    opts.registry_generation = 12345;
    auto explicit_gen = planner.shape("abc", opts);
    REQUIRE(explicit_gen.options.registry_generation == 12345);
}

TEST_CASE("TextRunPlanner handles empty text, cache hits, and ASCII breaks",
          "[canvas][font][planner][coverage]") {
    auto& planner = TextRunPlanner::instance();
    planner.clear_cache();

    FontOptions opts;
    opts.family_stack = {"Inter"};
    opts.size = 12.0f;

    auto empty = planner.shape("", opts);
    REQUIRE(empty.text.empty());
    REQUIRE_FALSE(empty.empty());
    REQUIRE(empty.index_map.scalar_count() == 0);
    REQUIRE(empty.index_map.scalar_offsets == std::vector<std::uint32_t>{0});
    REQUIRE(empty.line_breaks.empty());
    REQUIRE(empty.runs.size() == 1);
    REQUIRE(empty.runs[0].logical_start == 0);
    REQUIRE(empty.runs[0].logical_end == 0);

    const std::string spaced = "A B\tC\nD";
    auto first = planner.shape(spaced, opts);
    auto second = planner.shape(spaced, opts);
    REQUIRE(second.text == first.text);
    REQUIRE(second.options.hash() == first.options.hash());
    REQUIRE(second.index_map.scalar_offsets == first.index_map.scalar_offsets);
    REQUIRE(second.line_breaks.size() == 3);
    REQUIRE(second.line_breaks[0].utf8_offset == 2);
    REQUIRE(second.line_breaks[0].kind == LineBreakOpportunity::Kind::Soft);
    REQUIRE(second.line_breaks[1].utf8_offset == 4);
    REQUIRE(second.line_breaks[1].kind == LineBreakOpportunity::Kind::Soft);
    REQUIRE(second.line_breaks[2].utf8_offset == 5);
    REQUIRE(second.line_breaks[2].kind == LineBreakOpportunity::Kind::Hard);

    planner.clear_cache();
    auto after_clear = planner.shape(spaced, opts);
    REQUIRE(after_clear.text == first.text);
    REQUIRE(after_clear.index_map.scalar_offsets == first.index_map.scalar_offsets);
    REQUIRE(after_clear.line_breaks.size() == first.line_breaks.size());
}

TEST_CASE("ShapedText value types report default metrics and emptiness",
          "[canvas][font][shaped-text][coverage]") {
    RunMetrics metrics;
    REQUIRE(metrics.line_height() == 0.0f);
    metrics.ascent = 9.0f;
    metrics.descent = 3.0f;
    metrics.leading = 2.0f;
    REQUIRE(metrics.line_height() == 14.0f);

    UnicodeIndexMap empty_map;
    REQUIRE(empty_map.scalar_count() == 0);
    empty_map.scalar_offsets = {0, 2, 6};
    REQUIRE(empty_map.scalar_count() == 2);

    ShapedText shaped;
    REQUIRE(shaped.empty());
    REQUIRE(shaped.text.empty());
    REQUIRE(shaped.total_width == 0.0f);
    REQUIRE(shaped.clusters.empty());
    REQUIRE(shaped.line_breaks.empty());

    ShapedRun run;
    run.logical_start = 2;
    run.logical_end = 6;
    run.advance_total = 42.0f;
    run.metrics = metrics;
    run.glyph_ids = {1, 2};
    run.advances = {20.0f, 22.0f};
    run.offsets_x = {0.0f, 0.5f};
    run.offsets_y = {0.0f, 1.0f};
    run.cluster_indices = {2, 4};

    shaped.text = "abcdef";
    shaped.total_width = run.advance_total;
    shaped.overall_metrics = run.metrics;
    shaped.runs.push_back(run);
    shaped.clusters.push_back({2, 6, 0, 0, 2});
    shaped.line_breaks.push_back({6, LineBreakOpportunity::Kind::Soft});

    REQUIRE_FALSE(shaped.empty());
    REQUIRE(shaped.total_width == 42.0f);
    REQUIRE(shaped.overall_metrics.line_height() == 14.0f);
    REQUIRE(shaped.runs[0].logical_start == 2);
    REQUIRE(shaped.runs[0].logical_end == 6);
    REQUIRE(shaped.clusters[0].glyph_count == 2);
    REQUIRE(shaped.line_breaks[0].kind == LineBreakOpportunity::Kind::Soft);
}
