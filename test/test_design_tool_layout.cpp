// Automated test for design tool layout structure
// Validates that the JS creates the correct view tree with proper sizing
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/state/store.hpp>
#include <pulp/canvas/canvas.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <stdexcept>

using namespace pulp::view;
namespace fs = std::filesystem;

static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string find_js_file(const std::string& name) {
    // Search relative to the test binary
    auto dir = fs::current_path();
    while (!dir.empty()) {
        auto candidate = dir / "examples" / "design-tool" / name;
        if (fs::exists(candidate)) return candidate.string();
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

static void load_design_tool(View& root, ScriptEngine& engine, WidgetBridge& bridge) {
    auto oklch_path = find_js_file("oklch.js");
    if (!oklch_path.empty()) {
        bridge.load_script(read_file(oklch_path));
    }

    auto js_path = find_js_file("design-tool.js");
    if (js_path.empty()) {
        throw std::runtime_error("design-tool.js not found");
    }

    bridge.load_script(read_file(js_path));
    root.layout_children();
}

TEST_CASE("Design tool: JS creates three-column layout", "[design-tool]") {
    auto js_path = find_js_file("design-tool.js");
    if (js_path.empty()) {
        SKIP("design-tool.js not found");
        return;
    }

    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::column;
    root.set_bounds({0, 0, 1100, 700});

    pulp::state::StateStore store;
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);

    load_design_tool(root, engine, bridge);

    auto* toolbar = bridge.widget("toolbar");
    auto* main_area = bridge.widget("main-area");
    auto* status = bridge.widget("status-bar");
    REQUIRE(toolbar != nullptr);
    REQUIRE(main_area != nullptr);
    REQUIRE(status != nullptr);

    // Toolbar should be 44px tall
    REQUIRE_THAT(toolbar->bounds().height, Catch::Matchers::WithinAbs(44.0f, 1.0f));

    // Status bar should be 28px tall
    REQUIRE_THAT(status->bounds().height, Catch::Matchers::WithinAbs(28.0f, 1.0f));

    // Main area should fill remaining space
    REQUIRE(main_area->bounds().height > 600.0f);
}

TEST_CASE("Design tool: left panel is 310px, right panel is 272px", "[design-tool]") {
    auto js_path = find_js_file("design-tool.js");
    if (js_path.empty()) {
        SKIP("design-tool.js not found");
        return;
    }

    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::column;
    root.set_bounds({0, 0, 1100, 700});

    pulp::state::StateStore store;
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);

    load_design_tool(root, engine, bridge);

    // Main area is child 1, should be a row with 3 children
    auto* main_area = root.child_at(1);
    REQUIRE(main_area->child_count() >= 3);

    // Left panel
    auto* left = main_area->child_at(0);
    REQUIRE_THAT(left->bounds().width, Catch::Matchers::WithinAbs(310.0f, 2.0f));

    // Right panel
    auto* right = main_area->child_at(main_area->child_count() - 1);
    REQUIRE_THAT(right->bounds().width, Catch::Matchers::WithinAbs(272.0f, 2.0f));

    // Center panel should fill remaining
    auto* center = main_area->child_at(1);
    REQUIRE(center->bounds().width > 400.0f);
}

TEST_CASE("Design tool: toolbar has space-between layout", "[design-tool]") {
    auto js_path = find_js_file("design-tool.js");
    if (js_path.empty()) {
        SKIP("design-tool.js not found");
        return;
    }

    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::column;
    root.set_bounds({0, 0, 1100, 700});

    pulp::state::StateStore store;
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);

    load_design_tool(root, engine, bridge);

    auto* toolbar = root.child_at(0);
    REQUIRE(toolbar->child_count() >= 2);

    // First child (toolbar-left) should be at left
    auto* left_group = toolbar->child_at(0);
    REQUIRE(left_group->bounds().x < 50.0f);

    // Last child (toolbar-right) should be near right edge
    auto* right_group = toolbar->child_at(toolbar->child_count() - 1);
    float right_edge = right_group->bounds().x + right_group->bounds().width;
    REQUIRE(right_edge > 1000.0f); // Near the right side of 1100px
}

TEST_CASE("Design tool: help badges exist for color system controls", "[design-tool]") {
    auto js_path = find_js_file("design-tool.js");
    if (js_path.empty()) {
        SKIP("design-tool.js not found");
        return;
    }

    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::column;
    root.set_bounds({0, 0, 1100, 700});

    pulp::state::StateStore store;
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);

    load_design_tool(root, engine, bridge);

    auto* template_help = bridge.widget("template-help");
    auto* harmony_help = bridge.widget("harmony-help");
    auto* mode_help = bridge.widget("mode-help");

    REQUIRE(template_help != nullptr);
    REQUIRE(harmony_help != nullptr);
    REQUIRE(mode_help != nullptr);
    REQUIRE_THAT(template_help->bounds().width, Catch::Matchers::WithinAbs(18.0f, 1.0f));
    REQUIRE_THAT(template_help->bounds().height, Catch::Matchers::WithinAbs(18.0f, 1.0f));

    auto* help_modal = bridge.widget("help-modal");
    REQUIRE(help_modal != nullptr);
    REQUIRE_FALSE(help_modal->visible());

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('template-help', 'click', 0);"));
    root.layout_children();
    REQUIRE(help_modal->visible());
}

TEST_CASE("Design tool: palette dots stay circular", "[design-tool]") {
    auto js_path = find_js_file("design-tool.js");
    if (js_path.empty()) {
        SKIP("design-tool.js not found");
        return;
    }

    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::column;
    root.set_bounds({0, 0, 1100, 700});

    pulp::state::StateStore store;
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);

    load_design_tool(root, engine, bridge);

    auto* accent_dot = bridge.widget("ramp-0-dot");
    REQUIRE(accent_dot != nullptr);
    REQUIRE_THAT(accent_dot->bounds().width, Catch::Matchers::WithinAbs(14.0f, 1.0f));
    REQUIRE_THAT(accent_dot->bounds().height, Catch::Matchers::WithinAbs(14.0f, 1.0f));
}

TEST_CASE("Design tool: token search compacts the list and hides non-matching groups", "[design-tool]") {
    auto js_path = find_js_file("design-tool.js");
    if (js_path.empty()) {
        SKIP("design-tool.js not found");
        return;
    }

    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::column;
    root.set_bounds({0, 0, 1100, 700});

    pulp::state::StateStore store;
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);
    load_design_tool(root, engine, bridge);

    auto* token_list = bridge.widget("token-list");
    auto* background_group = bridge.widget("tg-0");
    auto* accent_group = bridge.widget("tg-2");
    REQUIRE(token_list != nullptr);
    REQUIRE(background_group != nullptr);
    REQUIRE(accent_group != nullptr);

    const float initial_height = token_list->bounds().height;
    REQUIRE(initial_height > 300.0f);

    engine.evaluate("setText('token-search', 'accent'); updateTokenFilterLayout('accent'); layout();");
    root.layout_children();

    REQUIRE_FALSE(background_group->visible());
    REQUIRE(accent_group->visible());
    REQUIRE(token_list->bounds().height < initial_height);
}

TEST_CASE("Design tool: palette rows keep mini ramps when collapsed and gamut editor when expanded", "[design-tool]") {
    auto js_path = find_js_file("design-tool.js");
    if (js_path.empty()) {
        SKIP("design-tool.js not found");
        return;
    }

    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::column;
    root.set_bounds({0, 0, 1100, 700});

    pulp::state::StateStore store;
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);
    load_design_tool(root, engine, bridge);

    auto* mini_ramp = bridge.widget("ramp-0-row");
    auto* first_swatch = bridge.widget("ramp-0-s0");
    auto* editor = bridge.widget("ramp-0-editor");
    REQUIRE(mini_ramp != nullptr);
    REQUIRE(first_swatch != nullptr);
    REQUIRE(editor != nullptr);

    REQUIRE(mini_ramp->bounds().width > 80.0f);
    REQUIRE(first_swatch->bounds().width > 6.0f);
    REQUIRE_FALSE(editor->visible());

    engine.evaluate("expandedPalette = 0; buildShadeRamps(); layout();");
    root.layout_children();

    auto* expanded_editor = bridge.widget("ramp-0-editor");
    auto* gamut = bridge.widget("ramp-0-gamut");
    REQUIRE(expanded_editor != nullptr);
    REQUIRE(gamut != nullptr);
    REQUIRE(expanded_editor->visible());
    REQUIRE(expanded_editor->bounds().height > 250.0f);
    REQUIRE_THAT(gamut->bounds().height, Catch::Matchers::WithinAbs(130.0f, 2.0f));
}

TEST_CASE("Design tool: brand/style cues resolve to deterministic audio-plugin presets", "[design-tool]") {
    auto js_path = find_js_file("design-tool.js");
    if (js_path.empty()) {
        SKIP("design-tool.js not found");
        return;
    }

    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::column;
    root.set_bounds({0, 0, 1100, 700});

    pulp::state::StateStore store;
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);
    load_design_tool(root, engine, bridge);

    REQUIRE(engine.evaluate("inferFallbackPreset('k1', 'give this the FabFilter precision analyzer vibe')").toString() == "precision_knob");
    REQUIRE(engine.evaluate("inferFallbackPreset('k1', 'make it feel like Moog studio hardware')").toString() == "heritage_knob");
    REQUIRE(engine.evaluate("inferFallbackPreset('slider1', 'make this feel like a Waves console strip')").toString() == "console_slider");
    REQUIRE(engine.evaluate("inferFallbackPreset('t1', 'give it an Arturia Pigments cyberpunk glow')").toString() == "illuminated_toggle");
}

TEST_CASE("Design tool: family-only widget look specs resolve to concrete presets", "[design-tool]") {
    auto js_path = find_js_file("design-tool.js");
    if (js_path.empty()) {
        SKIP("design-tool.js not found");
        return;
    }

    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::column;
    root.set_bounds({0, 0, 1100, 700});

    pulp::state::StateStore store;
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);
    load_design_tool(root, engine, bridge);

    REQUIRE(engine.evaluate("applyWidgetLook('k1', { family: 'precision_analyzer' })").getWithDefault<bool>(false));
    REQUIRE(engine.evaluate("widgetLookState.k1.preset").toString() == "precision_knob");

    REQUIRE(engine.evaluate("applyWidgetLook('slider1', { family: 'console_strip' })").getWithDefault<bool>(false));
    REQUIRE(engine.evaluate("widgetLookState.slider1.preset").toString() == "console_slider");
}

TEST_CASE("Design tool: state pills drive disabled and error preview states", "[design-tool]") {
    auto js_path = find_js_file("design-tool.js");
    if (js_path.empty()) {
        SKIP("design-tool.js not found");
        return;
    }

    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::column;
    root.set_bounds({0, 0, 1100, 700});

    pulp::state::StateStore store;
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);
    load_design_tool(root, engine, bridge);

    auto* sample_input = bridge.widget("sample-input");
    auto* sample_combo = bridge.widget("sample-combo");
    auto* panel_content = bridge.widget("panel-content");
    REQUIRE(sample_input != nullptr);
    REQUIRE(sample_combo != nullptr);
    REQUIRE(panel_content != nullptr);

    REQUIRE(sample_input->enabled());
    REQUIRE(sample_combo->enabled());

    engine.evaluate("applyStateToPreview(3)");
    root.layout_children();

    REQUIRE_FALSE(sample_input->enabled());
    REQUIRE_FALSE(sample_combo->enabled());
    REQUIRE_THAT(sample_input->opacity(), Catch::Matchers::WithinAbs(0.4f, 0.01f));
    REQUIRE_THAT(sample_combo->opacity(), Catch::Matchers::WithinAbs(0.4f, 0.01f));

    engine.evaluate("applyStateToPreview(4)");
    root.layout_children();

    REQUIRE(sample_input->enabled());
    REQUIRE(sample_combo->enabled());
    REQUIRE(panel_content->border_color() == pulp::canvas::Color::hex(0xe94560));
}
