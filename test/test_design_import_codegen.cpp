#include "test_design_import_shared.hpp"

TEST_CASE("parse_v0_tsx normalizes JSON and unsupported-source fallback diagnostics",
          "[view][import][diagnostics]") {
    auto json_ir = parse_v0_tsx(R"json({
        "type": "frame",
        "name": "JSON Root",
        "children": [{ "type": "text", "content": "Gain" }]
    })json");

    REQUIRE(json_ir.source == DesignSource::v0);
    REQUIRE(json_ir.capture_method == "adapter_parse");
    REQUIRE(json_ir.source_adapter == "v0-tsx");
    REQUIRE(json_ir.source_version == "1");
    REQUIRE(json_ir.root.confidence == IRConfidence::pass);
    REQUIRE(json_ir.root.stable_anchor_id.has_value());

    auto fallback_ir = parse_v0_tsx("const gain = 0.5;");

    REQUIRE(fallback_ir.source == DesignSource::v0);
    REQUIRE(fallback_ir.capture_method == "adapter_parse");
    REQUIRE(fallback_ir.source_adapter == "v0-tsx");
    REQUIRE(fallback_ir.source_version == "1");
    REQUIRE(fallback_ir.root.confidence == IRConfidence::diverge);
    REQUIRE(fallback_ir.fallback_reason.find("no supported host JSX tags") != std::string::npos);
    REQUIRE(has_import_diagnostic(fallback_ir.diagnostics, "fallback-used"));
    REQUIRE(fallback_ir.diagnostics[0].kind == ImportDiagnosticKind::fallback_used);
    REQUIRE(fallback_ir.root.stable_anchor_id.has_value());
}

TEST_CASE("parse_v0_tsx preserves inline-style host controls for baked C++",
          "[view][import][cpp-codegen]") {
    auto ir = parse_v0_tsx(R"tsx(
        export default function ControlStrip() {
            return (
                <section style={{
                    display: "flex",
                    flexDirection: "row",
                    gap: 12,
                    padding: 16,
                    backgroundColor: "#101216",
                    width: 320,
                    height: 120
                }}>
                    <button
                        aria-label="Bypass"
                        onClick={() => setBypassed(!bypassed)}
                        style={{ borderRadius: 6, color: "#ffffff" }}>
                        BYP
                    </button>
                    <label style={{ fontSize: 11, color: "#8aa2ff" }}>GAIN</label>
                    <input
                        type="range"
                        aria-label="Gain"
                        min={0}
                        max={1}
                        step={0.01}
                        value={0.65}
                        style={{ width: 96, height: 18 }} />
                    <svg width={24} height={24}>
                        <path d="M 2 12 L 22 12" stroke="#8aa2ff" strokeWidth={2} />
                    </svg>
                </section>
            );
        }
    )tsx");

    REQUIRE(ir.source == DesignSource::v0);
    REQUIRE(ir.root.type == "frame");
    REQUIRE(ir.root.confidence == IRConfidence::diverge);
    REQUIRE(has_import_diagnostic(ir.diagnostics, "capture-partial"));
    REQUIRE_FALSE(has_import_diagnostic(ir.diagnostics, "fallback-used"));
    REQUIRE(ir.root.name == "section");
    REQUIRE(ir.root.layout.direction == LayoutDirection::row);
    REQUIRE(ir.root.style.background_color.has_value());
    REQUIRE(*ir.root.style.background_color == "#101216");
    REQUIRE(ir.root.stable_anchor_id.has_value());

    const auto* button = find_descendant(ir.root, [](const IRNode& node) {
        return node.type == "button" && node.text_content == "BYP";
    });
    REQUIRE(button != nullptr);
    REQUIRE(button->style.border_radius.has_value());
    REQUIRE(*button->style.border_radius == 6.0f);

    const auto* range = find_descendant(ir.root, [](const IRNode& node) {
        auto type = node.attributes.find("type");
        return node.type == "input" && type != node.attributes.end() && type->second == "range";
    });
    REQUIRE(range != nullptr);
    REQUIRE(range->style.width.has_value());
    REQUIRE(*range->style.width == 96.0f);

    const auto* label = find_descendant(ir.root, [](const IRNode& node) {
        return node.type == "text" && node.text_content == "GAIN";
    });
    REQUIRE(label != nullptr);
    REQUIRE(label->style.font_size.has_value());
    REQUIRE(*label->style.font_size == 11.0f);

    const auto* path = find_descendant(ir.root, [](const IRNode& node) {
        return node.type == "path" && node.attributes.count("d") != 0;
    });
    REQUIRE(path != nullptr);

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(result.source.find("std::make_unique<pulp::view::TextButton>") != std::string::npos);
    REQUIRE(result.source.find("std::make_unique<pulp::view::Fader>") != std::string::npos);
    REQUIRE(result.source.find("std::make_unique<pulp::view::Label>") != std::string::npos);
    REQUIRE(result.source.find("std::make_unique<pulp::view::SvgPathWidget>") != std::string::npos);
}

TEST_CASE("generate_pulp_cpp resolves figma-plugin asset_ref image sources",
          "[view][import][cpp-codegen][figma-plugin][asset-ref]") {
    DesignIR ir;
    ir.source = DesignSource::figma_plugin;
    ir.source_adapter = "figma-plugin";
    ir.root.type = "image";
    ir.root.name = "Imported Figma Image";
    ir.root.attributes["asset_ref"] = "3:43";
    ir.root.style.width = 64.0f;
    ir.root.style.height = 32.0f;

    IRAssetRef asset;
    asset.asset_id = "3:43";
    asset.original_uri = "figma://KCKIyZoWXjde6qVNCm4qPa/3:43";
    asset.local_path = "/resolved/import/assets/3_43.png";
    asset.mime = "image/png";
    ir.asset_manifest.assets.push_back(asset);

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(result.source.find("set_image_source(\"file:///resolved/import/assets/3_43.png\")") != std::string::npos);
}

TEST_CASE("generate_pulp_cpp emits a DesignFrameView for a faithful_svg node",
          "[view][import][cpp-codegen][faithful-svg]") {
    DesignIR ir;
    ir.source = DesignSource::figma_plugin;
    ir.source_adapter = "figma-plugin";
    ir.root.type = "frame";
    ir.root.name = "Faithful Panel";
    ir.root.style.width = 200.0f;
    ir.root.style.height = 100.0f;
    ir.root.render_mode = NodeRenderMode::faithful_svg;
    ir.root.svg_asset_id = "panel-svg";

    // A knob (SVG-patch) overlay and a dropdown (native) overlay.
    IRInteractiveElement knob;
    knob.kind = InteractiveElementKind::knob;
    knob.cx = 50.0f; knob.cy = 60.0f; knob.hit_radius = 20.0f;
    knob.svg_patch_d = "M0 0 L1 1";
    knob.default_value = 0.25f;
    knob.param_key = "osc.shape";   // host-param binding key (geometry-detected control)
    ir.root.interactive_elements.push_back(knob);

    IRInteractiveElement dd;
    dd.kind = InteractiveElementKind::dropdown;
    dd.x = 10.0f; dd.y = 10.0f; dd.w = 80.0f; dd.h = 20.0f;
    dd.options = {"Sine", "Saw"};
    dd.selected_index = 1;
    ir.root.interactive_elements.push_back(dd);

    IRAssetRef asset;
    asset.asset_id = "panel-svg";
    asset.original_uri =
        "data:image/svg+xml,<svg xmlns=\"http://www.w3.org/2000/svg\" "
        "width=\"200\" height=\"100\"><rect width=\"200\" height=\"100\" fill=\"#222\"/></svg>";
    asset.mime = "image/svg+xml";
    ir.asset_manifest.assets.push_back(asset);

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    // The faithful node lowers to a DesignFrameView built from the embedded
    // (base64) SVG, not the native widget tree.
    REQUIRE(result.source.find("std::make_unique<pulp::view::DesignFrameView>") != std::string::npos);
    REQUIRE(result.source.find("pulp::runtime::base64_decode") != std::string::npos);
    REQUIRE(result.source.find("#include <pulp/view/design_frame_view.hpp>") != std::string::npos);
    // Both typed overlays are reconstructed.
    REQUIRE(result.source.find("DesignFrameElement::Kind::knob") != std::string::npos);
    REQUIRE(result.source.find("DesignFrameElement::Kind::dropdown") != std::string::npos);
    REQUIRE(result.source.find("el.needle_d = \"M0 0 L1 1\"") != std::string::npos);
    REQUIRE(result.source.find("el.options = {") != std::string::npos);
    REQUIRE(result.source.find("el.selected_index = 1;") != std::string::npos);
    // The knob's host-param binding key is emitted, and the frame self-wires
    // gestures to the host-param surface (parity with make_faithful_svg_frame).
    REQUIRE(result.source.find("el.param_key = \"osc.shape\";") != std::string::npos);
    REQUIRE(result.source.find("route_changes_to_host_params(true);") != std::string::npos);
}

TEST_CASE("generate_pulp_cpp falls back to native widgets when a faithful_svg asset is unresolved",
          "[view][import][cpp-codegen][faithful-svg]") {
    DesignIR ir;
    ir.source = DesignSource::figma_plugin;
    ir.source_adapter = "figma-plugin";
    ir.root.type = "frame";
    ir.root.name = "Faithful Panel (missing asset)";
    ir.root.style.width = 120.0f;
    ir.root.style.height = 80.0f;
    ir.root.render_mode = NodeRenderMode::faithful_svg;
    ir.root.svg_asset_id = "not-in-manifest";  // no matching IRAssetRef

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    // With no resolvable SVG, codegen must NOT emit a DesignFrameView; it falls
    // back to the normal native emit so the output still compiles and renders.
    REQUIRE(result.source.find("std::make_unique<pulp::view::DesignFrameView>") == std::string::npos);
    REQUIRE(result.source.find("std::make_unique<pulp::view::View>") != std::string::npos);
    REQUIRE(result.source.find("unresolved at codegen time") != std::string::npos);
}

TEST_CASE("generate_pulp_cpp emits all faithful overlay kinds and chunks a large SVG",
          "[view][import][cpp-codegen][faithful-svg]") {
    DesignIR ir;
    ir.source = DesignSource::figma_plugin;
    ir.source_adapter = "figma-plugin";
    ir.root.type = "frame";
    ir.root.name = "Faithful Panel (all overlays)";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 300.0f;
    ir.root.render_mode = NodeRenderMode::faithful_svg;
    ir.root.svg_asset_id = "big-svg";

    IRInteractiveElement tf;
    tf.kind = InteractiveElementKind::text_field;
    tf.x = 8.0f; tf.y = 8.0f; tf.w = 120.0f; tf.h = 24.0f;
    tf.placeholder = "Name";
    tf.bg_color = "#1A1A1A";
    ir.root.interactive_elements.push_back(tf);

    IRInteractiveElement tabs;
    tabs.kind = InteractiveElementKind::tab_group;
    tabs.x = 8.0f; tabs.y = 40.0f; tabs.w = 200.0f; tabs.h = 28.0f;
    tabs.options = {"A", "B", "C"};
    ir.root.interactive_elements.push_back(tabs);

    IRInteractiveElement step;
    step.kind = InteractiveElementKind::stepper;
    step.x = 8.0f; step.y = 80.0f; step.w = 100.0f; step.h = 24.0f;
    step.options = {"x1", "x2"};
    ir.root.interactive_elements.push_back(step);

    IRInteractiveElement fader;          // SVG-patch thumb over a track
    fader.kind = InteractiveElementKind::fader;
    fader.x = 8.0f; fader.y = 120.0f; fader.w = 12.0f; fader.h = 80.0f;
    fader.svg_patch_d = "M14 200L14 190";
    fader.default_value = 0.4f;
    ir.root.interactive_elements.push_back(fader);

    IRInteractiveElement flashToggle;    // press-flash command button
    flashToggle.kind = InteractiveElementKind::toggle;
    flashToggle.x = 8.0f; flashToggle.y = 220.0f; flashToggle.w = 40.0f; flashToggle.h = 20.0f;
    flashToggle.flash = true;
    ir.root.interactive_elements.push_back(flashToggle);

    IRInteractiveElement swap;           // swap-link button
    swap.kind = InteractiveElementKind::swap;
    swap.x = 60.0f; swap.y = 120.0f; swap.w = 60.0f; swap.h = 20.0f;
    swap.target_frame = 2;
    ir.root.interactive_elements.push_back(swap);

    IRInteractiveElement act;            // command button
    act.kind = InteractiveElementKind::action;
    act.x = 60.0f; act.y = 150.0f; act.w = 30.0f; act.h = 20.0f;
    act.action = "octave_up";
    ir.root.interactive_elements.push_back(act);

    IRInteractiveElement pad;            // xy pad
    pad.kind = InteractiveElementKind::xy_pad;
    pad.x = 60.0f; pad.y = 180.0f; pad.w = 80.0f; pad.h = 80.0f;
    pad.default_value = 0.3f; pad.default_value_y = 0.7f;
    ir.root.interactive_elements.push_back(pad);

    IRInteractiveElement lbl;            // value readout
    lbl.kind = InteractiveElementKind::value_label;
    lbl.x = 60.0f; lbl.y = 270.0f; lbl.w = 80.0f; lbl.h = 16.0f;
    lbl.text = "-6.0 dB"; lbl.value_left_align = true;
    ir.root.interactive_elements.push_back(lbl);

    IRInteractiveElement cst;            // registered custom control
    cst.kind = InteractiveElementKind::custom;
    cst.x = 160.0f; cst.y = 120.0f; cst.w = 60.0f; cst.h = 40.0f;
    cst.factory_id = "acme.spinner"; cst.custom_props = "{\"max\":11}";
    ir.root.interactive_elements.push_back(cst);

    // A large SVG (> ~8KB base64) so the embed spans multiple chunk literals.
    std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"400\" height=\"300\">";
    for (int i = 0; i < 400; ++i)
        svg += "<rect x=\"0\" y=\"0\" width=\"400\" height=\"1\" fill=\"#202020\"/>";
    svg += "</svg>";
    REQUIRE(svg.size() > 6000);  // base64 (~4/3×) clears the 8000-char chunk size

    IRAssetRef asset;
    asset.asset_id = "big-svg";
    asset.original_uri = "data:image/svg+xml," + svg;
    asset.mime = "image/svg+xml";
    ir.asset_manifest.assets.push_back(asset);

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(result.source.find("std::make_unique<pulp::view::DesignFrameView>") != std::string::npos);
    REQUIRE(result.source.find("DesignFrameElement::Kind::text_field") != std::string::npos);
    REQUIRE(result.source.find("DesignFrameElement::Kind::tab_group") != std::string::npos);
    REQUIRE(result.source.find("DesignFrameElement::Kind::stepper") != std::string::npos);
    REQUIRE(result.source.find("DesignFrameElement::Kind::fader") != std::string::npos);
    REQUIRE(result.source.find("DesignFrameElement::Kind::toggle") != std::string::npos);
    REQUIRE(result.source.find("DesignFrameElement::Kind::swap") != std::string::npos);
    REQUIRE(result.source.find("DesignFrameElement::Kind::action") != std::string::npos);
    REQUIRE(result.source.find("DesignFrameElement::Kind::xy_pad") != std::string::npos);
    REQUIRE(result.source.find("DesignFrameElement::Kind::value_label") != std::string::npos);
    REQUIRE(result.source.find("el.placeholder = \"Name\"") != std::string::npos);
    REQUIRE(result.source.find("el.bg_color = \"#1A1A1A\"") != std::string::npos);
    REQUIRE(result.source.find("el.needle_d = \"M14 200L14 190\"") != std::string::npos);  // fader thumb path
    REQUIRE(result.source.find("el.flash = true;") != std::string::npos);                  // press-flash toggle
    REQUIRE(result.source.find("el.target_frame = 2;") != std::string::npos);              // swap link
    REQUIRE(result.source.find("el.action = \"octave_up\"") != std::string::npos);         // command id
    REQUIRE(result.source.find("el.text = \"-6.0 dB\"") != std::string::npos);             // readout
    REQUIRE(result.source.find("el.value_left_align = true;") != std::string::npos);       // label align
    REQUIRE(result.source.find("el.value_y = ") != std::string::npos);                     // xy_pad Y axis
    REQUIRE(result.source.find("DesignFrameElement::Kind::custom") != std::string::npos);  // Tier-3
    REQUIRE(result.source.find("el.factory_id = \"acme.spinner\"") != std::string::npos);  // factory id
    REQUIRE(result.source.find("el.custom_props = ") != std::string::npos);                // opaque props
    // No element carries a param_key here, so no host-param routing is wired.
    REQUIRE(result.source.find("route_changes_to_host_params") == std::string::npos);
    // The base64 embed spans multiple chunk literals (the chunk loop ran > once):
    // each interior chunk ends with a `",` line, so at least one is present.
    REQUIRE(result.source.find("\",") != std::string::npos);
}

TEST_CASE("generate_pulp_cpp preserves figma-plugin bleed sprite geometry",
          "[view][import][cpp-codegen][figma-plugin][fidelity]") {
    DesignIR ir;
    ir.source = DesignSource::figma_plugin;
    ir.source_adapter = "figma-plugin";
    ir.root.type = "image";
    ir.root.name = "Imported Bleed Sprite";
    ir.root.attributes["asset_ref"] = "sprite";
    ir.root.attributes["png_natural_w"] = "420";
    ir.root.attributes["png_natural_h"] = "484";
    ir.root.attributes["art_core_x"] = "148";
    ir.root.attributes["art_core_y"] = "0";
    ir.root.attributes["art_core_w"] = "115";
    ir.root.attributes["art_core_h"] = "129";
    ir.root.style.width = 62.0f;
    ir.root.style.height = 68.0f;
    ir.root.style.position = "absolute";
    ir.root.style.left = 20.0f;
    ir.root.style.top = 30.0f;
    ir.root.style.render_bounds = IRStyle::RenderBounds{210.0f, 116.0f, -74.0f, 0.0f};

    IRAssetRef asset;
    asset.asset_id = "sprite";
    asset.original_uri = "figma://fixture/sprite";
    asset.local_path = "/resolved/import/assets/sprite.png";
    asset.mime = "image/png";
    ir.asset_manifest.assets.push_back(asset);

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(result.source.find("set_image_source(\"file:///resolved/import/assets/sprite.png\")") != std::string::npos);
    REQUIRE(result.source.find("_image_flex.preferred_width") != std::string::npos);
    REQUIRE(result.source.find("_image_flex.dim_width") != std::string::npos);
    REQUIRE(result.source.find("->set_left(") != std::string::npos);
}

TEST_CASE("generate_pulp_cpp suppresses decorative child hits under promoted native widgets",
          "[view][import][cpp-codegen][hit-test]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.style.width = 120.0f;
    ir.root.style.height = 120.0f;
    ir.root.layout.direction = LayoutDirection::column;

    IRNode knob;
    knob.type = "frame";
    knob.name = "Gain Knob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Gain";
    knob.style.width = 80.0f;
    knob.style.height = 80.0f;
    knob.layout.direction = LayoutDirection::column;

    IRNode decorative;
    decorative.type = "frame";
    decorative.name = "Imported Knob Art";
    decorative.style.width = 80.0f;
    decorative.style.height = 40.0f;

    IRNode nested_button;
    nested_button.type = "button";
    nested_button.name = "Nested Fine Button";
    nested_button.text_content = "Fine";
    nested_button.style.width = 60.0f;
    nested_button.style.height = 20.0f;

    knob.children.push_back(std::move(decorative));
    knob.children.push_back(std::move(nested_button));
    ir.root.children.push_back(std::move(knob));

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(result.source.find("std::make_unique<pulp::view::Knob>") != std::string::npos);
    REQUIRE(result.source.find("std::make_unique<pulp::view::TextButton>") != std::string::npos);
    REQUIRE(count_occurrences(result.source, "->set_hit_testable(false);") == 1);
}

TEST_CASE("generate_pulp_cpp suppresses extracted decorative child helpers under promoted native widgets",
          "[view][import][cpp-codegen][hit-test]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.style.width = 120.0f;
    ir.root.style.height = 120.0f;
    ir.root.layout.direction = LayoutDirection::column;

    IRNode knob;
    knob.type = "frame";
    knob.name = "Gain Knob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Gain";
    knob.style.width = 80.0f;
    knob.style.height = 80.0f;
    knob.layout.direction = LayoutDirection::column;

    IRNode decorative;
    decorative.type = "frame";
    decorative.name = "DecorativeLayer";
    decorative.style.width = 80.0f;
    decorative.style.height = 40.0f;

    knob.children.push_back(std::move(decorative));
    ir.root.children.push_back(std::move(knob));

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    const auto helper_pos = result.source.find("auto child_");
    REQUIRE(helper_pos != std::string::npos);
    const auto disable_pos = result.source.find("->set_hit_testable(false);", helper_pos);
    REQUIRE(disable_pos != std::string::npos);
    const auto add_pos = result.source.find("->add_child(std::move(child_", disable_pos);
    REQUIRE(add_pos != std::string::npos);
    REQUIRE(count_occurrences(result.source, "->set_hit_testable(false);") == 1);
}

TEST_CASE("generate_pulp_cpp preserves interactive descendants under promoted native widgets",
          "[view][import][cpp-codegen][hit-test]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.style.width = 120.0f;
    ir.root.style.height = 120.0f;
    ir.root.layout.direction = LayoutDirection::column;

    IRNode knob;
    knob.type = "frame";
    knob.name = "Gain Knob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Gain";
    knob.style.width = 80.0f;
    knob.style.height = 80.0f;
    knob.layout.direction = LayoutDirection::column;

    IRNode container;
    container.type = "frame";
    container.name = "Imported Knob Controls";
    container.style.width = 80.0f;
    container.style.height = 40.0f;
    container.layout.direction = LayoutDirection::column;

    IRNode nested_button;
    nested_button.type = "button";
    nested_button.name = "Nested Fine Button";
    nested_button.text_content = "Fine";
    nested_button.style.width = 60.0f;
    nested_button.style.height = 20.0f;

    container.children.push_back(std::move(nested_button));
    knob.children.push_back(std::move(container));
    ir.root.children.push_back(std::move(knob));

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(result.source.find("std::make_unique<pulp::view::Knob>") != std::string::npos);
    REQUIRE(result.source.find("std::make_unique<pulp::view::TextButton>") != std::string::npos);
    REQUIRE(count_occurrences(result.source, "->set_hit_testable(false);") == 0);
    REQUIRE(count_occurrences(result.source, "->set_pointer_events(pulp::view::View::PointerEvents::box_none);") == 1);
}

TEST_CASE("generate_pulp_cpp preserves extracted interactive-descendant wrappers under promoted native widgets",
          "[view][import][cpp-codegen][hit-test]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.style.width = 120.0f;
    ir.root.style.height = 120.0f;
    ir.root.layout.direction = LayoutDirection::column;

    IRNode knob;
    knob.type = "frame";
    knob.name = "Gain Knob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Gain";
    knob.style.width = 80.0f;
    knob.style.height = 80.0f;
    knob.layout.direction = LayoutDirection::column;

    IRNode container;
    container.type = "frame";
    container.name = "InteractiveLayer";
    container.style.width = 80.0f;
    container.style.height = 40.0f;
    container.layout.direction = LayoutDirection::column;

    IRNode nested_button;
    nested_button.type = "button";
    nested_button.name = "Nested Fine Button";
    nested_button.text_content = "Fine";
    nested_button.style.width = 60.0f;
    nested_button.style.height = 20.0f;

    container.children.push_back(std::move(nested_button));
    knob.children.push_back(std::move(container));
    ir.root.children.push_back(std::move(knob));

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    const auto helper_pos = result.source.find("auto child_");
    REQUIRE(helper_pos != std::string::npos);
    const auto pointer_pos = result.source.find(
        "->set_pointer_events(pulp::view::View::PointerEvents::box_none);",
        helper_pos);
    REQUIRE(pointer_pos != std::string::npos);
    const auto add_pos = result.source.find("->add_child(std::move(child_", pointer_pos);
    REQUIRE(add_pos != std::string::npos);
    REQUIRE(count_occurrences(result.source, "->set_hit_testable(false);") == 0);
    REQUIRE(count_occurrences(result.source, "->set_pointer_events(pulp::view::View::PointerEvents::box_none);") == 1);
}

TEST_CASE("parse_v0_tsx preserves simple useState event contracts in baked C++ manifest",
          "[view][import][cpp-codegen]") {
    auto ir = parse_v0_tsx(R"tsx(
        import { useState } from "react";

        export default function ControlStrip() {
            const [gain, setGain] = useState(0.65);
            const [enabled, setEnabled] = useState(true);
            return (
                <section>
                    <button type="button" onClick={() => setEnabled(!enabled)}>
                        {enabled ? "ON" : "OFF"}
                    </button>
                    <input
                        type="checkbox"
                        checked={enabled}
                        onChange={() => setEnabled(!enabled)} />
                    <input
                        type="range"
                        min={0}
                        max={1}
                        step={0.01}
                        value={gain}
                        onChange={(event) => setGain(Number(event.currentTarget.value))} />
                    <meter value={gain} />
                </section>
            );
        }
    )tsx");

    REQUIRE_FALSE(has_import_diagnostic(ir.diagnostics, "fallback-used"));

    const auto* button = find_descendant(ir.root, [](const IRNode& node) {
        return node.type == "button";
    });
    REQUIRE(button != nullptr);
    REQUIRE(button->attributes.at("pulpValueKey") == "enabled");
    REQUIRE(button->attributes.at("pulpInitialValue") == "true");
    REQUIRE(button->attributes.at("pulpEventContract") == "button:onClick:setState");

    const auto* range = find_descendant(ir.root, [](const IRNode& node) {
        auto type = node.attributes.find("type");
        return node.type == "input" && type != node.attributes.end() && type->second == "range";
    });
    REQUIRE(range != nullptr);
    REQUIRE(range->attributes.at("pulpValueKey") == "gain");
    REQUIRE(range->attributes.at("pulpInitialValue") == "0.65");
    REQUIRE(range->attributes.at("pulpEventContract") == "range:onChange:setState");
    REQUIRE(range->attributes.at("pulpGestureContract") == "range:drag");
    REQUIRE(range->style.width == 120.0f);
    REQUIRE(range->style.height == 20.0f);

    const auto* checkbox = find_descendant(ir.root, [](const IRNode& node) {
        auto type = node.attributes.find("type");
        return node.type == "input" && type != node.attributes.end() && type->second == "checkbox";
    });
    REQUIRE(checkbox != nullptr);
    REQUIRE(checkbox->attributes.at("pulpValueKey") == "enabled");
    REQUIRE(checkbox->attributes.at("pulpInitialValue") == "true");
    REQUIRE(checkbox->attributes.at("pulpEventContract") == "checkbox:onChange:setState");
    REQUIRE(checkbox->attributes.at("pulpGestureContract") == "checkbox:toggle");
    REQUIRE(checkbox->style.width == 18.0f);
    REQUIRE(checkbox->style.height == 18.0f);

    const auto* meter = find_descendant(ir.root, [](const IRNode& node) {
        return node.type == "meter";
    });
    REQUIRE(meter != nullptr);
    REQUIRE(meter->attributes.at("pulpMeterValueKey") == "gain");
    REQUIRE(meter->style.width == 12.0f);
    REQUIRE(meter->style.height == 64.0f);

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(result.binding_manifest.find("\"value_key\": \"enabled\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"value_key\": \"gain\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"event_contract\": \"button:onClick:setState\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"event_contract\": \"range:onChange:setState\"") != std::string::npos);
}

TEST_CASE("parse_v0_tsx resolves indexed useState value bindings",
          "[view][import][cpp-codegen]") {
    // Regression: value={params[0]} validated its base identifier but returned
    // the full "params[0]" expression as the lookup key, so the initial value
    // (keyed by the base "params") was silently dropped.
    auto ir = parse_v0_tsx(R"tsx(
        import { useState } from "react";

        export default function Bank() {
            const [params, setParams] = useState([0.5, 0.25]);
            return (
                <section>
                    <input
                        type="range"
                        value={params[0]}
                        onChange={(event) => setParams(Number(event.currentTarget.value))} />
                </section>
            );
        }
    )tsx");

    REQUIRE_FALSE(has_import_diagnostic(ir.diagnostics, "fallback-used"));
    const auto* range = find_descendant(ir.root, [](const IRNode& node) {
        auto type = node.attributes.find("type");
        return node.type == "input" && type != node.attributes.end() && type->second == "range";
    });
    REQUIRE(range != nullptr);
    // The index is preserved in the value key (the binding layer can target the
    // specific element)...
    REQUIRE(range->attributes.at("pulpValueKey") == "params[0]");
    // ...and the initial value is resolved via the base identifier, not dropped.
    REQUIRE(range->attributes.count("pulpInitialValue") == 1);
    REQUIRE(range->attributes.at("pulpDefaultValueSource") == "useState");
}

TEST_CASE("parse_v0_tsx preserves grid template source contracts",
          "[view][import][cpp-codegen]") {
    auto ir = parse_v0_tsx(R"tsx(
        export default function GridPanel() {
            return (
                <section style={{ width: 420 }}>
                    <div style={{ display: "grid", gridTemplateColumns: "70px repeat(3, 1fr)", gap: 6 }}>
                        <span>Label</span>
                        <button type="button">A</button>
                        <button type="button">B</button>
                        <button type="button">C</button>
                    </div>
                </section>
            );
        }
    )tsx");

    const auto* grid = find_descendant(ir.root, [](const IRNode& node) {
        return node.layout.display && *node.layout.display == "grid";
    });
    REQUIRE(grid != nullptr);
    REQUIRE(grid->attributes.at("pulpGridTemplateColumns") == "70px repeat(3, 1fr)");
    REQUIRE(grid->layout.gap == 6.0f);

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(result.source.find("GridStyle::parse_template(\"70px repeat(3, 1fr)\")") != std::string::npos);
    REQUIRE(result.source.find("grid.column_gap = 6.0f;") != std::string::npos);
    REQUIRE(result.source.find("grid.row_gap = 6.0f;") != std::string::npos);
}

TEST_CASE("parse_v0_tsx maps React Native primitives into baked C++ contracts",
          "[view][import][cpp-codegen]") {
    auto ir = parse_v0_tsx(R"tsx(
        import React, { useState } from 'react';
        import { Pressable, Text, View } from 'react-native';

        export default function GainStage() {
            const [armed, setArmed] = useState(true);
            const [gain, setGain] = useState(0.72);
            const increaseGain = () => setGain(Math.min(1, Number((gain + 0.05).toFixed(2))));
            return (
                <View testID="rn-gain-stage">
                    <Pressable accessibilityLabel="Toggle bypass" onPress={() => setArmed(!armed)}>
                        <Text>{armed ? 'ARMED' : 'BYPASS'}</Text>
                    </Pressable>
                    <Pressable accessibilityLabel="Increase gain" onPress={increaseGain}>
                        <Text>+</Text>
                    </Pressable>
                </View>
            );
        }
    )tsx");

    REQUIRE_FALSE(has_import_diagnostic(ir.diagnostics, "fallback-used"));

    const auto* pressable = find_descendant(ir.root, [](const IRNode& node) {
        return node.type == "button" && node.attributes.find("onPress") != node.attributes.end();
    });
    REQUIRE(pressable != nullptr);
    REQUIRE(pressable->attributes.at("jsxTag") == "pressable");
    REQUIRE(pressable->attributes.at("pulpValueKey") == "armed");
    REQUIRE(pressable->attributes.at("pulpInitialValue") == "true");
    REQUIRE(pressable->attributes.at("pulpEventContract") == "button:onClick:setState");
    REQUIRE(pressable->attributes.at("pulpGestureContract") == "button:click");

    const auto* increase = find_descendant(ir.root, [](const IRNode& node) {
        auto label = node.attributes.find("accessibilityLabel");
        return node.type == "button" && label != node.attributes.end() &&
               label->second == "Increase gain";
    });
    REQUIRE(increase != nullptr);
    REQUIRE(increase->attributes.at("pulpValueKey") == "gain");
    REQUIRE(increase->attributes.at("pulpInitialValue") == "0.72");
    REQUIRE(increase->attributes.at("pulpRouteType") == "native_cpp");
    REQUIRE(increase->attributes.at("pulpSourceFamily") == "pressable");
    REQUIRE(increase->attributes.at("pulpEventContract") == "button:onClick:setState");
    REQUIRE(increase->attributes.at("pulpGestureContract") == "button:click");

    const auto* text = find_descendant(ir.root, [](const IRNode& node) {
        return node.type == "text" && node.attributes.find("jsxTag") != node.attributes.end() &&
               node.attributes.at("jsxTag") == "text";
    });
    REQUIRE(text != nullptr);

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE_FALSE(result.source.ends_with("\n\n"));
    REQUIRE(result.binding_manifest.find("\"value_key\": \"armed\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"value_key\": \"gain\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"source_family\": \"pressable\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"event_contract\": \"button:onClick:setState\"") != std::string::npos);
}

TEST_CASE("JSX snapshot dynamic API scanner detects non-deterministic APIs",
          "[view][import][diagnostics]") {
    auto scan = detect_jsx_snapshot_dynamic_apis(
        "setInterval(function(){}, 16); setTimeout(function(){}, 16);"
        "requestAnimationFrame(function(){}); Date.now(); new Date();"
        "performance.now(); Math.random(); fetch('/state');");
    REQUIRE(scan.has_dynamic_apis());
    REQUIRE(scan.tokens.size() == 8);
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "setInterval") != scan.tokens.end());
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "setTimeout") != scan.tokens.end());
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "requestAnimationFrame") != scan.tokens.end());
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "Date.now") != scan.tokens.end());
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "new Date") != scan.tokens.end());
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "performance.now") != scan.tokens.end());
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "Math.random") != scan.tokens.end());
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "fetch") != scan.tokens.end());

    auto interpolation_scan = detect_jsx_snapshot_dynamic_apis(
        R"(const label = `literal setInterval Date.now ${Date.now()} ${Math.random()} ${fetch("/state")}`;)");
    REQUIRE(interpolation_scan.has_dynamic_apis());
    REQUIRE(interpolation_scan.tokens.size() == 3);
    REQUIRE(std::find(interpolation_scan.tokens.begin(), interpolation_scan.tokens.end(), "Date.now") != interpolation_scan.tokens.end());
    REQUIRE(std::find(interpolation_scan.tokens.begin(), interpolation_scan.tokens.end(), "Math.random") != interpolation_scan.tokens.end());
    REQUIRE(std::find(interpolation_scan.tokens.begin(), interpolation_scan.tokens.end(), "fetch") != interpolation_scan.tokens.end());
    REQUIRE(std::find(interpolation_scan.tokens.begin(), interpolation_scan.tokens.end(), "setInterval") == interpolation_scan.tokens.end());

    auto nested_template_scan = detect_jsx_snapshot_dynamic_apis(
        R"(const label = `outer ${`inner ${performance.now()}`} ${new Date()}`;)");
    REQUIRE(nested_template_scan.has_dynamic_apis());
    REQUIRE(nested_template_scan.tokens.size() == 2);
    REQUIRE(std::find(nested_template_scan.tokens.begin(), nested_template_scan.tokens.end(), "performance.now") != nested_template_scan.tokens.end());
    REQUIRE(std::find(nested_template_scan.tokens.begin(), nested_template_scan.tokens.end(), "new Date") != nested_template_scan.tokens.end());

    auto braced_expression_scan = detect_jsx_snapshot_dynamic_apis(
        R"(const label = `${ { value: Date.now() } } ${format("}")}`;)");
    REQUIRE(braced_expression_scan.has_dynamic_apis());
    REQUIRE(braced_expression_scan.tokens.size() == 1);
    REQUIRE(std::find(braced_expression_scan.tokens.begin(), braced_expression_scan.tokens.end(), "Date.now") != braced_expression_scan.tokens.end());

    REQUIRE_FALSE(detect_jsx_snapshot_dynamic_apis(
        "// setInterval Date.now Math.random fetch(\n"
        "/* setTimeout requestAnimationFrame new Date performance.now */\n"
        "const literal = \"setInterval Date.now Math.random fetch(\";\n"
        "const single = 'setTimeout requestAnimationFrame new Date performance.now';\n"
        "const template = `setInterval Date.now Math.random fetch(`;").has_dynamic_apis());
    REQUIRE_FALSE(detect_jsx_snapshot_dynamic_apis(
        R"JS(const label = `${"Date.now()"} ${/* Math.random() */ 1}`;)JS").has_dynamic_apis());
    REQUIRE_FALSE(detect_jsx_snapshot_dynamic_apis(
        "const single = 'escaped \\' Date.now()';\n"
        "const double_quote = \"escaped \\\" Math.random()\";\n"
        "const template = `escaped \\` fetch(\"/state\")`;\n").has_dynamic_apis());
    REQUIRE_FALSE(detect_jsx_snapshot_dynamic_apis("const x = 1;").has_dynamic_apis());
}

#ifndef _WIN32
TEST_CASE("DesignIR asset manifest fetches network assets through cache and verifies hashes",
          "[view][import][assets][network]") {
    TempDir tmp("pulp-design-ir-network-assets");
    const auto bin = tmp.path / "bin";
    const auto curl = bin / "curl";
    fs::create_directories(bin);
    write_text(curl,
               "#!/bin/sh\n"
               "out=''\n"
               "while [ \"$#\" -gt 0 ]; do\n"
               "  case \"$1\" in\n"
               "    --output) shift; out=\"$1\" ;;\n"
               "  esac\n"
               "  shift\n"
               "done\n"
               "[ -n \"$out\" ] || exit 9\n"
               "printf '%s' '<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"1\" height=\"1\"/></svg>' > \"$out\"\n");
    fs::permissions(curl,
                    fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::add);

    const auto url = std::string("https://example.test/icon.svg");
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Network";
    ir.root.style.background_image = "url(" + url + ")";

    auto old_path = read_env_var("PATH").value_or("");
    ScopedEnvVar path_override("PATH", bin.string() + ":" + old_path);

    DesignIrAssetOptions options;
    options.allow_network_fetch = true;
    options.network_timeout_ms = 30000;
    options.cache_directory = tmp.path / "asset-cache";
    options.expected_hash_by_uri[url] = "not-the-actual-hash";

    auto manifest = collect_design_ir_assets(ir, options);
    REQUIRE(manifest.assets.size() == 1);
    const auto& fetched = manifest.assets[0];
    REQUIRE(fetched.original_uri == url);
    REQUIRE(fetched.source_url == url);
    REQUIRE(fetched.mime == "image/svg+xml");
    REQUIRE_FALSE(fetched.local_path);
    REQUIRE_FALSE(fetched.content_hash.empty());
    REQUIRE(has_diagnostic(fetched, "asset-hash-mismatch"));
    REQUIRE_FALSE(fs::exists(options.cache_directory / "by-hash"));
    REQUIRE_FALSE(fs::exists(options.cache_directory / "by-url"));

    const auto actual_hash = fetched.content_hash;
    options.expected_hash_by_uri[url] = actual_hash;
    auto verified = collect_design_ir_assets(ir, options);
    REQUIRE(verified.assets.size() == 1);
    REQUIRE(verified.assets[0].content_hash == actual_hash);
    REQUIRE(verified.assets[0].diagnostics.empty());
    REQUIRE(verified.assets[0].local_path);
    REQUIRE(fs::exists(*verified.assets[0].local_path));

    fs::remove(curl);
    options.expected_hash_by_uri.clear();
    auto cached = collect_design_ir_assets(ir, options);
    REQUIRE(cached.assets.size() == 1);
    REQUIRE(cached.assets[0].content_hash == actual_hash);
    REQUIRE(cached.assets[0].diagnostics.empty());

    options.expected_hash_by_uri[url] = "definitely-not-the-cached-hash";
    auto cached_mismatch = collect_design_ir_assets(ir, options);
    REQUIRE(cached_mismatch.assets.size() == 1);
    REQUIRE(cached_mismatch.assets[0].content_hash == actual_hash);
    REQUIRE(has_diagnostic(cached_mismatch.assets[0], "asset-hash-mismatch"));
}

TEST_CASE("DesignIR asset manifest reports network fetch failures and timeouts",
          "[view][import][assets][network]") {
    TempDir tmp("pulp-design-ir-network-diagnostics");
    const auto bin = tmp.path / "bin";
    const auto curl = bin / "curl";
    fs::create_directories(bin);
    write_text(curl,
               "#!/bin/sh\n"
               "url=''\n"
               "while [ \"$#\" -gt 0 ]; do\n"
               "  case \"$1\" in\n"
               "    http://*|https://*) url=\"$1\" ;;\n"
               "  esac\n"
               "  shift\n"
               "done\n"
               "case \"$url\" in\n"
               "  *slow*) sleep 5 ;;\n"
               "  *) printf 'fetch failed for %s\\n' \"$url\" >&2; exit 28 ;;\n"
               "esac\n");
    fs::permissions(curl,
                    fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::add);

    auto old_path = read_env_var("PATH").value_or("");
    ScopedEnvVar path_override("PATH", bin.string() + ":" + old_path);

    DesignIrAssetOptions options;
    options.allow_network_fetch = true;
    options.cache_directory = tmp.path / "asset-cache";
    options.network_timeout_ms = 30000;

    DesignIR failed_ir;
    failed_ir.root.type = "frame";
    failed_ir.root.name = "FetchFail";
    failed_ir.root.style.background_image = "url(https://example.test/fail.svg)";
    auto failed = collect_design_ir_assets(failed_ir, options);
    REQUIRE(failed.assets.size() == 1);
    REQUIRE(has_diagnostic(failed.assets[0], "asset-fetch-failed"));

    options.network_timeout_ms = 100;
    DesignIR timeout_ir;
    timeout_ir.root.type = "frame";
    timeout_ir.root.name = "FetchTimeout";
    timeout_ir.root.style.background_image = "url(https://example.test/slow.svg)";
    auto timed_out = collect_design_ir_assets(timeout_ir, options);
    REQUIRE(timed_out.assets.size() == 1);
    REQUIRE(has_diagnostic(timed_out.assets[0], "asset-fetch-timeout"));
}

TEST_CASE("DesignIR asset manifest reports missing fetcher and empty network downloads",
          "[view][import][assets][network]") {
    TempDir tmp("pulp-design-ir-network-edge-diagnostics");
    const auto bin = tmp.path / "bin";
    fs::create_directories(bin);

    auto old_path = read_env_var("PATH").value_or("");
    ScopedEnvVar path_override("PATH", bin.string());

    DesignIrAssetOptions options;
    options.allow_network_fetch = true;
    options.cache_directory = tmp.path / "asset-cache";
    options.network_timeout_ms = 10000;

    DesignIR missing_fetcher_ir;
    missing_fetcher_ir.root.type = "frame";
    missing_fetcher_ir.root.name = "MissingFetcher";
    missing_fetcher_ir.root.style.background_image =
        "url(https://example.test/missing-fetcher.svg)";
    auto missing_fetcher = collect_design_ir_assets(missing_fetcher_ir, options);
    REQUIRE(missing_fetcher.assets.size() == 1);
    REQUIRE(has_diagnostic(missing_fetcher.assets[0], "asset-fetcher-missing"));

    const auto curl = bin / "curl";
    write_text(curl,
               "#!/bin/sh\n"
               "out=''\n"
               "while [ \"$#\" -gt 0 ]; do\n"
               "  case \"$1\" in\n"
               "    --output) shift; out=\"$1\" ;;\n"
               "  esac\n"
               "  shift\n"
               "done\n"
               "[ -n \"$out\" ] || exit 9\n"
               ": > \"$out\"\n");
    fs::permissions(curl,
                    fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::add);

    DesignIR empty_download_ir;
    empty_download_ir.root.type = "frame";
    empty_download_ir.root.name = "EmptyDownload";
    empty_download_ir.root.style.background_image =
        "url(https://example.test/empty.svg)";
    auto empty_download = collect_design_ir_assets(empty_download_ir, options);
    REQUIRE(empty_download.assets.size() == 1);
    REQUIRE(has_diagnostic(empty_download.assets[0], "asset-empty"));

    set_env_var("PATH", old_path);
}
#endif

// pulp #709 / #468 — Claude Design imports are manually-exported HTML
// parsed via the Stitch HTML pipeline and re-tagged as Claude.
TEST_CASE("parse_claude_html delegates to Stitch pipeline and tags source",
          "[view][import][issue-709][issue-468]") {
    const auto html = std::string{
        R"(<!DOCTYPE html><html><body>
              <div class="container">
                <h1>Hello Claude</h1>
                <button>Click me</button>
              </div>
           </body></html>)"};

    const auto ir = parse_claude_html(html);
    REQUIRE(ir.source == DesignSource::claude);
    // Same HTML fed directly to parse_stitch_html should produce a
    // tree of the same shape; delegation is the contract, not a new
    // parser implementation.
    const auto stitch_ir = parse_stitch_html(html);
    REQUIRE(ir.root.children.size() == stitch_ir.root.children.size());
}

// pulp #709 — render_claude_bridge_scaffold is the library form of the
// CLI's `pulp import-design --from claude` scaffold output. Lives in
// the library (not in the CLI source) so coverage can be asserted
// without needing the spawned-subprocess instrumentation that codecov
// can't see through.
TEST_CASE("render_claude_bridge_scaffold emits a buildable EditorBridge starter",
          "[view][import][issue-709][issue-468]") {
    const auto scaffold = render_claude_bridge_scaffold("ui.js");

    // Threads the path through the file header so users can trace the
    // generated handlers back to the imported view.
    REQUIRE(scaffold.find("ui.js") != std::string::npos);

    // The framework surface MUST be referenced by full name — that's
    // the whole point of the scaffold (#709 acceptance criterion).
    REQUIRE(scaffold.find("pulp::view::EditorBridge") != std::string::npos);
    REQUIRE(scaffold.find("#include <pulp/view/editor_bridge.hpp>") != std::string::npos);
    REQUIRE(scaffold.find("#include <pulp/view/web_view.hpp>") != std::string::npos);

    // Demonstrates each of the patterns plugin authors will copy:
    //   - one no-payload handler
    //   - one typed-payload handler using EditorBridge::get_float
    //   - the WebView attach call
    //   - the comment pointer to attach_native_runtime for #468
    REQUIRE(scaffold.find(R"(add_handler("hello")") != std::string::npos);
    REQUIRE(scaffold.find(R"(add_handler("set_value")") != std::string::npos);
    REQUIRE(scaffold.find("EditorBridge::get_float") != std::string::npos);
    REQUIRE(scaffold.find("EditorBridge::ok_response()") != std::string::npos);
    REQUIRE(scaffold.find("attach_webview(panel)") != std::string::npos);
    REQUIRE(scaffold.find("attach_native_runtime") != std::string::npos);
    REQUIRE(scaffold.find("MyPluginEditor") != std::string::npos);

    // Path is interpolated, not hard-coded — feed a different path and
    // confirm the new value lands in the header.
    const auto other = render_claude_bridge_scaffold("editor/imported.js");
    REQUIRE(other.find("editor/imported.js") != std::string::npos);
    REQUIRE(other.find("ui.js") == std::string::npos);
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

TEST_CASE("detect_audio_widget matches whole words not substrings", "[view][import]") {
    // Regression: substring matching promoted any name *containing*
    // a keyword. These embed a keyword as a substring but are NOT audio widgets —
    // word-boundary tokenization must return none.
    REQUIRE(detect_audio_widget("Dialog") == AudioWidgetType::none);      // "dial"og
    REQUIRE(detect_audio_widget("Radial") == AudioWidgetType::none);      // ra"dial"
    REQUIRE(detect_audio_widget("Parameter") == AudioWidgetType::none);   // para"meter"
    REQUIRE(detect_audio_widget("Diameter") == AudioWidgetType::none);    // dia"meter"
    REQUIRE(detect_audio_widget("sublevel") == AudioWidgetType::none);    // sub"level"
    REQUIRE(detect_audio_widget("Knobby") == AudioWidgetType::none);      // "knob"by

    // True positives still recognized: separators, acronym camelCase, plurals.
    REQUIRE(detect_audio_widget("VUMeter") == AudioWidgetType::meter);    // acronym→Word: {vu,meter}
    REQUIRE(detect_audio_widget("xy pad") == AudioWidgetType::xy_pad);    // space-separated
    REQUIRE(detect_audio_widget("XYPad") == AudioWidgetType::xy_pad);     // {xy,pad}
    REQUIRE(detect_audio_widget("Knob 01") == AudioWidgetType::knob);     // letter↔digit split keeps "knob"
    REQUIRE(detect_audio_widget("main-fader") == AudioWidgetType::fader);
    REQUIRE(detect_audio_widget("Knobs") == AudioWidgetType::knob);       // simple plural still matches
    REQUIRE(detect_audio_widget("Faders") == AudioWidgetType::fader);
}

// ── Figma JSON parsing ──────────────────────────────────────────────────

TEST_CASE("parse_figma_json parses IR format", "[view][import]") {
    auto json = R"json({
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
    })json";

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

TEST_CASE("parse_figma_json covers layout style and audio shape metadata edges",
          "[view][import]") {
    auto json = R"json({
        "type": "frame",
        "name": "Rack",
        "_layoutHeight": 144,
        "_layoutWidth": 480,
        "layout": {
            "direction": "row",
            "gap": 6,
            "wrap": true,
            "paddingTop": 2,
            "paddingRight": 4,
            "paddingBottom": 6,
            "paddingLeft": 8,
            "justify": "space-around",
            "align": "center",
            "width_mode": "fill",
            "height_mode": "hug"
        },
        "style": {
            "backgroundGradient": "linear-gradient(#111,#222)",
            "opacity": 0.75,
            "border": "1px solid #333333",
            "boxShadow": "0 4px 12px #00000040",
            "filter": "blur(2px)",
            "fontFamily": "Inter",
            "fontStyle": "italic",
            "textAlign": "center",
            "letterSpacing": 1.5,
            "lineHeight": 1.2,
            "textTransform": "uppercase",
            "overflow": "hidden",
            "cursor": "pointer",
            "position": "absolute",
            "top": 1,
            "left": 2,
            "right": 3,
            "bottom": 4,
            "zIndex": 9,
            "transform": "rotate(2deg)",
            "minWidth": 100,
            "minHeight": 40,
            "maxWidth": 640,
            "maxHeight": 320
        },
        "children": [
            {
                "type": "text",
                "name": "title",
                "content": "Drive",
                "fill": "#f5e0dc",
                "fontSize": 13,
                "fontWeight": "bold",
                "fontFamily": "Inter Tight"
            },
            {
                "type": "frame",
                "name": "DriveKnob",
                "width": 92,
                "height": 110,
                "children": [
                    {
                        "type": "ellipse",
                        "name": "ring",
                        "width": 64,
                        "height": 64,
                        "stroke": { "fill": "#cba6f7" }
                    },
                    { "type": "text", "name": "caption", "content": "Drive" },
                    { "type": "text", "name": "value", "content": "72%" }
                ]
            },
            {
                "type": "frame",
                "name": "NestedKnobContainer",
                "children": [
                    { "type": "frame", "name": "InnerKnob", "children": [] }
                ]
            }
        ],
        "tokens": {
            "strings": { "copy.title": "Drive" }
        }
    })json";

    auto ir = parse_figma_json(json);

    REQUIRE(ir.root.attributes.at("_layoutHeight") == "144");
    REQUIRE(ir.root.attributes.at("_layoutWidth") == "480");
    REQUIRE(ir.root.layout.direction == LayoutDirection::row);
    REQUIRE(ir.root.layout.wrap);
    REQUIRE(ir.root.layout.justify == LayoutAlign::space_around);
    REQUIRE(ir.root.layout.align == LayoutAlign::center);
    REQUIRE(ir.root.layout.width_mode == SizingMode::fill);
    REQUIRE(ir.root.layout.height_mode == SizingMode::hug);
    REQUIRE(ir.root.layout.padding_left == 8.0f);
    REQUIRE(ir.root.style.background_gradient == "linear-gradient(#111,#222)");
    REQUIRE(ir.root.style.opacity == 0.75f);
    REQUIRE(box_shadow_to_css(ir.root.style.box_shadow) == "0 4px 12px #00000040");
    REQUIRE(ir.root.style.z_index == 9);
    REQUIRE(ir.tokens.strings["copy.title"] == "Drive");

    const auto& title = ir.root.children[0];
    REQUIRE(title.style.color == "#f5e0dc");
    REQUIRE(title.style.font_size == 13.0f);
    REQUIRE(title.style.font_weight == 700);
    REQUIRE(title.style.font_family == "Inter Tight");

    const auto& knob = ir.root.children[1];
    REQUIRE(knob.audio_widget == AudioWidgetType::knob);
    REQUIRE(knob.attributes.at("shape_width") == "64");
    REQUIRE(knob.attributes.at("shape_height") == "64");
    REQUIRE(knob.children[0].attributes.at("stroke_color") == "#cba6f7");

    const auto& container = ir.root.children[2];
    REQUIRE(container.audio_widget == AudioWidgetType::none);
    REQUIRE(container.layout.direction == LayoutDirection::row);
}

// ── Code generation ─────────────────────────────────────────────────────

TEST_CASE("generate_pulp_js emits motion.setProvenance per vendor + root",
          "[view][import][motion][provenance]") {
    // Figma export with a recognizable root node name should emit
    // `motion.setProvenance('design-import', 'figma:<root-name>')` so
    // any animations the bundle drives self-attribute through the
    // motion observability publish channel.
    {
        DesignIR ir;
        ir.source = DesignSource::figma;
        ir.root.type = "frame";
        ir.root.name = "Card/Hover";
        CodeGenOptions opts;
        opts.mode = CodeGenMode::web_compat;
        opts.include_comments = false;
        auto js = generate_pulp_js(ir, opts);
        REQUIRE(js.find("motion.setProvenance('design-import', 'figma:Card/Hover')")
                != std::string::npos);
    }
    // Stitch, v0, Pencil, Claude — same shape, different vendor key.
    struct Case { DesignSource src; const char* vendor; };
    for (const Case& c : {
             Case{DesignSource::stitch, "stitch"},
             Case{DesignSource::v0,     "v0"},
             Case{DesignSource::pencil, "pencil"},
             Case{DesignSource::claude, "claude"},
         }) {
        DesignIR ir;
        ir.source = c.src;
        ir.root.type = "frame";
        ir.root.name = "Panel";
        CodeGenOptions opts;
        opts.include_comments = false;
        auto js = generate_pulp_js(ir, opts);
        std::string expected =
            std::string("motion.setProvenance('design-import', '") +
            c.vendor + ":Panel')";
        REQUIRE(js.find(expected) != std::string::npos);
    }
    // When the root has no name but there's a source_file, the file's
    // basename (without extension) is the id.
    {
        DesignIR ir;
        ir.source = DesignSource::figma;
        ir.root.type = "frame";
        ir.source_file = "/path/to/HeaderLayout.json";
        CodeGenOptions opts;
        opts.include_comments = false;
        auto js = generate_pulp_js(ir, opts);
        REQUIRE(js.find("motion.setProvenance('design-import', 'figma:HeaderLayout')")
                != std::string::npos);
    }
}

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

TEST_CASE("generate_pulp_js reconstructs the value/range/unit/binding stack from metadata",
          "[view][import][issue-3192]") {
    // The figma-plugin export carries each widget's value / range / unit /
    // binding as NODE METADATA (audio_min/max/default + attributes
    // units/binding), NOT as child text nodes. The native codegen must
    // reconstruct the Pulp Library display stack from that metadata: the widget
    // label, the formatted value, then a small grey sub-stack (min / max / units
    // / binding). This is generalizable from metadata — no per-instance
    // hardcoding.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.layout.direction = LayoutDirection::row;

    IRNode knob;
    knob.type = "frame";
    knob.name = "Knob — Cutoff";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Cutoff";
    knob.audio_min = 20.0f;
    knob.audio_max = 20000.0f;
    knob.audio_default = 880.0f;
    knob.has_audio_range = true;
    knob.attributes["units"] = "Hz";
    knob.attributes["binding"] = "filter.cutoff_hz";
    ir.root.children.push_back(knob);

    CodeGenOptions opts;
    opts.include_comments = false;
    auto js = generate_pulp_js(ir, opts);

    // Widget label.
    REQUIRE(js.find("'Cutoff'") != std::string::npos);
    // Formatted value: 880 is whole → no decimals.
    REQUIRE(js.find("'880'") != std::string::npos);
    // Grey sub-stack: min / max / units / binding, each its own label.
    REQUIRE(js.find("'20'") != std::string::npos);
    REQUIRE(js.find("'20000'") != std::string::npos);
    REQUIRE(js.find("'Hz'") != std::string::npos);
    REQUIRE(js.find("'filter.cutoff_hz'") != std::string::npos);
    // Sub-stack uses the small grey color.
    REQUIRE(js.find("'#6c7086'") != std::string::npos);
}

TEST_CASE("generate_pulp_js formats fractional audio values with one decimal",
          "[view][import][issue-3192]") {
    // The value formatter is generalizable: a whole value prints with no
    // decimals (matching the reference's 880 / 0 / -60), a fractional value
    // prints with one decimal (e.g. -6.5). NB: the reference shows the meter's
    // whole -6 as "-6.0" because the Pulp Library meter component uses a fixed
    // 1-decimal level format; that per-component decimal convention is not
    // recoverable from the value alone (it's whole), so the generalizable rule
    // formats whole -6 as "-6" — the exact "-6.0" trailing zero is a deferred
    // cosmetic, not a fidelity-gated metric.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";

    IRNode meter;
    meter.type = "frame";
    meter.name = "Meter — Out L";
    meter.audio_widget = AudioWidgetType::meter;
    meter.audio_label = "Out L";
    meter.audio_min = -60.0f;
    meter.audio_max = 0.0f;
    meter.audio_default = -6.5f;   // genuinely fractional → 1 decimal
    meter.has_audio_range = true;
    meter.attributes["units"] = "dB";
    meter.attributes["binding"] = "meter.out_l";
    ir.root.children.push_back(meter);

    CodeGenOptions opts;
    opts.include_comments = false;
    auto js = generate_pulp_js(ir, opts);

    // Fractional value formatted with one decimal: -6.5.
    REQUIRE(js.find("'-6.5'") != std::string::npos);
    // Whole-number range bounds print without decimals.
    REQUIRE(js.find("'-60'") != std::string::npos);
    REQUIRE(js.find("'0'") != std::string::npos);
    REQUIRE(js.find("'meter.out_l'") != std::string::npos);
}

TEST_CASE("generate_pulp_js escapes text containing newlines / quotes / backslashes (pulp #81)",
          "[view][import][issue-81]") {
    // pulp #81: a Claude Design HTML file with multi-line <style>/<script>
    // blocks (Spectr's editor.html is the canonical reproducer) used to
    // emit raw newlines into the generated `createLabel('id', 'text', ...)`
    // call, which made the resulting JS unparseable ("unexpected end of
    // string" in pulp-screenshot). Same problem for any text containing
    // `'`, `\`, `\r`, `\t`. The fix routes all user-text emissions through
    // js_single_quote_escape(). This test pins that behavior — every text
    // surface that previously emitted raw user text must now escape the
    // standard JS string-literal control characters.
    DesignIR ir;
    ir.source = DesignSource::claude;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.layout.direction = LayoutDirection::column;

    IRNode multiline;
    multiline.type = "text";
    multiline.name = "style_block";
    multiline.text_content = "line1\nline2\twith\ttabs\nthird's quote\nbackslash\\here";
    ir.root.children.push_back(multiline);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    opts.include_comments = false;
    auto js = generate_pulp_js(ir, opts);

    // No raw newline character should sit between two single quotes — that
    // would un-terminate the JS string.
    REQUIRE(js.find("'line1\nline2") == std::string::npos);
    REQUIRE(js.find("third's quote") == std::string::npos);
    REQUIRE(js.find("backslash\\here") == std::string::npos);

    // Positive: every control character should appear in its escaped form
    // somewhere in the generated JS.
    REQUIRE(js.find("\\n") != std::string::npos);
    REQUIRE(js.find("\\t") != std::string::npos);
    REQUIRE(js.find("\\'") != std::string::npos);
    REQUIRE(js.find("\\\\") != std::string::npos);

    // Every emitted createLabel line should have an even number of
    // unescaped single quotes — uneven means a literal was un-terminated.
    std::istringstream stream(js);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("createLabel") == std::string::npos) continue;
        std::size_t single_quotes = 0;
        bool escaped = false;
        for (char c : line) {
            if (escaped) { escaped = false; continue; }
            if (c == '\\') { escaped = true; continue; }
            if (c == '\'') ++single_quotes;
        }
        INFO("createLabel line had odd single-quote count: " << line);
        REQUIRE(single_quotes % 2 == 0);
    }
}

TEST_CASE("generate_pulp_js bridge_native_js mode produces Pulp API", "[view][import]") {
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
    opts.mode = CodeGenMode::bridge_native_js;
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

TEST_CASE("generate_pulp_js sprite knob emits an interactive single-frame strip + core-fit",
          "[view][import][sprite]") {
    // Interactive sprite knobs (task #22): in sprite mode a recognized knob
    // carrying captured body art stays a native Knob skinned with a
    // single-frame strip (createKnob + setKnobSpriteStrip), and the recovered
    // opaque core is passed through (setKnobSpriteCore) so the engine fits the
    // disc to the box and sweeps the native indicator within it — it is NOT
    // demoted to a bare image, and silver style is NOT applied.
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
    knob.style.width = 64.0f;
    knob.style.height = 64.0f;
    // Post-hoist + asset-resolution state stamped by the CLI importer.
    knob.attributes["asset_path"] = "/tmp/synthetic-knob-body.png";
    knob.attributes["png_natural_w"] = "128";
    knob.attributes["png_natural_h"] = "192";
    knob.attributes["art_core_x"] = "14";
    knob.attributes["art_core_y"] = "10";
    knob.attributes["art_core_w"] = "100";
    knob.attributes["art_core_h"] = "100";
    knob.attributes["sprite_strip_frame_count"] = "1";
    ir.root.children.push_back(knob);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    opts.use_silver_knobs = false;  // --knob-style sprite
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("createKnob('GainKnob") != std::string::npos);
    REQUIRE(js.find("setKnobSpriteStrip('GainKnob") != std::string::npos);
    REQUIRE(js.find(", 1, 'vertical')") != std::string::npos);
    REQUIRE(js.find("setKnobSpriteCore('GainKnob") != std::string::npos);
    REQUIRE(js.find(", 100, 100)") != std::string::npos);   // core w, h
    REQUIRE(js.find("setWidgetStyle('GainKnob") == std::string::npos);
}

TEST_CASE("generate_pulp_js silver knob still wins over a sprite skin", "[view][import][sprite]") {
    // The default (silver) mode keeps the native-vector body even when body
    // art is present: silver style applied, no sprite strip emitted.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Controls";

    IRNode knob;
    knob.type = "knob";
    knob.name = "GainKnob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.style.width = 64.0f;
    knob.style.height = 64.0f;
    knob.attributes["asset_path"] = "/tmp/synthetic-knob-body.png";
    knob.attributes["art_core_w"] = "100";
    knob.attributes["art_core_h"] = "100";
    ir.root.children.push_back(knob);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    opts.use_silver_knobs = true;  // default
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("createKnob('GainKnob") != std::string::npos);
    REQUIRE(js.find("setWidgetStyle('GainKnob") != std::string::npos);
    REQUIRE(js.find("setKnobSpriteStrip('GainKnob") == std::string::npos);
    REQUIRE(js.find("setKnobSpriteCore('GainKnob") == std::string::npos);
}

TEST_CASE("generate_pulp_js bridge_native_js mode handles audio widgets with Yoga constraints", "[view][import]") {
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
    opts.mode = CodeGenMode::bridge_native_js;
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

// ── Figma-plugin fidelity fixes (pulp #3192) ──────────────────────────────────

TEST_CASE("parse_design_ir_json parses nested padding object", "[view][import][issue-3192]") {
    // The figma-plugin export ships container padding as a nested
    // {top,right,bottom,left} object. The parser previously only understood a
    // uniform float or camelCase per-side keys, so the nested form was dropped
    // (parsed as 0) and content hugged the panel edge.
    const auto json = std::string{R"json({
        "type": "frame",
        "name": "Panel",
        "layout": { "padding": { "top": 24, "right": 32, "bottom": 24, "left": 32 } }
    })json"};
    const auto parsed = parse_design_ir_json(json);
    REQUIRE(parsed.root.layout.padding_top == Catch::Approx(24.0f));
    REQUIRE(parsed.root.layout.padding_right == Catch::Approx(32.0f));
    REQUIRE(parsed.root.layout.padding_bottom == Catch::Approx(24.0f));
    REQUIRE(parsed.root.layout.padding_left == Catch::Approx(32.0f));
}

TEST_CASE("parse_design_ir_json keeps uniform-float and per-side padding forms",
          "[view][import][issue-3192]") {
    // Back-compat: the legacy float and camelCase per-side forms must keep
    // working alongside the new nested-object form.
    const auto uniform = parse_design_ir_json(
        R"json({ "type": "frame", "layout": { "padding": 10 } })json");
    REQUIRE(uniform.root.layout.padding_top == Catch::Approx(10.0f));
    REQUIRE(uniform.root.layout.padding_left == Catch::Approx(10.0f));

    const auto per_side = parse_design_ir_json(
        R"json({ "type": "frame", "layout": { "paddingTop": 5, "paddingLeft": 7 } })json");
    REQUIRE(per_side.root.layout.padding_top == Catch::Approx(5.0f));
    REQUIRE(per_side.root.layout.padding_left == Catch::Approx(7.0f));
    REQUIRE(per_side.root.layout.padding_right == Catch::Approx(0.0f));
}

TEST_CASE("native codegen emits container padding (issue-3192)", "[view][import][issue-3192]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 300.0f;
    ir.root.layout.padding_top = 24.0f;
    ir.root.layout.padding_right = 32.0f;
    ir.root.layout.padding_bottom = 24.0f;
    ir.root.layout.padding_left = 32.0f;

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);

    // Non-uniform padding → per-side setFlex calls with the exact values.
    REQUIRE(js.find("'padding_top', 24") != std::string::npos);
    REQUIRE(js.find("'padding_right', 32") != std::string::npos);
    REQUIRE(js.find("'padding_bottom', 24") != std::string::npos);
    REQUIRE(js.find("'padding_left', 32") != std::string::npos);
}

TEST_CASE("native codegen wraps multi-line text at its design width (issue-3192)",
          "[view][import][issue-3192]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 800.0f;

    // A subtitle paragraph: design box is wider AND taller than one line, so it
    // should wrap inside its width instead of overflowing.
    IRNode subtitle;
    subtitle.type = "text";
    subtitle.name = "Subtitle";
    subtitle.text_content = "A long subtitle that should soft-wrap inside its box";
    subtitle.style.width = 720.0f;
    subtitle.style.height = 26.0f;     // two lines at 11px
    subtitle.style.font_size = 11.0f;
    ir.root.children.push_back(subtitle);

    // A title: same font but a one-line-high box → must stay single line
    // (no forced wrap box) so it doesn't wrap when Pulp's metrics run wide.
    IRNode title;
    title.type = "text";
    title.name = "Title";
    title.text_content = "Title that fits one line";
    title.style.width = 284.0f;
    title.style.height = 22.0f;        // one line at 18px
    title.style.font_size = 18.0f;
    title.style.font_weight = 600;
    ir.root.children.push_back(title);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);

    // Subtitle: bounded width + multi-line so it wraps.
    REQUIRE(js.find("setFlex('Subtitle") != std::string::npos);
    REQUIRE(js.find("'width', 720") != std::string::npos);
    REQUIRE(js.find("setMultiLine('Subtitle") != std::string::npos);

    // Title: NO hard width / multi-line box (stays single line). It still gets a
    // min_width, but must not be forced into a wrap box.
    REQUIRE(js.find("setMultiLine('Title") == std::string::npos);
    REQUIRE(js.find("'width', 284") == std::string::npos);
}

TEST_CASE("native codegen emits font weight and family for text (issue-3192)",
          "[view][import][issue-3192]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";

    IRNode title;
    title.type = "text";
    title.name = "Title";
    title.text_content = "Bold Inter Title";
    title.style.font_size = 18.0f;
    title.style.font_weight = 600;
    title.style.font_family = "Inter";
    ir.root.children.push_back(title);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("setFontWeight('Title") != std::string::npos);
    REQUIRE(js.find("'600'") != std::string::npos);
    REQUIRE(js.find("setFontFamily('Title") != std::string::npos);
    REQUIRE(js.find("'Inter'") != std::string::npos);
}

TEST_CASE("native codegen log-tapers a Hz-unit knob's initial value (issue-3192)",
          "[view][import][issue-3192]") {
    // A frequency knob's value→angle map is logarithmic. The native silver knob
    // maps a 0..1 value linearly to its sweep, so the imported value must be the
    // LOG-normalized position — 880 Hz in [20, 20000] lands near center (~0.55),
    // not at the far end (a raw 880 would clamp to 1.0) and not at the linear
    // position (~0.04, indicator pointing the wrong way).
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";

    IRNode knob;
    knob.type = "knob";
    knob.name = "Cutoff";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Cutoff";
    knob.audio_min = 20.0f;
    knob.audio_max = 20000.0f;
    knob.audio_default = 880.0f;
    knob.attributes["units"] = "Hz";
    ir.root.children.push_back(knob);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);

    // Compute the expected log-normalized value and assert the emitted setValue
    // matches it (and is nowhere near the raw 880 or the linear ~0.04).
    const float expected =
        (std::log(880.0f) - std::log(20.0f)) / (std::log(20000.0f) - std::log(20.0f));
    REQUIRE(expected == Catch::Approx(0.5478f).margin(0.01f));
    // The emitted value should be the log position, not the raw value. Only one
    // knob exists, so a "setValue('Cutoff" prefix uniquely identifies it.
    REQUIRE(js.find("setValue('Cutoff") != std::string::npos);
    REQUIRE(js.find("', 880") == std::string::npos);
    REQUIRE(js.find("', 0.54") != std::string::npos);
}

TEST_CASE("native codegen uses linear taper for non-frequency knobs (issue-3192)",
          "[view][import][issue-3192]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";

    IRNode knob;
    knob.type = "knob";
    knob.name = "Drive";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Drive";
    knob.audio_min = 0.0f;
    knob.audio_max = 10.0f;
    knob.audio_default = 5.0f;        // linear midpoint → 0.5
    knob.attributes["units"] = "dB";
    ir.root.children.push_back(knob);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("setValue('Drive") != std::string::npos);
    REQUIRE(js.find("', 0.5)") != std::string::npos);
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

TEST_CASE("generate_pulp_js bridge_native_js mode covers layout and audio widget edge branches",
          "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::pencil;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.layout.direction = LayoutDirection::row;
    ir.root.layout.justify = LayoutAlign::space_between;
    ir.root.layout.align = LayoutAlign::center;
    ir.root.layout.gap = 10.0f;
    ir.root.layout.padding_top = 2.0f;
    ir.root.layout.padding_right = 4.0f;
    ir.root.layout.padding_bottom = 6.0f;
    ir.root.layout.padding_left = 8.0f;
    ir.root.attributes["_layoutHeight"] = "180";
    ir.root.attributes["_layoutWidth"] = "420";
    ir.root.style.background_color = "#111111";
    ir.root.style.border_radius = 6.0f;

    IRNode left;
    left.type = "text";
    left.name = "left label";
    left.text_content = "Left";
    left.style.font_size = 12.0f;
    left.style.color = "#ffffff";
    ir.root.children.push_back(left);

    IRNode right;
    right.type = "text";
    right.name = "right.label";
    right.text_content = "Right";
    right.style.font_weight = 600;
    ir.root.children.push_back(right);

    IRNode knob;
    knob.type = "frame";
    knob.name = "ToneKnob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Tone";
    knob.audio_default = 0.33f;
    knob.style.width = 90.0f;
    knob.attributes["shape_width"] = "72";
    knob.attributes["shape_height"] = "72";
    IRNode ring;
    ring.type = "ellipse";
    ring.attributes["stroke_color"] = "#fab387";
    knob.children.push_back(ring);
    IRNode value;
    value.type = "text";
    value.text_content = "-6 dB";
    knob.children.push_back(value);
    ir.root.children.push_back(knob);

    IRNode xy;
    xy.type = "frame";
    xy.name = "FilterXYPad";
    xy.audio_widget = AudioWidgetType::xy_pad;
    xy.style.width = 72.0f;
    ir.root.children.push_back(xy);

    IRNode waveform;
    waveform.type = "frame";
    waveform.name = "MainWaveform";
    waveform.audio_widget = AudioWidgetType::waveform;
    waveform.style.width = 180.0f;
    waveform.style.height = 44.0f;
    ir.root.children.push_back(waveform);

    IRNode spectrum;
    spectrum.type = "frame";
    spectrum.name = "SpectrumAnalyzer";
    spectrum.audio_widget = AudioWidgetType::spectrum;
    spectrum.style.width = 160.0f;
    spectrum.style.height = 48.0f;
    ir.root.children.push_back(spectrum);

    IRNode spacer;
    spacer.type = "rectangle";
    spacer.name = "divider";
    spacer.style.height = 2.0f;
    spacer.style.background_color = "#333333";
    ir.root.children.push_back(spacer);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    opts.include_comments = false;
    opts.preview_mode = true;
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("createRow('root', '')") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'height', 180)") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'width', 420)") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'padding_top', 2)") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'padding_left', 8)") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'justify_content', 'space-between')") != std::string::npos);
    REQUIRE(js.find("setTextAlign('right_label1', 'right')") != std::string::npos);
    REQUIRE(js.find("setCornerRadius('root', 'All', 6)") != std::string::npos);
    REQUIRE(js.find("setWidgetStyle('ToneKnob2', 'minimal')") != std::string::npos);
    REQUIRE(js.find("setBorder('ToneKnob2', '#fab387', 2.5, 36)") != std::string::npos);
    REQUIRE(js.find("createLabel('ToneKnob2_val', '-6 dB'") != std::string::npos);
    REQUIRE(js.find("createXYPad('FilterXYPad3'") != std::string::npos);
    REQUIRE(js.find("createWaveform('MainWaveform4'") != std::string::npos);
    REQUIRE(js.find("createSpectrum('SpectrumAnalyzer5'") != std::string::npos);
    REQUIRE(js.find("createRow('divider6'") != std::string::npos);
    REQUIRE(js.find("setBackground('divider6', '#333333')") != std::string::npos);
}

TEST_CASE("generate_pulp_js web compat emits extended style and layout properties",
          "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::v0;
    ir.root.type = "frame";
    ir.root.name = "Root Panel";
    ir.root.layout.direction = LayoutDirection::row;
    ir.root.layout.gap = 2.5f;
    ir.root.layout.padding_top = 1.0f;
    ir.root.layout.padding_right = 2.0f;
    ir.root.layout.padding_bottom = 3.0f;
    ir.root.layout.padding_left = 4.0f;
    ir.root.layout.justify = LayoutAlign::center;
    ir.root.layout.align = LayoutAlign::center;
    ir.root.layout.wrap = true;
    ir.root.layout.width_mode = SizingMode::fill;
    ir.root.layout.height_mode = SizingMode::fill;
    ir.root.style.background_color = "#101010";
    ir.root.style.background_gradient = "linear-gradient(#101010,#202020)";
    ir.root.style.color = "#eeeeee";
    ir.root.style.opacity = 0.5f;
    ir.root.style.border_radius = 3.5f;
    ir.root.style.border = "1px solid #444";
    ir.root.style.box_shadow = parse_css_box_shadow("0 1px 2px #000");
    ir.root.style.filter = "blur(1px)";
    ir.root.style.font_family = "Inter";
    ir.root.style.font_size = 15.0f;
    ir.root.style.font_weight = 500;
    ir.root.style.font_style = "italic";
    ir.root.style.text_align = "center";
    ir.root.style.letter_spacing = 0.5f;
    ir.root.style.line_height = 1.3f;
    ir.root.style.text_transform = "uppercase";
    ir.root.style.overflow = "hidden";
    ir.root.style.cursor = "grab";
    ir.root.style.position = "absolute";
    ir.root.style.top = 1.0f;
    ir.root.style.left = 2.0f;
    ir.root.style.right = 3.0f;
    ir.root.style.bottom = 4.0f;
    ir.root.style.z_index = 12;
    ir.root.style.transform = "scale(1.1)";
    ir.root.style.width = 200.0f;
    ir.root.style.height = 100.0f;
    ir.root.style.min_width = 80.0f;
    ir.root.style.min_height = 30.0f;
    ir.root.style.max_width = 400.0f;
    ir.root.style.max_height = 160.0f;

    IRNode button;
    button.type = "button";
    button.name = "Send Button";
    button.text_content = "Send";
    ir.root.children.push_back(button);

    IRNode input;
    input.type = "input";
    input.name = "amount-input";
    ir.root.children.push_back(input);

    IRNode image;
    image.type = "image";
    image.name = "logo.png";
    ir.root.children.push_back(image);

    ir.tokens.strings["copy.cta"] = "Send";

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = false;
    opts.root_variable = "panelRoot";
    opts.indent_spaces = 4;
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("const panelRoot = document.createElement('div')") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.flexDirection = 'row'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.gap = '2.5px'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.paddingTop = '1px'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.paddingLeft = '4px'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.justifyContent = 'center'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.alignItems = 'center'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.flexWrap = 'wrap'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.flexGrow = '1'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.background = 'linear-gradient(#101010,#202020)'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.opacity = '0.5'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.borderRadius = '3.5px'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.boxShadow = '0 1px 2px #000'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.zIndex = '12'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.maxHeight = '160px'") != std::string::npos);
    REQUIRE(js.find("document.createElement('button')") != std::string::npos);
    REQUIRE(js.find("document.createElement('input')") != std::string::npos);
    REQUIRE(js.find("document.createElement('img')") != std::string::npos);
    REQUIRE(js.find("theme.strings[\"copy.cta\"] = 'Send'") != std::string::npos);
    REQUIRE(js.find("document.body.appendChild(panelRoot)") != std::string::npos);
}

// ─── Visual overrides reach EVERY node kind, not just the branch that had them ───
//
// These all guard one bug shape: a per-View property emitted from one lowering
// branch, so the design looks wrong only for node kinds that take a different
// branch. Nothing errors — the boxes are the right size in the right place, they
// just aren't faded / shadowed / filled. Each case below renders a node kind
// through generate_pulp_js and asserts the property survives the lowering.

namespace {

// A node of every kind that carries the same fade + shadow, so a branch that
// drops one is visible as a missing setOpacity for that id.
DesignIR ir_with_faded_node_of_every_kind() {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 300.0f;

    auto fade = [](IRNode& n) {
        n.style.opacity = 0.5f;
        n.style.box_shadow = parse_css_box_shadow("0 2px 8px #00000080");
        n.style.mix_blend_mode = "multiply";
        // Asymmetric on purpose: a uniform radius lowers through the single
        // 'All' call and would hide a missing per-corner emit.
        n.style.border_top_left_radius = 8.0f;
        n.style.border_top_right_radius = 8.0f;
        n.style.border_bottom_right_radius = 0.0f;
        n.style.border_bottom_left_radius = 0.0f;
    };

    IRNode text;
    text.type = "text";
    text.name = "Caption";
    text.text_content = "Ghosted";
    text.style.font_size = 12.0f;
    fade(text);
    ir.root.children.push_back(text);

    IRNode image;
    image.type = "image";
    image.name = "Overlay";
    image.attributes["asset_path"] = "/tmp/grain.png";
    image.style.width = 64.0f;
    image.style.height = 64.0f;
    fade(image);
    ir.root.children.push_back(image);

    IRNode knob;
    knob.type = "frame";
    knob.name = "Cutoff";
    knob.audio_widget = AudioWidgetType::knob;
    knob.style.width = 48.0f;
    knob.style.height = 48.0f;
    fade(knob);
    ir.root.children.push_back(knob);

    IRNode container;
    container.type = "frame";
    container.name = "Group";
    container.style.width = 100.0f;
    container.style.height = 40.0f;
    IRNode inner;
    inner.type = "text";
    inner.text_content = "x";
    container.children.push_back(inner);
    fade(container);
    ir.root.children.push_back(container);

    return ir;
}

std::string native_js(const DesignIR& ir) {
    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    return generate_pulp_js(ir, opts);
}

// Count non-overlapping occurrences — a property emitted from BOTH a branch and
// the shared lambda writes the line twice, which is how the setBoxShadow
// duplicate survived review.
size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (size_t p = haystack.find(needle); p != std::string::npos;
         p = haystack.find(needle, p + needle.size()))
        ++n;
    return n;
}

}  // namespace

TEST_CASE("native codegen fades text, image, widget and container alike",
          "[view][import][visual-overrides]") {
    const auto js = native_js(ir_with_faded_node_of_every_kind());

    // Four faded nodes in, four setOpacity calls out. A branch-local emit fails
    // this by count, so it cannot be satisfied by fixing only one kind.
    REQUIRE(count_occurrences(js, "setOpacity(") == 4);
    REQUIRE(count_occurrences(js, "setBoxShadow(") == 4);
    REQUIRE(count_occurrences(js, "setMixBlendMode(") == 4);

    // Four nodes with four distinct corners each. Codegen carried one
    // setCornerRadius(id, 'All', r) and dropped asymmetric corners on the
    // floor, under a comment claiming the IR could not hold them — it could.
    REQUIRE(count_occurrences(js, "setCornerRadius(") == 16);
    REQUIRE(count_occurrences(js, "'TopLeft', 8") == 4);
    REQUIRE(count_occurrences(js, "'BottomRight', 0") == 4);
    // The uniform path must not also fire, or 'All' squares the rounded pair.
    REQUIRE(js.find("'All'") == std::string::npos);
}

TEST_CASE("native codegen keeps one call when every corner agrees",
          "[view][import][visual-overrides]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 300.0f;
    ir.root.style.border_radius = 6.0f;

    const auto js = native_js(ir);
    REQUIRE(count_occurrences(js, "setCornerRadius(") == 1);
    REQUIRE(js.find("'All', 6") != std::string::npos);
}

TEST_CASE("native codegen fades a text node — the branch that had no setOpacity",
          "[view][import][visual-overrides]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.style.width = 200.0f;

    IRNode text;
    text.type = "text";
    text.name = "Caption";
    text.text_content = "Ghosted";
    text.style.font_size = 12.0f;
    text.style.opacity = 0.35f;
    ir.root.children.push_back(text);

    const auto js = native_js(ir);

    // The label's own id must be the one faded — not its parent.
    const auto create = js.find("createLabel('");
    REQUIRE(create != std::string::npos);
    const auto id_start = create + std::string("createLabel('").size();
    const auto label_id = js.substr(id_start, js.find('\'', id_start) - id_start);
    REQUIRE(js.find("setOpacity('" + label_id + "', 0.35)") != std::string::npos);
}

TEST_CASE("native codegen emits an audio widget's shadow, blend and opacity",
          "[view][import][visual-overrides]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.style.width = 200.0f;

    IRNode knob;
    knob.type = "frame";
    knob.name = "Cutoff";
    knob.audio_widget = AudioWidgetType::knob;
    knob.style.width = 48.0f;
    knob.style.height = 48.0f;
    knob.style.opacity = 0.3f;  // designer's "disabled" fade
    knob.style.box_shadow = parse_css_box_shadow("0 2px 8px #00000080");
    ir.root.children.push_back(knob);

    const auto js = native_js(ir);

    REQUIRE(js.find("createKnob('") != std::string::npos);
    REQUIRE(js.find("setOpacity('") != std::string::npos);
    REQUIRE(js.find("setBoxShadow('") != std::string::npos);
}

TEST_CASE("native codegen positions an unlabeled non-knob widget absolutely",
          "[view][import][visual-overrides]") {
    // Only the knob sub-branch emitted the position, so every other widget kind
    // lost it. Fader is the canonical miss: a channel-strip fader pinned at an
    // absolute offset drifted to wherever flex put it.
    for (auto wtype : {AudioWidgetType::fader, AudioWidgetType::meter,
                       AudioWidgetType::xy_pad, AudioWidgetType::waveform}) {
        DesignIR ir;
        ir.source = DesignSource::figma;
        ir.root.type = "frame";
        ir.root.style.width = 400.0f;

        IRNode w;
        w.type = "frame";
        w.audio_widget = wtype;   // no label/value/range → no wrapper column
        w.style.width = 40.0f;
        w.style.height = 120.0f;
        w.style.position = "absolute";
        w.style.left = 72.0f;
        w.style.top = 24.0f;
        ir.root.children.push_back(w);

        const auto js = native_js(ir);

        INFO("widget kind index " << static_cast<int>(wtype) << " js:\n" << js);
        REQUIRE(js.find("setPosition('") != std::string::npos);
        REQUIRE(js.find("', 72)") != std::string::npos);   // setLeft
        REQUIRE(js.find("', 24)") != std::string::npos);   // setTop
    }
}

TEST_CASE("native codegen paints a childless non-frame node's gradient and fade",
          "[view][import][visual-overrides]") {
    // The fall-through branch. Reachable for real input: the v0 lane maps void
    // tags (<input>, <canvas>, …) to childless non-frame nodes, and a
    // gradient-filled <rect> lands here because synthesize_node declines to
    // path-ify it — on the stated grounds that the frame paints its own box.
    auto ir = parse_v0_tsx(R"tsx(
        export default function P() {
            return (
                <div style={{ display: "flex" }}>
                    <input style={{
                        background: "linear-gradient(90deg, #0f0, #00f)",
                        opacity: 0.25,
                        borderRadius: 6,
                        width: 80,
                        height: 20
                    }} />
                </div>
            );
        }
    )tsx");

    const auto js = native_js(ir);
    INFO(js);

    REQUIRE(js.find("setBackgroundGradient('input0', 'linear-gradient(90deg, #0f0, #00f)')")
            != std::string::npos);
    REQUIRE(js.find("setOpacity('input0', 0.25)") != std::string::npos);
    REQUIRE(js.find("setCornerRadius('input0', 'All', 6)") != std::string::npos);
}

TEST_CASE("native codegen paints a text node's background gradient",
          "[view][import][visual-overrides]") {
    // v0 maps `background: linear-gradient(...)` on any inline tag, and <span>
    // lowers to a text node — the gradient plate behind a heading was dropped.
    auto ir = parse_v0_tsx(R"tsx(
        export default function P() {
            return (
                <div style={{ display: "flex" }}>
                    <span style={{ background: "linear-gradient(90deg, #f00, #00f)" }}>Hi</span>
                </div>
            );
        }
    )tsx");

    const auto js = native_js(ir);
    INFO(js);

    REQUIRE(js.find("setBackgroundGradient('span0', 'linear-gradient(90deg, #f00, #00f)')")
            != std::string::npos);
}

TEST_CASE("native codegen never emits a gradient box behind an SVG path",
          "[view][import][visual-overrides]") {
    // The counter-example that keeps setBackgroundGradient OUT of the shared
    // visual-override lambda: a gradient on a node that lowers to an
    // SvgPathWidget would paint a gradient SQUARE behind the path — the stray
    // box behind a knob that synthesize_node exists to prevent.
    //
    // The node must carry AUTHORED path_data. A bare primitive does not prove
    // anything here: synthesize_node moves its gradient onto the synthesized
    // path and RESETS style.background_gradient, so the lambda would find
    // nothing to emit and this test would pass with the bug present — it did,
    // until the fixture was fixed. An authored path takes synthesize_node's
    // early return, so the style gradient survives to codegen and the branch is
    // the only thing standing between it and the box.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.style.width = 200.0f;

    IRNode glyph;
    glyph.type = "path";
    glyph.name = "Play";
    glyph.attributes["path_data"] = "M0 0 L32 16 L0 32 Z";
    glyph.attributes["svg_viewbox"] = "0 0 32 32";
    glyph.attributes["svg_fill_gradient"] = "linear-gradient(90deg, #f00, #900)";
    glyph.style.width = 32.0f;
    glyph.style.height = 32.0f;
    glyph.style.background_gradient = "linear-gradient(90deg, #f00, #900)";
    glyph.style.opacity = 0.6f;
    ir.root.children.push_back(glyph);

    const auto js = native_js(ir);
    INFO(js);

    REQUIRE(js.find("createSvgPath('") != std::string::npos);
    // The gradient rides the path, and ONLY the path.
    REQUIRE(js.find("setSvgFillGradient('") != std::string::npos);
    REQUIRE(js.find("setBackgroundGradient(") == std::string::npos);
    // The fade still rides along — opacity is a View property, unlike the fill.
    REQUIRE(js.find("setOpacity('") != std::string::npos);
}

TEST_CASE("native codegen keeps a synthesized primitive's gradient off its box",
          "[view][import][visual-overrides]") {
    // The other half of the same invariant, via the path synthesize_node DOES
    // take: a filled ellipse gets its gradient moved onto the synthesized path,
    // cleared off its own style, and must not paint a box either.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.style.width = 200.0f;

    IRNode dot;
    dot.type = "ellipse";
    dot.name = "Record";
    dot.style.width = 32.0f;
    dot.style.height = 32.0f;
    dot.style.background_gradient = "linear-gradient(90deg, #f00, #900)";
    dot.style.opacity = 0.6f;
    ir.root.children.push_back(dot);

    const auto js = native_js(ir);
    INFO(js);

    REQUIRE(js.find("createSvgPath('") != std::string::npos);
    REQUIRE(js.find("setSvgFillGradient('") != std::string::npos);
    REQUIRE(js.find("setBackgroundGradient(") == std::string::npos);
    REQUIRE(js.find("setOpacity('") != std::string::npos);
}

TEST_CASE("native codegen paints a gradient behind a transparent image",
          "[view][import][visual-overrides]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 300.0f;

    // The canonical case: a transparent PNG over a gradient plate. The image
    // node owns BOTH, and the gradient paints the box behind the bitmap — so a
    // codegen that emits the image and forgets the box loses the plate and the
    // art lands on nothing.
    IRNode image;
    image.type = "image";
    image.name = "Grain";
    image.attributes["asset_path"] = "/tmp/grain.png";
    image.style.width = 64.0f;
    image.style.height = 64.0f;
    image.style.background_gradient = "linear-gradient(180deg, #ffffff, #000000)";
    ir.root.children.push_back(image);

    const auto js = native_js(ir);
    REQUIRE(js.find("setBackgroundGradient('") != std::string::npos);
    REQUIRE(js.find("linear-gradient(180deg, #ffffff, #000000)") != std::string::npos);
    // Both, not either: the plate is behind the bitmap, not instead of it.
    REQUIRE(js.find("setImageSource(") != std::string::npos);
}
