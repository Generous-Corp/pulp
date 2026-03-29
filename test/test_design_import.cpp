#include <catch2/catch_test_macros.hpp>
#include <pulp/view/design_import.hpp>

using namespace pulp::view;

// ── Design source parsing ───────────────────────────────────────────────

TEST_CASE("parse_design_source recognizes valid sources", "[view][import]") {
    REQUIRE(parse_design_source("figma") == DesignSource::figma);
    REQUIRE(parse_design_source("stitch") == DesignSource::stitch);
    REQUIRE(parse_design_source("v0") == DesignSource::v0);
    REQUIRE(parse_design_source("pencil") == DesignSource::pencil);
    REQUIRE_FALSE(parse_design_source("unknown").has_value());
}

TEST_CASE("design_source_name returns display names", "[view][import]") {
    REQUIRE(std::string(design_source_name(DesignSource::figma)) == "Figma");
    REQUIRE(std::string(design_source_name(DesignSource::v0)) == "v0");
}

// ── Audio widget detection ──────────────────────────────────────────────

TEST_CASE("detect_audio_widget identifies widget types from names", "[view][import]") {
    REQUIRE(detect_audio_widget("GainKnob") == AudioWidgetType::knob);
    REQUIRE(detect_audio_widget("master_dial") == AudioWidgetType::knob);
    REQUIRE(detect_audio_widget("VolumeFader") == AudioWidgetType::fader);
    REQUIRE(detect_audio_widget("mix_slider") == AudioWidgetType::fader);
    REQUIRE(detect_audio_widget("OutputMeter") == AudioWidgetType::meter);
    REQUIRE(detect_audio_widget("vu_display") == AudioWidgetType::meter);
    REQUIRE(detect_audio_widget("level_indicator") == AudioWidgetType::meter);
    REQUIRE(detect_audio_widget("FilterXYPad") == AudioWidgetType::xy_pad);
    REQUIRE(detect_audio_widget("xy_pad_control") == AudioWidgetType::xy_pad);
    REQUIRE(detect_audio_widget("WaveformDisplay") == AudioWidgetType::waveform);
    REQUIRE(detect_audio_widget("oscilloscope_view") == AudioWidgetType::waveform);
    REQUIRE(detect_audio_widget("SpectrumAnalyzer") == AudioWidgetType::spectrum);
    REQUIRE(detect_audio_widget("analyser_view") == AudioWidgetType::spectrum);
    REQUIRE(detect_audio_widget("header_label") == AudioWidgetType::none);
    REQUIRE(detect_audio_widget("save_button") == AudioWidgetType::none);
}

// ── Figma JSON parsing ──────────────────────────────────────────────────

TEST_CASE("parse_figma_json parses IR format", "[view][import]") {
    auto json = R"({
        "type": "frame",
        "name": "PluginUI",
        "layout": { "direction": "column", "gap": 16, "padding": 12 },
        "style": { "backgroundColor": "#1a1a2e", "borderRadius": 8 },
        "children": [
            {
                "type": "text",
                "name": "title",
                "content": "My Plugin",
                "style": { "fontSize": 24, "fontWeight": 700, "color": "#e0e0e0" }
            },
            {
                "type": "frame",
                "name": "controls",
                "layout": { "direction": "row", "gap": 8 },
                "children": [
                    { "type": "slider", "name": "GainKnob", "label": "Gain", "min": 0, "max": 1 }
                ]
            }
        ],
        "tokens": {
            "colors": { "bg.primary": "#1a1a2e", "accent.primary": "#e94560" },
            "dimensions": { "spacing.md": 16 }
        }
    })";

    auto ir = parse_figma_json(json);

    REQUIRE(ir.source == DesignSource::figma);
    REQUIRE(ir.root.type == "frame");
    REQUIRE(ir.root.name == "PluginUI");
    REQUIRE(ir.root.layout.direction == LayoutDirection::column);
    REQUIRE(ir.root.layout.gap == 16.0f);
    REQUIRE(ir.root.layout.padding_top == 12.0f);
    REQUIRE(ir.root.style.background_color == "#1a1a2e");
    REQUIRE(ir.root.style.border_radius == 8.0f);
    REQUIRE(ir.root.children.size() == 2);

    // Text child
    auto& title = ir.root.children[0];
    REQUIRE(title.type == "text");
    REQUIRE(title.text_content == "My Plugin");
    REQUIRE(title.style.font_size == 24.0f);
    REQUIRE(title.style.font_weight == 700);

    // Controls container
    auto& controls = ir.root.children[1];
    REQUIRE(controls.layout.direction == LayoutDirection::row);
    REQUIRE(controls.children.size() == 1);

    // Audio widget auto-detection
    auto& knob = controls.children[0];
    REQUIRE(knob.audio_widget == AudioWidgetType::knob);
    REQUIRE(knob.audio_label == "Gain");
    REQUIRE(knob.audio_min == 0.0f);
    REQUIRE(knob.audio_max == 1.0f);

    // Tokens
    REQUIRE(ir.tokens.colors.count("bg.primary") == 1);
    REQUIRE(ir.tokens.colors["bg.primary"] == "#1a1a2e");
    REQUIRE(ir.tokens.dimensions.count("spacing.md") == 1);
    REQUIRE(ir.tokens.dimensions["spacing.md"] == 16.0f);
}

// ── Code generation ─────────────────────────────────────────────────────

TEST_CASE("generate_pulp_js produces valid web-compat JS", "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "TestUI";
    ir.root.layout.direction = LayoutDirection::column;
    ir.root.layout.gap = 8.0f;
    ir.root.style.background_color = "#1a1a2e";

    IRNode text;
    text.type = "text";
    text.name = "title";
    text.text_content = "Hello";
    text.style.font_size = 18.0f;
    text.style.color = "#ffffff";
    ir.root.children.push_back(text);

    ir.tokens.colors["bg.primary"] = "#1a1a2e";

    CodeGenOptions opts;
    opts.include_comments = false;
    auto js = generate_pulp_js(ir, opts);

    // Should create root element
    REQUIRE(js.find("const root = document.createElement('div')") != std::string::npos);
    // Should set flex layout
    REQUIRE(js.find("root.style.display = 'flex'") != std::string::npos);
    REQUIRE(js.find("root.style.flexDirection = 'column'") != std::string::npos);
    REQUIRE(js.find("root.style.gap = '8px'") != std::string::npos);
    // Should set background
    REQUIRE(js.find("root.style.backgroundColor = '#1a1a2e'") != std::string::npos);
    // Should create text child
    REQUIRE(js.find("document.createElement('span')") != std::string::npos);
    REQUIRE(js.find(".textContent = 'Hello'") != std::string::npos);
    REQUIRE(js.find(".style.fontSize = '18px'") != std::string::npos);
    // Should append to body
    REQUIRE(js.find("document.body.appendChild(root)") != std::string::npos);
    // Should include token assignments
    REQUIRE(js.find("theme.colors[\"bg.primary\"]") != std::string::npos);
}

TEST_CASE("generate_pulp_js handles audio widgets", "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Controls";

    IRNode knob;
    knob.type = "knob";
    knob.name = "GainKnob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Gain";
    knob.audio_min = 0.0f;
    knob.audio_max = 1.0f;
    knob.audio_default = 0.75f;
    ir.root.children.push_back(knob);

    auto js = generate_pulp_js(ir);

    REQUIRE(js.find("createKnob") != std::string::npos);
    REQUIRE(js.find("label: 'Gain'") != std::string::npos);
    REQUIRE(js.find("defaultValue: 0.75") != std::string::npos);
}

TEST_CASE("generate_pulp_js respects CodeGenOptions", "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.source_file = "test.json";
    ir.root.type = "frame";
    ir.root.name = "Root";

    CodeGenOptions opts;
    opts.include_comments = true;
    auto with_comments = generate_pulp_js(ir, opts);
    REQUIRE(with_comments.find("// Generated by Pulp") != std::string::npos);

    opts.include_comments = false;
    auto no_comments = generate_pulp_js(ir, opts);
    REQUIRE(no_comments.find("// Generated") == std::string::npos);
}

// ── W3C Design Tokens ───────────────────────────────────────────────────

TEST_CASE("parse_w3c_tokens reads W3C format", "[view][import]") {
    auto json = R"({
        "color": {
            "primary": { "$value": "#89B4FA", "$type": "color" },
            "bg": { "$value": "#1E1E2E", "$type": "color" }
        },
        "spacing": {
            "md": { "$value": "8", "$type": "dimension" },
            "lg": { "$value": "16", "$type": "dimension" }
        },
        "font": {
            "family": { "$value": "Inter", "$type": "fontFamily" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    REQUIRE(theme.colors.count("color.primary") == 1);
    REQUIRE(theme.colors["color.primary"].r == 0x89);
    REQUIRE(theme.colors["color.primary"].g == 0xB4);
    REQUIRE(theme.colors["color.primary"].b == 0xFA);

    REQUIRE(theme.colors.count("color.bg") == 1);
    REQUIRE(theme.colors["color.bg"].r == 0x1E);

    REQUIRE(theme.dimensions.count("spacing.md") == 1);
    REQUIRE(theme.dimensions["spacing.md"] == 8.0f);
    REQUIRE(theme.dimensions["spacing.lg"] == 16.0f);

    REQUIRE(theme.strings.count("font.family") == 1);
    REQUIRE(theme.strings["font.family"] == "Inter");
}

TEST_CASE("export_w3c_tokens produces W3C format", "[view][import]") {
    Theme theme;
    theme.colors["bg.primary"] = color_from_hex(0x1E1E2E);
    theme.colors["accent.primary"] = color_from_hex(0x89B4FA);
    theme.dimensions["spacing.md"] = 8.0f;
    theme.strings["font.family"] = "Inter";

    auto json = export_w3c_tokens(theme);

    REQUIRE(json.find("\"$value\"") != std::string::npos);
    REQUIRE(json.find("\"$type\": \"color\"") != std::string::npos);
    REQUIRE(json.find("\"$type\": \"dimension\"") != std::string::npos);
    REQUIRE(json.find("\"$type\": \"string\"") != std::string::npos);
    REQUIRE(json.find("#1e1e2e") != std::string::npos);
}

TEST_CASE("W3C token round-trip preserves colors", "[view][import]") {
    Theme original;
    original.colors["bg.primary"] = color_from_hex(0x1A1A2E);
    original.colors["accent.primary"] = color_from_hex(0xE94560);
    original.dimensions["spacing.md"] = 8.0f;

    auto w3c = export_w3c_tokens(original);
    auto restored = parse_w3c_tokens(w3c);

    // Colors should round-trip (names get prefixed by group)
    REQUIRE(restored.colors.count("bg.primary") == 1);
    REQUIRE(restored.colors["bg.primary"].r == original.colors["bg.primary"].r);
    REQUIRE(restored.colors["bg.primary"].g == original.colors["bg.primary"].g);
    REQUIRE(restored.colors["bg.primary"].b == original.colors["bg.primary"].b);
}

// ── IR ↔ Theme conversion ───────────────────────────────────────────────

TEST_CASE("ir_tokens_to_theme converts token maps to Theme", "[view][import]") {
    IRTokens tokens;
    tokens.colors["bg.primary"] = "#1a1a2e";
    tokens.dimensions["spacing.md"] = 8.0f;
    tokens.strings["font.family"] = "Inter";

    auto theme = ir_tokens_to_theme(tokens);

    REQUIRE(theme.colors.count("bg.primary") == 1);
    REQUIRE(theme.colors["bg.primary"].r == 0x1a);
    REQUIRE(theme.dimensions["spacing.md"] == 8.0f);
    REQUIRE(theme.strings["font.family"] == "Inter");
}

TEST_CASE("theme_to_ir_tokens converts Theme to token maps", "[view][import]") {
    Theme theme;
    theme.colors["bg.primary"] = color_from_hex(0x1A1A2E);
    theme.dimensions["spacing.md"] = 8.0f;
    theme.strings["font.family"] = "Inter";

    auto tokens = theme_to_ir_tokens(theme);

    REQUIRE(tokens.colors["bg.primary"] == "#1a1a2e");
    REQUIRE(tokens.dimensions["spacing.md"] == 8.0f);
    REQUIRE(tokens.strings["font.family"] == "Inter");
}

// ── Stitch HTML parsing ─────────────────────────────────────────────────

TEST_CASE("parse_stitch_html extracts text from simple HTML", "[view][import]") {
    auto html = "<h1>Plugin Title</h1>\n<p>Description text</p>";
    auto ir = parse_stitch_html(html);

    REQUIRE(ir.source == DesignSource::stitch);
    REQUIRE(ir.root.type == "frame");
    REQUIRE(ir.root.children.size() >= 1);
    // At least the first match is extracted
    REQUIRE(ir.root.children[0].text_content == "Plugin Title");
}

TEST_CASE("parse_stitch_html accepts JSON IR directly", "[view][import]") {
    auto json = R"({"type": "frame", "name": "Screen", "children": []})";
    auto ir = parse_stitch_html(json);
    REQUIRE(ir.root.name == "Screen");
}

// ── v0 TSX parsing ──────────────────────────────────────────────────────

TEST_CASE("parse_v0_tsx extracts className patterns", "[view][import]") {
    auto tsx = R"(
        export default function Component() {
            return (
                <div className="flex flex-col gap-4 p-4 bg-slate-900">
                    <div className="flex flex-row gap-2">
                        <span>Label</span>
                    </div>
                </div>
            );
        }
    )";
    auto ir = parse_v0_tsx(tsx);

    REQUIRE(ir.source == DesignSource::v0);
    REQUIRE(ir.root.type == "frame");
    // Should extract at least the two className entries
    REQUIRE(ir.root.children.size() >= 2);
}

TEST_CASE("parse_v0_tsx accepts JSON IR directly", "[view][import]") {
    auto json = R"({"type": "frame", "name": "V0Screen", "children": []})";
    auto ir = parse_v0_tsx(json);
    REQUIRE(ir.root.name == "V0Screen");
}

// ── Pencil JSON parsing ─────────────────────────────────────────────────

TEST_CASE("parse_pencil_json parses node tree", "[view][import]") {
    auto json = R"({
        "type": "frame",
        "name": "PencilDesign",
        "layout": { "direction": "row", "gap": 12 },
        "children": [
            { "type": "text", "name": "label", "content": "Volume" }
        ],
        "variables": {
            "colors": { "primary": "#ff5500" },
            "dimensions": { "radius": 8 }
        }
    })";

    auto ir = parse_pencil_json(json);

    REQUIRE(ir.source == DesignSource::pencil);
    REQUIRE(ir.root.name == "PencilDesign");
    REQUIRE(ir.root.layout.direction == LayoutDirection::row);
    REQUIRE(ir.root.children.size() == 1);
    REQUIRE(ir.tokens.colors["primary"] == "#ff5500");
    REQUIRE(ir.tokens.dimensions["radius"] == 8.0f);
}
