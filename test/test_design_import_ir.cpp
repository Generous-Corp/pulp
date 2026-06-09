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

    const auto& k1 = parsed.root.interactive_elements[1];
    REQUIRE(k1.svg_patch_d.empty());
    REQUIRE_FALSE(k1.source_node_id.has_value());
    REQUIRE(k1.default_value == 0.5f);
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
            }
        ]
    })json");

    REQUIRE(parsed.capture_method == "runtime_snapshot");
    REQUIRE(parsed.settle_rounds == 2);
    REQUIRE(parsed.diagnostics.size() == 7);
    REQUIRE(parsed.diagnostics[0].kind == ImportDiagnosticKind::legacy_field_shortcut);
    REQUIRE(parsed.diagnostics[1].kind == ImportDiagnosticKind::capture_partial);
    REQUIRE(parsed.diagnostics[2].severity == ImportDiagnosticSeverity::error);
    REQUIRE(parsed.diagnostics[2].kind == ImportDiagnosticKind::fallback_used);
    REQUIRE(parsed.diagnostics[3].kind == ImportDiagnosticKind::unresolved_asset);
    REQUIRE(parsed.diagnostics[4].kind == ImportDiagnosticKind::snapshot_semantics_warning);
    REQUIRE(parsed.diagnostics[5].kind == ImportDiagnosticKind::fallback_used);
    REQUIRE(parsed.diagnostics[6].kind == ImportDiagnosticKind::unknown);

    const auto json = serialize_design_ir(parsed);
    REQUIRE(json.find("\"kind\":\"legacy_field_shortcut\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"capture_partial\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"fallback_used\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"unresolved_asset\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"snapshot_semantics_warning\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"unknown\"") != std::string::npos);
    REQUIRE(json.find("\"severity\":\"error\"") != std::string::npos);
}
