#include "test_design_import_shared.hpp"

// ── Design source parsing ───────────────────────────────────────────────

TEST_CASE("parse_design_source recognizes valid sources", "[view][import]") {
    REQUIRE(parse_design_source("figma") == DesignSource::figma);
    REQUIRE(parse_design_source("stitch") == DesignSource::stitch);
    REQUIRE(parse_design_source("v0") == DesignSource::v0);
    REQUIRE(parse_design_source("pencil") == DesignSource::pencil);
    REQUIRE(parse_design_source("claude") == DesignSource::claude);
    REQUIRE(parse_design_source("designmd") == DesignSource::designmd);
    REQUIRE(parse_design_source("jsx") == DesignSource::jsx);
    REQUIRE_FALSE(parse_design_source("unknown").has_value());
}

TEST_CASE("parse coerces CSS string dimensions to floats", "[view][import][parse]") {
    // v0 / Stitch / Pencil emit CSS string dims ("100px", "12"); a bare
    // getWithDefault<double> on a string returns 0 and degenerates the
    // dimension. parse_ir_style now routes string values through the length
    // parser so px-suffixed and numeric strings coerce correctly.
    const std::string json = R"({
      "version": 1, "source": "figma",
      "root": {"type": "frame", "name": "Root",
        "style": {"width": "120px", "height": "80", "borderWidth": "1px",
                  "opacity": "0.5"}}
    })";
    const auto ir = parse_design_ir_json(json);
    REQUIRE(ir.root.style.width == 120.0f);
    REQUIRE(ir.root.style.height == 80.0f);
    REQUIRE(ir.root.style.border_width == 1.0f);
    REQUIRE(ir.root.style.opacity == 0.5f);

    // A non-length string ("auto", "50%") is NOT a px length, so the float
    // field stays unset for the sizing-mode / percent path to interpret.
    const auto ir2 = parse_design_ir_json(
        R"({"version":1,"source":"figma",
            "root":{"type":"frame","style":{"width":"auto","height":"50%"}}})");
    REQUIRE_FALSE(ir2.root.style.width.has_value());
    REQUIRE_FALSE(ir2.root.style.height.has_value());
}

TEST_CASE("design_source_name returns display names", "[view][import]") {
    REQUIRE(std::string(design_source_name(DesignSource::figma)) == "Figma");
    REQUIRE(std::string(design_source_name(DesignSource::v0)) == "v0");
    REQUIRE(std::string(design_source_name(DesignSource::claude)) == "Claude Design");
    REQUIRE(std::string(design_source_name(DesignSource::designmd)) == "DESIGN.md");
    REQUIRE(std::string(design_source_name(DesignSource::jsx)) == "JSX instrument");
}

TEST_CASE("DesignIR v1 canonical JSON round-trips source metadata and assets",
          "[view][import][ir-v1][assets]") {
    DesignIR ir;
    ir.source = DesignSource::stitch;
    ir.source_file = "https://example.test/design.html";
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.layout.display = "flex";
    ir.root.layout.direction = LayoutDirection::row;
    ir.root.layout.gap = 8.0f;
    ir.root.layout.row_gap = 4.0f;
    ir.root.layout.column_gap = 6.0f;
    ir.root.layout.margin_top = 1.0f;
    ir.root.layout.margin_right = 2.0f;
    ir.root.layout.margin_bottom = 3.0f;
    ir.root.layout.margin_left = 4.0f;
    ir.root.layout.justify = LayoutAlign::space_between;
    ir.root.layout.align = LayoutAlign::center;
    ir.root.layout.align_self = "stretch";
    ir.root.layout.align_content = "space-between";
    ir.root.layout.flex_grow = 1.0f;
    ir.root.layout.flex_shrink = 0.0f;
    ir.root.layout.flex_basis = "auto";
    ir.root.layout.order = 2;
    ir.root.layout.aspect_ratio = 1.5f;
    ir.root.layout.overflow_x = "hidden";
    ir.root.layout.overflow_y = "auto";
    ir.root.layout.width_mode = SizingMode::fill;
    ir.root.layout.height_mode = SizingMode::hug;
    ir.root.style.background_color = "#101010";
    ir.root.style.background_image = "url(data:image/svg+xml,%3Csvg%20xmlns='http://www.w3.org/2000/svg'/%3E)";
    ir.root.style.background_repeat = "no-repeat";
    ir.root.style.background_size = "cover";
    ir.root.style.object_fit = "contain";
    ir.root.style.border_color = "#ffffff";
    ir.root.style.border_width = 2.0f;
    ir.root.style.border_style = "solid";
    ir.root.style.border_top_left_radius = 3.0f;
    ir.root.style.border_top_right_radius = 4.0f;
    ir.root.style.border_bottom_right_radius = 5.0f;
    ir.root.style.border_bottom_left_radius = 6.0f;
    ir.root.style.backdrop_filter = "blur(4px)";
    ir.root.style.text_decoration = "underline";
    ir.root.style.white_space = "nowrap";
    ir.root.style.text_overflow = "ellipsis";
    ir.root.stable_anchor_id = "stitch:panel";
    ir.root.anchor_strategy = "adapter";
    ir.root.source_node_id = "node-1";
    ir.root.source_adapter = "stitch";
    ir.root.source_version = "1";
    ir.root.attributes["zeta"] = "last";
    ir.root.attributes["alpha"] = "first";

    IRNode label;
    label.type = "text";
    label.name = "Title";
    label.text_content = "Gain";
    ir.root.children.push_back(label);

    ir.tokens.colors["accent"] = "#ff00ff";
    ir.tokens.dimensions["spacing.md"] = 8.0f;
    ir.tokens.strings["copy.title"] = "Gain";
    ir.tokens.source_identity["colors.accent"] = IRTokenIdentity{
        "var-1", "palette", "dark", "stitch"
    };

    refresh_design_ir_asset_manifest(ir);

    const auto canonical = serialize_design_ir(ir);
    const auto parsed = parse_design_ir_json(canonical);
    const auto canonical_again = serialize_design_ir(parsed);

    REQUIRE(canonical_again == canonical);
    REQUIRE(parsed.version == 1);
    REQUIRE(parsed.source == DesignSource::stitch);
    REQUIRE(parsed.source_file == "https://example.test/design.html");
    REQUIRE(parsed.root.layout.display == "flex");
    REQUIRE(parsed.root.layout.flex_grow == 1.0f);
    REQUIRE(parsed.root.layout.overflow_y == "auto");
    REQUIRE(parsed.root.style.background_repeat == "no-repeat");
    REQUIRE(parsed.root.style.background_size == "cover");
    REQUIRE(parsed.root.style.object_fit == "contain");
    REQUIRE(parsed.root.style.border_top_left_radius == 3.0f);
    REQUIRE(parsed.root.style.text_overflow == "ellipsis");
    REQUIRE(parsed.root.stable_anchor_id == "stitch:panel");
    REQUIRE(parsed.root.anchor_strategy == "adapter");
    REQUIRE(parsed.root.source_adapter == "stitch");
    REQUIRE(parsed.root.attributes.at("alpha") == "first");
    REQUIRE(parsed.tokens.source_identity.at("colors.accent").source_mode == "dark");
    REQUIRE(parsed.asset_manifest.version == 1);
    REQUIRE(parsed.asset_manifest.assets.size() == 1);
    REQUIRE(parsed.asset_manifest.assets[0].mime == "image/svg+xml");
    REQUIRE_FALSE(parsed.asset_manifest.assets[0].content_hash.empty());
}

TEST_CASE("DesignIR round-trips faithful_svg render mode and interactive elements",
          "[view][import][ir-v1][faithful-svg]") {
    // Plan B: a node materialized as a faithful SVG render carries its render
    // mode, the SVG asset id, and a typed list of source-identified interactive
    // overlays. All three must survive canonical serialize -> parse -> serialize.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "ELYSIUM";
    ir.root.render_mode = NodeRenderMode::faithful_svg;
    ir.root.svg_asset_id = "asset-frame-svg";

    IRInteractiveElement knob;
    knob.kind = InteractiveElementKind::knob;
    knob.cx = 120.5f;
    knob.cy = 240.25f;
    knob.hit_radius = 32.0f;
    knob.svg_patch_d = "M120 208L120 200";
    knob.default_value = 0.33f;
    knob.source_node_id = "3:225";
    knob.param_key = "filter.cutoff";   // host-param binding key (geometry-detected control)
    ir.root.interactive_elements.push_back(knob);

    IRInteractiveElement knob2;          // a second, minimal overlay (no source id)
    knob2.cx = 300.0f;
    knob2.cy = 240.0f;
    knob2.hit_radius = 28.0f;
    knob2.default_value = 0.5f;
    ir.root.interactive_elements.push_back(knob2);

    const auto canonical = serialize_design_ir(ir);
    const auto parsed = parse_design_ir_json(canonical);
    const auto canonical_again = serialize_design_ir(parsed);

    REQUIRE(canonical_again == canonical);
    REQUIRE(parsed.root.render_mode == NodeRenderMode::faithful_svg);
    REQUIRE(parsed.root.svg_asset_id == "asset-frame-svg");
    REQUIRE(parsed.root.interactive_elements.size() == 2);

    const auto& k0 = parsed.root.interactive_elements[0];
    REQUIRE(k0.kind == InteractiveElementKind::knob);
    REQUIRE(k0.cx == 120.5f);
    REQUIRE(k0.cy == 240.25f);
    REQUIRE(k0.hit_radius == 32.0f);
    REQUIRE(k0.svg_patch_d == "M120 208L120 200");
    REQUIRE(k0.default_value == 0.33f);
    REQUIRE(k0.source_node_id == "3:225");
    REQUIRE(k0.param_key == "filter.cutoff");   // binding key survives round-trip

    const auto& k1 = parsed.root.interactive_elements[1];
    REQUIRE(k1.svg_patch_d.empty());
    REQUIRE_FALSE(k1.source_node_id.has_value());
    REQUIRE(k1.default_value == 0.5f);
    REQUIRE(k1.param_key.empty());              // absent key stays empty (omitted from JSON)
}

TEST_CASE("DesignIR defaults to normal render mode and omits faithful_svg keys",
          "[view][import][ir-v1][faithful-svg]") {
    // A node with no faithful-vector data must stay `normal` and not emit any of
    // the Plan-B keys — the lanes coexist with zero footprint on normal nodes.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Plain";
    const auto canonical = serialize_design_ir(ir);
    REQUIRE(canonical.find("render_mode") == std::string::npos);
    REQUIRE(canonical.find("svg_asset_id") == std::string::npos);
    REQUIRE(canonical.find("interactive_elements") == std::string::npos);

    const auto parsed = parse_design_ir_json(canonical);
    REQUIRE(parsed.root.render_mode == NodeRenderMode::normal);
    REQUIRE_FALSE(parsed.root.svg_asset_id.has_value());
    REQUIRE(parsed.root.interactive_elements.empty());
}

TEST_CASE("DesignIR round-trips dropdown / text_field / tab_group overlay elements",
          "[view][import][ir-v1][faithful-svg]") {
    // The native-overlay interactive kinds (Plan B "full A") carry a rect + their
    // own typed data and must survive serialize -> parse -> serialize, and must
    // NOT collapse to `knob` (the prior interactive_kind_from_id bug).
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.render_mode = NodeRenderMode::faithful_svg;
    ir.root.svg_asset_id = "asset-svg";

    IRInteractiveElement dropdown;
    dropdown.kind = InteractiveElementKind::dropdown;
    dropdown.x = 210; dropdown.y = 180; dropdown.w = 120; dropdown.h = 28;
    dropdown.options = {"1/4 Delay", "1/8 Delay", "Reverb"};
    dropdown.selected_index = 0; dropdown.source_node_id = "9:1";
    dropdown.label = "Delay Mode";  // design caption -> generated-param name (round-trip below)
    ir.root.interactive_elements.push_back(dropdown);

    IRInteractiveElement search;
    search.kind = InteractiveElementKind::text_field;
    search.x = 16; search.y = 50; search.w = 280; search.h = 32; search.placeholder = "Search";
    ir.root.interactive_elements.push_back(search);

    IRInteractiveElement tabs;
    tabs.kind = InteractiveElementKind::tab_group;
    tabs.x = 320; tabs.y = 48; tabs.w = 160; tabs.h = 28;
    tabs.options = {"1", "2", "3", "4"}; tabs.selected_index = 2;
    ir.root.interactive_elements.push_back(tabs);

    const auto canonical = serialize_design_ir(ir);
    const auto parsed = parse_design_ir_json(canonical);
    REQUIRE(serialize_design_ir(parsed) == canonical);
    REQUIRE(parsed.root.interactive_elements.size() == 3);

    const auto& d = parsed.root.interactive_elements[0];
    REQUIRE(d.kind == InteractiveElementKind::dropdown);   // NOT collapsed to knob
    REQUIRE(d.w == 120.0f);
    REQUIRE(d.options.size() == 3);
    REQUIRE(d.options[2] == "Reverb");
    REQUIRE(d.source_node_id == "9:1");
    REQUIRE(d.label == "Delay Mode");                      // caption survives round-trip

    const auto& s = parsed.root.interactive_elements[1];
    REQUIRE(s.kind == InteractiveElementKind::text_field);
    REQUIRE(s.placeholder == "Search");
    REQUIRE(s.w == 280.0f);
    REQUIRE(s.label.empty());                              // omitted when unset

    const auto& t = parsed.root.interactive_elements[2];
    REQUIRE(t.kind == InteractiveElementKind::tab_group);
    REQUIRE(t.options.size() == 4);
    REQUIRE(t.selected_index == 2);
}

TEST_CASE("DesignIR round-trips fader / toggle / switch interactive elements",
          "[view][import][ir-v1][faithful-svg][p1a]") {
    // P1a: fader + toggle close the IR<->runtime<->schema gap. The runtime
    // (DesignFrameElement::Kind) already backs both; this proves they survive
    // serialize -> parse -> serialize without collapsing to `knob`.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.render_mode = NodeRenderMode::faithful_svg;
    ir.root.svg_asset_id = "asset-svg";

    IRInteractiveElement fader;             // SVG-patch thumb translated over a track
    fader.kind = InteractiveElementKind::fader;
    fader.x = 40; fader.y = 20; fader.w = 12; fader.h = 120;
    fader.cx = 46; fader.cy = 80;
    fader.svg_patch_d = "M46 80L46 70";
    fader.default_value = 0.25f;
    fader.label = "Level";
    ir.root.interactive_elements.push_back(fader);

    IRInteractiveElement toggle;            // press-flash command button (dice/random)
    toggle.kind = InteractiveElementKind::toggle;
    toggle.x = 10; toggle.y = 10; toggle.w = 44; toggle.h = 22;
    toggle.default_value = 1.0f;
    toggle.flash = true;                    // press-flash, not a sticky flip
    ir.root.interactive_elements.push_back(toggle);

    IRInteractiveElement sw;                // a toggle WITH a dot = a switch
    sw.kind = InteractiveElementKind::toggle;
    sw.x = 80; sw.y = 10; sw.w = 44; sw.h = 22;
    sw.cx = 90; sw.cy = 21;
    sw.svg_patch_d = "M90 21a3 3 0 106 0";
    sw.default_value = 0.0f;
    ir.root.interactive_elements.push_back(sw);

    const auto canonical = serialize_design_ir(ir);
    const auto parsed = parse_design_ir_json(canonical);
    REQUIRE(serialize_design_ir(parsed) == canonical);     // stable round-trip
    REQUIRE(parsed.root.interactive_elements.size() == 3);

    const auto& f = parsed.root.interactive_elements[0];
    REQUIRE(f.kind == InteractiveElementKind::fader);      // NOT collapsed to knob
    REQUIRE(f.svg_patch_d == "M46 80L46 70");
    REQUIRE(f.h == 120.0f);
    REQUIRE(f.default_value == 0.25f);
    REQUIRE(f.label == "Level");

    const auto& tg = parsed.root.interactive_elements[1];
    REQUIRE(tg.kind == InteractiveElementKind::toggle);
    REQUIRE(tg.w == 44.0f);
    REQUIRE(tg.svg_patch_d.empty());                       // a plain toggle has no dot
    REQUIRE(tg.flash == true);                             // press-flash survives round-trip

    const auto& s2 = parsed.root.interactive_elements[2];
    REQUIRE(s2.kind == InteractiveElementKind::toggle);
    REQUIRE(s2.svg_patch_d == "M90 21a3 3 0 106 0");       // switch keeps its dot path
    REQUIRE(s2.flash == false);                            // sticky switch, flash omitted
}

TEST_CASE("DesignIR round-trips swap / action / xy_pad / value_label elements",
          "[view][import][ir-v1][faithful-svg][p1b]") {
    // P1b: these four close the rest of the IR<->runtime gap. The runtime backs
    // each (DesignFrameElement target_frame / action / value_y / text); this
    // proves they survive serialize -> parse -> serialize with their typed data.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.render_mode = NodeRenderMode::faithful_svg;
    ir.root.svg_asset_id = "asset-svg";

    IRInteractiveElement swap;              // swap-link: clicking switches frames
    swap.kind = InteractiveElementKind::swap;
    swap.x = 10; swap.y = 10; swap.w = 60; swap.h = 24;
    swap.target_frame = 3;
    ir.root.interactive_elements.push_back(swap);

    IRInteractiveElement act;               // command button (octave +)
    act.kind = InteractiveElementKind::action;
    act.x = 80; act.y = 10; act.w = 30; act.h = 24;
    act.action = "octave_up";
    ir.root.interactive_elements.push_back(act);

    IRInteractiveElement pad;               // 2D xy pad
    pad.kind = InteractiveElementKind::xy_pad;
    pad.x = 10; pad.y = 50; pad.w = 120; pad.h = 120;
    pad.cx = 70; pad.cy = 110;
    pad.svg_patch_d = "M70 110l4 0";
    pad.default_value = 0.25f;              // X
    pad.default_value_y = 0.8f;             // Y
    ir.root.interactive_elements.push_back(pad);

    IRInteractiveElement lbl;               // live readout
    lbl.kind = InteractiveElementKind::value_label;
    lbl.x = 10; lbl.y = 180; lbl.w = 80; lbl.h = 16;
    lbl.text = "0.0 dB";
    lbl.value_left_align = true;
    ir.root.interactive_elements.push_back(lbl);

    const auto canonical = serialize_design_ir(ir);
    const auto parsed = parse_design_ir_json(canonical);
    REQUIRE(serialize_design_ir(parsed) == canonical);     // stable round-trip
    REQUIRE(parsed.root.interactive_elements.size() == 4);

    const auto& sw = parsed.root.interactive_elements[0];
    REQUIRE(sw.kind == InteractiveElementKind::swap);
    REQUIRE(sw.target_frame == 3);

    const auto& a = parsed.root.interactive_elements[1];
    REQUIRE(a.kind == InteractiveElementKind::action);
    REQUIRE(a.action == "octave_up");

    const auto& p = parsed.root.interactive_elements[2];
    REQUIRE(p.kind == InteractiveElementKind::xy_pad);
    REQUIRE(p.default_value == 0.25f);
    REQUIRE(p.default_value_y == 0.8f);
    REQUIRE(p.svg_patch_d == "M70 110l4 0");

    const auto& l = parsed.root.interactive_elements[3];
    REQUIRE(l.kind == InteractiveElementKind::value_label);
    REQUIRE(l.text == "0.0 dB");
    REQUIRE(l.value_left_align == true);
}

TEST_CASE("DesignIR diagnoses an unknown interactive kind instead of silent-knobbing",
          "[view][import][ir-v1][faithful-svg][p1a]") {
    // P1a acceptance: an unrecognized `kind` string must NOT be silently treated
    // as a working knob. Forward-compat is preserved (it still parses + renders
    // as a knob so the import never blanks), and the parser log_warns (the full
    // ordered ladder + structured import report is the P7 work). Here we pin the
    // forward-compat fallback and that a sibling known element still parses.
    const std::string envelope = R"json({
      "format_version": "2026.05-figma-plugin-v1",
      "provenance": {"adapter": "figma-plugin", "version": "test"},
      "root": {
        "type": "frame", "render_mode": "faithful_svg", "svg_asset_id": "a",
        "interactive_elements": [
          {"kind": "wormhole", "x": 0, "y": 0, "w": 10, "h": 10, "source_node_id": "9:9"},
          {"kind": "fader", "x": 0, "y": 20, "w": 8, "h": 80, "svg_patch_d": "M4 80L4 70"}
        ]
      }
    })json";
    const auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.interactive_elements.size() == 2);
    // Unknown kind falls back to knob (render never blanks) — but it is the
    // diagnosed floor, not a confident classification.
    CHECK(ir.root.interactive_elements[0].kind == InteractiveElementKind::knob);
    CHECK(ir.root.interactive_elements[0].source_node_id == "9:9");
    // The known sibling is unaffected.
    CHECK(ir.root.interactive_elements[1].kind == InteractiveElementKind::fader);
}

TEST_CASE("figma block component semantics land in normalized attributes",
          "[view][import][components]") {
    // Audit item 4: the plugin/REST/.fig lanes all emit component metadata in
    // the node's `figma` block, and parse_ir_node must preserve ALL of it into
    // the namespaced attribute keys — not just key/name. Covers a typed TEXT
    // property (with Figma's "#id" name uniquifier stripped), a BOOLEAN, an
    // INSTANCE_SWAP (the swapped component id/name is the value), a VARIANT
    // entry, the variant axis map, set name, master id, and the remote flag.
    const std::string envelope = R"json({
      "format_version": "2026.05-figma-plugin-v1",
      "provenance": {"adapter": "figma-plugin", "version": "test"},
      "root": {
        "type": "frame", "name": "Root",
        "children": [{
          "type": "frame", "name": "Knob Instance",
          "figma": {
            "parent_id": "1:1", "z_order": 0,
            "absolute_transform": [[1,0,0],[0,1,0]],
            "visible": true, "locked": false, "blend_mode": "PASS_THROUGH",
            "component_key": "abc123key",
            "component_set_name": "Knob",
            "main_component_id": "12:34",
            "main_component_name": "size=lg, state=default",
            "remote_library": true,
            "component_properties": {
              "label#9:0": {"type": "TEXT", "value": "Drive"},
              "showValue#9:1": {"type": "BOOLEAN", "value": true},
              "icon#9:2": {"type": "INSTANCE_SWAP", "value": "77:5"},
              "size": {"type": "VARIANT", "value": "lg"},
              "detents#9:3": {"type": "NUMBER", "value": 11}
            },
            "variant_properties": {"size": "lg", "state": "default"}
          }
        }]
      }
    })json";
    const auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.children.size() == 1);
    const auto& a = ir.root.children[0].attributes;
    CHECK(a.at("figmaComponentKey") == "abc123key");
    CHECK(a.at("figmaComponentSetName") == "Knob");
    CHECK(a.at("figmaMainComponentId") == "12:34");
    CHECK(a.at("figmaMainComponentName") == "size=lg, state=default");
    CHECK(a.at("figmaRemoteLibrary") == "true");
    // Typed properties: "#id" suffix stripped, value stringified, type kept.
    CHECK(a.at("figmaComponentProperty.label") == "Drive");
    CHECK(a.at("figmaComponentPropertyType.label") == "TEXT");
    CHECK(a.at("figmaComponentProperty.showValue") == "true");
    CHECK(a.at("figmaComponentPropertyType.showValue") == "BOOLEAN");
    CHECK(a.at("figmaComponentProperty.icon") == "77:5");
    CHECK(a.at("figmaComponentPropertyType.icon") == "INSTANCE_SWAP");
    CHECK(a.at("figmaComponentProperty.size") == "lg");
    CHECK(a.at("figmaComponentPropertyType.size") == "VARIANT");
    CHECK(a.at("figmaComponentProperty.detents") == "11");
    CHECK(a.at("figmaComponentPropertyType.detents") == "NUMBER");
    // Variant axis selections, one attribute per axis.
    CHECK(a.at("figmaVariant.size") == "lg");
    CHECK(a.at("figmaVariant.state") == "default");
}

TEST_CASE("figma block variable bindings land in namespaced attributes",
          "[view][import][variables]") {
    // Audit item 5: token DEFINITIONS always flowed (envelope tokens block →
    // parse_ir_tokens → IRTokens) but no node said WHICH token drives WHICH
    // property. The producers now emit figma.bound_variables
    // ({property: token name}); parse_ir_node must preserve every entry as a
    // figmaBoundVariable.<property> attribute, opaque and additive, alongside
    // the token definitions it points into.
    const std::string envelope = R"json({
      "format_version": "2026.05-figma-plugin-v1",
      "provenance": {"adapter": "figma-plugin", "version": "test"},
      "tokens": {
        "colors": {"theme.brand.primary": "#ff0000"},
        "dimensions": {"theme.radius.md": 8},
        "strings": {"theme.label.gain": "Gain"}
      },
      "token_source_identity": {
        "colors.theme.brand.primary": {
          "sourceId": "VariableID:1:1",
          "sourceCollection": "Theme",
          "sourceMode": "Light",
          "sourceAdapter": "figma-plugin"
        }
      },
      "root": {
        "type": "frame", "name": "Root", "figma_node_id": "1:1",
        "children": [
          {
            "type": "frame", "name": "Bound", "figma_node_id": "1:2",
            "style": {"backgroundColor": "#ff0000"},
            "figma": {
              "parent_id": "1:1", "z_order": 0,
              "absolute_transform": [[1,0,0],[0,1,0]],
              "visible": true, "locked": false, "blend_mode": "PASS_THROUGH",
              "bound_variables": {
                "fills": "theme.brand.primary",
                "fills.1": "theme/secondary with spaces",
                "cornerRadius": "theme.radius.md",
                "componentProperties.label#9:0": "theme.label.gain"
              }
            }
          },
          {
            "type": "frame", "name": "Plain", "figma_node_id": "1:3",
            "style": {"backgroundColor": "#ff0000"},
            "figma": {
              "parent_id": "1:1", "z_order": 1,
              "absolute_transform": [[1,0,0],[0,1,0]],
              "visible": true, "locked": false, "blend_mode": "PASS_THROUGH"
            }
          }
        ]
      }
    })json";
    const auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.children.size() == 2);
    const auto& a = ir.root.children[0].attributes;
    CHECK(a.at("figmaBoundVariable.fills") == "theme.brand.primary");
    // Token names are opaque passthrough — slashes/spaces are the producer's
    // business, this reader must not normalize them.
    CHECK(a.at("figmaBoundVariable.fills.1") == "theme/secondary with spaces");
    CHECK(a.at("figmaBoundVariable.cornerRadius") == "theme.radius.md");
    CHECK(a.at("figmaBoundVariable.componentProperties.label#9:0") ==
          "theme.label.gain");
    // The bindings point into token definitions that round-trip beside them.
    CHECK(ir.tokens.colors.at("theme.brand.primary") == "#ff0000");
    CHECK(ir.tokens.dimensions.at("theme.radius.md") == 8.0f);
    CHECK(ir.tokens.strings.at("theme.label.gain") == "Gain");
    const auto& identity =
        ir.tokens.source_identity.at("colors.theme.brand.primary");
    CHECK(identity.source_id == "VariableID:1:1");
    CHECK(identity.source_collection == "Theme");
    CHECK(identity.source_mode == "Light");
    CHECK(identity.source_adapter == "figma-plugin");
    // The resolved literal remains directly renderable while its binding and
    // stable source-node identity stay queryable as separate provenance.
    CHECK(ir.root.children[0].style.background_color == "#ff0000");
    CHECK(ir.root.children[0].source_node_id == "1:2");
    CHECK(ir.root.children[1].style.background_color == "#ff0000");
    // Negative: a node without bound_variables gains no binding attribute.
    for (const auto& [key, value] : ir.root.children[1].attributes) {
        (void)value;
        CHECK(key.rfind("figmaBoundVariable.", 0) != 0);
    }
    // Canonical serialization sorts the unordered attribute/token maps. A
    // frozen DesignIR can be parsed again without losing either the resolved
    // paint or the token identity, independent of producer object-key order.
    const auto canonical = serialize_design_ir(ir);
    const auto reparsed = parse_design_ir_json(canonical);
    CHECK(serialize_design_ir(reparsed) == canonical);
    CHECK(reparsed.root.children[0].attributes.at("figmaBoundVariable.fills") ==
          "theme.brand.primary");
    CHECK(reparsed.root.children[0].style.background_color == "#ff0000");
    CHECK(reparsed.tokens.source_identity.at("colors.theme.brand.primary").source_id ==
          "VariableID:1:1");
}

TEST_CASE("figma-plugin synthetic multi-root keeps child anchors unique",
          "[view][import][variables][anchors]") {
    const auto ir = parse_figma_plugin_json(R"json({
      "format_version": "2026.05-figma-plugin-v1",
      "provenance": {"adapter": "figma-plugin", "version": "test"},
      "tokens": {"colors": {}, "dimensions": {}, "strings": {}},
      "root": {
        "type": "frame", "name": "<multi-export>",
        "children": [
          {"type": "frame", "name": "First", "figma_node_id": "1:2"},
          {"type": "frame", "name": "Second", "figma_node_id": "1:3"}
        ]
      }
    })json");
    REQUIRE(ir.root.children.size() == 2);
    CHECK_FALSE(ir.root.source_node_id.has_value());
    REQUIRE(ir.root.stable_anchor_id.has_value());
    CHECK(ir.root.children[0].source_node_id == "1:2");
    CHECK(ir.root.children[1].source_node_id == "1:3");
    CHECK(ir.root.children[0].stable_anchor_id == "figma-plugin:1:2");
    CHECK(ir.root.children[1].stable_anchor_id == "figma-plugin:1:3");
    CHECK(ir.root.stable_anchor_id != ir.root.children[0].stable_anchor_id);
    CHECK(ir.root.stable_anchor_id != ir.root.children[1].stable_anchor_id);
}

TEST_CASE("figma block without component metadata preserves nothing extra",
          "[view][import][components]") {
    // Negative: a plain (non-instance) node's figma block must not sprout any
    // figma* component attributes — the preservation is strictly additive.
    const std::string envelope = R"json({
      "format_version": "2026.05-figma-plugin-v1",
      "provenance": {"adapter": "figma-plugin", "version": "test"},
      "root": {
        "type": "frame", "name": "Root",
        "children": [{
          "type": "frame", "name": "Plain",
          "figma": {
            "parent_id": "1:1", "z_order": 0,
            "absolute_transform": [[1,0,0],[0,1,0]],
            "visible": true, "locked": false, "blend_mode": "PASS_THROUGH"
          }
        }]
      }
    })json";
    const auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.root.children.size() == 1);
    for (const auto& [key, value] : ir.root.children[0].attributes) {
        INFO(key << " = " << value);
        CHECK(key.rfind("figma", 0) != 0);
    }
}

TEST_CASE("DesignIR round-trips the P7 import-report fields (F0 carrier chain)",
          "[view][import][ir-v1][faithful-svg][p7]") {
    // P7-F0: the resolution provenance (rung / confidence / conflicts /
    // verification) must survive JSON->IR->JSON so a low-confidence or conflicted
    // control is visible at the host materialize boundary, not just in the TS
    // importer. (The LOGIC that fills these is P7-F2; this pins the carrier.)
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.render_mode = NodeRenderMode::faithful_svg;
    ir.root.svg_asset_id = "asset-svg";

    IRInteractiveElement conflicted;   // a control the ladder resolved with a conflict
    conflicted.kind = InteractiveElementKind::knob;
    conflicted.cx = 50; conflicted.cy = 50; conflicted.hit_radius = 20;
    conflicted.resolution_rung = 2;            // resolved via Tier-1 affordance
    conflicted.confidence_score = 0.55f;       // low confidence
    conflicted.conflict_signals = {"name=knob but geometry is a wide track+thumb"};
    conflicted.verification_pass = false;      // render verification flagged it
    ir.root.interactive_elements.push_back(conflicted);

    IRInteractiveElement clean;        // a confidently-resolved control (defaults)
    clean.kind = InteractiveElementKind::fader;
    clean.x = 10; clean.y = 10; clean.w = 8; clean.h = 80;
    ir.root.interactive_elements.push_back(clean);

    const auto canonical = serialize_design_ir(ir);
    const auto parsed = parse_design_ir_json(canonical);
    REQUIRE(serialize_design_ir(parsed) == canonical);     // stable round-trip
    REQUIRE(parsed.root.interactive_elements.size() == 2);

    const auto& c = parsed.root.interactive_elements[0];
    REQUIRE(c.resolution_rung == 2);
    REQUIRE(c.confidence_score == 0.55f);
    REQUIRE(c.conflict_signals.size() == 1);
    REQUIRE(c.conflict_signals[0] == "name=knob but geometry is a wide track+thumb");
    REQUIRE(c.verification_pass == false);

    // Defaults are lean: a clean control emits none of the report fields and
    // parses back to the confident defaults.
    const auto& k = parsed.root.interactive_elements[1];
    REQUIRE(k.resolution_rung == 0);
    REQUIRE(k.confidence_score == 1.0f);
    REQUIRE(k.conflict_signals.empty());
    REQUIRE(k.verification_pass == true);
    // The clean element serialized none of the report keys (lean default).
    CHECK(canonical.find("confidence_score") != std::string::npos);  // only the conflicted one
    CHECK(canonical.find("resolution_rung\":0") == std::string::npos);
}

TEST_CASE("DesignIR round-trips a custom (Tier-3) interactive element",
          "[view][import][ir-v1][faithful-svg][p7]") {
    // P7 Tier-3: a registered-control element carries factory_id + opaque props
    // through serialize -> parse -> serialize so the materializer can look up the
    // factory. NOT collapsed to knob.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.render_mode = NodeRenderMode::faithful_svg;
    ir.root.svg_asset_id = "asset-svg";

    IRInteractiveElement c;
    c.kind = InteractiveElementKind::custom;
    c.x = 10; c.y = 10; c.w = 60; c.h = 40;
    c.factory_id = "acme.spinner";
    c.custom_props = "{\"min\":0,\"max\":11}";
    ir.root.interactive_elements.push_back(c);

    const auto canonical = serialize_design_ir(ir);
    const auto parsed = parse_design_ir_json(canonical);
    REQUIRE(serialize_design_ir(parsed) == canonical);
    REQUIRE(parsed.root.interactive_elements.size() == 1);
    const auto& p = parsed.root.interactive_elements[0];
    REQUIRE(p.kind == InteractiveElementKind::custom);
    REQUIRE(p.factory_id == "acme.spinner");
    REQUIRE(p.custom_props == "{\"min\":0,\"max\":11}");
}

TEST_CASE("collect_import_report surfaces per-control resolution provenance (P7)",
          "[view][import][ir-v1][faithful-svg][p7][report]") {
    // P7 import report: walk the IR's interactive elements (recursively) and
    // surface rung/confidence/conflicts/verification so a low-confidence or
    // conflicted control is SEEN — with a CI-gateable summary.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.render_mode = NodeRenderMode::faithful_svg;

    IRInteractiveElement clean;             // confident knob
    clean.kind = InteractiveElementKind::knob;
    clean.source_node_id = "1:1";
    ir.root.interactive_elements.push_back(clean);

    IRInteractiveElement conflicted;        // flagged: name/geometry conflict
    conflicted.kind = InteractiveElementKind::knob;
    conflicted.source_node_id = "1:2";
    conflicted.resolution_rung = 2;
    conflicted.confidence_score = 0.4f;
    conflicted.conflict_signals = {"resolved kind knob expects square but geometry is stretched"};
    conflicted.verification_pass = false;
    ir.root.interactive_elements.push_back(conflicted);

    // A nested child carrying an inert (rung 5) control — the recursion must find it.
    IRNode child;
    child.type = "frame";
    IRInteractiveElement inert;
    inert.kind = InteractiveElementKind::knob;
    inert.source_node_id = "2:1";
    inert.resolution_rung = 5;              // inert (warn) rung
    inert.confidence_score = 0.2f;
    child.interactive_elements.push_back(inert);
    ir.root.children.push_back(child);

    const auto report = collect_import_report(ir.root);  // default threshold 0.6
    REQUIRE(report.controls.size() == 3);
    REQUIRE(report.conflicted == 1);
    REQUIRE(report.low_confidence == 2);   // 0.4 and 0.2 are below 0.6
    REQUIRE(report.unresolved == 1);       // the rung-5 inert control
    REQUIRE(report.ok() == false);         // conflicts + unresolved → CI gate fails

    // JSON is well-formed and carries the summary + a conflict.
    const auto json = import_report_to_json(report);
    REQUIRE(json.find("\"total\":3") != std::string::npos);
    REQUIRE(json.find("\"conflicted\":1") != std::string::npos);
    REQUIRE(json.find("\"ok\":false") != std::string::npos);
    REQUIRE(json.find("expects square but geometry is stretched") != std::string::npos);

    // A fully-clean import passes the gate.
    DesignIR clean_ir;
    clean_ir.root.type = "frame";
    IRInteractiveElement ok_el;
    ok_el.kind = InteractiveElementKind::fader;
    ok_el.source_node_id = "3:1";
    clean_ir.root.interactive_elements.push_back(ok_el);
    const auto clean_report = collect_import_report(clean_ir.root);
    REQUIRE(clean_report.ok() == true);
    REQUIRE(clean_report.conflicted == 0);
}

TEST_CASE("apply_placement_verification flags degenerate + out-of-frame overlays (P7)",
          "[view][import][ir-v1][faithful-svg][p7][report]") {
    // P7 render-placement verification (structural): an overlay with no extent,
    // or one that falls entirely outside the frame, can't render where it claims
    // — flag it (verification_pass=false + a conflict) so the report/gate sees it.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.render_mode = NodeRenderMode::faithful_svg;
    ir.root.style.width = 200.0f;     // frame region [0,0,200,100]
    ir.root.style.height = 100.0f;

    IRInteractiveElement good;        // a normal in-frame dropdown
    good.kind = InteractiveElementKind::dropdown;
    good.x = 10; good.y = 10; good.w = 80; good.h = 24; good.source_node_id = "1:1";
    ir.root.interactive_elements.push_back(good);

    IRInteractiveElement degenerate;  // zero-area box, no hit radius → can't render
    degenerate.kind = InteractiveElementKind::text_field;
    degenerate.x = 5; degenerate.y = 5; degenerate.w = 0; degenerate.h = 0;
    degenerate.source_node_id = "1:2";
    ir.root.interactive_elements.push_back(degenerate);

    IRInteractiveElement off;         // entirely off to the right of the frame
    off.kind = InteractiveElementKind::knob;
    off.cx = 400; off.cy = 50; off.hit_radius = 10; off.source_node_id = "1:3";
    ir.root.interactive_elements.push_back(off);

    const int flagged = apply_placement_verification(ir.root, 200.0f, 100.0f);
    REQUIRE(flagged == 2);
    CHECK(ir.root.interactive_elements[0].verification_pass == true);   // good
    CHECK(ir.root.interactive_elements[1].verification_pass == false);  // degenerate
    CHECK(ir.root.interactive_elements[2].verification_pass == false);  // off-frame
    CHECK(ir.root.interactive_elements[1].conflict_signals.size() == 1);
    CHECK(ir.root.interactive_elements[2].conflict_signals[0].find("outside the frame")
          != std::string::npos);

    // The report then surfaces them and the gate would fail.
    const auto report = collect_import_report(ir.root);
    REQUIRE(report.controls.size() == 3);
    REQUIRE(report.conflicted == 2);
    REQUIRE(report.ok() == false);

    // Unknown frame size (0) skips the bounds check but still catches degenerates.
    DesignIR ir2;
    ir2.root.type = "frame";
    IRInteractiveElement off2;        // off-frame, but frame size unknown → not flagged
    off2.kind = InteractiveElementKind::knob;
    off2.cx = 400; off2.cy = 50; off2.hit_radius = 10;
    ir2.root.interactive_elements.push_back(off2);
    REQUIRE(apply_placement_verification(ir2.root, 0.0f, 0.0f) == 0);  // bounds unknown
}

TEST_CASE("DesignIR serialization preserves parsed envelope version by default",
          "[view][import][ir-v1]") {
    auto parsed = parse_design_ir_json(R"json({
        "version": 2,
        "source": "jsx",
        "root": { "type": "frame", "name": "Future IR" }
    })json");

    REQUIRE(parsed.version == 2);

    const auto canonical = serialize_design_ir(parsed);
    REQUIRE(canonical.find("\"version\":2") != std::string::npos);
    REQUIRE(parse_design_ir_json(canonical).version == 2);

    DesignIrJsonOptions force_v1;
    force_v1.version = 1;
    REQUIRE(serialize_design_ir(parsed, force_v1).find("\"version\":1") != std::string::npos);
}

TEST_CASE("parse_design_ir_json accepts legacy bare-node IR JSON",
          "[view][import][ir-v1]") {
    const auto legacy = std::string{R"json({
        "type": "frame",
        "name": "Legacy",
        "layout": { "direction": "column", "gap": 12 },
        "style": { "backgroundColor": "#202020" },
        "tokens": {
            "colors": { "bg": "#202020" }
        }
    })json"};

    const auto parsed = parse_design_ir_json(legacy);
    REQUIRE(parsed.version == 1);
    REQUIRE(parsed.root.type == "frame");
    REQUIRE(parsed.root.name == "Legacy");
    REQUIRE(parsed.root.layout.direction == LayoutDirection::column);
    REQUIRE(parsed.root.layout.gap == 12.0f);
    REQUIRE(parsed.root.style.background_color == "#202020");
    REQUIRE(parsed.tokens.colors.at("bg") == "#202020");
}

TEST_CASE("DesignIR v1 canonical equivalence covers static source adapters",
          "[view][import][ir-v1]") {
    auto assert_canonical_round_trip = [](DesignIR ir) {
        refresh_design_ir_asset_manifest(ir);
        const auto canonical = serialize_design_ir(ir);
        const auto parsed = parse_design_ir_json(canonical);
        REQUIRE(serialize_design_ir(parsed) == canonical);
        REQUIRE(parsed.version == 1);
        REQUIRE(parsed.asset_manifest.version == 1);
    };

    SECTION("figma JSON") {
        auto ir = parse_figma_json(R"json({
            "type": "frame",
            "name": "Figma Panel",
            "id": "figma-node-1",
            "style": { "backgroundColor": "#18191c" },
            "tokens": {
                "colors": { "accent": "#57a6ff" },
                "sourceIdentity": {
                    "colors.accent": {
                        "sourceId": "var-accent",
                        "sourceCollection": "palette",
                        "sourceMode": "dark",
                        "sourceAdapter": "figma"
                    }
                }
            },
            "children": [
                { "type": "text", "name": "Title", "content": "Gain" }
            ]
        })json");
        REQUIRE(ir.tokens.source_identity.at("colors.accent").source_adapter == "figma");
        assert_canonical_round_trip(std::move(ir));
    }

    SECTION("stitch HTML") {
        assert_canonical_round_trip(parse_stitch_html(
            "<!doctype html><main><label>Frequency</label><span>8473 Hz</span></main>"));
    }

    SECTION("v0 TSX") {
        assert_canonical_round_trip(parse_v0_tsx(
            "export default function App(){ return <div className=\"flex flex-row gap-2 bg-slate-900\" />; }"));
    }

    SECTION("pencil JSON") {
        assert_canonical_round_trip(parse_pencil_json(R"json({
            "type": "frame",
            "name": "Pencil Card",
            "nodeId": "pencil-node-1",
            "style": { "backgroundImage": "url(data:image/svg+xml,%3Csvg%2F%3E)" },
            "children": [
                { "type": "text", "name": "Label", "content": "Mix" }
            ]
        })json"));
    }

    SECTION("claude HTML") {
        assert_canonical_round_trip(parse_claude_html(
            "<!doctype html><html><body><section><h1>Parameters</h1></section></body></html>"));
    }
}

TEST_CASE("DesignIR asset manifest records data URI local image and font assets",
          "[view][import][assets]") {
    TempDir tmp("pulp-design-ir-assets");
    const auto image_path = tmp.path / "meter.png";
    const auto font_path = tmp.path / "Inter.woff2";
    write_text(image_path, "\x89PNG\r\n\x1a\nnot-a-real-png-but-sniffable");
    write_text(font_path, "wOF2font-bytes");

    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Assets";
    ir.root.style.background_image = "url(data:image/svg+xml,%3Csvg%20xmlns='http://www.w3.org/2000/svg'/%3E)";
    ir.root.attributes["src"] = "meter.png";
    ir.root.attributes["fontUrl"] = "Inter.woff2";

    DesignIrAssetOptions options;
    options.base_directory = tmp.path;
    refresh_design_ir_asset_manifest(ir, options);

    REQUIRE(ir.asset_manifest.assets.size() == 3);

    bool saw_data = false;
    bool saw_image = false;
    bool saw_font = false;
    for (const auto& asset : ir.asset_manifest.assets) {
        REQUIRE_FALSE(asset.asset_id.empty());
        REQUIRE_FALSE(asset.content_hash.empty());
        REQUIRE(asset.diagnostics.empty());
        if (asset.original_uri.rfind("data:", 0) == 0) {
            saw_data = true;
            REQUIRE(asset.mime == "image/svg+xml");
        } else if (asset.original_uri == "meter.png") {
            saw_image = true;
            REQUIRE(asset.mime == "image/png");
            REQUIRE(asset.local_path);
        } else if (asset.original_uri == "Inter.woff2") {
            saw_font = true;
            REQUIRE(asset.mime == "font/woff2");
            REQUIRE(asset.local_path);
        }
    }
    REQUIRE(saw_data);
    REQUIRE(saw_image);
    REQUIRE(saw_font);

    const auto round_trip = parse_design_ir_json(serialize_design_ir(ir));
    REQUIRE(round_trip.asset_manifest.assets.size() == 3);
    REQUIRE(serialize_design_ir(round_trip) == serialize_design_ir(ir));
}

TEST_CASE("DesignIR parses camelCase source metadata and static HTML CSS assets",
          "[view][import][ir-v1][assets]") {
    SECTION("sourceNodeId is accepted as source metadata") {
        auto ir = parse_design_ir_json(R"json({
            "type": "frame",
            "name": "Screen",
            "sourceNodeId": "node-camel-1"
        })json");

        REQUIRE(ir.root.source_node_id);
        REQUIRE(*ir.root.source_node_id == "node-camel-1");
        REQUIRE(serialize_design_ir(ir).find("\"source_node_id\":\"node-camel-1\"")
                != std::string::npos);
    }

    SECTION("static Claude HTML CSS urls and fonts enter the asset manifest") {
        TempDir tmp("pulp-design-ir-static-html-assets");
        write_text(tmp.path / "hero.svg",
                   "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"1\" height=\"1\"/></svg>");
        write_text(tmp.path / "Inter.woff2", "wOF2font-bytes");

        auto ir = parse_claude_html(R"html(
            <!doctype html>
            <html>
            <head>
            <style>
            @font-face { font-family: "Inter"; src: url("Inter.woff2") format("woff2"); }
            .hero { background-image: url("hero.svg"); }
            </style>
            </head>
            <body><section class="hero"><h1>Parameters</h1></section></body>
            </html>
        )html");

        DesignIrAssetOptions options;
        options.base_directory = tmp.path;
        refresh_design_ir_asset_manifest(ir, options);

        bool saw_svg = false;
        bool saw_font = false;
        for (const auto& asset : ir.asset_manifest.assets) {
            REQUIRE(asset.diagnostics.empty());
            if (asset.original_uri == "hero.svg") {
                saw_svg = true;
                REQUIRE(asset.mime == "image/svg+xml");
                REQUIRE_FALSE(asset.content_hash.empty());
            } else if (asset.original_uri == "Inter.woff2") {
                saw_font = true;
                REQUIRE(asset.mime == "font/woff2");
                REQUIRE(asset.font_family);
                REQUIRE(*asset.font_family == "Inter");
                REQUIRE_FALSE(asset.content_hash.empty());
            }
        }
        REQUIRE(saw_svg);
        REQUIRE(saw_font);
    }
}

TEST_CASE("DesignIR asset manifest preserves top-level asset refs and writes asset ids",
          "[view][import][assets]") {
    TempDir tmp("pulp-design-ir-top-level-assets");
    const auto image_path = tmp.path / "hero.png";
    write_text(image_path, "\x89PNG\r\n\x1a\nhero-bytes");

    auto ir = parse_design_ir_json(R"json({
        "type": "frame",
        "name": "Screen",
        "children": [
            { "type": "image", "name": "Hero", "src": "hero.png" }
        ]
    })json");

    DesignIrAssetOptions options;
    options.base_directory = tmp.path;
    refresh_design_ir_asset_manifest(ir, options);

    REQUIRE(ir.asset_manifest.assets.size() == 1);
    const auto& asset = ir.asset_manifest.assets[0];
    REQUIRE(asset.original_uri == "hero.png");
    REQUIRE(asset.mime == "image/png");
    REQUIRE(asset.local_path);
    REQUIRE_FALSE(asset.content_hash.empty());

    REQUIRE(ir.root.children.size() == 1);
    const auto& image = ir.root.children[0];
    REQUIRE(image.attributes.at("src") == "hero.png");
    REQUIRE(image.attributes.at("srcAssetId") == asset.asset_id);

    const auto round_trip = parse_design_ir_json(serialize_design_ir(ir));
    REQUIRE(round_trip.root.children[0].attributes.at("src") == "hero.png");
    REQUIRE(round_trip.root.children[0].attributes.at("srcAssetId") == asset.asset_id);
}

TEST_CASE("DesignIR asset manifest keeps URI aliases for deduped refs",
          "[view][import][assets]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Aliases";

    IRNode first;
    first.type = "image";
    first.name = "Plain";
    first.attributes["src"] = "hero.png";
    IRNode second;
    second.type = "image";
    second.name = "DotSlash";
    second.attributes["src"] = "./hero.png";
    ir.root.children.push_back(std::move(first));
    ir.root.children.push_back(std::move(second));

    DesignIrAssetOptions options;
    options.base_url = "https://cdn.example.test/screens/index.html";
    refresh_design_ir_asset_manifest(ir, options);

    REQUIRE(ir.asset_manifest.assets.size() == 1);
    const auto& asset = ir.asset_manifest.assets[0];
    REQUIRE(asset.original_uri == "hero.png");
    REQUIRE(asset.source_url);
    REQUIRE(*asset.source_url == "https://cdn.example.test/screens/hero.png");
    REQUIRE(std::find(asset.original_uri_aliases.begin(),
                      asset.original_uri_aliases.end(),
                      "./hero.png") != asset.original_uri_aliases.end());
    REQUIRE(ir.root.children[0].attributes.at("srcAssetId") == asset.asset_id);
    REQUIRE(ir.root.children[1].attributes.at("srcAssetId") == asset.asset_id);

    const auto round_trip = parse_design_ir_json(serialize_design_ir(ir));
    REQUIRE(round_trip.asset_manifest.assets.size() == 1);
    REQUIRE(round_trip.asset_manifest.assets[0].original_uri_aliases == asset.original_uri_aliases);
    REQUIRE(round_trip.root.children[1].attributes.at("srcAssetId") == asset.asset_id);
}

TEST_CASE("DesignIR asset manifest refresh rewrites stale asset ids",
          "[view][import][assets]") {
    TempDir tmp("pulp-design-ir-refresh-asset-ids");
    const auto asset_dir = tmp.path / "assets";
    fs::create_directories(asset_dir);
    write_text(asset_dir / "hero.png", "\x89PNG\r\n\x1a\nhero-bytes");

    auto ir = parse_design_ir_json(R"json({
        "type": "frame",
        "name": "Screen",
        "children": [
            { "type": "image", "name": "Hero", "src": "assets/hero.png" }
        ]
    })json");

    DesignIrAssetOptions unresolved_options;
    refresh_design_ir_asset_manifest(ir, unresolved_options);
    REQUIRE(ir.asset_manifest.assets.size() == 1);
    const auto stale_id = ir.root.children[0].attributes.at("srcAssetId");
    REQUIRE(has_diagnostic(ir.asset_manifest.assets[0], "asset-unresolved"));

    DesignIrAssetOptions resolved_options;
    resolved_options.base_directory = tmp.path;
    refresh_design_ir_asset_manifest(ir, resolved_options);

    REQUIRE(ir.asset_manifest.assets.size() == 1);
    const auto& resolved = ir.asset_manifest.assets[0];
    REQUIRE(resolved.local_path);
    REQUIRE(resolved.asset_id != stale_id);
    REQUIRE(ir.root.children[0].attributes.at("srcAssetId") == resolved.asset_id);
}

TEST_CASE("DesignIR asset manifest keeps distinct external assets with identical bytes",
          "[view][import][assets]") {
    TempDir tmp("pulp-design-ir-distinct-assets");
    write_text(tmp.path / "a.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>");
    write_text(tmp.path / "b.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>");

    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Distinct";
    IRNode first;
    first.type = "image";
    first.name = "A";
    first.attributes["src"] = "a.svg";
    IRNode second;
    second.type = "image";
    second.name = "B";
    second.attributes["src"] = "b.svg";
    ir.root.children.push_back(std::move(first));
    ir.root.children.push_back(std::move(second));

    DesignIrAssetOptions options;
    options.base_directory = tmp.path;
    refresh_design_ir_asset_manifest(ir, options);

    REQUIRE(ir.asset_manifest.assets.size() == 2);
    const IRAssetRef* a = nullptr;
    const IRAssetRef* b = nullptr;
    for (const auto& asset : ir.asset_manifest.assets) {
        if (asset.original_uri == "a.svg") a = &asset;
        if (asset.original_uri == "b.svg") b = &asset;
    }
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->content_hash == b->content_hash);
    REQUIRE(a->asset_id != b->asset_id);
    REQUIRE(ir.root.children[0].attributes.at("srcAssetId") == a->asset_id);
    REQUIRE(ir.root.children[1].attributes.at("srcAssetId") == b->asset_id);
}

TEST_CASE("DesignIR asset manifest records unresolved and network-gated diagnostics",
          "[view][import][assets]") {
    TempDir tmp("pulp-design-ir-asset-diagnostics");

    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Diagnostics";
    ir.root.style.background_image = "url(https://example.test/hero.png)";
    ir.root.attributes["src"] = "missing.png";

    DesignIrAssetOptions options;
    options.base_directory = tmp.path;
    auto manifest = collect_design_ir_assets(ir, options);

    REQUIRE(manifest.assets.size() == 2);
    bool saw_missing = false;
    bool saw_network = false;
    for (const auto& asset : manifest.assets) {
        if (asset.original_uri == "missing.png") {
            saw_missing = true;
            REQUIRE(has_diagnostic(asset, "asset-unresolved"));
            REQUIRE(asset.diagnostics[0].kind == ImportDiagnosticKind::unresolved_asset);
            REQUIRE(asset.content_hash.empty());
        } else if (asset.original_uri == "https://example.test/hero.png") {
            saw_network = true;
            REQUIRE(has_diagnostic(asset, "asset-network-fetch-disabled"));
            REQUIRE(asset.diagnostics[0].kind == ImportDiagnosticKind::unresolved_asset);
            REQUIRE(asset.content_hash.empty());
        }
    }
    REQUIRE(saw_missing);
    REQUIRE(saw_network);
}

TEST_CASE("DesignIR parser normalization promotes interactive frames from library APIs",
          "[view][import][diagnostics]") {
    const auto json = R"json({
        "version": 1,
        "source": "stitch",
        "root": {
            "type": "frame",
            "name": "Root",
            "children": [
                {
                    "type": "frame",
                    "name": "Click Me",
                    "attributes": { "role": "button" },
                    "children": []
                }
            ]
        }
    })json";

    auto ir = parse_design_ir_json(json);
    REQUIRE(ir.root.children.size() == 1);
    REQUIRE(ir.root.children[0].type == "button");
}

TEST_CASE("Interactive promotion ignores presentational cursor-only frames",
          "[view][import][diagnostics]") {
    IRNode node;
    node.type = "frame";
    node.name = "Decorative";
    node.style.cursor = "pointer";
    node.attributes["role"] = "presentation";

    REQUIRE(classify_interactive_signal(node) == WidgetPromotionSignal::none);
    REQUIRE(promote_interactive_frames(node) == 0);
    REQUIRE(node.type == "frame");
}

TEST_CASE("Interactive promotion runs before content-hash anchors",
          "[view][import][diagnostics]") {
    const std::string json = R"json({
        "type": "frame",
        "name": "Root",
        "children": [
            {
                "type": "frame",
                "name": "Click Me",
                "attributes": { "role": "button" },
                "children": []
            }
        ]
    })json";

    auto ir = parse_stitch_html(json);
    REQUIRE(ir.root.children.size() == 1);
    const auto& promoted = ir.root.children[0];
    REQUIRE(promoted.type == "button");

    const auto promoted_anchor = compute_anchor_id(
        promoted, /*parent_anchor=*/"", /*sibling_tag_index_for_path=*/0,
        /*depth=*/1, /*sig_index_for_content_hash=*/0,
        AnchorStrategy::content_hash);
    auto stale_frame = promoted;
    stale_frame.type = "frame";
    const auto stale_anchor = compute_anchor_id(
        stale_frame, /*parent_anchor=*/"", /*sibling_tag_index_for_path=*/0,
        /*depth=*/1, /*sig_index_for_content_hash=*/0,
        AnchorStrategy::content_hash);

    REQUIRE(promoted.stable_anchor_id == promoted_anchor);
    REQUIRE(promoted.stable_anchor_id != stale_anchor);
}

TEST_CASE("DesignIR diagnostics and provenance round trip canonical JSON",
          "[view][import][diagnostics]") {
    DesignIR ir;
    ir.version = 2;
    ir.source = DesignSource::jsx;
    ir.source_file = "panel.bundle.js";
    ir.capture_method = "runtime_snapshot";
    ir.settle_rounds = 4;
    ir.fallback_reason = "none";
    ir.source_adapter = "jsx-runtime";
    ir.source_version = "1";
    ir.imported_at = "2026-05-21T08:00:00Z";
    ir.root.type = "frame";
    ir.root.name = "Panel";

    ImportDiagnostic diagnostic;
    diagnostic.severity = ImportDiagnosticSeverity::warning;
    diagnostic.kind = ImportDiagnosticKind::snapshot_semantics_warning;
    diagnostic.code = "snapshot-dynamic-api";
    diagnostic.path = "<source>";
    diagnostic.message = "setInterval";
    diagnostic.anchor_id = "anchor-1";
    diagnostic.property = "onTick";
    ir.diagnostics.push_back(diagnostic);

    auto json = serialize_design_ir(ir);
    REQUIRE(json.find("\"capture_method\":\"runtime_snapshot\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"snapshot_semantics_warning\"") != std::string::npos);
    REQUIRE(json.find("\"anchor_id\":\"anchor-1\"") != std::string::npos);
    REQUIRE(json.find("\"property\":\"onTick\"") != std::string::npos);

    auto reparsed = parse_design_ir_json(json);
    REQUIRE(reparsed.version == 2);
    REQUIRE(reparsed.capture_method == "runtime_snapshot");
    REQUIRE(reparsed.settle_rounds == 4);
    REQUIRE(reparsed.source_adapter == "jsx-runtime");
    REQUIRE(reparsed.imported_at == "2026-05-21T08:00:00Z");
    REQUIRE(reparsed.diagnostics.size() == 1);
    REQUIRE(reparsed.diagnostics[0].kind == ImportDiagnosticKind::snapshot_semantics_warning);
    REQUIRE(reparsed.diagnostics[0].anchor_id == "anchor-1");
    REQUIRE(reparsed.diagnostics[0].property == "onTick");
}

TEST_CASE("DesignIR diagnostic kinds parse and serialize every normalized bucket",
          "[view][import][diagnostics]") {
    const auto parsed = parse_design_ir_json(R"json({
        "version": 1,
        "source": "jsx",
        "captureMethod": "runtime_snapshot",
        "settleRounds": 2,
        "root": { "type": "frame", "name": "Diagnostics" },
        "diagnostics": [
            {
                "severity": "info",
                "kind": "legacy_field_shortcut",
                "code": "legacy-ir",
                "path": "<root>",
                "message": "legacy shortcut"
            },
            {
                "severity": "warning",
                "kind": "capture_partial",
                "code": "capture-partial",
                "path": "<capture>",
                "message": "partial capture"
            },
            {
                "severity": "error",
                "kind": "fallback_used",
                "code": "runtime-fallback",
                "path": "<runtime>",
                "message": "runtime fallback"
            },
            {
                "severity": "warning",
                "code": "asset-fetch-failed",
                "path": "https://example.test/asset.svg",
                "message": "asset failed"
            },
            {
                "severity": "warning",
                "code": "snapshot-dynamic-api",
                "path": "<source>",
                "message": "Date.now"
            },
            {
                "severity": "warning",
                "code": "fallback-used",
                "path": "<root>",
                "message": "fallback"
            },
            {
                "severity": "warning",
                "code": "unknown-code",
                "path": "<root>",
                "message": "unknown"
            },
            {
                "severity": "warning",
                "kind": "unsupported_node",
                "code": "slice-skipped",
                "path": "12:34",
                "message": "SLICE skipped"
            },
            {
                "severity": "warning",
                "code": "unsupported-node",
                "path": "12:35",
                "message": "STICKY skipped"
            }
        ]
    })json");

    REQUIRE(parsed.capture_method == "runtime_snapshot");
    REQUIRE(parsed.settle_rounds == 2);
    REQUIRE(parsed.diagnostics.size() == 9);
    REQUIRE(parsed.diagnostics[0].kind == ImportDiagnosticKind::legacy_field_shortcut);
    REQUIRE(parsed.diagnostics[1].kind == ImportDiagnosticKind::capture_partial);
    REQUIRE(parsed.diagnostics[2].severity == ImportDiagnosticSeverity::error);
    REQUIRE(parsed.diagnostics[2].kind == ImportDiagnosticKind::fallback_used);
    REQUIRE(parsed.diagnostics[3].kind == ImportDiagnosticKind::unresolved_asset);
    REQUIRE(parsed.diagnostics[4].kind == ImportDiagnosticKind::snapshot_semantics_warning);
    REQUIRE(parsed.diagnostics[5].kind == ImportDiagnosticKind::fallback_used);
    REQUIRE(parsed.diagnostics[6].kind == ImportDiagnosticKind::unknown);
    // The node-dispatch taxonomy: explicit `kind` parses, and a kind-less
    // producer entry classifies from its dispatch code.
    REQUIRE(parsed.diagnostics[7].kind == ImportDiagnosticKind::unsupported_node);
    REQUIRE(parsed.diagnostics[8].kind == ImportDiagnosticKind::unsupported_node);

    const auto json = serialize_design_ir(parsed);
    REQUIRE(json.find("\"kind\":\"legacy_field_shortcut\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"capture_partial\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"fallback_used\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"unresolved_asset\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"snapshot_semantics_warning\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"unknown\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"unsupported_node\"") != std::string::npos);
    REQUIRE(json.find("\"severity\":\"error\"") != std::string::npos);
}

TEST_CASE("apply_param_binding_manifest binds descriptive knobs by node id, sigil wins",
          "[view][import][param-binding]") {
    using namespace pulp::view;
    IRNode root;

    // A descriptively-named knob: has provenance, no key yet → manifest binds it.
    IRInteractiveElement descriptive;
    descriptive.kind = InteractiveElementKind::knob;
    descriptive.source_node_id = "10:1";
    root.interactive_elements.push_back(descriptive);

    // A sigil-bound knob: already has a key → manifest must NOT overwrite it.
    IRInteractiveElement sigiled;
    sigiled.kind = InteractiveElementKind::knob;
    sigiled.source_node_id = "10:2";
    sigiled.param_key = "explicit.sigil";
    root.interactive_elements.push_back(sigiled);

    // A knob whose node id is absent from the manifest → untouched.
    IRInteractiveElement unmapped;
    unmapped.kind = InteractiveElementKind::knob;
    unmapped.source_node_id = "10:3";
    root.interactive_elements.push_back(unmapped);

    // A knob with no provenance at all → nothing to key on.
    IRInteractiveElement anon;
    anon.kind = InteractiveElementKind::knob;
    root.interactive_elements.push_back(anon);

    // A knob nested in a child node → recursion must reach it.
    IRNode child;
    IRInteractiveElement nested;
    nested.kind = InteractiveElementKind::knob;
    nested.source_node_id = "10:4";
    child.interactive_elements.push_back(nested);
    root.children.push_back(child);

    std::unordered_map<std::string, std::string> manifest{
        {"10:1", "filter.cutoff"},
        {"10:2", "should.be.ignored"},   // sigil wins
        {"10:4", "lfo.rate"},
        {"10:9", "no.such.node"},        // no matching element
    };

    const int bound = apply_param_binding_manifest(root, manifest);
    REQUIRE(bound == 2);  // descriptive (10:1) + nested (10:4); sigil untouched

    REQUIRE(root.interactive_elements[0].param_key == "filter.cutoff");
    REQUIRE(root.interactive_elements[1].param_key == "explicit.sigil");  // NOT overwritten
    REQUIRE(root.interactive_elements[2].param_key.empty());              // unmapped
    REQUIRE(root.interactive_elements[3].param_key.empty());              // no provenance
    REQUIRE(root.children[0].interactive_elements[0].param_key == "lfo.rate");
}

TEST_CASE("apply_param_binding_manifest skips empty manifest values and empty map",
          "[view][import][param-binding]") {
    using namespace pulp::view;
    IRNode root;
    IRInteractiveElement knob;
    knob.kind = InteractiveElementKind::knob;
    knob.source_node_id = "10:1";
    root.interactive_elements.push_back(knob);

    REQUIRE(apply_param_binding_manifest(root, {}) == 0);
    REQUIRE(root.interactive_elements[0].param_key.empty());

    // An empty-string value is a no-op (never binds a control to "").
    REQUIRE(apply_param_binding_manifest(root, {{"10:1", ""}}) == 0);
    REQUIRE(root.interactive_elements[0].param_key.empty());
}

TEST_CASE("parse_param_binding_manifest_json reads a node-id → key object",
          "[view][import][param-binding]") {
    using namespace pulp::view;
    std::string err;

    auto ok = parse_param_binding_manifest_json(
        R"({"10:1": "filter.cutoff", "10:2": "lfo.rate", "blank": "", "10:3": 5})",
        &err);
    REQUIRE(ok.has_value());
    REQUIRE(ok->size() == 2);              // blank value + non-string dropped leniently
    REQUIRE((*ok)["10:1"] == "filter.cutoff");
    REQUIRE((*ok)["10:2"] == "lfo.rate");
    REQUIRE(ok->count("blank") == 0);
    REQUIRE(ok->count("10:3") == 0);

    // Malformed JSON → nullopt with an error message.
    err.clear();
    auto bad = parse_param_binding_manifest_json("{not json", &err);
    REQUIRE_FALSE(bad.has_value());
    REQUIRE_FALSE(err.empty());

    // A non-object root (array) → nullopt.
    auto arr = parse_param_binding_manifest_json(R"(["10:1"])", &err);
    REQUIRE_FALSE(arr.has_value());
}

TEST_CASE("a path's gradient paint survives the IR JSON round-trip",
          "[view][import][svg]") {
    using namespace pulp::view;

    // A vector's gradient rides on the path, not on the box behind it, so it
    // travels as its own `fillGradient` field rather than as a background. The
    // decoder writes the field and the bridge has setSvgFillGradient; this
    // asserts the middle link, which is the one that has repeatedly been the
    // unplugged end of a chain like this. The node is named "Wedge", not
    // "Dial": a vector named for a knob is recognized INTO a knob widget and
    // never reaches the svg branch at all.
    const auto ir = parse_design_ir_json(R"JSON({
      "version": 1, "source": "figma",
      "root": {
        "type": "frame", "name": "Panel",
        "children": [{
          "type": "path", "name": "Wedge",
          "pathData": "M0 0 L10 0 L10 10 Z",
          "viewBox": "0 0 10 10",
          "fill": "#ff0000",
          "fillGradient": "linear-gradient(180deg, #ffffff 0%, #000000 100%)"
        }]
      }
    })JSON");

    REQUIRE(ir.root.children.size() == 1);
    const auto& dial = ir.root.children[0].attributes;

    // Carried BESIDE the solid, not instead of it: the widget falls back to the
    // solid when the gradient string won't parse, which needs both present.
    REQUIRE(dial.count("svg_fill_gradient") == 1);
    REQUIRE(dial.at("svg_fill_gradient") ==
            "linear-gradient(180deg, #ffffff 0%, #000000 100%)");
    REQUIRE(dial.at("svg_fill") == "#ff0000");

    // And it reaches the generated JS — a parsed attribute nothing emits is
    // the same silent drop as never parsing it.
    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);
    REQUIRE(js.find("setSvgFillGradient(") != std::string::npos);
    REQUIRE(js.find("linear-gradient(180deg, #ffffff 0%, #000000 100%)") !=
            std::string::npos);
}

TEST_CASE("a control's art layers stay art, and are not each promoted to a control",
          "[view][import][audio-widget]") {
    using namespace pulp::view;

    // A designer's knob is a frame named for the control, holding art layers:
    // "knob base", "knob indicator", "knob ring". Every one of those tokenizes
    // to {knob, …} and matched the name heuristic, so ONE designer knob promoted
    // to THREE stacked built-in knobs, each painting Pulp's stock skin over the
    // art it was meant to be.
    const auto ir = parse_design_ir_json(R"JSON({
      "version": 1, "source": "figma",
      "root": {
        "type": "frame", "name": "Panel",
        "children": [{
          "type": "frame", "name": "sound / knob / small unipolar",
          "children": [
            {"type": "ellipse", "name": "knob base"},
            {"type": "frame",   "name": "knob indicator"},
            {"type": "vector",  "name": "knob ring", "pathData": "M0 0 L10 0 L10 10 Z"}
          ]
        }]
      }
    })JSON");

    const auto& knob = ir.root.children.at(0);
    REQUIRE(knob.children.size() == 3);
    for (const auto& layer : knob.children)
        REQUIRE(layer.audio_widget == AudioWidgetType::none);

    // And the art survives to the render rather than being replaced: the ring
    // lowers to its own path.
    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);
    REQUIRE(js.find("createKnob(") == std::string::npos);
    REQUIRE(js.find("setWidgetStyle(") == std::string::npos);
    REQUIRE(js.find("createSvgPath(") != std::string::npos);
}

TEST_CASE("a control-named row does not swallow the controls inside it",
          "[view][import][audio-widget]") {
    using namespace pulp::view;

    // The other side of the rule. "KnobRow" tokenizes to {knob, row} and matches
    // too, and so do the knobs inside it — so the suppression has to tell a
    // control from a layer by something other than the name that made them look
    // alike. Each of these brings art of its own; that is the difference.
    const auto ir = parse_design_ir_json(R"JSON({
      "version": 1, "source": "figma",
      "root": {
        "type": "frame", "name": "Panel",
        "children": [{
          "type": "frame", "name": "KnobRow",
          "children": [
            {"type": "frame", "name": "Knob 1",
             "children": [{"type": "ellipse", "name": "body"}]},
            {"type": "frame", "name": "Knob 2",
             "children": [{"type": "ellipse", "name": "body"}]}
          ]
        }]
      }
    })JSON");

    const auto& row = ir.root.children.at(0);
    REQUIRE(row.audio_widget == AudioWidgetType::none);   // a row of knobs is a row
    REQUIRE(row.children.at(0).audio_widget == AudioWidgetType::knob);
    REQUIRE(row.children.at(1).audio_widget == AudioWidgetType::knob);
}

TEST_CASE("a differently-named control inside a control keeps its promotion",
          "[view][import][audio-widget]") {
    using namespace pulp::view;

    // Only layers of the SAME kind are treated as that control's art. A fader
    // sitting inside a knob-named frame is its own control, not a knob's layer.
    const auto ir = parse_design_ir_json(R"JSON({
      "version": 1, "source": "figma",
      "root": {
        "type": "frame", "name": "Panel",
        "children": [{
          "type": "frame", "name": "Knob Panel",
          "children": [
            {"type": "ellipse", "name": "knob base"},
            {"type": "frame",   "name": "Fader"}
          ]
        }]
      }
    })JSON");

    const auto& panel = ir.root.children.at(0);
    REQUIRE(panel.children.at(0).audio_widget == AudioWidgetType::none);   // layer
    REQUIRE(panel.children.at(1).audio_widget == AudioWidgetType::fader);  // control
}
// ── interactive_element `kind`: the wire contract ────────────────────────────

TEST_CASE("every interactive kind survives the wire round-trip",
          "[view][import][ir-v1]") {
    // The IR reader maps an UNRECOGNIZED kind to `knob`. That makes a kind
    // dropped from (or misspelled in) interactive_kind_from_id indistinguishable
    // from a working import at the type level: a real control quietly becomes a
    // knob. The existing round-trip case pins three kinds; the coercion applies
    // to all of them, so pin the whole enum. Extend this list when the enum
    // grows — a new kind that serializes but does not parse lands here.
    // The wire id is spelled out rather than read back from the writer, so a
    // rename that changes BOTH sides in step still fails here — the string is
    // the cross-tool contract (the Figma plugin's schema and the REST exporter
    // emit it), not an internal detail.
    const struct { InteractiveElementKind kind; const char* wire; } kAll[] = {
        {InteractiveElementKind::knob, "knob"},
        {InteractiveElementKind::fader, "fader"},
        {InteractiveElementKind::toggle, "toggle"},
        {InteractiveElementKind::dropdown, "dropdown"},
        {InteractiveElementKind::text_field, "text_field"},
        {InteractiveElementKind::tab_group, "tab_group"},
        {InteractiveElementKind::stepper, "stepper"},
        {InteractiveElementKind::swap, "swap"},
        {InteractiveElementKind::action, "action"},
        {InteractiveElementKind::xy_pad, "xy_pad"},
        {InteractiveElementKind::value_label, "value_label"},
        {InteractiveElementKind::custom, "custom"},
    };

    for (const auto& [kind, wire] : kAll) {
        DesignIR ir;
        ir.source = DesignSource::figma;
        ir.root.type = "frame";
        ir.root.render_mode = NodeRenderMode::faithful_svg;
        ir.root.svg_asset_id = "asset-svg";

        IRInteractiveElement el;
        el.kind = kind;
        el.x = 10; el.y = 20; el.w = 30; el.h = 40;
        el.source_node_id = "1:1";
        ir.root.interactive_elements.push_back(el);

        // Named in the failure message, since `kind` is an opaque int here.
        INFO("kind wire id: " << wire);
        const auto canonical = serialize_design_ir(ir);
        // The kind reached the wire under its documented id...
        CHECK(canonical.find(std::string("\"kind\":\"") + wire + "\"") != std::string::npos);
        // ...and came back as itself, not as a coerced knob.
        const auto parsed = parse_design_ir_json(canonical);
        REQUIRE(parsed.root.interactive_elements.size() == 1);
        CHECK(parsed.root.interactive_elements[0].kind == kind);
    }
}

TEST_CASE("an unrecognized interactive kind degrades to knob rather than failing the load",
          "[view][import][ir-v1]") {
    // Deliberate, and deliberately NOT symmetric with the annotated-capture
    // tool, which hard-errors on an unknown kind. The tool AUTHORS the IR, so
    // refusing bad input costs one re-run. This is the RUNTIME reader, which
    // may be handed a file written by a newer importer: hard-failing here would
    // drop a whole design because one element used a kind this build predates.
    // The element is not accepted silently — parse_ir_interactive_element logs
    // the unknown kind and its source node — but the load survives.
    // Built from the serializer and then retagged, so the envelope is exactly
    // the shape the reader expects and `kind` is the only unusual thing in it.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.render_mode = NodeRenderMode::faithful_svg;
    ir.root.svg_asset_id = "asset-svg";
    IRInteractiveElement el;
    el.kind = InteractiveElementKind::fader;   // a real kind, retagged below
    el.x = 10; el.y = 20; el.w = 30; el.h = 40;
    el.source_node_id = "1:1";
    ir.root.interactive_elements.push_back(el);

    std::string json = serialize_design_ir(ir);
    const auto at = json.find("\"kind\":\"fader\"");
    REQUIRE(at != std::string::npos);
    json.replace(at, std::string("\"kind\":\"fader\"").size(),
                 "\"kind\":\"holographic_slider\"");

    const auto parsed = parse_design_ir_json(json);
    REQUIRE(parsed.root.interactive_elements.size() == 1);
    CHECK(parsed.root.interactive_elements[0].kind == InteractiveElementKind::knob);
    // The geometry still lands, so the fallback renders where the design put it.
    CHECK(parsed.root.interactive_elements[0].x == 10);
    CHECK(parsed.root.interactive_elements[0].w == 30);
}
// ── Captured-art knob cleaned-disc asset durability ─────────────────────

namespace {

// Build a one-node IR whose knob carries the captured-art metadata that makes
// enrich re-encode a cleaned disc: an asset_ref resolving to `png`, a
// render_bounds (gates the opaque-core pass) and knob_ind_r_out (gates the
// indicator clean).
DesignIR make_captured_art_knob_ir(const fs::path& png) {
    DesignIR ir;
    ir.root.type = "knob";
    ir.root.attributes["asset_ref"] = "knob_body";
    ir.root.attributes["knob_ind_r_out"] = "0.9";
    ir.root.style.width = 60.0f;
    ir.root.style.height = 60.0f;
    ir.root.style.render_bounds = IRStyle::RenderBounds{60.0f, 60.0f, 0.0f, 0.0f};

    IRAssetRef asset;
    asset.asset_id = "knob_body";
    asset.original_uri = "knob_body.png";
    asset.local_path = png.string();
    ir.asset_manifest.assets.push_back(asset);
    return ir;
}

std::string enriched_knob_asset_path(const fs::path& png) {
    DesignIR ir = make_captured_art_knob_ir(png);
    enrich_imported_image_asset_metadata(ir, ir.asset_manifest);
    return ir.root.attributes["asset_path"];
}

bool is_under(const fs::path& path, const fs::path& dir) {
    const auto rel = path.lexically_relative(dir);
    return !rel.empty() && *rel.begin() != "..";
}

} // namespace

TEST_CASE("captured-art knob cleaned disc lands in the durable asset cache",
          "[view][import][knob-art]") {
    // The cleaned-disc path is serialized into asset_path and reloaded at
    // RUNTIME by a shipped baked UI. Writing it under the OS temp dir means a
    // temp sweep or reboot silently unskins the knob with zero diagnostics.
    TempDir source("pulp-knobclean-src");
    const fs::path png = source.path / "knob.png";
    write_text(png, read_fixture("test/fixtures/imports/figma-plugin/synthetic-knob.png"));

    // Route the durable cache to a directory that is NOT under the OS temp dir
    // so the "escaped temp" assertion is about the code, not the environment.
    // The ctest working directory is the build tree; assert that premise rather
    // than let a temp-hosted working dir quietly void the check below.
    REQUIRE_FALSE(is_under(fs::current_path(), fs::temp_directory_path()));
    // Unique per-invocation directory: the fixed "knobclean-home" name is
    // shared state between concurrent runs of this binary from the same
    // working directory — one process's remove_all below races another's
    // CHECK(fs::exists(emitted)) and deletes the emitted disc under it.
    const fs::path home = fs::current_path() /
        ("knobclean-home-" + source.path.filename().string());
    fs::create_directories(home);
    ScopedEnvVar cache("PULP_IMPORT_ASSET_CACHE", (home / "import-assets").string());

    const std::string emitted = enriched_knob_asset_path(png);
    REQUIRE_FALSE(emitted.empty());
    // The clean actually ran (a plain passthrough would keep the source path).
    REQUIRE(fs::path(emitted) != png);
    CHECK(fs::exists(emitted));
    CHECK_FALSE(is_under(fs::path(emitted), fs::temp_directory_path()));
    CHECK(is_under(fs::path(emitted), home));

    std::error_code ec;
    fs::remove_all(home, ec);
}

TEST_CASE("captured-art knob cleaned disc filename is content-addressed",
          "[view][import][knob-art]") {
    // std::hash is implementation-defined and unstable across runs/compilers,
    // and keying on the source path makes the same bytes cache twice. The rest
    // of the asset pipeline is sha256 content-addressed; this path must match.
    TempDir source("pulp-knobclean-addr");
    const auto knob_png = read_fixture("test/fixtures/imports/figma-plugin/synthetic-knob.png");
    const fs::path a = source.path / "a" / "knob.png";
    const fs::path b = source.path / "b" / "renamed.png";
    write_text(a, knob_png);
    write_text(b, knob_png);

    const fs::path other = source.path / "other.png";
    write_text(other, read_fixture("test/fixtures/import-fidelity/assets/knob_ref.png"));

    ScopedEnvVar cache("PULP_IMPORT_ASSET_CACHE",
                       (source.path / "cache").string());

    // Same bytes, different source paths → one content-addressed name.
    const std::string name_a = fs::path(enriched_knob_asset_path(a)).filename().string();
    const std::string name_b = fs::path(enriched_knob_asset_path(b)).filename().string();
    REQUIRE_FALSE(name_a.empty());
    CHECK(name_a == name_b);

    // Different bytes → a different name (no collision, no stale reuse).
    const std::string name_other =
        fs::path(enriched_knob_asset_path(other)).filename().string();
    REQUIRE_FALSE(name_other.empty());
    CHECK(name_other != name_a);
}

// ── Layer blend mode: the shared supported-blend table ─────────────────
//
// The producer side lives in the three lanes (fig/scene.mjs, the plugin's
// extract-pure.ts, figma_rest_export.py); the consumer side is
// is_supported_blend_keyword + validate_blend_modes here. Two channels reach
// the parser: normalized `style.mixBlendMode` (CSS sources, and the lanes'
// own lowering) and the raw `figma.blend_mode` provenance block (promoted
// only when supported — the producer already diagnosed unmappable raws).

TEST_CASE("figma-block blend promotion honors the supported-blend table",
          "[view][import][parse][blend]") {
    // A supported raw mode promotes to the CSS keyword (plugin + REST
    // envelopes carry the raw Figma spelling in the figma block).
    const auto ir = parse_design_ir_json(R"({
      "version": 1, "source": "figma",
      "root": {"type": "frame", "name": "Root", "children": [
        {"type": "frame", "name": "noise",
         "figma": {"blend_mode": "MULTIPLY"}},
        {"type": "frame", "name": "soft",
         "figma": {"blend_mode": "SOFT_LIGHT"}},
        {"type": "frame", "name": "plain",
         "figma": {"blend_mode": "NORMAL"}},
        {"type": "frame", "name": "burn",
         "figma": {"blend_mode": "LINEAR_BURN"}}
      ]}
    })");
    REQUIRE(ir.root.children.size() == 4);
    CHECK(ir.root.children[0].style.mix_blend_mode == "multiply");
    CHECK(ir.root.children[1].style.mix_blend_mode == "soft-light");
    CHECK_FALSE(ir.root.children[2].style.mix_blend_mode.has_value());
    // An unmappable raw mode must NOT reach the style channel: "linear-burn"
    // is invalid CSS on the web path and a silent normal-fallback on the
    // native path. The producer that wrote the raw value also wrote its own
    // blend-unsupported diagnostic, so the consumer stays quiet here rather
    // than double-diagnosing the same loss.
    CHECK_FALSE(ir.root.children[3].style.mix_blend_mode.has_value());
    CHECK_FALSE(has_import_diagnostic(ir.diagnostics, "blend-unsupported"));
}

TEST_CASE("an unsupported style.mixBlendMode is cleared and diagnosed",
          "[view][import][parse][blend]") {
    // The normalized style channel is a contract: hand-authored IR (or a
    // buggy adapter) writing a keyword outside the supported-blend table gets
    // it cleared WITH a blend-unsupported diagnostic — never passed through
    // as invalid CSS, never silently dropped.
    const auto ir = parse_design_ir_json(R"({
      "version": 1, "source": "figma",
      "root": {"type": "frame", "name": "Root", "children": [
        {"type": "frame", "name": "ok",
         "style": {"mixBlendMode": "screen"}},
        {"type": "frame", "name": "spelled",
         "style": {"mixBlendMode": "SOFT_LIGHT"}},
        {"type": "frame", "name": "additive",
         "style": {"mixBlendMode": "plus-lighter"}},
        {"type": "frame", "name": "snake",
         "style": {"mix_blend_mode": "multiply"}},
        {"type": "frame", "name": "bad", "id": "9:9",
         "style": {"mixBlendMode": "linear-burn"}}
      ]}
    })");
    REQUIRE(ir.root.children.size() == 5);
    CHECK(ir.root.children[0].style.mix_blend_mode == "screen");
    // UPPER_SNAKE normalizes through the same spelling transform.
    CHECK(ir.root.children[1].style.mix_blend_mode == "soft-light");
    // The CSS Compositing L2 additive pair stays supported for CSS-authored
    // sources — the native bridge maps both plus-* keywords to kPlus.
    CHECK(ir.root.children[2].style.mix_blend_mode == "plus-lighter");
    // The plugin/REST lanes emit the snake_case spelling; resolve_key reads it.
    CHECK(ir.root.children[3].style.mix_blend_mode == "multiply");
    CHECK_FALSE(ir.root.children[4].style.mix_blend_mode.has_value());

    REQUIRE(has_import_diagnostic(ir.diagnostics, "blend-unsupported"));
    const auto it = std::find_if(
        ir.diagnostics.begin(), ir.diagnostics.end(),
        [](const ImportDiagnostic& d) { return d.code == "blend-unsupported"; });
    CHECK(it->severity == ImportDiagnosticSeverity::warning);
    CHECK(it->kind == ImportDiagnosticKind::unsupported_property);
    CHECK(it->path == "9:9");
    CHECK(it->message.find("linear-burn") != std::string::npos);
}
