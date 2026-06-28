// pulp::design module + component catalog + metadata.
// Verifies the catalog is well-formed (every entry maps a Figma component to a
// real native class + header + tokens), the theme helper produces the token-true
// Ink & Signal theme, and apply_ink_signal restyles a view tree.

#include <catch2/catch_test_macros.hpp>

#include <pulp/design/design_system.hpp>
#include <pulp/view/theme_presets.hpp>

#include <set>

using namespace pulp::design;
using pulp::view::Theme;

TEST_CASE("ink_signal_theme matches the ink-signal preset", "[design-system]") {
    const auto* preset = pulp::view::find_preset("ink-signal");
    REQUIRE(preset != nullptr);

    for (bool dark : {false, true}) {
        Theme via_helper = ink_signal_theme(dark);
        Theme via_preset = pulp::view::theme_from_preset(*preset, dark);
        // Same accent + background seed → same derived theme.
        REQUIRE(via_helper.colors.at("accent.primary") == via_preset.colors.at("accent.primary"));
        REQUIRE(via_helper.colors.at("bg.primary") == via_preset.colors.at("bg.primary"));
        REQUIRE(via_helper.colors.at("meter.green") == via_preset.colors.at("meter.green"));
    }
}

TEST_CASE("light and dark themes differ", "[design-system]") {
    Theme light = ink_signal_theme(false);
    Theme dark = ink_signal_theme(true);
    // A real design system has distinct appearances — the background must change.
    REQUIRE_FALSE(light.colors.at("bg.primary") == dark.colors.at("bg.primary"));
}

TEST_CASE("apply_ink_signal restyles a view tree", "[design-system]") {
    pulp::view::View root;
    apply_ink_signal(root, /*dark=*/true);
    // The root resolves a flagship audio token rather than the fallback.
    auto resolved = root.resolve_color("knob.arc", pulp::view::Color{0, 0, 0, 0});
    auto expected = ink_signal_theme(true).colors.at("knob.arc");
    REQUIRE(resolved == expected);
}

TEST_CASE("catalog is non-empty and uniquely named", "[design-system]") {
    const auto& all = catalog();
    REQUIRE(all.size() >= 30);

    std::set<std::string> names;
    for (const auto& info : all) {
        INFO("component: " << info.name);
        REQUIRE_FALSE(info.name.empty());
        REQUIRE_FALSE(info.native_class.empty());
        REQUIRE_FALSE(info.header.empty());
        REQUIRE_FALSE(info.figma_component.empty());
        REQUIRE_FALSE(info.usage.empty());
        REQUIRE_FALSE(info.reskin_tokens.empty());
        // Native classes are fully qualified into pulp::view.
        REQUIRE(info.native_class.rfind("pulp::view::", 0) == 0);
        // Headers are under the pulp tree.
        REQUIRE(info.header.rfind("pulp/", 0) == 0);
        REQUIRE(names.insert(info.name).second);  // no duplicate names
    }
}

TEST_CASE("every catalog reskin token is a real theme token", "[design-system]") {
    Theme theme = ink_signal_theme(true);
    for (const auto& info : catalog()) {
        for (const auto& token : info.reskin_tokens) {
            INFO(info.name << " references token " << token);
            REQUIRE(theme.colors.count(token) == 1);
        }
    }
}

TEST_CASE("find and in_category resolve correctly", "[design-system]") {
    const auto* knob = find("Knob");
    REQUIRE(knob != nullptr);
    REQUIRE(knob->category == Category::controls);
    REQUIRE(knob->native_class == "pulp::view::Knob");

    REQUIRE(find("NoSuchComponent") == nullptr);

    auto controls = in_category(Category::controls);
    REQUIRE_FALSE(controls.empty());
    for (const auto* c : controls) {
        REQUIRE(c->category == Category::controls);
    }

    // Every category is non-empty (the catalog covers all 8 groups).
    for (auto cat : {Category::controls, Category::inputs, Category::indicators,
                     Category::navigation, Category::containers, Category::overlays,
                     Category::audio, Category::feedback}) {
        INFO("category: " << category_name(cat));
        REQUIRE_FALSE(in_category(cat).empty());
    }
}

TEST_CASE("Figma file key is the Ink & Signal library", "[design-system]") {
    REQUIRE(kFigmaFileKey == "q9iDYZzg86YrOQKr6I3bY0");
}
