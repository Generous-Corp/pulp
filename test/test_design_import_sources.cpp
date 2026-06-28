#include "test_design_import_shared.hpp"

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
    REQUIRE(theme.colors["color.safe"].r8() == 0xFF);

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
    REQUIRE(theme.colors["color.primary"].r8() == 0x89);
    REQUIRE(theme.colors["color.primary"].g8() == 0xB4);

    REQUIRE(theme.colors.count("color.bg") == 1);
    REQUIRE(theme.colors["color.bg"].r8() == 0x1E);

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
    REQUIRE(restored.colors["color.primary"].r8() == original.colors["color.primary"].r8());
    REQUIRE(restored.colors["color.primary"].g8() == original.colors["color.primary"].g8());
    REQUIRE(restored.colors["color.primary"].b8() == original.colors["color.primary"].b8());
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
    REQUIRE(theme.colors["color.primary"].r8() == 0x89);
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

    REQUIRE(restored.colors["color.accent"].r8() == 0xE9);
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

    // Step 3: Generate native-bridge JS
    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
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
    REQUIRE(restored.colors["bg.primary"].r8() == 0x1a);

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

TEST_CASE("parse_pencil_json covers sizing layout padding and widget metadata edges",
          "[view][import]") {
    auto json = R"json({
        "type": "frame",
        "name": "PencilRack",
        "layout": "horizontal",
        "gap": 7,
        "padding": [3, 5],
        "justifyContent": "end",
        "alignItems": "end",
        "children": [
            {
                "type": "frame",
                "name": "FillPanel",
                "layout": "vertical",
                "width": "fill_container",
                "height": "fit_content(120)",
                "padding": [1, 2, 3, 4],
                "children": [
                    {
                        "type": "text",
                        "name": "Readout",
                        "content": "Ready",
                        "fontSize": 10,
                        "fontWeight": "600",
                        "fontFamily": "Mono",
                        "fill": "#eeeeee"
                    }
                ]
            },
            {
                "type": "frame",
                "name": "MainFader",
                "width": 44,
                "height": 120,
                "children": [
                    {
                        "type": "rectangle",
                        "name": "track",
                        "width": 6,
                        "height": 90,
                        "stroke": { "fill": "#ff5500" }
                    },
                    { "type": "text", "name": "label", "content": "Level" }
                ]
            },
            {
                "type": "frame",
                "name": "KnobRow",
                "children": [
                    { "type": "frame", "name": "DriveKnob", "children": [] }
                ]
            }
        ]
    })json";

    auto ir = parse_pencil_json(json);

    REQUIRE(ir.root.layout.direction == LayoutDirection::row);
    REQUIRE(ir.root.layout.gap == 7.0f);
    REQUIRE(ir.root.layout.padding_top == 3.0f);
    REQUIRE(ir.root.layout.padding_bottom == 3.0f);
    REQUIRE(ir.root.layout.padding_left == 5.0f);
    REQUIRE(ir.root.layout.padding_right == 5.0f);
    REQUIRE(ir.root.layout.justify == LayoutAlign::flex_end);
    REQUIRE(ir.root.layout.align == LayoutAlign::flex_end);

    const auto& fill_panel = ir.root.children[0];
    REQUIRE(fill_panel.layout.direction == LayoutDirection::column);
    REQUIRE(fill_panel.layout.width_mode == SizingMode::fill);
    REQUIRE(fill_panel.layout.height_mode == SizingMode::hug);
    REQUIRE(fill_panel.layout.padding_top == 1.0f);
    REQUIRE(fill_panel.layout.padding_right == 2.0f);
    REQUIRE(fill_panel.layout.padding_bottom == 3.0f);
    REQUIRE(fill_panel.layout.padding_left == 4.0f);
    REQUIRE(fill_panel.children[0].style.font_size == 10.0f);
    REQUIRE(fill_panel.children[0].style.font_weight == 600);
    REQUIRE(fill_panel.children[0].style.font_family == "Mono");
    REQUIRE(fill_panel.children[0].style.color == "#eeeeee");

    const auto& fader = ir.root.children[1];
    REQUIRE(fader.audio_widget == AudioWidgetType::fader);
    REQUIRE(fader.style.width == 44.0f);
    REQUIRE(fader.style.height == 120.0f);
    REQUIRE(fader.attributes.at("shape_width") == "6");
    REQUIRE(fader.attributes.at("shape_height") == "90");
    REQUIRE(fader.children[0].attributes.at("stroke_color") == "#ff5500");

    const auto& knob_row = ir.root.children[2];
    REQUIRE(knob_row.audio_widget == AudioWidgetType::none);
    REQUIRE(knob_row.layout.direction == LayoutDirection::row);
}

TEST_CASE("generate_pulp_js bridge_native_js mode covers fill-height and leaf fallback branches",
          "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::pencil;
    ir.root.type = "frame";
    ir.root.name = "FillRoot";
    ir.root.layout.height_mode = SizingMode::fill;

    IRNode leaf;
    leaf.type = "rectangle";
    leaf.name = "empty-divider";
    ir.root.children.push_back(leaf);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    opts.include_comments = false;
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("createCol('root', '')") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'flex_grow', 1)") != std::string::npos);
    REQUIRE(js.find("createRow('empty_divider0', 'root')") != std::string::npos);
    REQUIRE(js.find("setFlex('empty_divider0', 'height', 1)") != std::string::npos);
}

// ──────────────────────────────────────────────────────────────────────────
// Pulp Library knob recognition through the figma-plugin lane.
//
// The TypeScript extractor (tools/figma-plugin/src/extract.ts) matches an
// INSTANCE node's `component_set_key` against `library-manifest.json`. When
// matched, it stamps `library_widget_kind` and lifts the instance's structured
// component properties to the JSON envelope's node root:
//   audio_widget=<kind>, label, min, max, default, attributes.{units,binding}
// design_ir_json.cpp::parse_ir_node maps these to IRNode.audio_widget /
// audio_label / audio_min / audio_max / audio_default and the attributes map.
//
// This test pins that contract — without it the figma-plugin extractor and
// design_import.cpp parser can drift apart silently.
TEST_CASE("parse_figma_plugin_json maps Pulp / Knob envelope onto IR widget",
          "[view][import][figma-plugin][pulp-library][audio-widget][knob]") {
    // Envelope shape mirrors what tools/figma-plugin/src/serialize.ts emits for
    // an instance of the Pulp / Knob component-set.
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "provenance": {
            "adapter": "figma-plugin",
            "version": "0.1.0",
            "source_uri": "figma://design/vxW6btjzQtc4t9ITLNjev0/Pulp-Library"
        },
        "root": {
            "type": "frame",
            "name": "VolumeKnob",
            "audio_widget": "knob",
            "label": "Cutoff",
            "min": 20,
            "max": 20000,
            "default": 880,
            "attributes": {
                "units": "Hz",
                "binding": "filter.cutoff_hz"
            },
            "style": { "width": 56, "height": 56 },
            "layout": { "direction": "column" },
            "children": []
        }
    })JSON";

    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.source == DesignSource::figma_plugin);
    REQUIRE(ir.root.audio_widget == AudioWidgetType::knob);
    REQUIRE(ir.root.audio_label == "Cutoff");
    REQUIRE(ir.root.audio_min == Catch::Approx(20.0f));
    REQUIRE(ir.root.audio_max == Catch::Approx(20000.0f));
    REQUIRE(ir.root.audio_default == Catch::Approx(880.0f));
    REQUIRE(ir.root.attributes.at("units") == "Hz");
    REQUIRE(ir.root.attributes.at("binding") == "filter.cutoff_hz");
}

TEST_CASE("parse_figma_plugin_json handles a Pulp / Knob without optional units",
          "[view][import][figma-plugin][pulp-library][audio-widget][knob]") {
    // An instance can leave `units` empty (the Pulp / Knob default is an
    // empty string). The extractor SHOULD omit empty units; check that the
    // parser still maps the rest of the envelope correctly without it.
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "root": {
            "type": "frame",
            "name": "Mix",
            "audio_widget": "knob",
            "label": "Mix",
            "min": 0,
            "max": 1,
            "default": 0.5,
            "attributes": { "binding": "param.mix" },
            "style": { "width": 32, "height": 32 },
            "children": []
        }
    })JSON";

    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.audio_widget == AudioWidgetType::knob);
    REQUIRE(ir.root.audio_label == "Mix");
    REQUIRE(ir.root.audio_min == Catch::Approx(0.0f));
    REQUIRE(ir.root.audio_max == Catch::Approx(1.0f));
    REQUIRE(ir.root.audio_default == Catch::Approx(0.5f));
    REQUIRE(ir.root.attributes.at("binding") == "param.mix");
    REQUIRE(ir.root.attributes.count("units") == 0);
}

// ──────────────────────────────────────────────────────────────────────────
// figma-plugin audio-widget binding wire-up.
//
// The figma-plugin extractor emits a recognized Pulp Library control as an
// audio widget plus a single free-form `attributes["binding"]` string. Before
// this slice that string reached the IR but was dropped — the native
// materializer and the binding-manifest codegen only consume the
// `pulp*`-prefixed binding contract. These tests pin that a recognized widget's
// `binding` is now normalized into the SAME canonical pulp* binding
// representation (so it produces a real native param hookup AND a
// binding-manifest entry), while an unrecognized/generic node gets nothing.

TEST_CASE("figma-plugin knob binding lowers to a real native param binding",
          "[view][import][figma-plugin][binding-wireup]") {
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "root": {
            "type": "frame",
            "name": "VolumeKnob",
            "audio_widget": "knob",
            "label": "Cutoff",
            "min": 20,
            "max": 20000,
            "default": 880,
            "attributes": {
                "units": "Hz",
                "binding": "filter.cutoff_hz"
            },
            "style": { "width": 56, "height": 56 },
            "layout": { "direction": "column" },
            "children": []
        }
    })JSON";

    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.audio_widget == AudioWidgetType::knob);

    // The raw evidence is preserved.
    REQUIRE(ir.root.attributes.at("binding") == "filter.cutoff_hz");

    // The binding is materialized into the canonical pulp* binding contract —
    // the SAME representation the JSX / Claude path feeds. This is the real
    // native param binding (param_key drives the manifest + codegen helper),
    // not merely a preserved string.
    REQUIRE(ir.root.attributes.at("pulpParamKey") == "filter.cutoff_hz");
    REQUIRE(ir.root.attributes.at("pulpBindingModule") == "filter");
    REQUIRE(ir.root.attributes.at("pulpBindingParam") == "cutoff_hz");
    REQUIRE(ir.root.attributes.at("pulpRouteType") == "native_cpp");
    REQUIRE(ir.root.attributes.at("pulpRouteId") == "figma-plugin:filter.cutoff_hz");

    // The binding-manifest codegen emits a binding entry carrying the param —
    // proof the downstream consumer picks the binding up via its existing logic.
    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(result.binding_manifest.find("\"param_key\": \"filter.cutoff_hz\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"binding_module\": \"filter\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"binding_param\": \"cutoff_hz\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"native_primitive\": \"knob\"") != std::string::npos);

    // And the generated native helper emits a live bind_knob() hookup.
    REQUIRE(result.source.find("ctx.bind_knob(") != std::string::npos);
    REQUIRE(result.source.find("\"filter.cutoff_hz\"") != std::string::npos);
}

TEST_CASE("figma-plugin knob binding without a module dot binds the whole key",
          "[view][import][figma-plugin][binding-wireup]") {
    // "param.mix" splits module/param on the first dot; a binding with no dot
    // (defensive) keeps the whole string as the param key.
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "root": {
            "type": "frame",
            "name": "Mix",
            "audio_widget": "knob",
            "label": "Mix",
            "min": 0,
            "max": 1,
            "default": 0.5,
            "attributes": { "binding": "param.mix" },
            "style": { "width": 32, "height": 32 },
            "children": []
        }
    })JSON";

    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.audio_widget == AudioWidgetType::knob);
    REQUIRE(ir.root.attributes.at("pulpParamKey") == "param.mix");
    REQUIRE(ir.root.attributes.at("pulpBindingModule") == "param");
    REQUIRE(ir.root.attributes.at("pulpBindingParam") == "mix");
    // Raw evidence preserved.
    REQUIRE(ir.root.attributes.at("binding") == "param.mix");
}

TEST_CASE("figma-plugin meter binding lowers to a meter source/channel input",
          "[view][import][figma-plugin][binding-wireup]") {
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "root": {
            "type": "frame",
            "name": "OutputMeter",
            "audio_widget": "meter",
            "label": "Out L",
            "attributes": { "binding": "meter.out_l" },
            "style": { "width": 12, "height": 64 },
            "children": []
        }
    })JSON";

    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.audio_widget == AudioWidgetType::meter);
    // A meter reads from a metering source/channel, not a writable param.
    REQUIRE(ir.root.attributes.at("pulpMeterSource") == "meter");
    REQUIRE(ir.root.attributes.at("pulpMeterChannel") == "out_l");
    REQUIRE(ir.root.attributes.count("pulpParamKey") == 0);
    REQUIRE(ir.root.attributes.at("binding") == "meter.out_l");

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(result.binding_manifest.find("\"meter_source\": \"meter\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"meter_channel\": \"out_l\"") != std::string::npos);
}

TEST_CASE("figma-plugin generic frame with a binding attribute gets no synthesized binding",
          "[view][import][figma-plugin][binding-wireup]") {
    // A node that is NOT a semantically-recognized audio widget must stay a
    // generic/visual node — no pulp* binding is synthesized, and the codegen
    // emits no binding-manifest entry for it. This pins the recognized-widget
    // gate (audio_widget != none).
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "root": {
            "type": "frame",
            "name": "DecorativePanel",
            "attributes": { "binding": "filter.cutoff_hz" },
            "style": { "width": 200, "height": 120 },
            "layout": { "direction": "column" },
            "children": []
        }
    })JSON";

    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.audio_widget == AudioWidgetType::none);

    // No binding contract synthesized.
    REQUIRE(ir.root.attributes.count("pulpParamKey") == 0);
    REQUIRE(ir.root.attributes.count("pulpBindingModule") == 0);
    REQUIRE(ir.root.attributes.count("pulpBindingParam") == 0);
    REQUIRE(ir.root.attributes.count("pulpRouteId") == 0);
    REQUIRE(ir.root.attributes.count("pulpMeterSource") == 0);
    // Raw attribute is still preserved untouched.
    REQUIRE(ir.root.attributes.at("binding") == "filter.cutoff_hz");

    // No binding-manifest entry for an unrecognized node — the manifest holds
    // an empty entries array and never mentions the binding's param.
    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(result.binding_manifest.find("\"entries\": []") != std::string::npos);
    REQUIRE(result.binding_manifest.find("filter.cutoff_hz") == std::string::npos);
}

TEST_CASE("figma-plugin knob with explicit pulp* binding is not overwritten",
          "[view][import][figma-plugin][binding-wireup]") {
    // No-regression: a node that already carries the canonical pulp* binding
    // (e.g. authored or produced by another writer) keeps its values; the
    // figma-plugin `binding` normalization is a no-op there.
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "root": {
            "type": "frame",
            "name": "Knob",
            "audio_widget": "knob",
            "attributes": {
                "binding": "filter.cutoff_hz",
                "pulpParamKey": "osc.detune",
                "pulpBindingModule": "osc",
                "pulpBindingParam": "detune"
            },
            "style": { "width": 56, "height": 56 },
            "children": []
        }
    })JSON";

    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.attributes.at("pulpParamKey") == "osc.detune");
    REQUIRE(ir.root.attributes.at("pulpBindingModule") == "osc");
    REQUIRE(ir.root.attributes.at("pulpBindingParam") == "detune");
}

// ──────────────────────────────────────────────────────────────────────────
// Pulp / Fader + Pulp / Meter recognition.
//
// Mirrors the knob contract for library widgets added in Pulp Library v0.2.0:
//   - Pulp / Fader  (component_set_key 1c2b727f0c0e11026512725aeb546997f16042bd)
//   - Pulp / Meter  (component_set_key 52e1636086b855cb2d20d341d4cfa15e94151eef)
//
// Same envelope shape as Pulp / Knob; only audio_widget enum changes.
TEST_CASE("parse_figma_plugin_json maps Pulp / Fader envelope onto IR widget",
          "[view][import][figma-plugin][pulp-library][audio-widget][fader]") {
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "provenance": {
            "adapter": "figma-plugin",
            "version": "0.1.0",
            "source_uri": "figma://design/vxW6btjzQtc4t9ITLNjev0/Pulp-Library"
        },
        "root": {
            "type": "frame",
            "name": "LevelFader",
            "audio_widget": "fader",
            "label": "Master",
            "min": -inf-replaced-below,
            "max": 6,
            "default": 0,
            "attributes": {
                "units": "dB",
                "binding": "param.master_level"
            },
            "style": { "width": 28, "height": 120 },
            "layout": { "direction": "column" },
            "children": []
        }
    })JSON";
    // The literal "-inf-replaced-below" above is a sentinel — replace it
    // with a real number (-60) so the JSON parser doesn't choke. (Keeps
    // the envelope structure inline-readable in this test source.)
    auto json = envelope;
    auto pos = json.find("-inf-replaced-below");
    REQUIRE(pos != std::string::npos);
    json.replace(pos, std::string("-inf-replaced-below").size(), "-60");

    auto ir = parse_figma_plugin_json(json);
    REQUIRE(ir.source == DesignSource::figma_plugin);
    REQUIRE(ir.root.audio_widget == AudioWidgetType::fader);
    REQUIRE(ir.root.audio_label == "Master");
    REQUIRE(ir.root.audio_min == Catch::Approx(-60.0f));
    REQUIRE(ir.root.audio_max == Catch::Approx(6.0f));
    REQUIRE(ir.root.audio_default == Catch::Approx(0.0f));
    REQUIRE(ir.root.attributes.at("units") == "dB");
    REQUIRE(ir.root.attributes.at("binding") == "param.master_level");
}

TEST_CASE("parse_figma_plugin_json maps Pulp / Meter envelope onto IR widget",
          "[view][import][figma-plugin][pulp-library][audio-widget][meter]") {
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "root": {
            "type": "frame",
            "name": "OutputMeter",
            "audio_widget": "meter",
            "label": "Out L",
            "min": -60,
            "max": 0,
            "default": -12,
            "attributes": {
                "units": "dB",
                "binding": "meter.out_l"
            },
            "style": { "width": 18, "height": 120 },
            "children": []
        }
    })JSON";
    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.audio_widget == AudioWidgetType::meter);
    REQUIRE(ir.root.audio_label == "Out L");
    REQUIRE(ir.root.audio_min == Catch::Approx(-60.0f));
    REQUIRE(ir.root.audio_max == Catch::Approx(0.0f));
    REQUIRE(ir.root.audio_default == Catch::Approx(-12.0f));
    REQUIRE(ir.root.attributes.at("units") == "dB");
    REQUIRE(ir.root.attributes.at("binding") == "meter.out_l");
}

// ── Codegen emits derived skin setters for fader/meter ──────────────────

TEST_CASE("Codegen emits setFaderSkin from derived skin attributes",
          "[view][import][skin][fader]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "root";
    ir.root.style.width = 200;
    ir.root.style.height = 300;

    IRNode fader;
    fader.type = "frame";
    fader.name = "Fader — Master";
    fader.audio_widget = AudioWidgetType::fader;
    fader.audio_min = -60.0f;
    fader.audio_max = 6.0f;
    fader.audio_default = 0.0f;  // 0 dB → normalized ≈ 0.909
    fader.style.width = 96;
    fader.style.height = 230;
    // Stamped by the importer's PNG sampler (here supplied directly).
    fader.attributes["skin_track_color"] = "#1f2129";
    fader.attributes["skin_fill_color"] = "#3677cf";
    fader.attributes["skin_thumb_color"] = "#eaeaf0";
    fader.attributes["skin_thumb_border_color"] = "#69696f";
    ir.root.children.push_back(fader);

    CodeGenOptions opts;
    opts.skin_faders = true;
    std::string js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("setFaderSkin(") != std::string::npos);
    REQUIRE(js.find("'#1f2129'") != std::string::npos);
    REQUIRE(js.find("'#3677cf'") != std::string::npos);
    // The captured value is normalised into [0,1] (≈0.909), not emitted raw.
    REQUIRE(js.find("setValue(") != std::string::npos);
    REQUIRE(js.find("setValue('Fader__Master2', 0)") == std::string::npos);

    // Opt-out: --fader-style=default suppresses the skin call.
    CodeGenOptions plain;
    plain.skin_faders = false;
    std::string plain_js = generate_pulp_js(ir, plain);
    REQUIRE(plain_js.find("setFaderSkin(") == std::string::npos);
}

TEST_CASE("Codegen emits setMeterColors + normalized level from derived skin",
          "[view][import][skin][meter]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "root";
    ir.root.style.width = 200;
    ir.root.style.height = 300;

    IRNode meter;
    meter.type = "frame";
    meter.name = "Meter — Out L";
    meter.audio_widget = AudioWidgetType::meter;
    meter.audio_min = -60.0f;
    meter.audio_max = 0.0f;
    meter.audio_default = -6.0f;  // → normalized 0.9
    meter.style.width = 69;
    meter.style.height = 228;
    meter.attributes["skin_meter_gradient"] = "#33a74d,#ffab33,#ff6b66";
    meter.attributes["skin_meter_background"] = "#0f1217";
    ir.root.children.push_back(meter);

    CodeGenOptions opts;
    opts.skin_meters = true;
    std::string js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("setMeterColors(") != std::string::npos);
    REQUIRE(js.find("#33a74d,#ffab33,#ff6b66") != std::string::npos);
    // Level normalised to 0.9, not emitted as a raw dB value.
    REQUIRE(js.find("setMeterLevel(") != std::string::npos);
    REQUIRE(js.find("0.9") != std::string::npos);

    CodeGenOptions plain;
    plain.skin_meters = false;
    std::string plain_js = generate_pulp_js(ir, plain);
    REQUIRE(plain_js.find("setMeterColors(") == std::string::npos);
}

// ── Derived narrow widths flow to render ────────────────────────────────

TEST_CASE("Codegen renders fader at derived thumb width + emits thin track width",
          "[view][import][skin][fader][width]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "root";
    ir.root.style.width = 200;
    ir.root.style.height = 300;

    IRNode fader;
    fader.type = "frame";
    fader.name = "Fader — Master";
    fader.audio_widget = AudioWidgetType::fader;
    fader.audio_min = -60.0f;
    fader.audio_max = 6.0f;
    fader.audio_default = 0.0f;
    fader.style.width = 96;     // node box
    fader.style.height = 230;
    // Stamped by the sampler: thumb slab width (→ shape_width / widget width)
    // and the thin track width (→ setFaderTrackWidth).
    fader.attributes["shape_width"] = "28";
    fader.attributes["skin_track_width"] = "5";
    ir.root.children.push_back(fader);

    CodeGenOptions opts;
    opts.skin_faders = true;
    std::string js = generate_pulp_js(ir, opts);

    // The fader WIDGET renders at the narrow thumb width (28), while the column
    // keeps the box width (96) so the narrow widget centres in its slot.
    REQUIRE(js.find("setFlex('Fader__Master0', 'width', 28)") != std::string::npos);
    REQUIRE(js.find("setFlex('Fader__Master0_col', 'min_width', 96)") != std::string::npos);
    // The thin track width flows through to the render path.
    REQUIRE(js.find("setFaderTrackWidth('Fader__Master0', 5)") != std::string::npos);
}

TEST_CASE("Codegen renders meter at derived narrow bar width, centred in column",
          "[view][import][skin][meter][width]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "root";
    ir.root.style.width = 200;
    ir.root.style.height = 300;

    IRNode meter;
    meter.type = "frame";
    meter.name = "Meter — Out L";
    meter.audio_widget = AudioWidgetType::meter;
    meter.audio_min = -60.0f;
    meter.audio_max = 0.0f;
    meter.audio_default = -6.0f;
    meter.style.width = 69;     // node box
    meter.style.height = 228;
    meter.attributes["shape_width"] = "18";  // derived narrow bar width
    ir.root.children.push_back(meter);

    CodeGenOptions opts;
    opts.skin_meters = true;
    std::string js = generate_pulp_js(ir, opts);

    // The meter renders at the narrow bar width (18), centred via the column
    // which keeps the box width (69) as its min_width.
    REQUIRE(js.find("setFlex('Meter__Out_L0', 'width', 18)") != std::string::npos);
    REQUIRE(js.find("setFlex('Meter__Out_L0_col', 'min_width', 69)") != std::string::npos);
}

// ──────────────────────────────────────────────────────────────────────────
// Pulp / XYPad + Pulp / Waveform + Pulp / Spectrum recognition.
//
// Library v0.3.0 adds three component-sets in
// https://www.figma.com/design/vxW6btjzQtc4t9ITLNjev0/Pulp-Library:
//   Pulp / XYPad     component_set_key 9dc09d4cbf65341f12c21ece408ad653886059b9
//   Pulp / Waveform  component_set_key 2c0797af5c939638ec6a89d893ba310a088ce46c
//   Pulp / Spectrum  component_set_key f6730821fc7557e93f904d171a45339207abf9e3
//
// XYPad uniquely carries a second-axis binding via attributes.binding_y;
// Waveform and Spectrum's `binding` carries an audio-source path
// (bus.master_l / bus.fft.master_l) instead of a parameter route.

TEST_CASE("parse_figma_plugin_json maps Pulp / XYPad envelope onto IR widget",
          "[view][import][figma-plugin][pulp-library][audio-widget][xy-pad]") {
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "root": {
            "type": "frame",
            "name": "FilterPad",
            "audio_widget": "xy_pad",
            "label": "Filter",
            "min": 20,
            "max": 20000,
            "default": 880,
            "attributes": {
                "units": "Hz",
                "binding": "filter.cutoff_hz",
                "binding_y": "filter.resonance"
            },
            "style": { "width": 120, "height": 120 },
            "children": []
        }
    })JSON";
    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.source == DesignSource::figma_plugin);
    REQUIRE(ir.root.audio_widget == AudioWidgetType::xy_pad);
    REQUIRE(ir.root.audio_label == "Filter");
    REQUIRE(ir.root.audio_min == Catch::Approx(20.0f));
    REQUIRE(ir.root.audio_max == Catch::Approx(20000.0f));
    REQUIRE(ir.root.audio_default == Catch::Approx(880.0f));
    REQUIRE(ir.root.attributes.at("units") == "Hz");
    REQUIRE(ir.root.attributes.at("binding") == "filter.cutoff_hz");
    // XYPad-specific second-axis binding rides on attributes.binding_y.
    REQUIRE(ir.root.attributes.at("binding_y") == "filter.resonance");
}

TEST_CASE("parse_figma_plugin_json maps Pulp / Waveform envelope onto IR widget",
          "[view][import][figma-plugin][pulp-library][audio-widget][waveform]") {
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "root": {
            "type": "frame",
            "name": "Master scope",
            "audio_widget": "waveform",
            "label": "Master",
            "min": -1,
            "max": 1,
            "attributes": {
                "binding": "bus.master_l"
            },
            "style": { "width": 200, "height": 64 },
            "children": []
        }
    })JSON";
    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.audio_widget == AudioWidgetType::waveform);
    REQUIRE(ir.root.audio_label == "Master");
    REQUIRE(ir.root.audio_min == Catch::Approx(-1.0f));
    REQUIRE(ir.root.audio_max == Catch::Approx(1.0f));
    REQUIRE(ir.root.attributes.at("binding") == "bus.master_l");
    // Waveform envelopes default to empty units and an audio-source
    // path in binding (rather than a param route). Codegen branches on
    // audio_widget to interpret accordingly.
    REQUIRE(ir.root.attributes.count("units") == 0);
    REQUIRE(ir.root.attributes.count("binding_y") == 0);
}

TEST_CASE("parse_figma_plugin_json maps Pulp / Spectrum envelope onto IR widget",
          "[view][import][figma-plugin][pulp-library][audio-widget][spectrum]") {
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "root": {
            "type": "frame",
            "name": "MasterFFT",
            "audio_widget": "spectrum",
            "label": "Spectrum",
            "min": -60,
            "max": 0,
            "attributes": {
                "units": "dB",
                "binding": "bus.fft.master_l"
            },
            "style": { "width": 200, "height": 64 },
            "children": []
        }
    })JSON";
    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.audio_widget == AudioWidgetType::spectrum);
    REQUIRE(ir.root.audio_label == "Spectrum");
    REQUIRE(ir.root.audio_min == Catch::Approx(-60.0f));
    REQUIRE(ir.root.audio_max == Catch::Approx(0.0f));
    REQUIRE(ir.root.attributes.at("units") == "dB");
    // FFT source binding — the `fft.` segment is the convention
    // distinguishing this from a parameter route to consumers that care.
    REQUIRE(ir.root.attributes.at("binding") == "bus.fft.master_l");
}

// ──────────────────────────────────────────────────────────────────────────
// Phase B5 — Kitchen-sink smoke fixture.
//
// A single envelope that exercises every public Pulp Library widget at
// once (Knob, Fader, Meter, XYPad, Waveform, Spectrum) plus all three
// binding shapes (param route, audio-source path, XYPad's second-axis
// binding_y). This is the canonical multi-widget smoke fixture for the
// figma-plugin → import-design path — the per-widget Phase-3 / Phase-5
// tests above pin one widget at a time; this test pins that the SAME
// envelope shape carries them all simultaneously.
//
// Acceptance:
//   1. Parser tolerates the full multi-widget envelope.
//   2. Each child node is recognized with the correct audio_widget enum.
//   3. Raw bindings are preserved on attributes for every widget.
//   4. The binding-wireup pass produces canonical pulp* keys for the
//      param-route widgets (knob/fader/meter) and leaves the audio-source
//      widgets (waveform/spectrum) carrying only the raw `binding`.
//   5. XYPad-specific binding_y rides on attributes.binding_y.
//   6. Audio ranges round-trip on every widget.
//
// The envelope is hand-rolled so the parser contract stays deterministic and
// does not depend on a live Figma document or plugin publish state.
TEST_CASE("kitchen-sink envelope parses all 6 Pulp Library widgets from one root frame",
          "[view][import][figma-plugin][pulp-library][kitchen-sink]") {
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "provenance": {
            "adapter": "figma-plugin",
            "version": "0.4.0",
            "source_uri": "figma://design/kitchen-sink-smoke"
        },
        "root": {
            "type": "frame",
            "name": "PluginEditor",
            "style": { "width": 720, "height": 360 },
            "layout": { "direction": "row" },
            "children": [
                {
                    "type": "frame",
                    "name": "Cutoff",
                    "audio_widget": "knob",
                    "label": "Cutoff",
                    "min": 20,
                    "max": 20000,
                    "default": 880,
                    "attributes": { "units": "Hz", "binding": "filter.cutoff_hz" },
                    "style": { "width": 56, "height": 56 },
                    "children": []
                },
                {
                    "type": "frame",
                    "name": "Master",
                    "audio_widget": "fader",
                    "label": "Master",
                    "min": -60,
                    "max": 6,
                    "default": 0,
                    "attributes": { "units": "dB", "binding": "param.master_level" },
                    "style": { "width": 28, "height": 120 },
                    "children": []
                },
                {
                    "type": "frame",
                    "name": "OutL",
                    "audio_widget": "meter",
                    "label": "Out L",
                    "min": -60,
                    "max": 0,
                    "default": -12,
                    "attributes": { "units": "dB", "binding": "meter.out_l" },
                    "style": { "width": 18, "height": 120 },
                    "children": []
                },
                {
                    "type": "frame",
                    "name": "FilterPad",
                    "audio_widget": "xy_pad",
                    "label": "Filter",
                    "min": 20,
                    "max": 20000,
                    "default": 880,
                    "attributes": {
                        "units": "Hz",
                        "binding": "filter.cutoff_hz",
                        "binding_y": "filter.resonance"
                    },
                    "style": { "width": 120, "height": 120 },
                    "children": []
                },
                {
                    "type": "frame",
                    "name": "Master scope",
                    "audio_widget": "waveform",
                    "label": "Master",
                    "min": -1,
                    "max": 1,
                    "attributes": { "binding": "bus.master_l" },
                    "style": { "width": 200, "height": 64 },
                    "children": []
                },
                {
                    "type": "frame",
                    "name": "MasterFFT",
                    "audio_widget": "spectrum",
                    "label": "Spectrum",
                    "min": -60,
                    "max": 0,
                    "attributes": { "units": "dB", "binding": "bus.fft.master_l" },
                    "style": { "width": 200, "height": 64 },
                    "children": []
                }
            ]
        }
    })JSON";

    auto ir = parse_figma_plugin_json(envelope);

    // 1 — Envelope tolerated end-to-end.
    REQUIRE(ir.source == DesignSource::figma_plugin);
    REQUIRE(ir.root.name == "PluginEditor");
    REQUIRE(ir.root.children.size() == 6);

    const auto& knob = ir.root.children[0];
    const auto& fader = ir.root.children[1];
    const auto& meter = ir.root.children[2];
    const auto& xy = ir.root.children[3];
    const auto& wave = ir.root.children[4];
    const auto& spec = ir.root.children[5];

    // 2 — Every child recognized with the correct audio_widget enum.
    REQUIRE(knob.audio_widget == AudioWidgetType::knob);
    REQUIRE(fader.audio_widget == AudioWidgetType::fader);
    REQUIRE(meter.audio_widget == AudioWidgetType::meter);
    REQUIRE(xy.audio_widget == AudioWidgetType::xy_pad);
    REQUIRE(wave.audio_widget == AudioWidgetType::waveform);
    REQUIRE(spec.audio_widget == AudioWidgetType::spectrum);

    // 3 — Raw bindings preserved on each widget.
    REQUIRE(knob.attributes.at("binding") == "filter.cutoff_hz");
    REQUIRE(fader.attributes.at("binding") == "param.master_level");
    REQUIRE(meter.attributes.at("binding") == "meter.out_l");
    REQUIRE(xy.attributes.at("binding") == "filter.cutoff_hz");
    REQUIRE(wave.attributes.at("binding") == "bus.master_l");
    REQUIRE(spec.attributes.at("binding") == "bus.fft.master_l");

    // 4 — Binding-wireup smoke. The knob's `binding` synthesises the
    //     canonical pulp* param-key contract (full pinning lives in the
    //     `[binding-wireup]` per-widget tests); the meter's `binding`
    //     lowers to a meter source/channel (NOT a pulpParamKey). This
    //     test asserts only that both wire-up flavours coexist on one
    //     envelope without interference — full coverage of each flavour
    //     is in the dedicated binding-wireup tests above.
    REQUIRE(knob.attributes.at("pulpParamKey") == "filter.cutoff_hz");
    REQUIRE(meter.attributes.count("pulpParamKey") == 0);
    REQUIRE(meter.attributes.at("pulpMeterSource") == "meter");
    REQUIRE(meter.attributes.at("pulpMeterChannel") == "out_l");

    // 5 — XYPad second-axis binding rides on binding_y; other widgets
    //     do not carry it.
    REQUIRE(xy.attributes.at("binding_y") == "filter.resonance");
    REQUIRE(knob.attributes.count("binding_y") == 0);
    REQUIRE(wave.attributes.count("binding_y") == 0);

    // 6 — Audio ranges round-trip on every widget that carries them.
    REQUIRE(knob.audio_min == Catch::Approx(20.0f));
    REQUIRE(knob.audio_max == Catch::Approx(20000.0f));
    REQUIRE(fader.audio_min == Catch::Approx(-60.0f));
    REQUIRE(fader.audio_max == Catch::Approx(6.0f));
    REQUIRE(meter.audio_min == Catch::Approx(-60.0f));
    REQUIRE(meter.audio_max == Catch::Approx(0.0f));
    REQUIRE(xy.audio_min == Catch::Approx(20.0f));
    REQUIRE(xy.audio_max == Catch::Approx(20000.0f));
    REQUIRE(wave.audio_min == Catch::Approx(-1.0f));
    REQUIRE(wave.audio_max == Catch::Approx(1.0f));
    REQUIRE(spec.audio_min == Catch::Approx(-60.0f));
    REQUIRE(spec.audio_max == Catch::Approx(0.0f));
}
