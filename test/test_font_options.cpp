#include <catch2/catch_test_macros.hpp>

#include "pulp/canvas/font_options.hpp"
#include "pulp/canvas/font_resolver.hpp"
#include "pulp/canvas/font_scope.hpp"

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
