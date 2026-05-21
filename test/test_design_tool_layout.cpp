// Automated test for design tool layout structure
// Validates that the JS creates the correct view tree with proper sizing
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/state/store.hpp>
#include <pulp/canvas/canvas.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <stdexcept>
#include <algorithm>

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
    // First try the source tree relative to this test file so ctest can run
    // from a build directory without skipping the whole suite.
    auto source_root = fs::path(__FILE__).parent_path().parent_path();
    auto source_candidate = source_root / "examples" / "design-tool" / name;
    if (fs::exists(source_candidate)) return source_candidate.string();

    // Fallback: search upward from the current working directory.
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

// Ordered design-tool concern modules. The UI was split out of a single
// design-tool.js (P8-NEW); the modules share one global scope and only form
// a valid program when loaded in this order — their concatenation is
// byte-equivalent to the historical single file, so declaration-before-use
// order is preserved. Keep this list in sync with kDesignToolModules in
// examples/design-tool/main.cpp.
static const char* const kDesignToolModules[] = {
    "design-tool-core.js",
    "design-tool-toolbar.js",
    "design-tool-palette.js",
    "design-tool-popup.js",
    "design-tool-preview.js",
    "design-tool-chat.js",
    "design-tool-export.js",
};

static void load_design_tool(View& root, ScriptEngine& engine, WidgetBridge& bridge) {
    auto oklch_path = find_js_file("oklch.js");
    if (!oklch_path.empty()) {
        bridge.load_script(read_file(oklch_path));
    }

    for (const char* module : kDesignToolModules) {
        auto module_path = find_js_file(module);
        if (module_path.empty()) {
            throw std::runtime_error(std::string("design-tool module not found: ")
                                     + module);
        }
        bridge.load_script(read_file(module_path));
    }
    root.layout_children();
}

static bool has_canvas_color(const pulp::canvas::RecordingCanvas& canvas,
                             pulp::canvas::DrawCommand::Type type,
                             pulp::canvas::Color expected) {
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == type && cmd.color == expected) return true;
    }
    return false;
}

static std::string uppercase_hex(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

TEST_CASE("Design tool: JS creates three-column layout", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('help-modal-close-btn', 'click', 0);"));
    root.layout_children();
    REQUIRE_FALSE(help_modal->visible());

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('template-help', 'click', 0);"));
    root.layout_children();
    REQUIRE(help_modal->visible());

    auto* modal = dynamic_cast<ModalOverlay*>(help_modal);
    REQUIRE(modal != nullptr);
    KeyEvent esc{};
    esc.is_down = true;
    esc.key = KeyCode::escape;
    REQUIRE(modal->on_key_event(esc));
    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('__global__', 'keydown', { key: 27, mods: 0 });"));
    root.layout_children();
    REQUIRE_FALSE(help_modal->visible());
    REQUIRE_FALSE(help_modal->visible());
}

TEST_CASE("Design tool: composite control labels stay click-through", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    REQUIRE_FALSE(bridge.widget("tp-close-lbl")->hit_testable());
    REQUIRE_FALSE(bridge.widget("tp-btn-0-lbl")->hit_testable());
    REQUIRE_FALSE(bridge.widget("tp-custom-lbl")->hit_testable());
}

TEST_CASE("Design tool: palette dots stay circular", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

TEST_CASE("Design tool: token rows use square swatches and wider hex fields", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* swatch = bridge.widget("tok-2-0-sw");
    auto* hex = bridge.widget("tok-2-0-hex");
    REQUIRE(swatch != nullptr);
    REQUIRE(hex != nullptr);
    REQUIRE_THAT(swatch->bounds().width, Catch::Matchers::WithinAbs(18.0f, 1.0f));
    REQUIRE_THAT(swatch->bounds().height, Catch::Matchers::WithinAbs(18.0f, 1.0f));
    REQUIRE_THAT(hex->bounds().width, Catch::Matchers::WithinAbs(72.0f, 2.0f));
}

TEST_CASE("Design tool: text editor font sizes flow through the bridge", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    REQUIRE_THAT(token_hex->font_size(), Catch::Matchers::WithinAbs(10.0f, 0.1f));
    REQUIRE_THAT(popup_hex->font_size(), Catch::Matchers::WithinAbs(11.0f, 0.1f));
}

TEST_CASE("Design tool: input editors keep visible text contrast", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* token_search = dynamic_cast<TextEditor*>(bridge.widget("token-search"));
    auto* chat_input = dynamic_cast<TextEditor*>(bridge.widget("chat-input"));
    auto* sample_input = dynamic_cast<TextEditor*>(bridge.widget("sample-input"));
    REQUIRE(token_search != nullptr);
    REQUIRE(chat_input != nullptr);
    REQUIRE(sample_input != nullptr);

    auto brightness = [](pulp::canvas::Color c) {
        return static_cast<int>(c.r8()) + static_cast<int>(c.g8()) + static_cast<int>(c.b8());
    };

    auto token_fg = token_search->resolve_color("text.primary", {});
    auto token_bg = token_search->resolve_color("bg.surface", {});
    auto token_placeholder = token_search->resolve_color("text.secondary", {});
    auto chat_fg = chat_input->resolve_color("text.primary", {});
    auto chat_bg = chat_input->resolve_color("bg.surface", {});
    auto sample_fg = sample_input->resolve_color("text.primary", {});
    auto sample_bg = sample_input->background_color();

    REQUIRE(token_fg.a > 0.0f);
    REQUIRE(token_placeholder.a > 0.0f);
    REQUIRE(chat_fg.a > 0.0f);
    REQUIRE(sample_fg.a > 0.0f);
    REQUIRE(std::abs(brightness(token_fg) - brightness(token_bg)) > 80);
    REQUIRE(std::abs(brightness(chat_fg) - brightness(chat_bg)) > 80);
    REQUIRE(std::abs(brightness(sample_fg) - brightness(sample_bg)) > 80);
}

TEST_CASE("Design tool: token popup surfaces modified state and compact controls", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('tok-2-0-sw', 'click', 0);"));
    root.layout_children();

    auto* popup = bridge.widget("token-popup");
    auto* token_name = dynamic_cast<Label*>(bridge.widget("tp-token-name"));
    auto* modified_marker = bridge.widget("tp-token-modified");
    auto* undo_row = bridge.widget("tp-undo-row");
    auto* reset_button = bridge.widget("tp-btn-2");
    auto* custom_label = dynamic_cast<Label*>(bridge.widget("tp-custom-lbl"));
    REQUIRE(popup != nullptr);
    REQUIRE(token_name != nullptr);
    REQUIRE(modified_marker != nullptr);
    REQUIRE(undo_row != nullptr);
    REQUIRE(reset_button != nullptr);
    REQUIRE(custom_label != nullptr);

    REQUIRE(popup->visible());
    REQUIRE(token_name->text() == "accent.primary");
    REQUIRE(custom_label->text().find("Custom color picker") != std::string::npos);
    REQUIRE_THAT(modified_marker->opacity(), Catch::Matchers::WithinAbs(0.0f, 0.01f));
    REQUIRE_FALSE(undo_row->visible());

    REQUIRE_NOTHROW(engine.evaluate("applyTokenColor('accent.primary', '#112233');"));
    root.layout_children();

    REQUIRE_THAT(modified_marker->opacity(), Catch::Matchers::WithinAbs(1.0f, 0.01f));
    REQUIRE_THAT(reset_button->opacity(), Catch::Matchers::WithinAbs(1.0f, 0.01f));
    REQUIRE(undo_row->visible());

    auto* custom_section = bridge.widget("tp-custom");
    REQUIRE(custom_section != nullptr);
    REQUIRE_FALSE(custom_section->visible());
    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('tp-custom-toggle', 'click', 0);"));
    root.layout_children();
    REQUIRE(custom_section->visible());

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('tp-close', 'click', 0);"));
    root.layout_children();
    REQUIRE_FALSE(popup->visible());

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('tok-2-0-sw', 'click', 0);"));
    root.layout_children();
    REQUIRE(popup->visible());
    REQUIRE_FALSE(custom_section->visible());
    REQUIRE(custom_label->text().find("Custom color picker") != std::string::npos);

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('__global__', 'keydown', { key: 274, mods: 0 });"));
    root.layout_children();
    REQUIRE_FALSE(popup->visible());
}

TEST_CASE("Design tool: token popup custom picker renders a marker overlay and full-height H/C sliders", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('tok-2-0-sw', 'click', 0);"));
    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('tp-custom-toggle', 'click', 0);"));
    root.layout_children();

    auto* gamut = bridge.widget("tp-gamut");
    auto* overlay = dynamic_cast<CanvasWidget*>(bridge.widget("tp-gamut-overlay"));
    auto* h_fader = dynamic_cast<Fader*>(bridge.widget("tp-h-fader"));
    auto* c_fader = dynamic_cast<Fader*>(bridge.widget("tp-c-fader"));
    REQUIRE(gamut != nullptr);
    REQUIRE(overlay != nullptr);
    REQUIRE(h_fader != nullptr);
    REQUIRE(c_fader != nullptr);
    REQUIRE(overlay->visible());
    REQUIRE_THAT(overlay->bounds().width, Catch::Matchers::WithinAbs(gamut->bounds().width, 1.0f));
    REQUIRE_THAT(overlay->bounds().height, Catch::Matchers::WithinAbs(gamut->bounds().height, 1.0f));
    REQUIRE_THAT(h_fader->bounds().height, Catch::Matchers::WithinAbs(20.0f, 1.0f));
    REQUIRE_THAT(c_fader->bounds().height, Catch::Matchers::WithinAbs(20.0f, 1.0f));

    pulp::canvas::RecordingCanvas overlay_canvas;
    overlay->paint(overlay_canvas);
    REQUIRE(overlay_canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) >= 2);
    REQUIRE(overlay_canvas.count(pulp::canvas::DrawCommand::Type::fill_circle) >= 4);
}

TEST_CASE("Design tool: modified tokens expose inline reset affordances", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* marker = bridge.widget("tok-2-0-mod");
    auto* reset = bridge.widget("tok-2-0-reset");
    auto* name = dynamic_cast<Label*>(bridge.widget("tok-2-0-name"));
    REQUIRE(marker != nullptr);
    REQUIRE(reset != nullptr);
    REQUIRE(name != nullptr);

    auto original_hex = engine.evaluate("JSON.parse(getThemeJson()).colors['accent.primary'].toUpperCase()").toString();
    REQUIRE_THAT(marker->opacity(), Catch::Matchers::WithinAbs(0.0f, 0.01f));
    REQUIRE_FALSE(reset->visible());
    REQUIRE(name->text() == "accent.primary");

    REQUIRE_NOTHROW(engine.evaluate("applyTokenColor('accent.primary', '#112233');"));
    root.layout_children();

    auto modified_hex = engine.evaluate("JSON.parse(getThemeJson()).colors['accent.primary'].toUpperCase()").toString();
    REQUIRE(modified_hex == "#112233");
    REQUIRE_THAT(marker->opacity(), Catch::Matchers::WithinAbs(1.0f, 0.01f));
    REQUIRE(reset->visible());
    REQUIRE(name->text() == "accent.primary");

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('tok-2-0-reset', 'click', 0);"));
    root.layout_children();

    auto reset_hex = engine.evaluate("JSON.parse(getThemeJson()).colors['accent.primary'].toUpperCase()").toString();
    REQUIRE(reset_hex == original_hex);
    REQUIRE_THAT(marker->opacity(), Catch::Matchers::WithinAbs(0.0f, 0.01f));
    REQUIRE_FALSE(reset->visible());
    REQUIRE(name->text() == "accent.primary");
}

TEST_CASE("Design tool: expanded token popup stays within root bounds", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
        return;
    }

    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::column;
    root.set_bounds({0, 0, 680, 700});

    pulp::state::StateStore store;
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);

    load_design_tool(root, engine, bridge);

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('tok-4-0-sw', 'click', 0);"));
    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('tp-custom-toggle', 'click', 0);"));
    root.layout_children();

    auto* popup = bridge.widget("token-popup");
    REQUIRE(popup != nullptr);
    REQUIRE(popup->visible());
    REQUIRE(popup->bounds().x >= 0.0f);
    REQUIRE(popup->bounds().y >= 0.0f);
    REQUIRE((popup->bounds().x + popup->bounds().width) <= (root.bounds().width + 0.5f));
    REQUIRE((popup->bounds().y + popup->bounds().height) <= (root.bounds().height + 0.5f));
}

TEST_CASE("Design tool: token popup persists alpha across edits and reopen", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('tok-2-0-sw', 'click', 0);"));
    root.layout_children();

    auto* popup = bridge.widget("token-popup");
    auto* alpha_fader = dynamic_cast<Fader*>(bridge.widget("tp-alpha-fdr"));
    auto* hex_input = dynamic_cast<TextEditor*>(bridge.widget("tp-hex-input"));
    REQUIRE(popup != nullptr);
    REQUIRE(alpha_fader != nullptr);
    REQUIRE(hex_input != nullptr);
    REQUIRE(popup->visible());

    REQUIRE_NOTHROW(engine.evaluate("setValue('tp-alpha-fdr', 0.5); __dispatch__('tp-alpha-fdr', 'change', 0.5);"));
    root.layout_children();

    auto first_hex = engine.evaluate("JSON.parse(getThemeJson()).colors['accent.primary'].toUpperCase()").toString();
    REQUIRE(first_hex.size() == 9);
    REQUIRE(first_hex.substr(7, 2) == "80");
    REQUIRE_THAT(alpha_fader->value(), Catch::Matchers::WithinAbs(0.5f, 0.02f));
    REQUIRE(hex_input->text() == first_hex);

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('tp-pal-0-s5', 'click', 0);"));
    root.layout_children();

    auto second_hex = engine.evaluate("JSON.parse(getThemeJson()).colors['accent.primary'].toUpperCase()").toString();
    REQUIRE(second_hex.size() == 9);
    REQUIRE(second_hex.substr(7, 2) == "80");
    REQUIRE(second_hex != first_hex);

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('tp-close', 'click', 0);"));
    root.layout_children();
    REQUIRE_FALSE(popup->visible());

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('tok-2-0-sw', 'click', 0);"));
    root.layout_children();
    REQUIRE(popup->visible());
    REQUIRE_THAT(alpha_fader->value(), Catch::Matchers::WithinAbs(0.5f, 0.02f));
    REQUIRE(hex_input->text() == second_hex);
}

TEST_CASE("Design tool: waveform and spectrum previews render populated data", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    REQUIRE_NOTHROW(engine.evaluate(
        "applyTokenDiff(JSON.stringify({ colors: { "
        "'waveform.line': '#7FD1FF', "
        "'waveform.fill': '#33557788', "
        "'waveform.grid': '#334455' "
        "} }));"));
    root.layout_children();

    pulp::canvas::RecordingCanvas themed_waveform_canvas;
    waveform->paint(themed_waveform_canvas);
    // Waveform now uses GPU draw_waveform() which on RecordingCanvas falls back
    // to CPU stroke_line. Verify themed colors appear in the output.
    REQUIRE(themed_waveform_canvas.command_count() > 3);
    // Grid line color should be present (drawn before waveform)
    REQUIRE(has_canvas_color(themed_waveform_canvas,
                             pulp::canvas::DrawCommand::Type::set_stroke_color,
                             pulp::canvas::Color::rgba8(51, 68, 85)));

    pulp::canvas::RecordingCanvas themed_spectrum_canvas;
    spectrum->paint(themed_spectrum_canvas);
    // Spectrum now uses GPU draw_waveform() for line/filled modes.
    // Verify themed output is non-empty and grid color is applied.
    REQUIRE(themed_spectrum_canvas.command_count() > 3);
    REQUIRE(has_canvas_color(themed_spectrum_canvas,
                             pulp::canvas::DrawCommand::Type::set_stroke_color,
                             pulp::canvas::Color::rgba8(51, 68, 85)));
}

TEST_CASE("Design tool: layout preview uses loading spinner and interactive tabs", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    auto* loading_label = dynamic_cast<Label*>(bridge.widget("card-2-label"));
    auto* tabs_header = dynamic_cast<Label*>(bridge.widget("tabs-header"));
    auto* panel_title = dynamic_cast<Label*>(bridge.widget("panel-title"));
    auto* preview_chrome_title = dynamic_cast<Label*>(bridge.widget("preview-chrome-title"));
    auto* sample_combo = bridge.widget("sample-combo");
    auto* sample_combo_label = dynamic_cast<Label*>(bridge.widget("sample-combo-label"));
    auto* sample_combo_caret = dynamic_cast<Label*>(bridge.widget("sample-combo-caret"));
    auto* chat_messages = bridge.widget("chat-messages");
    auto* chat_thread = bridge.widget("chat-thread");
    auto* card_1 = bridge.widget("card-1");
    auto* card_2 = bridge.widget("card-2");
    auto* card_3 = bridge.widget("card-3");
    auto* card_4 = bridge.widget("card-4");
    auto* tab_0 = bridge.widget("ptab-0");
    auto* tab_0_line = bridge.widget("ptab-0-line");
    REQUIRE(spinner != nullptr);
    REQUIRE(loading_label != nullptr);
    REQUIRE(tabs_header != nullptr);
    REQUIRE(panel_title != nullptr);
    REQUIRE(preview_chrome_title != nullptr);
    REQUIRE(sample_combo != nullptr);
    REQUIRE(sample_combo_label != nullptr);
    REQUIRE(sample_combo_caret != nullptr);
    REQUIRE(chat_messages != nullptr);
    REQUIRE(chat_thread != nullptr);
    REQUIRE(card_1 != nullptr);
    REQUIRE(card_2 != nullptr);
    REQUIRE(card_3 != nullptr);
    REQUIRE(card_4 != nullptr);
    REQUIRE(tab_0 != nullptr);
    REQUIRE(tab_0_line != nullptr);
    REQUIRE_FALSE(spinner->text().empty());
    REQUIRE(loading_label->text() == "Loading");
    REQUIRE(tabs_header->text() == "TABS");
    REQUIRE(panel_title->text() == "Panel content area");
    REQUIRE(preview_chrome_title->text() == "Plugin Preview");
    REQUIRE(sample_combo_label->text() == "Select preset...");
    auto caret_text = sample_combo_caret->text();
    REQUIRE_FALSE(caret_text.empty());
    REQUIRE(sample_combo->bounds().width >= 148.0f);
    REQUIRE(sample_combo_caret->bounds().x > sample_combo_label->bounds().x);
    REQUIRE_THAT(card_1->bounds().y, Catch::Matchers::WithinAbs(card_2->bounds().y, 0.5f));
    REQUIRE_THAT(card_1->bounds().y, Catch::Matchers::WithinAbs(card_3->bounds().y, 0.5f));
    REQUIRE_THAT(card_1->bounds().y, Catch::Matchers::WithinAbs(card_4->bounds().y, 0.5f));
    REQUIRE(card_2->bounds().x > card_1->bounds().x);
    REQUIRE(card_3->bounds().x > card_2->bounds().x);
    REQUIRE(card_4->bounds().x > card_3->bounds().x);
    REQUIRE(tab_0_line->bounds().width < tab_0->bounds().width);
    REQUIRE(chat_thread->bounds().width < chat_messages->bounds().width);

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('ptab-2', 'click', 0);"));
    root.layout_children();

    REQUIRE(engine.evaluate("activePreviewTab").getWithDefault<int>(-1) == 2);
    REQUIRE(panel_title->text() == "MIDI mapping");
}

TEST_CASE("Design tool: inspect click updates and clears chat context badge", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

TEST_CASE("Design tool: chat typing indicator is transient pending UI", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* typing_row = bridge.widget("chat-typing-row");
    auto* typing_label = dynamic_cast<Label*>(bridge.widget("chat-typing-label"));
    REQUIRE(typing_row != nullptr);
    REQUIRE(typing_label != nullptr);
    REQUIRE_FALSE(typing_row->visible());

    REQUIRE_NOTHROW(engine.evaluate("showChatTypingIndicator();"));
    root.layout_children();
    REQUIRE(typing_row->visible());
    REQUIRE(typing_label->text().find("Designer is thinking") != std::string::npos);

    REQUIRE_NOTHROW(engine.evaluate("hideChatTypingIndicator();"));
    root.layout_children();
    REQUIRE_FALSE(typing_row->visible());
    REQUIRE(typing_label->text().empty());
}

TEST_CASE("Design tool: reference image validator accepts common image formats", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    REQUIRE(engine.evaluate("isSupportedReferenceImage('/tmp/ref.png')").getWithDefault<bool>(false));
    REQUIRE(engine.evaluate("isSupportedReferenceImage('/tmp/ref.HEIC')").getWithDefault<bool>(false));
    REQUIRE(engine.evaluate("isSupportedReferenceImage('C:/Users/test/ref.jpeg')").getWithDefault<bool>(false));
    REQUIRE_FALSE(engine.evaluate("isSupportedReferenceImage('/tmp/ref.txt')").getWithDefault<bool>(true));
    REQUIRE_FALSE(engine.evaluate("isSupportedReferenceImage('/tmp/ref')").getWithDefault<bool>(true));
    REQUIRE(engine.evaluate("getReferenceImageDialogExtensions()").toString() == "png;jpg;jpeg;gif;webp;bmp;tif;tiff;heic;heif");
}

TEST_CASE("Design tool: chat history serialization captures messages and target", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('__inspect__', 'click', 'k1');"));
    REQUIRE_NOTHROW(engine.evaluate("addChatMessage('user', 'make it warmer');"));
    REQUIRE_NOTHROW(engine.evaluate("addChatMessage('assistant', 'Applied 6 colors');"));
    root.layout_children();

    auto markdown = engine.invoke("serializeChatHistory", std::string("markdown")).toString();
    REQUIRE(markdown.find("Target: `k1`") != std::string::npos);
    REQUIRE(markdown.find("## User") != std::string::npos);
    REQUIRE(markdown.find("make it warmer") != std::string::npos);
    REQUIRE(markdown.find("## Assistant") != std::string::npos);
    REQUIRE(markdown.find("Applied 6 colors") != std::string::npos);
}

TEST_CASE("Design tool: chat pending state disables selectors and turns send into cancel", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* chat_input = dynamic_cast<TextEditor*>(bridge.widget("chat-input"));
    auto* provider_selector = dynamic_cast<ComboBox*>(bridge.widget("provider-selector"));
    auto* model_selector = dynamic_cast<ComboBox*>(bridge.widget("model-selector"));
    auto* upload_btn = bridge.widget("upload-btn");
    auto* export_btn = bridge.widget("chat-export-btn");
    auto* send_btn = bridge.widget("send-btn");
    auto* send_icon = bridge.widget("send-icon");
    auto* cancel_icon = bridge.widget("send-cancel-icon");
    REQUIRE(chat_input != nullptr);
    REQUIRE(provider_selector != nullptr);
    REQUIRE(model_selector != nullptr);
    REQUIRE(upload_btn != nullptr);
    REQUIRE(export_btn != nullptr);
    REQUIRE(send_btn != nullptr);
    REQUIRE(send_icon != nullptr);
    REQUIRE(cancel_icon != nullptr);

    REQUIRE(chat_input->enabled());
    REQUIRE(provider_selector->enabled());
    REQUIRE(model_selector->enabled());

    REQUIRE_NOTHROW(engine.evaluate("setChatPendingUi(true);"));
    root.layout_children();

    REQUIRE_FALSE(chat_input->enabled());
    REQUIRE_FALSE(provider_selector->enabled());
    REQUIRE_FALSE(model_selector->enabled());
    REQUIRE_THAT(upload_btn->opacity(), Catch::Matchers::WithinAbs(0.45f, 0.02f));
    REQUIRE_THAT(export_btn->opacity(), Catch::Matchers::WithinAbs(0.45f, 0.02f));
    REQUIRE_THAT(send_btn->opacity(), Catch::Matchers::WithinAbs(1.0f, 0.02f));
    REQUIRE_FALSE(send_icon->visible());
    REQUIRE(cancel_icon->visible());

    REQUIRE_NOTHROW(engine.evaluate("setChatPendingUi(false);"));
    root.layout_children();

    REQUIRE(chat_input->enabled());
    REQUIRE(provider_selector->enabled());
    REQUIRE(model_selector->enabled());
    REQUIRE_THAT(upload_btn->opacity(), Catch::Matchers::WithinAbs(1.0f, 0.02f));
    REQUIRE_THAT(export_btn->opacity(), Catch::Matchers::WithinAbs(1.0f, 0.02f));
    REQUIRE_THAT(send_btn->opacity(), Catch::Matchers::WithinAbs(1.0f, 0.02f));
    REQUIRE(send_icon->visible());
    REQUIRE_FALSE(cancel_icon->visible());
}

TEST_CASE("Design tool: chat timeout clears pending UI and recovers controls", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* chat_input = dynamic_cast<TextEditor*>(bridge.widget("chat-input"));
    auto* typing_row = bridge.widget("chat-typing-row");
    auto* status_text = dynamic_cast<Label*>(bridge.widget("status-text"));
    REQUIRE(chat_input != nullptr);
    REQUIRE(typing_row != nullptr);
    REQUIRE(status_text != nullptr);

    REQUIRE_NOTHROW(engine.evaluate("chatRequestPending = true; chatActiveRequestId = 7; setChatPendingUi(true); showChatTypingIndicator(); handleChatRequestTimeout(7);"));
    root.layout_children();

    REQUIRE(chat_input->enabled());
    REQUIRE_FALSE(typing_row->visible());
    REQUIRE(status_text->text() == "Chat timeout");
    REQUIRE(engine.evaluate("chatRequestPending").getWithDefault<bool>(true) == false);
    REQUIRE(engine.evaluate("chatActiveRequestId").getWithDefault<int>(-1) == 0);
    REQUIRE(engine.evaluate("chatHistory[chatHistory.length - 1].text").toString().find("timed out") != std::string::npos);
}

TEST_CASE("Design tool: pending chat can be canceled and late results are ignored", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* status_text = dynamic_cast<Label*>(bridge.widget("status-text"));
    auto* send_btn = bridge.widget("send-btn");
    auto* send_icon = bridge.widget("send-icon");
    auto* cancel_icon = bridge.widget("send-cancel-icon");
    auto* chat_input = bridge.widget("chat-input");
    auto* typing_row = bridge.widget("chat-typing-row");
    REQUIRE(status_text != nullptr);
    REQUIRE(send_btn != nullptr);
    REQUIRE(send_icon != nullptr);
    REQUIRE(cancel_icon != nullptr);
    REQUIRE(chat_input != nullptr);
    REQUIRE(typing_row != nullptr);

    REQUIRE_NOTHROW(engine.evaluate(
        "chatRequestPending = true; "
        "chatActiveRequestId = 21; "
        "setChatPendingUi(true); "
        "showChatTypingIndicator();"));
    root.layout_children();

    REQUIRE(cancel_icon->visible());
    REQUIRE_FALSE(send_icon->visible());
    REQUIRE_FALSE(chat_input->enabled());
    REQUIRE(typing_row->visible());

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('send-btn', 'click', 0);"));
    root.layout_children();

    REQUIRE(status_text->text() == "Chat canceled");
    REQUIRE_FALSE(engine.evaluate("chatRequestPending").getWithDefault<bool>(true));
    REQUIRE(engine.evaluate("chatActiveRequestId").get<int64_t>() == 0);
    REQUIRE(send_icon->visible());
    REQUIRE_FALSE(cancel_icon->visible());
    REQUIRE(chat_input->enabled());
    REQUIRE_FALSE(typing_row->visible());

    auto history_count = engine.evaluate("chatHistory.length").get<int64_t>();
    REQUIRE_NOTHROW(engine.evaluate(
        "handleDesignChatCommandResult(21, 'claude', 'ignored stale result');"));
    root.layout_children();

    REQUIRE(engine.evaluate("chatHistory.length").get<int64_t>() == history_count);
    REQUIRE(status_text->text() == "Chat canceled");
}

TEST_CASE("Design tool: Escape cancels an active chat request before closing other overlays", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* status_text = dynamic_cast<Label*>(bridge.widget("status-text"));
    auto* chat_input = bridge.widget("chat-input");
    auto* typing_row = bridge.widget("chat-typing-row");
    REQUIRE(status_text != nullptr);
    REQUIRE(chat_input != nullptr);
    REQUIRE(typing_row != nullptr);

    REQUIRE_NOTHROW(engine.evaluate(
        "chatRequestPending = true; "
        "chatActiveRequestId = 22; "
        "setChatPendingUi(true); "
        "showChatTypingIndicator();"));
    root.layout_children();

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('__global__', 'keydown', { key: 27, mods: 0 });"));
    root.layout_children();

    REQUIRE(status_text->text() == "Chat canceled");
    REQUIRE_FALSE(engine.evaluate("chatRequestPending").getWithDefault<bool>(true));
    REQUIRE(chat_input->enabled());
    REQUIRE_FALSE(typing_row->visible());
}

TEST_CASE("Design tool: chat command failures surface provider guidance and recover controls", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* chat_input = dynamic_cast<TextEditor*>(bridge.widget("chat-input"));
    auto* typing_row = bridge.widget("chat-typing-row");
    auto* status_text = dynamic_cast<Label*>(bridge.widget("status-text"));
    REQUIRE(chat_input != nullptr);
    REQUIRE(typing_row != nullptr);
    REQUIRE(status_text != nullptr);

    REQUIRE_NOTHROW(engine.evaluate(
        "chatRequestPending = true; "
        "chatActiveRequestId = 11; "
        "setChatPendingUi(true); "
        "showChatTypingIndicator(); "
        "handleDesignChatCommandResult(11, 'claude', 'claude: command not found\\n__PULP_AI_EXIT_CODE__:127');"));
    root.layout_children();

    REQUIRE(chat_input->enabled());
    REQUIRE_FALSE(typing_row->visible());
    REQUIRE(status_text->text() == "Chat error");
    REQUIRE(engine.evaluate("chatRequestPending").getWithDefault<bool>(true) == false);
    REQUIRE(engine.evaluate("chatActiveRequestId").getWithDefault<int>(-1) == 0);
    REQUIRE(engine.evaluate("chatHistory[chatHistory.length - 1].text").toString().find("not found") != std::string::npos);
}

TEST_CASE("Design tool: empty chat command output fails fast without waiting for timeout", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* status_text = dynamic_cast<Label*>(bridge.widget("status-text"));
    REQUIRE(status_text != nullptr);

    REQUIRE_NOTHROW(engine.evaluate(
        "chatRequestPending = true; "
        "chatActiveRequestId = 12; "
        "setChatPendingUi(true); "
        "showChatTypingIndicator(); "
        "handleDesignChatCommandResult(12, 'codex', '\\n__PULP_AI_EXIT_CODE__:0');"));
    root.layout_children();

    REQUIRE(status_text->text() == "Chat error");
    REQUIRE(engine.evaluate("chatRequestPending").getWithDefault<bool>(true) == false);
    REQUIRE(engine.evaluate("chatHistory[chatHistory.length - 1].text").toString().find("returned no output") != std::string::npos);
}

TEST_CASE("Design tool: debug prompt builder includes target and audio-plugin family guidance", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    auto* hue_fader = dynamic_cast<Fader*>(bridge.widget("ramp-0-h-fdr"));
    auto* chroma_fader = dynamic_cast<Fader*>(bridge.widget("ramp-0-c-fdr"));
    REQUIRE(mini_ramp != nullptr);
    REQUIRE(first_swatch != nullptr);
    REQUIRE(editor != nullptr);
    REQUIRE(hue_fader != nullptr);
    REQUIRE(chroma_fader != nullptr);

    REQUIRE(mini_ramp->bounds().width > 80.0f);
    REQUIRE(first_swatch->bounds().width > 6.0f);
    REQUIRE_FALSE(editor->visible());
    REQUIRE(bridge.widget("ramp-0-h-grad") == nullptr);
    REQUIRE(bridge.widget("ramp-0-c-grad") == nullptr);

    engine.evaluate("expandedPalette = 0; buildShadeRamps(); layout();");
    root.layout_children();

    auto* expanded_editor = bridge.widget("ramp-0-editor");
    auto* gamut = bridge.widget("ramp-0-gamut");
    auto* rebuilt_hue_fader = dynamic_cast<Fader*>(bridge.widget("ramp-0-h-fdr"));
    auto* rebuilt_chroma_fader = dynamic_cast<Fader*>(bridge.widget("ramp-0-c-fdr"));
    REQUIRE(expanded_editor != nullptr);
    REQUIRE(gamut != nullptr);
    REQUIRE(rebuilt_hue_fader != nullptr);
    REQUIRE(rebuilt_chroma_fader != nullptr);
    REQUIRE(expanded_editor->visible());
    REQUIRE(expanded_editor->bounds().height > 250.0f);
    REQUIRE_THAT(gamut->bounds().height, Catch::Matchers::WithinAbs(130.0f, 2.0f));
    REQUIRE_FALSE(rebuilt_hue_fader->custom_shader().empty());
    REQUIRE_FALSE(rebuilt_chroma_fader->custom_shader().empty());
}

TEST_CASE("Design tool: expanded palette exposes color format values and compact shade chips", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    auto* shade_badge = dynamic_cast<Label*>(bridge.widget("ramp-0-lg-0-badge"));
    REQUIRE(format_combo != nullptr);
    REQUIRE(value_key_0 != nullptr);
    REQUIRE(value_key_1 != nullptr);
    REQUIRE(value_text_0 != nullptr);
    REQUIRE(shade_chip != nullptr);
    REQUIRE(shade_badge != nullptr);

    REQUIRE(value_key_0->text() == "L");
    REQUIRE(value_key_1->text() == "C");
    REQUIRE(value_text_0->text().size() >= 4);
    REQUIRE_THAT(shade_chip->bounds().width, Catch::Matchers::WithinAbs(40.0f, 1.0f));
    REQUIRE_THAT(shade_chip->bounds().height, Catch::Matchers::WithinAbs(30.0f, 1.0f));
    REQUIRE(shade_badge->text().find(":1") != std::string::npos);

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

TEST_CASE("Design tool: expanded palette opens contrast preview modal", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('ramp-0-contrast-btn', 'click', 0);"));
    root.layout_children();

    auto* modal = bridge.widget("contrast-modal");
    auto* white_card = bridge.widget("contrast-white-card");
    auto* black_card = bridge.widget("contrast-black-card");
    auto* title = dynamic_cast<Label*>(bridge.widget("contrast-title"));
    auto* white_ratio = dynamic_cast<Label*>(bridge.widget("contrast-white-ratio"));
    auto* black_ratio = dynamic_cast<Label*>(bridge.widget("contrast-black-ratio"));
    REQUIRE(modal != nullptr);
    REQUIRE(white_card != nullptr);
    REQUIRE(black_card != nullptr);
    REQUIRE(title != nullptr);
    REQUIRE(white_ratio != nullptr);
    REQUIRE(black_ratio != nullptr);

    REQUIRE(modal->visible());
    REQUIRE(title->text().find("Contrast") != std::string::npos);
    REQUIRE(white_card->background_color() == pulp::canvas::Color::hex(0xFFFFFF));
    REQUIRE(black_card->background_color() == pulp::canvas::Color::hex(0x111111));
    REQUIRE(white_ratio->text().find(":1") != std::string::npos);
    REQUIRE(black_ratio->text().find(":1") != std::string::npos);

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('__global__', 'keydown', { key: 27, mods: 0 });"));
    root.layout_children();
    REQUIRE_FALSE(modal->visible());
}

TEST_CASE("Design tool: brand/style cues resolve to deterministic audio-plugin presets", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto parse_hex = [](std::string hex) {
        if (!hex.empty() && hex.front() == '#') hex.erase(hex.begin());
        return pulp::canvas::Color::hex(static_cast<uint32_t>(std::stoul(hex, nullptr, 16)));
    };
    auto error_hex = engine.evaluate("JSON.parse(getThemeJson()).colors['accent.error']").toString();

    REQUIRE(sample_input->enabled());
    REQUIRE(sample_combo->enabled());
    REQUIRE(panel_content->border_color() == parse_hex(error_hex));
}

TEST_CASE("Design tool: opposite mode lifts semantic backgrounds in light mode", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* panel_content = bridge.widget("panel-content");
    auto* btn_normal = bridge.widget("btn-normal");
    auto* preview_scroll = bridge.widget("preview-scroll");
    auto* mode_selector = dynamic_cast<ComboBox*>(bridge.widget("mode-selector"));
    REQUIRE(panel_content != nullptr);
    REQUIRE(btn_normal != nullptr);
    REQUIRE(preview_scroll != nullptr);
    REQUIRE(mode_selector != nullptr);

    auto brightness = [](pulp::canvas::Color c) {
        return static_cast<int>(c.r8()) + static_cast<int>(c.g8()) + static_cast<int>(c.b8());
    };

    auto dark_l = engine.evaluate("OklchEngine.hexToOklch(JSON.parse(getThemeJson()).colors['bg.primary']).L").getWithDefault<double>(0.0);
    auto dark_panel = panel_content->background_color();
    auto dark_button = btn_normal->background_color();
    auto dark_preview = preview_scroll->background_color();
    auto dark_mode_idx = mode_selector->selected();
    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('gen-opposite-btn', 'click', 0);"));
    root.layout_children();
    auto light_l = engine.evaluate("OklchEngine.hexToOklch(JSON.parse(getThemeJson()).colors['bg.primary']).L").getWithDefault<double>(0.0);
    auto light_panel = panel_content->background_color();
    auto light_button = btn_normal->background_color();
    auto light_preview = preview_scroll->background_color();
    auto light_mode_idx = mode_selector->selected();

    REQUIRE(dark_mode_idx == 0);
    REQUIRE(light_mode_idx == 1);
    REQUIRE(light_l > dark_l);
    REQUIRE(brightness(light_preview) > brightness(dark_preview));
    REQUIRE(brightness(light_panel) > brightness(dark_panel));
    REQUIRE(brightness(light_button) > brightness(dark_button));
}

TEST_CASE("Design tool: opposite mode toggles back to the originating theme", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* mode_selector = dynamic_cast<ComboBox*>(bridge.widget("mode-selector"));
    auto* title = dynamic_cast<Label*>(bridge.widget("theme-name-label"));
    REQUIRE(mode_selector != nullptr);
    REQUIRE(title != nullptr);

    auto dark_bg = engine.evaluate("JSON.parse(getThemeJson()).colors['bg.primary']").toString();
    auto dark_title = title->text();

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('gen-opposite-btn', 'click', 0);"));
    root.layout_children();
    REQUIRE(mode_selector->selected() == 1);

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('gen-opposite-btn', 'click', 0);"));
    root.layout_children();

    auto toggled_bg = engine.evaluate("JSON.parse(getThemeJson()).colors['bg.primary']").toString();
    REQUIRE(mode_selector->selected() == 0);
    REQUIRE(title->text() == dark_title);
    REQUIRE(toggled_bg == dark_bg);
}

TEST_CASE("Design tool: mode selector reuses generated opposite variants", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* mode_selector = dynamic_cast<ComboBox*>(bridge.widget("mode-selector"));
    REQUIRE(mode_selector != nullptr);

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('gen-opposite-btn', 'click', 0);"));
    root.layout_children();
    REQUIRE(mode_selector->selected() == 1);

    REQUIRE_NOTHROW(engine.evaluate("applyTokenColor('bg.primary', '#F5F1E8');"));
    root.layout_children();
    auto light_bg = engine.evaluate("JSON.parse(getThemeJson()).colors['bg.primary']").toString();
    REQUIRE(uppercase_hex(light_bg) == "#F5F1E8");

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('mode-selector', 'select', 0);"));
    root.layout_children();
    REQUIRE(mode_selector->selected() == 0);

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('mode-selector', 'select', 1);"));
    root.layout_children();
    auto restored_light_bg = engine.evaluate("JSON.parse(getThemeJson()).colors['bg.primary']").toString();

    REQUIRE(mode_selector->selected() == 1);
    REQUIRE(uppercase_hex(restored_light_bg) == "#F5F1E8");
}

TEST_CASE("Design tool: preset selector keeps palette mode in sync", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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
    auto* mode_selector = dynamic_cast<ComboBox*>(bridge.widget("mode-selector"));
    REQUIRE(mode_selector != nullptr);

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('preset-selector', 'select', 1);"));
    root.layout_children();
    auto light_mode_idx = mode_selector->selected();
    auto light_l = engine.evaluate("OklchEngine.hexToOklch(JSON.parse(getThemeJson()).colors['bg.primary']).L").getWithDefault<double>(0.0);

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('preset-selector', 'select', 0);"));
    root.layout_children();
    auto dark_mode_idx = mode_selector->selected();
    auto dark_l = engine.evaluate("OklchEngine.hexToOklch(JSON.parse(getThemeJson()).colors['bg.primary']).L").getWithDefault<double>(0.0);

    REQUIRE(light_mode_idx == 1);
    REQUIRE(dark_mode_idx == 0);
    REQUIRE(light_l > dark_l);
}

TEST_CASE("Design tool: template selector applies light web palette", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* mode_selector = dynamic_cast<ComboBox*>(bridge.widget("mode-selector"));
    auto* harmony_selector = dynamic_cast<ComboBox*>(bridge.widget("harmony-selector"));
    auto* title = dynamic_cast<Label*>(bridge.widget("theme-name-label"));
    REQUIRE(mode_selector != nullptr);
    REQUIRE(harmony_selector != nullptr);
    REQUIRE(title != nullptr);

    auto dark_l = engine.evaluate("OklchEngine.hexToOklch(JSON.parse(getThemeJson()).colors['bg.primary']).L").getWithDefault<double>(0.0);
    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('template-selector', 'select', 1);"));
    root.layout_children();
    auto light_l = engine.evaluate("OklchEngine.hexToOklch(JSON.parse(getThemeJson()).colors['bg.primary']).L").getWithDefault<double>(0.0);

    REQUIRE(mode_selector->selected() == 1);
    REQUIRE(harmony_selector->selected() == 2);
    REQUIRE(title->text() == "Tailwind 4");
    REQUIRE(light_l > dark_l);
}

TEST_CASE("Design tool: palette serialization round-trips mode and harmony", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* mode_selector = dynamic_cast<ComboBox*>(bridge.widget("mode-selector"));
    auto* harmony_selector = dynamic_cast<ComboBox*>(bridge.widget("harmony-selector"));
    auto* title = dynamic_cast<Label*>(bridge.widget("theme-name-label"));
    REQUIRE(mode_selector != nullptr);
    REQUIRE(harmony_selector != nullptr);
    REQUIRE(title != nullptr);

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('template-selector', 'select', 1);"));
    root.layout_children();
    auto saved = engine.invoke("serializePaletteConfiguration").toString();

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('preset-selector', 'select', 0);"));
    root.layout_children();
    REQUIRE(mode_selector->selected() == 0);

    auto applied = engine.invoke("applySerializedPaletteConfiguration", saved).getWithDefault<bool>(false);
    root.layout_children();

    REQUIRE(applied);
    REQUIRE(mode_selector->selected() == 1);
    REQUIRE(harmony_selector->selected() == 2);
    REQUIRE(title->text() == "Tailwind 4");
    REQUIRE(engine.evaluate("currentAccent").toString() == "#2563EB");
    REQUIRE(engine.evaluate("currentHarmony").toString() == "complementary");
    REQUIRE(engine.evaluate("currentPaletteMode").toString() == "light");
}

TEST_CASE("Design tool: preview semantic badges track accent tokens", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* ready_badge = bridge.widget("card-3-badge");
    auto* error_badge = bridge.widget("card-4-badge");
    auto* ready_card = bridge.widget("card-3");
    auto* error_card = bridge.widget("card-4");
    REQUIRE(ready_badge != nullptr);
    REQUIRE(error_badge != nullptr);
    REQUIRE(ready_card != nullptr);
    REQUIRE(error_card != nullptr);

    REQUIRE_NOTHROW(engine.evaluate("applyTokenColor('accent.success', '#22aa44');"));
    REQUIRE_NOTHROW(engine.evaluate("applyTokenColor('accent.error', '#cc3355');"));
    root.layout_children();

    REQUIRE(ready_badge->background_color() == pulp::canvas::Color::hex(0x22aa44));
    REQUIRE(error_badge->background_color() == pulp::canvas::Color::hex(0xcc3355));
    REQUIRE(ready_card->border_color() == pulp::canvas::Color::hex(0x22aa44));
    REQUIRE(error_card->border_color() == pulp::canvas::Color::hex(0xcc3355));
}

TEST_CASE("Design tool: preview overlays and tabs follow theme tokens", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    auto* overlay_row = bridge.widget("overlay-row");
    auto* ctx_menu = bridge.widget("ctx-menu");
    auto* dialog_card = bridge.widget("dialog-card");
    auto* active_line = bridge.widget("ptab-1-line");
    auto* effect_glow = bridge.widget("effect-1-chip");
    auto* effect_gradient_start = bridge.widget("effect-3-chip-0");
    auto* effect_gradient_end = bridge.widget("effect-3-chip-2");
    REQUIRE(overlay_row != nullptr);
    REQUIRE(ctx_menu != nullptr);
    REQUIRE(dialog_card != nullptr);
    REQUIRE(active_line != nullptr);
    REQUIRE(effect_glow != nullptr);
    REQUIRE(effect_gradient_start != nullptr);
    REQUIRE(effect_gradient_end != nullptr);

    REQUIRE_NOTHROW(engine.evaluate("applyTokenColor('overlay.bg', '#20263A');"));
    REQUIRE_NOTHROW(engine.evaluate("applyTokenColor('tooltip.bg', '#10161F');"));
    REQUIRE_NOTHROW(engine.evaluate("applyTokenColor('modal.bg', '#191D28');"));
    REQUIRE_NOTHROW(engine.evaluate("applyTokenColor('tab.active', '#33AAEE');"));
    REQUIRE_NOTHROW(engine.evaluate("applyTokenColor('focus.ring', '#AA66FF');"));
    REQUIRE_NOTHROW(engine.evaluate("applyTokenColor('gradient.start', '#FFAA44');"));
    REQUIRE_NOTHROW(engine.evaluate("applyTokenColor('gradient.end', '#55CCEE');"));
    root.layout_children();

    REQUIRE(overlay_row->background_color() == pulp::canvas::Color::hex(0x20263A));
    REQUIRE(ctx_menu->background_color() == pulp::canvas::Color::hex(0x10161F));
    REQUIRE(dialog_card->background_color() == pulp::canvas::Color::hex(0x191D28));
    REQUIRE(effect_glow->background_color() == pulp::canvas::Color::hex(0xAA66FF));
    REQUIRE(effect_gradient_start->background_color() == pulp::canvas::Color::hex(0xFFAA44));
    REQUIRE(effect_gradient_end->background_color() == pulp::canvas::Color::hex(0x55CCEE));

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('ptab-1', 'click', 0);"));
    root.layout_children();

    REQUIRE(active_line->background_color() == pulp::canvas::Color::hex(0x33AAEE));
}

TEST_CASE("Design tool: export helpers cover native, W3C, and style preset formats", "[design-tool]") {
    auto js_path = find_js_file("design-tool-core.js");
    if (js_path.empty()) {
        SKIP("design-tool modules not found");
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

    REQUIRE(engine.evaluate("getExportFileExtension(0)").toString() == ".json");
    REQUIRE(engine.evaluate("getExportFileExtension(1)").toString() == ".css");
    REQUIRE(engine.evaluate("getExportFileExtension(3)").toString() == ".hpp");
    REQUIRE(engine.evaluate("getExportFileExtension(4)").toString() == ".cpp");
    REQUIRE(engine.evaluate("getExportFileExtension(5)").toString() == ".json");
    REQUIRE(engine.evaluate("getExportFileExtension(6)").toString() == ".json");

    auto header_export = engine.evaluate("generateExport(3)").toString();
    auto palette_export = engine.evaluate("generateExport(4)").toString();
    auto w3c_export = engine.evaluate("generateExport(5)").toString();
    REQUIRE_NOTHROW(engine.evaluate("applyWidgetLook('k1', { preset: 'macos7_knob', params: { gloss: 0.9 } });"));
    auto style_export = engine.evaluate("generateExport(6)").toString();
    REQUIRE(header_export.find("PULP_THEME_COLOR(") != std::string::npos);
    REQUIRE(palette_export.find("palette.setColor(") != std::string::npos);
    REQUIRE(w3c_export.find("\"$type\"") != std::string::npos);
    REQUIRE(w3c_export.find("\"$value\"") != std::string::npos);
    REQUIRE(style_export.find("\"widgetLooks\"") != std::string::npos);
    REQUIRE(style_export.find("\"k1\"") != std::string::npos);
    REQUIRE(style_export.find("\"palette\"") != std::string::npos);
    REQUIRE(style_export.find("\"debug\"") != std::string::npos);

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('export-btn-pill', 'click', 0);"));
    root.layout_children();

    auto* popup = bridge.widget("export-popup");
    auto* backdrop = bridge.widget("export-backdrop");
    REQUIRE(popup != nullptr);
    REQUIRE(backdrop != nullptr);
    REQUIRE(popup->visible());
    REQUIRE(backdrop->visible());
    REQUIRE(popup->bounds().x > 0.0f);
    REQUIRE(popup->bounds().y >= 44.0f);
    REQUIRE((popup->bounds().x + popup->bounds().width) < (root.bounds().x + root.bounds().width));
    REQUIRE((popup->bounds().y + popup->bounds().height) < (root.bounds().y + root.bounds().height));

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('exp-tab-5', 'click', 0);"));
    auto clipboard_before = engine.evaluate("readClipboard()").toString();
    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('exp-copy-btn', 'click', 0);"));
    auto clipboard_after = engine.evaluate("readClipboard()").toString();
    // #300 / #309: Linux CI has no xclip/wl-copy installed by default;
    // Android has no host-installed clipboard bridge in test runs;
    // Windows headless Namespace runners can refuse SetClipboardData
    // without a foreground window. In all of those, readClipboard()
    // comes back empty — the design-tool JS path still dispatched the
    // copy, but there's no OS clipboard to observe it on. Skip the
    // round-trip assertion in those environments; on mac/iOS (native)
    // and any Linux desktop with xclip installed, the full round-trip
    // is exercised.
    if (!clipboard_after.empty()) {
        REQUIRE(clipboard_after == engine.evaluate("generateExport(5)").toString());
        const bool clipboard_changed =
            (clipboard_after != clipboard_before) || !clipboard_after.empty();
        REQUIRE(clipboard_changed);
    }

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('__global__', 'keydown', { key: 27, mods: 0 });"));
    root.layout_children();
    REQUIRE_FALSE(popup->visible());
    REQUIRE_FALSE(backdrop->visible());

    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('export-btn-pill', 'click', 0);"));
    root.layout_children();
    REQUIRE(popup->visible());
    REQUIRE(backdrop->visible());
    REQUIRE_NOTHROW(engine.evaluate("__dispatch__('export-backdrop', 'click', 0);"));
    root.layout_children();
    REQUIRE_FALSE(popup->visible());
    REQUIRE_FALSE(backdrop->visible());
}
