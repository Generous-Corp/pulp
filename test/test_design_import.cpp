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
    opts.mode = CodeGenMode::web_compat;
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

TEST_CASE("generate_pulp_js native mode produces Pulp API", "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "TestUI";
    ir.root.layout.direction = LayoutDirection::column;
    ir.root.layout.gap = 8.0f;
    ir.root.style.background_color = "#1a1a2e";
    ir.root.style.width = 320.0f;
    ir.root.style.height = 200.0f;

    IRNode text;
    text.type = "text";
    text.name = "title";
    text.text_content = "Hello";
    text.style.font_size = 18.0f;
    text.style.color = "#ffffff";
    ir.root.children.push_back(text);

    ir.tokens.colors["bg.primary"] = "#1a1a2e";

    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;
    auto js = generate_pulp_js(ir, opts);

    // Should use native API
    REQUIRE(js.find("createCol('root',") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'gap', 8)") != std::string::npos);
    REQUIRE(js.find("setBackground('root', '#1a1a2e')") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'width', 320)") != std::string::npos);
    // Labels use createLabel with height (Yoga requirement)
    REQUIRE(js.find("createLabel('title") != std::string::npos);
    REQUIRE(js.find("setFlex('title") != std::string::npos);
    REQUIRE(js.find("'height'") != std::string::npos);
    // Token assignments use setColorToken
    REQUIRE(js.find("setColorToken('bg.primary', '#1a1a2e')") != std::string::npos);
    // Should end with void 0
    REQUIRE(js.find("void 0;") != std::string::npos);
}

TEST_CASE("generate_pulp_js native mode handles audio widgets with Yoga constraints", "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Controls";
    ir.root.style.width = 300.0f;

    IRNode knob;
    knob.type = "knob";
    knob.name = "GainKnob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Gain";
    knob.audio_default = 0.75f;
    ir.root.children.push_back(knob);

    IRNode fader;
    fader.type = "fader";
    fader.name = "MixFader";
    fader.audio_widget = AudioWidgetType::fader;
    fader.audio_label = "Mix";
    fader.audio_default = 0.5f;
    ir.root.children.push_back(fader);

    IRNode meter;
    meter.type = "meter";
    meter.name = "OutputMeter";
    meter.audio_widget = AudioWidgetType::meter;
    meter.audio_label = "Out";
    ir.root.children.push_back(meter);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    auto js = generate_pulp_js(ir, opts);

    // Knob with wrapper column and proper sizing (IDs get numeric suffixes)
    REQUIRE(js.find("createKnob('GainKnob") != std::string::npos);
    REQUIRE(js.find("setLabel('GainKnob") != std::string::npos);
    REQUIRE(js.find("'Gain'") != std::string::npos);
    REQUIRE(js.find("setValue('GainKnob") != std::string::npos);
    // Knob size >= 56 (minimum)
    REQUIRE(js.find("'width', 56)") != std::string::npos);

    // Fader with min width >= 40, label as separate element
    REQUIRE(js.find("createFader('MixFader") != std::string::npos);
    REQUIRE(js.find("createLabel('MixFader") != std::string::npos);  // Separate label
    REQUIRE(js.find("'Mix'") != std::string::npos);
    REQUIRE(js.find("'width', 40)") != std::string::npos);

    // Meter with separate label (no built-in setLabel for Meter)
    REQUIRE(js.find("createMeter('OutputMeter") != std::string::npos);
    REQUIRE(js.find("'Out'") != std::string::npos);
    REQUIRE(js.find("setMeterLevel") != std::string::npos);
}

TEST_CASE("generate_pulp_js web-compat mode handles audio widgets", "[view][import]") {
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

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    auto js = generate_pulp_js(ir, opts);

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

TEST_CASE("parse_w3c_tokens resolves aliases", "[view][import]") {
    auto json = R"({
        "color": {
            "$type": "color",
            "blue": { "$value": "#3B82F6" },
            "primary": { "$value": "{color.blue}" },
            "accent": { "$value": "{color.primary}" }
        },
        "spacing": {
            "$type": "dimension",
            "base": { "$value": "8" },
            "md": { "$value": "{spacing.base}" },
            "lg": { "$value": "16" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    // Direct values
    REQUIRE(theme.colors.count("color.blue") == 1);
    REQUIRE(theme.colors["color.blue"].r == 0x3B);

    // Single alias: primary → blue
    REQUIRE(theme.colors.count("color.primary") == 1);
    REQUIRE(theme.colors["color.primary"].r == 0x3B);
    REQUIRE(theme.colors["color.primary"].g == 0x82);

    // Chained alias: accent → primary → blue
    REQUIRE(theme.colors.count("color.accent") == 1);
    REQUIRE(theme.colors["color.accent"].r == 0x3B);

    // Dimension alias: md → base
    REQUIRE(theme.dimensions["spacing.md"] == 8.0f);
    REQUIRE(theme.dimensions["spacing.lg"] == 16.0f);
}

TEST_CASE("parse_w3c_tokens inherits group $type", "[view][import]") {
    auto json = R"({
        "color": {
            "$type": "color",
            "bg": { "$value": "#1E1E2E" },
            "text": { "$value": "#CDD6F4" }
        },
        "size": {
            "$type": "dimension",
            "sm": { "$value": "4" },
            "md": { "$value": "8" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    // Tokens inherit $type from parent group
    REQUIRE(theme.colors.count("color.bg") == 1);
    REQUIRE(theme.colors.count("color.text") == 1);
    REQUIRE(theme.dimensions.count("size.sm") == 1);
    REQUIRE(theme.dimensions["size.sm"] == 4.0f);
    REQUIRE(theme.dimensions["size.md"] == 8.0f);
}

TEST_CASE("parse_w3c_tokens handles composite typography tokens", "[view][import]") {
    auto json = R"({
        "heading": {
            "$type": "typography",
            "$value": {
                "fontFamily": "Inter",
                "fontSize": "24",
                "fontWeight": "700",
                "lineHeight": "1.2"
            }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    REQUIRE(theme.strings.count("heading.fontFamily") == 1);
    REQUIRE(theme.strings["heading.fontFamily"] == "Inter");
    REQUIRE(theme.dimensions["heading.fontSize"] == 24.0f);
    REQUIRE(theme.dimensions["heading.fontWeight"] == 700.0f);
    REQUIRE(theme.dimensions["heading.lineHeight"] == 1.2f);
}

TEST_CASE("parse_w3c_tokens handles composite shadow tokens", "[view][import]") {
    auto json = R"({
        "shadow": {
            "$type": "shadow",
            "card": {
                "$value": {
                    "color": "#00000040",
                    "offsetX": "0",
                    "offsetY": "4",
                    "blur": "8",
                    "spread": "0"
                }
            }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    REQUIRE(theme.colors.count("shadow.card.color") == 1);
    REQUIRE(theme.dimensions["shadow.card.offsetY"] == 4.0f);
    REQUIRE(theme.dimensions["shadow.card.blur"] == 8.0f);
}

TEST_CASE("parse_w3c_tokens evaluates math expressions", "[view][import]") {
    auto json = R"({
        "spacing": {
            "$type": "dimension",
            "base": { "$value": "8" },
            "double": { "$value": "8 * 2" },
            "half": { "$value": "8 / 2" },
            "sum": { "$value": "4 + 12" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    REQUIRE(theme.dimensions["spacing.base"] == 8.0f);
    REQUIRE(theme.dimensions["spacing.double"] == 16.0f);
    REQUIRE(theme.dimensions["spacing.half"] == 4.0f);
    REQUIRE(theme.dimensions["spacing.sum"] == 16.0f);
}

TEST_CASE("parse_w3c_tokens resolves alias then evaluates math", "[view][import]") {
    auto json = R"({
        "spacing": {
            "$type": "dimension",
            "base": { "$value": "8" },
            "lg": { "$value": "{spacing.base} * 2" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    // {spacing.base} resolves to "8", then "8 * 2" evaluates to 16
    REQUIRE(theme.dimensions["spacing.lg"] == 16.0f);
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

// ── Token alias cycle detection ─────────────────────────────────────────

TEST_CASE("parse_w3c_tokens handles circular aliases without infinite loop", "[view][import]") {
    auto json = R"({
        "color": {
            "$type": "color",
            "a": { "$value": "{color.b}" },
            "b": { "$value": "{color.a}" },
            "safe": { "$value": "#FF0000" }
        }
    })";

    // Should not hang — circular refs terminate gracefully
    auto theme = parse_w3c_tokens(json);

    // The safe token should still resolve
    REQUIRE(theme.colors.count("color.safe") == 1);
    REQUIRE(theme.colors["color.safe"].r == 0xFF);

    // Circular tokens won't resolve to valid colors — they should not crash
    // (they may or may not be in the theme depending on what the unresolved
    // string looks like, but the important thing is no infinite loop)
}

TEST_CASE("parse_w3c_tokens handles self-referencing alias", "[view][import]") {
    auto json = R"({
        "spacing": {
            "$type": "dimension",
            "self": { "$value": "{spacing.self}" }
        }
    })";

    // Should not hang
    auto theme = parse_w3c_tokens(json);
    // Self-reference won't resolve to a valid number
}

// ── Figma Variables sync ───────────────────────────────────────────────

TEST_CASE("parse_figma_variables reads Figma variable format", "[view][import]") {
    auto json = R"({
        "variables": [
            { "name": "color/primary", "type": "COLOR", "resolvedValue": "#89B4FA" },
            { "name": "color/bg", "type": "COLOR", "resolvedValue": "#1E1E2E" },
            { "name": "spacing/md", "type": "FLOAT", "resolvedValue": "8" },
            { "name": "font/heading", "type": "STRING", "resolvedValue": "Inter" }
        ]
    })";

    auto theme = parse_figma_variables(json);

    REQUIRE(theme.colors.count("color.primary") == 1);
    REQUIRE(theme.colors["color.primary"].r == 0x89);
    REQUIRE(theme.colors["color.primary"].g == 0xB4);

    REQUIRE(theme.colors.count("color.bg") == 1);
    REQUIRE(theme.colors["color.bg"].r == 0x1E);

    REQUIRE(theme.dimensions["spacing.md"] == 8.0f);
    REQUIRE(theme.strings["font.heading"] == "Inter");
}

TEST_CASE("export_figma_variables produces Figma-compatible JSON", "[view][import]") {
    Theme theme;
    theme.colors["color.primary"] = color_from_hex(0x89B4FA);
    theme.dimensions["spacing.md"] = 8.0f;
    theme.strings["font.heading"] = "Inter";

    auto json = export_figma_variables(theme);

    REQUIRE(json.find("\"variables\"") != std::string::npos);
    REQUIRE(json.find("\"name\": \"color/primary\"") != std::string::npos);
    REQUIRE(json.find("\"type\": \"COLOR\"") != std::string::npos);
    REQUIRE(json.find("#89b4fa") != std::string::npos);
    REQUIRE(json.find("\"name\": \"spacing/md\"") != std::string::npos);
    REQUIRE(json.find("\"type\": \"FLOAT\"") != std::string::npos);
    REQUIRE(json.find("\"name\": \"font/heading\"") != std::string::npos);
    REQUIRE(json.find("\"type\": \"STRING\"") != std::string::npos);
}

TEST_CASE("Figma Variables round-trip preserves colors", "[view][import]") {
    Theme original;
    original.colors["color.primary"] = color_from_hex(0x89B4FA);
    original.dimensions["spacing.md"] = 8.0f;

    auto json = export_figma_variables(original);
    auto restored = parse_figma_variables(json);

    REQUIRE(restored.colors.count("color.primary") == 1);
    REQUIRE(restored.colors["color.primary"].r == original.colors["color.primary"].r);
    REQUIRE(restored.colors["color.primary"].g == original.colors["color.primary"].g);
    REQUIRE(restored.colors["color.primary"].b == original.colors["color.primary"].b);
    REQUIRE(restored.dimensions["spacing.md"] == 8.0f);
}

// ── Stitch Design System sync ──────────────────────────────────────────

TEST_CASE("parse_stitch_design_system reads Stitch format", "[view][import]") {
    auto json = R"({
        "name": "Dark Theme",
        "colors": { "primary": "#89B4FA", "background": "#1E1E2E" },
        "fonts": { "heading": "Inter", "body": "Roboto" },
        "roundness": "large",
        "spacing": 12
    })";

    auto theme = parse_stitch_design_system(json);

    REQUIRE(theme.colors.count("color.primary") == 1);
    REQUIRE(theme.colors["color.primary"].r == 0x89);
    REQUIRE(theme.colors.count("color.background") == 1);
    REQUIRE(theme.strings["font.heading"] == "Inter");
    REQUIRE(theme.strings["font.body"] == "Roboto");
    REQUIRE(theme.dimensions["roundness"] == 16.0f);  // "large" = 16
    REQUIRE(theme.dimensions["spacing.base"] == 12.0f);
}

TEST_CASE("export_stitch_design_system produces Stitch-compatible JSON", "[view][import]") {
    Theme theme;
    theme.colors["color.primary"] = color_from_hex(0x89B4FA);
    theme.strings["font.heading"] = "Inter";
    theme.dimensions["roundness"] = 8.0f;
    theme.dimensions["spacing.base"] = 12.0f;

    auto json = export_stitch_design_system(theme);

    REQUIRE(json.find("\"colors\"") != std::string::npos);
    REQUIRE(json.find("\"primary\"") != std::string::npos);
    REQUIRE(json.find("#89b4fa") != std::string::npos);
    REQUIRE(json.find("\"fonts\"") != std::string::npos);
    REQUIRE(json.find("\"heading\": \"Inter\"") != std::string::npos);
    REQUIRE(json.find("\"roundness\": \"medium\"") != std::string::npos);  // 8 = medium
    REQUIRE(json.find("\"spacing\": 12") != std::string::npos);
}

TEST_CASE("Stitch Design System round-trip preserves tokens", "[view][import]") {
    Theme original;
    original.colors["color.accent"] = color_from_hex(0xE94560);
    original.strings["font.body"] = "Roboto";
    original.dimensions["roundness"] = 4.0f;
    original.dimensions["spacing.base"] = 8.0f;

    auto json = export_stitch_design_system(original);
    auto restored = parse_stitch_design_system(json);

    REQUIRE(restored.colors["color.accent"].r == 0xE9);
    REQUIRE(restored.strings["font.body"] == "Roboto");
    REQUIRE(restored.dimensions["roundness"] == 4.0f);  // "small" maps back to 4
    REQUIRE(restored.dimensions["spacing.base"] == 8.0f);
}

// ── E2E pipeline test ──────────────────────────────────────────────────

// Keep this name ASCII-only so Catch2/CTest filters remain stable on Windows.
TEST_CASE("E2E: Figma IR -> code gen -> tokens -> round-trip", "[view][import][e2e]") {
    // Step 1: Parse a Figma IR with audio widgets and tokens
    auto json = R"({
        "type": "frame",
        "name": "PluginUI",
        "layout": { "direction": "column", "gap": 16, "padding": 12 },
        "style": { "backgroundColor": "#1a1a2e", "width": 340, "height": 280 },
        "children": [
            {
                "type": "text", "name": "title", "content": "My Plugin",
                "style": { "fontSize": 20, "fontWeight": 700, "color": "#e0e0e0" }
            },
            {
                "type": "frame", "name": "controls",
                "layout": { "direction": "row", "gap": 12 },
                "children": [
                    { "type": "slider", "name": "GainKnob", "label": "Gain", "min": 0, "max": 1, "default": 0.75 },
                    { "type": "slider", "name": "MixFader", "label": "Mix", "min": 0, "max": 1 },
                    { "type": "frame", "name": "OutputMeter", "label": "Out" }
                ]
            }
        ],
        "tokens": {
            "colors": { "bg.primary": "#1a1a2e", "accent": "#e94560" },
            "dimensions": { "spacing.md": 16 }
        }
    })";

    auto ir = parse_figma_json(json);
    REQUIRE(ir.root.name == "PluginUI");
    REQUIRE(ir.root.children.size() == 2);

    // Step 2: Audio widget detection
    auto& controls = ir.root.children[1];
    REQUIRE(controls.children[0].audio_widget == AudioWidgetType::knob);
    REQUIRE(controls.children[1].audio_widget == AudioWidgetType::fader);
    REQUIRE(controls.children[2].audio_widget == AudioWidgetType::meter);

    // Step 3: Generate native code
    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;
    auto js = generate_pulp_js(ir, opts);
    REQUIRE(!js.empty());
    REQUIRE(js.find("createCol('root',") != std::string::npos);
    REQUIRE(js.find("createKnob('GainKnob") != std::string::npos);
    REQUIRE(js.find("createFader('MixFader") != std::string::npos);
    REQUIRE(js.find("createMeter('OutputMeter") != std::string::npos);
    REQUIRE(js.find("setColorToken('bg.primary'") != std::string::npos);

    // Step 4: Token round-trip
    auto theme = ir_tokens_to_theme(ir.tokens);
    auto w3c = export_w3c_tokens(theme);
    auto restored = parse_w3c_tokens(w3c);
    REQUIRE(restored.colors.count("bg.primary") == 1);
    REQUIRE(restored.colors["bg.primary"].r == 0x1a);

    // Step 5: Figma Variables export/import cycle
    auto figma_vars = export_figma_variables(theme);
    auto from_figma = parse_figma_variables(figma_vars);
    REQUIRE(from_figma.colors.count("bg.primary") == 1);

    // Step 6: Stitch export/import cycle
    auto stitch_json = export_stitch_design_system(theme);
    auto from_stitch = parse_stitch_design_system(stitch_json);
    REQUIRE(!from_stitch.colors.empty());
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
