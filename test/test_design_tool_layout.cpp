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

TEST_CASE("Design tool: composite control labels stay click-through", "[design-tool]") {
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

    REQUIRE_FALSE(bridge.widget("state-pill-0-lbl")->hit_testable());
    REQUIRE_FALSE(bridge.widget("undo-btn")->hit_testable());
    REQUIRE_FALSE(bridge.widget("palette-save-lbl")->hit_testable());
    REQUIRE_FALSE(bridge.widget("palette-load-lbl")->hit_testable());
    REQUIRE_FALSE(bridge.widget("help-modal-close-label")->hit_testable());
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

TEST_CASE("Design tool: text editor font sizes flow through the bridge", "[design-tool]") {
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

    auto* token_hex = dynamic_cast<TextEditor*>(bridge.widget("tok-0-0-hex"));
    auto* popup_hex = dynamic_cast<TextEditor*>(bridge.widget("tp-hex-input"));
    REQUIRE(token_hex != nullptr);
    REQUIRE(popup_hex != nullptr);
    REQUIRE_THAT(token_hex->font_size(), Catch::Matchers::WithinAbs(9.0f, 0.1f));
    REQUIRE_THAT(popup_hex->font_size(), Catch::Matchers::WithinAbs(11.0f, 0.1f));
}

TEST_CASE("Design tool: waveform and spectrum previews render populated data", "[design-tool]") {
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

    auto* waveform = dynamic_cast<WaveformView*>(bridge.widget("waveform"));
    auto* waveform2 = dynamic_cast<WaveformView*>(bridge.widget("waveform2"));
    auto* spectrum = dynamic_cast<SpectrumView*>(bridge.widget("spectrum-demo"));
    REQUIRE(waveform != nullptr);
    REQUIRE(waveform2 != nullptr);
    REQUIRE(spectrum != nullptr);

    pulp::canvas::RecordingCanvas waveform_canvas;
    waveform->paint(waveform_canvas);
    REQUIRE(waveform_canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) > 10);

    pulp::canvas::RecordingCanvas waveform_canvas2;
    waveform2->paint(waveform_canvas2);
    REQUIRE(waveform_canvas2.count(pulp::canvas::DrawCommand::Type::stroke_line) > 10);

    pulp::canvas::RecordingCanvas spectrum_canvas;
    spectrum->paint(spectrum_canvas);
    REQUIRE(spectrum_canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) > 3);
}

TEST_CASE("Design tool: layout preview uses loading spinner and interactive tabs", "[design-tool]") {
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

    auto* spinner = dynamic_cast<Label*>(bridge.widget("card-2-spinner"));
    auto* tabs_header = bridge.widget("tabs-header");
    auto* panel_title = dynamic_cast<Label*>(bridge.widget("panel-title"));
    REQUIRE(spinner != nullptr);
    REQUIRE(tabs_header == nullptr);
    REQUIRE(panel_title != nullptr);
    REQUIRE_FALSE(spinner->text().empty());
    REQUIRE(panel_title->text() == "Panel content area");

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('ptab-2', 'click', 0);"));
    root.layout_children();

    REQUIRE(engine.evaluate("activePreviewTab").getWithDefault<int>(-1) == 2);
    REQUIRE(panel_title->text() == "MIDI mapping");
}

TEST_CASE("Design tool: inspect click updates and clears chat context badge", "[design-tool]") {
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

    auto* context_label = dynamic_cast<Label*>(bridge.widget("context-label"));
    auto* context_clear = bridge.widget("context-clear");
    REQUIRE(context_label != nullptr);
    REQUIRE(context_clear != nullptr);
    REQUIRE(context_label->text() == "Editing: All");
    REQUIRE_FALSE(context_clear->visible());

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('__inspect__', 'click', 'k1');"));
    root.layout_children();
    REQUIRE(context_label->text() == "Editing: k1");
    REQUIRE(context_clear->visible());

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('context-clear', 'click', 0);"));
    root.layout_children();
    REQUIRE(context_label->text() == "Editing: All");
    REQUIRE_FALSE(context_clear->visible());
}

TEST_CASE("Design tool: debug prompt builder includes target and audio-plugin family guidance", "[design-tool]") {
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

    engine.evaluate("setDesignDebugTarget('k1')");
    auto prompt = engine.invoke("buildDesignChatPrompt", std::string("make the gain knob look like macOS 7")).toString();
    REQUIRE(prompt.find("Current target = k1") != std::string::npos);
    REQUIRE(prompt.find("precision_analyzer") != std::string::npos);
    REQUIRE(prompt.find("Never emit shaderBody or shader unless explicitly asked") != std::string::npos);
}

TEST_CASE("Design tool: AI provider selector switches model options and effort visibility", "[design-tool]") {
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

    auto* effort_selector = bridge.widget("effort-selector");
    REQUIRE(effort_selector != nullptr);
    REQUIRE_FALSE(effort_selector->visible());

    engine.evaluate("setDesignDebugAIConfig('codex', 'gpt-5.4', 'xhigh');");
    root.layout_children();

    REQUIRE(effort_selector->visible());
    REQUIRE(engine.evaluate("getSelectedAIProvider()").toString() == "codex");
    REQUIRE(engine.evaluate("getSelectedAIModel()").toString() == "gpt-5.4");
    REQUIRE(engine.evaluate("getSelectedAIReasoningEffort()").toString() == "xhigh");
}

TEST_CASE("Design tool: debug state captures applied widget look metadata", "[design-tool]") {
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

    engine.evaluate("setDesignDebugTarget('k1')");
    engine.evaluate("lastChatRequestText = 'make the gain knob look like macOS 7'");
    auto summary = engine.invoke("applyDesignChatResponse", std::string(R"({"widgetLooks":{"k1":{"preset":"macos7_knob","params":{"gloss":0.92,"metalness":0.35,"rim":0.28}}}})")).toString();
    REQUIRE(summary.find("widget looks") != std::string::npos);

    auto debug_json = engine.invoke("getDesignDebugStateJson").toString();
    REQUIRE(debug_json.find("\"target\":\"k1\"") != std::string::npos);
    REQUIRE(debug_json.find("\"targetBounds\":{") != std::string::npos);
    REQUIRE(debug_json.find("\"width\":56") != std::string::npos);
    REQUIRE(debug_json.find("\"widgetLookIds\":[\"k1\"]") != std::string::npos);
    REQUIRE(debug_json.find("\"widgetLookCount\":1") != std::string::npos);
    REQUIRE(debug_json.find("\"status\":\"ok\"") != std::string::npos);
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

TEST_CASE("Design tool: expanded palette exposes color format values and compact shade chips", "[design-tool]") {
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

    engine.evaluate("expandedPalette = 0; buildShadeRamps(); layout();");
    root.layout_children();

    auto* format_combo = bridge.widget("ramp-0-format");
    auto* value_key_0 = dynamic_cast<Label*>(bridge.widget("ramp-0-value-0-key"));
    auto* value_key_1 = dynamic_cast<Label*>(bridge.widget("ramp-0-value-1-key"));
    auto* value_text_0 = dynamic_cast<Label*>(bridge.widget("ramp-0-value-0-text"));
    auto* shade_chip = bridge.widget("ramp-0-lg-0");
    REQUIRE(format_combo != nullptr);
    REQUIRE(value_key_0 != nullptr);
    REQUIRE(value_key_1 != nullptr);
    REQUIRE(value_text_0 != nullptr);
    REQUIRE(shade_chip != nullptr);

    REQUIRE(value_key_0->text() == "L");
    REQUIRE(value_key_1->text() == "C");
    REQUIRE(value_text_0->text().size() >= 4);
    REQUIRE_THAT(shade_chip->bounds().width, Catch::Matchers::WithinAbs(40.0f, 1.0f));
    REQUIRE_THAT(shade_chip->bounds().height, Catch::Matchers::WithinAbs(30.0f, 1.0f));

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('ramp-0-format', 'select', 1);"));
    root.layout_children();

    REQUIRE(value_key_0->text() == "R");
    REQUIRE(value_key_1->text() == "G");
    REQUIRE(value_text_0->text().size() == 2);

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('ramp-0-format', 'select', 2);"));
    root.layout_children();

    REQUIRE(value_key_0->text() == "R");
    REQUIRE(value_key_1->text() == "G");
    REQUIRE(value_text_0->text().size() >= 1);
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
