#include "test_design_import_shared.hpp"

// ──────────────────────────────────────────────────────────────────────────
// #43a-rev — `font_family_assets` top-level envelope field.
//
// Plugin emits a deduplicated catalogue of (family, style, weight, italic)
// tuples for every font referenced by text nodes. Runtime #43b consumes
// via Skia's SkFontMgr system-font matcher with the
// bundled OFL set as fallback.
//
// This test pins the contract that the C++ parser TOLERATES the new
// field without error. Today the parser doesn't yet bind it to IR
// fields — that's #43b — but the unknown-field tolerance is the
// invariant we need now so a v0.4.x plugin emitting the field can be
// consumed by older parsers gracefully.

TEST_CASE("parse_figma_plugin_json tolerates font_family_assets top-level field (issue-43a-rev)",
          "[view][import][figma-plugin][issue-43a-rev]") {
    const std::string envelope = R"JSON({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "v1",
        "font_family_assets": [
            { "family": "Inter", "style": "Regular", "weight": 400 },
            { "family": "Inter", "style": "Semi Bold", "weight": 600 },
            { "family": "Inter", "style": "Italic", "weight": 400, "italic": true },
            { "family": "Clash Grotesk", "style": "Medium", "weight": 500,
              "asset_id": "font-1a2b3c4d" }
        ],
        "root": {
            "type": "frame",
            "name": "Typed",
            "style": { "width": 200, "height": 100, "font_family": "Inter" },
            "children": [
                {
                    "type": "text",
                    "name": "title",
                    "content": "Hello",
                    "style": { "font_family": "Inter", "font_weight": 600 },
                    "children": []
                }
            ]
        }
    })JSON";
    // The whole point of the test: the parser must NOT throw on the new
    // top-level field. Behaviour beyond that is #43b territory.
    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(ir.source == DesignSource::figma_plugin);
    REQUIRE(ir.root.name == "Typed");
    REQUIRE(ir.root.children.size() == 1);
    REQUIRE(ir.root.children.front().name == "title");
}

// ── box-shadow promotion (pulp #41) ─────────────────────────────────────
// Before #41 IRStyle::box_shadow was a single opaque string; every layer past
// the first was silently dropped. These cover the parse/serialize helpers and
// the JSON round-trip that preserves a multi-layer stack.

TEST_CASE("parse_css_box_shadow parses a single drop-shadow layer", "[design-import][issue-41]") {
    auto layers = parse_css_box_shadow("0px 2px 4px rgba(0, 0, 0, 0.5)");
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].offset_x == Catch::Approx(0.0f));
    CHECK(layers[0].offset_y == Catch::Approx(2.0f));
    CHECK(layers[0].blur == Catch::Approx(4.0f));
    CHECK(layers[0].spread == Catch::Approx(0.0f));
    CHECK(layers[0].inset == false);
    // The comma INSIDE rgba(...) must NOT split the value into extra layers.
    CHECK(layers[0].color == "rgba(0, 0, 0, 0.5)");
}

TEST_CASE("parse_css_box_shadow keeps every comma-separated layer", "[design-import][issue-41]") {
    auto layers = parse_css_box_shadow(
        "0px 2px 4px #000000, 0px 8px 16px rgba(10, 20, 30, 0.4)");
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].color == "#000000");
    CHECK(layers[0].offset_y == Catch::Approx(2.0f));
    CHECK(layers[1].offset_y == Catch::Approx(8.0f));
    CHECK(layers[1].blur == Catch::Approx(16.0f));
    CHECK(layers[1].color == "rgba(10, 20, 30, 0.4)");
}

TEST_CASE("parse_css_box_shadow handles inset + spread + named color", "[design-import][issue-41]") {
    auto layers = parse_css_box_shadow("inset 10px 20px 30px 5px black");
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].inset == true);
    CHECK(layers[0].offset_x == Catch::Approx(10.0f));
    CHECK(layers[0].offset_y == Catch::Approx(20.0f));
    CHECK(layers[0].blur == Catch::Approx(30.0f));
    CHECK(layers[0].spread == Catch::Approx(5.0f));
    CHECK(layers[0].color == "black");
}

TEST_CASE("parse_css_box_shadow treats empty / none as no shadow", "[design-import][issue-41]") {
    CHECK(parse_css_box_shadow("").empty());
    CHECK(parse_css_box_shadow("   ").empty());
    CHECK(parse_css_box_shadow("none").empty());
    CHECK(parse_css_box_shadow("NONE").empty());
}

TEST_CASE("box_shadow_to_css round-trips a multi-layer stack losslessly", "[design-import][issue-41]") {
    const std::string css = "0px 2px 4px #000000, inset 0px 8px 16px rgba(10, 20, 30, 0.4)";
    auto layers = parse_css_box_shadow(css);
    REQUIRE(layers.size() == 2);
    // raw preservation means the serialized form matches the trimmed input.
    CHECK(box_shadow_to_css(layers) == css);
}

TEST_CASE("design IR JSON round-trip preserves a multi-shadow boxShadow", "[design-import][issue-41]") {
    auto parsed = parse_design_ir_json(R"json({
        "version": 2,
        "root": {
            "type": "frame",
            "name": "Panel",
            "style": { "boxShadow": "0px 2px 4px #000000, 0px 8px 16px rgba(10, 20, 30, 0.4)" }
        }
    })json");
    REQUIRE(parsed.root.style.box_shadow.size() == 2);
    CHECK(parsed.root.style.box_shadow[1].offset_y == Catch::Approx(8.0f));

    // Serialize → re-parse: both layers survive (the bug #41 fixes).
    const auto canonical = serialize_design_ir(parsed);
    const auto reparsed = parse_design_ir_json(canonical);
    REQUIRE(reparsed.root.style.box_shadow.size() == 2);
    CHECK(reparsed.root.style.box_shadow[0].color == "#000000");
    CHECK(reparsed.root.style.box_shadow[1].color == "rgba(10, 20, 30, 0.4)");
}

// ── #43b: bundled-font registration from font_family_assets ──────────────
TEST_CASE("parse_figma_plugin_json reads font_family_assets", "[design-import][issue-43b]") {
    auto ir = parse_figma_plugin_json(R"json({
        "format_version": "2026.05-figma-plugin-v1",
        "provenance": {"adapter": "figma-plugin"},
        "asset_manifest": {"version": 1, "assets": [
            {"asset_id": "userfont-1", "local_path": "assets/clash.ttf", "mime": "font/ttf"}]},
        "font_family_assets": [
            {"family": "Clash Grotesk", "style": "Regular", "weight": 500, "asset_id": "userfont-1"}],
        "root": {"type": "frame", "name": "R", "figma_node_id": "1:1",
                 "style": {"fontFamily": "Clash Grotesk"}}
    })json");
    REQUIRE(ir.font_family_assets.size() == 1);
    CHECK(ir.font_family_assets[0].family == "Clash Grotesk");
    CHECK(ir.font_family_assets[0].weight == 500);
    CHECK(ir.font_family_assets[0].asset_id == "userfont-1");
}

TEST_CASE("codegen emits registerFont before setFontFamily for bundled fonts", "[design-import][issue-43b]") {
    DesignIR ir;
    ir.source = DesignSource::figma_plugin;
    ir.root.type = "frame";
    ir.root.name = "R";
    ir.root.style.font_family = "Clash Grotesk";
    IRFontAsset fa;
    fa.family = "Clash Grotesk";
    fa.weight = 500;
    fa.asset_id = "userfont-1";
    fa.resolved_path = "/tmp/assets/clash.ttf";  // CLI normally stamps this
    ir.font_family_assets.push_back(fa);

    const auto js = generate_pulp_js(ir);
    const auto reg = js.find("registerFont('Clash Grotesk', '/tmp/assets/clash.ttf')");
    REQUIRE(reg != std::string::npos);
    // registerFont must precede any setFontFamily so the family resolves to the
    // bundled face, not a system fallback.
    const auto setf = js.find("setFontFamily");
    if (setf != std::string::npos) CHECK(reg < setf);
    // An unresolved font asset (no path) must NOT emit registerFont.
    DesignIR ir2 = ir;
    ir2.font_family_assets[0].resolved_path.clear();
    CHECK(generate_pulp_js(ir2).find("registerFont(") == std::string::npos);
}

TEST_CASE("design IR JSON round-trip preserves font_family_assets", "[design-import][issue-43b]") {
    DesignIR ir;
    ir.root.type = "frame"; ir.root.name = "R";
    IRFontAsset fa; fa.family = "Inter"; fa.weight = 400; fa.asset_id = "uf-2";
    fa.resolved_path = "/x/inter.ttf";
    ir.font_family_assets.push_back(fa);
    const auto reparsed = parse_design_ir_json(serialize_design_ir(ir));
    REQUIRE(reparsed.font_family_assets.size() == 1);
    CHECK(reparsed.font_family_assets[0].family == "Inter");
    CHECK(reparsed.font_family_assets[0].weight == 400);
    CHECK(reparsed.font_family_assets[0].resolved_path == "/x/inter.ttf");
}

// ───────────────────────────────────────────────────────────────────────────
// Design-import sprite & widget-recognition fidelity invariants.
// Each guards a general importer rule: widget recognition is not blocked by a
// decorative leaf child, render_bounds parses, snake_case align keywords are
// honored, a degenerate stroke is not double-drawn, single-line text is not
// flipped to multi-line, and a sprite image is sized preserving its source
// aspect ratio. Fixtures are synthetic; nothing here is tied to one design.
// ───────────────────────────────────────────────────────────────────────────
namespace {
const IRNode* find_node_by_name(const IRNode& n, const std::string& name) {
    if (n.name == name) return &n;
    for (const auto& c : n.children)
        if (const IRNode* r = find_node_by_name(c, name)) return r;
    return nullptr;
}

// Pull the float arg of the first `setFlex('<id>', '<dim>', N)` for a given id.
std::optional<float> emitted_flex(const std::string& js, const std::string& id,
                                  const std::string& dim) {
    const std::string needle = "setFlex('" + id + "', '" + dim + "', ";
    auto pos = js.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos += needle.size();
    return std::strtof(js.c_str() + pos, nullptr);
}

// Find the sanitized bridge id of the createImage call for a comment-labelled
// node (codegen prints `// <name>` immediately before the create call).
std::optional<std::string> image_id_after_comment(const std::string& js,
                                                   const std::string& name) {
    auto cpos = js.find("// " + name + "\n");
    if (cpos == std::string::npos) return std::nullopt;
    auto cipos = js.find("createImage('", cpos);
    if (cipos == std::string::npos) return std::nullopt;
    cipos += std::string("createImage('").size();
    auto end = js.find('\'', cipos);
    if (end == std::string::npos) return std::nullopt;
    return js.substr(cipos, end - cipos);
}
}  // namespace

TEST_CASE("figma-plugin knob with a stroked indicator child is recognized as a native knob",
          "[view][import][figma-plugin][fidelity]") {
    // A knob frame whose children are the captured silver-graphic image, a
    // ~0-width stroked pointer hairline, and value/label text. The hairline is
    // demoted image->frame; before the fix that leaf frame tripped the
    // has_child_containers gate and the whole knob fell through to a raw stack
    // of images instead of a native createKnob.
    const std::string json = R"json({
      "provenance": {"adapter":"figma-plugin","version":"test"},
      "root": {"type":"frame","name":"Root","style":{"width":200,"height":200},"children":[
        {"type":"frame","name":"Knob","style":{"width":62,"height":91,"position":"absolute","left":20,"top":20},"children":[
          {"type":"image","name":"Group 130","asset_ref":"a","style":{"width":62,"height":68,"position":"absolute","left":0,"top":0,"renderBounds":{"w":210,"h":116,"dx":-74,"dy":0}}},
          {"type":"image","name":"Vector 7","asset_ref":"b","style":{"width":0.0001,"height":8,"border":"2px solid #ffffff","borderColor":"#ffffff","borderWidth":2,"position":"absolute","left":31,"top":17}},
          {"type":"text","name":"VALUE","content":"VALUE","style":{"width":40,"height":12}},
          {"type":"text","name":"80%","content":"80%","style":{"width":40,"height":12}}
        ]}
      ]}
    })json";
    const auto ir = parse_figma_plugin_json(json);
    const IRNode* knob = find_node_by_name(ir.root, "Knob");
    REQUIRE(knob != nullptr);
    CHECK(knob->audio_widget == AudioWidgetType::knob);
    const auto js = generate_pulp_js(ir);
    CHECK(js.find("createKnob") != std::string::npos);
}

TEST_CASE("figma-plugin render_bounds parses into IRStyle (drop-shadow / bleed extent)",
          "[view][import][figma-plugin][fidelity]") {
    const std::string json = R"json({
      "provenance": {"adapter":"figma-plugin","version":"test"},
      "root": {"type":"frame","name":"Root","style":{"width":200,"height":200},"children":[
        {"type":"image","name":"Art","asset_ref":"a","style":{"width":62,"height":68,"renderBounds":{"w":210,"h":116,"dx":-74,"dy":0}}}
      ]}
    })json";
    const auto ir = parse_figma_plugin_json(json);
    const IRNode* art = find_node_by_name(ir.root, "Art");
    REQUIRE(art != nullptr);
    REQUIRE(art->style.render_bounds.has_value());
    CHECK(art->style.render_bounds->w == Catch::Approx(210.0f));
    CHECK(art->style.render_bounds->h == Catch::Approx(116.0f));
    CHECK(art->style.render_bounds->dx == Catch::Approx(-74.0f));
    CHECK(art->style.render_bounds->dy == Catch::Approx(0.0f));
}

TEST_CASE("figma-plugin snake_case align keywords are honored (titles not pinned to top)",
          "[view][import][figma-plugin][fidelity]") {
    // The figma-plugin export emits snake_case (`space_between`), not the CSS
    // kebab-case parse_align previously required; the keyword was silently
    // dropped, collapsing column justification.
    const std::string json = R"json({
      "provenance": {"adapter":"figma-plugin","version":"test"},
      "root": {"type":"frame","name":"Root","style":{"width":200,"height":400},
        "layout":{"direction":"column","justify":"space_between","align":"center"},"children":[
        {"type":"text","name":"Title","content":"SECTION","style":{"width":80,"height":20}}
      ]}
    })json";
    const auto ir = parse_figma_plugin_json(json);
    CHECK(ir.root.layout.justify == LayoutAlign::space_between);
    CHECK(ir.root.layout.align == LayoutAlign::center);
}

TEST_CASE("figma-plugin degenerate stroke hairline renders as a thin fill, not a doubled border",
          "[view][import][figma-plugin][fidelity]") {
    // A ~0-width 2px-stroke pointer: the fill becomes the line. Keeping the
    // border too drew it on both edges and rendered ~3x too wide.
    const std::string json = R"json({
      "provenance": {"adapter":"figma-plugin","version":"test"},
      "root": {"type":"frame","name":"Root","style":{"width":200,"height":200},"children":[
        {"type":"image","name":"Hairline","asset_ref":"a","style":{"width":0.0001,"height":8,"border":"2px solid #ababab","borderColor":"#ababab","borderWidth":2,"position":"absolute","left":31,"top":17}}
      ]}
    })json";
    const auto ir = parse_figma_plugin_json(json);
    const IRNode* h = find_node_by_name(ir.root, "Hairline");
    REQUIRE(h != nullptr);
    CHECK(h->style.background_color.has_value());        // the fill IS the line
    CHECK_FALSE(h->style.border_width.has_value());      // no doubled stroke
    CHECK_FALSE(h->style.border_color.has_value());
}

TEST_CASE("single-line text taller than its font is not flipped to multi-line (search box)",
          "[view][import][codegen][fidelity]") {
    // An 8-14px label in a ~30px Figma box (line-box padding) must stay
    // single-line so its vertical centering survives; the old font_h*1.6
    // heuristic false-fired and pushed the baseline down.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.style.width = 200.0f;
    ir.root.style.height = 200.0f;
    IRNode label;
    label.type = "text";
    label.name = "Search";
    label.text_content = "Search";
    label.style.width = 120.0f;
    label.style.height = 26.0f;       // > font*1.6 (old heuristic) but < ~1.8 line boxes (new)
    label.style.font_size = 14.0f;
    ir.root.children.push_back(label);
    const auto js = generate_pulp_js(ir);
    CHECK(js.find("setMultiLine") == std::string::npos);

    // A genuinely tall box still flips to multi-line.
    DesignIR ir2 = ir;
    ir2.root.children[0].style.height = 90.0f;
    const auto js2 = generate_pulp_js(ir2);
    CHECK(js2.find("setMultiLine") != std::string::npos);
}

TEST_CASE("sprite image is sized preserving its source PNG aspect ratio (never skewed)",
          "[view][import][codegen][fidelity]") {
    // A captured component-instance sprite: layout box 62x68, render_bounds
    // 210x116 (aspect 1.81), but the PNG is 420x484 (aspect 0.87) with its
    // solid core in the top. Codegen must scale by the core->box fit and emit
    // an element at the PNG's aspect — not stretch it into render_bounds.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.style.width = 300.0f;
    ir.root.style.height = 300.0f;
    IRNode img;
    img.type = "image";
    img.name = "Art";
    img.style.width = 62.0f;
    img.style.height = 68.0f;
    img.style.position = "absolute";
    img.style.left = 0.0f;
    img.style.top = 0.0f;
    img.style.render_bounds = IRStyle::RenderBounds{210.0f, 116.0f, -74.0f, 0.0f};
    img.attributes["asset_path"] = "knob.png";
    img.attributes["png_natural_w"] = "420";
    img.attributes["png_natural_h"] = "484";
    img.attributes["art_core_x"] = "148";
    img.attributes["art_core_y"] = "0";
    img.attributes["art_core_w"] = "115";
    img.attributes["art_core_h"] = "129";
    ir.root.children.push_back(img);

    const auto js = generate_pulp_js(ir);
    const auto id = image_id_after_comment(js, "Art");
    REQUIRE(id.has_value());
    const auto w = emitted_flex(js, *id, "width");
    const auto h = emitted_flex(js, *id, "height");
    REQUIRE(w.has_value());
    REQUIRE(h.has_value());
    REQUIRE(*h > 0.0f);
    // Emitted aspect must equal the source PNG aspect (420/484), NOT
    // render_bounds (210/116) — that is the no-skew invariant.
    CHECK((*w / *h) == Catch::Approx(420.0f / 484.0f).epsilon(0.02));
    // And the core (115*s x 129*s, s = min(62/115,68/129)) fills the 62x68 box.
    CHECK(*w == Catch::Approx(420.0f * (68.0f / 129.0f)).epsilon(0.03));
}

TEST_CASE("ordinary image (no render_bounds) keeps its declared box, not its PNG aspect",
          "[view][import][codegen][fidelity]") {
    // Aspect-preservation is ONLY for bleed sprites (render_bounds present).
    // A normal image/icon whose node is 100x100 but whose bitmap is 200x100
    // must fill its declared 100x100 slot (Figma image-fill intent), NOT be
    // contained to 100x50 — that would reshape ordinary images and shift flex
    // layout. Guards the gating fix for the sprite-sizing path.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.style.width = 300.0f;
    ir.root.style.height = 300.0f;
    IRNode img;
    img.type = "image";
    img.name = "Icon";
    img.style.width = 100.0f;
    img.style.height = 100.0f;
    img.attributes["asset_path"] = "icon.png";
    img.attributes["png_natural_w"] = "200";   // wide bitmap, no render_bounds
    img.attributes["png_natural_h"] = "100";
    ir.root.children.push_back(img);

    const auto js = generate_pulp_js(ir);
    const auto id = image_id_after_comment(js, "Icon");
    REQUIRE(id.has_value());
    const auto w = emitted_flex(js, *id, "width");
    const auto h = emitted_flex(js, *id, "height");
    REQUIRE(w.has_value());
    REQUIRE(h.has_value());
    CHECK(*w == Catch::Approx(100.0f));   // declared box preserved
    CHECK(*h == Catch::Approx(100.0f));   // NOT 50 (contain to PNG aspect)
}

TEST_CASE("asset_bleed sprite (no render_bounds) preserves PNG aspect",
          "[view][import][codegen][fidelity]") {
    // The importer's pixel-vs-box heuristic stamps asset_bleed=1 on sprites
    // that bleed past their box without a render_bounds extent. Those must
    // still preserve their source aspect (object-fit is storage-only, so the
    // element box has to carry the aspect) — otherwise they skew.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.style.width = 300.0f;
    ir.root.style.height = 300.0f;
    IRNode img;
    img.type = "image";
    img.name = "Bleed";
    img.style.width = 100.0f;
    img.style.height = 100.0f;
    img.style.position = "absolute";
    img.style.left = 10.0f;
    img.style.top = 10.0f;
    img.attributes["asset_path"] = "bleed.png";
    img.attributes["png_natural_w"] = "200";   // wide bitmap
    img.attributes["png_natural_h"] = "100";
    img.attributes["asset_bleed"] = "1";        // bleed marker, NO render_bounds
    ir.root.children.push_back(img);

    const auto js = generate_pulp_js(ir);
    const auto id = image_id_after_comment(js, "Bleed");
    REQUIRE(id.has_value());
    const auto w = emitted_flex(js, *id, "width");
    const auto h = emitted_flex(js, *id, "height");
    REQUIRE(w.has_value());
    REQUIRE(h.has_value());
    // 100x100 box, aspect 2.0 → contain → 100x50 (aspect preserved, not skewed).
    CHECK(*w == Catch::Approx(100.0f));
    CHECK(*h == Catch::Approx(50.0f));
    // …and centered in the box: 50px of vertical slack → top offset by +25
    // (10 → 35); no horizontal slack (width fills) → left unchanged (10).
    CHECK(js.find("setTop('" + *id + "', 35") != std::string::npos);
    CHECK(js.find("setLeft('" + *id + "', 10") != std::string::npos);
}


TEST_CASE("codegen routes a fixed container through the gross-size check",
          "[view][import][codegen][fidelity]") {
    // A fixed/fixed frame whose _layoutWidth snapshot quadruples its authored
    // width must surface a "gross-size" finding through the fidelity_report
    // sink wired into the container branch.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 100.0f;

    IRNode box;
    box.type = "frame";
    box.name = "Box";
    box.style.width = 100.0f;
    box.style.height = 100.0f;
    box.layout.width_mode = SizingMode::fixed;
    box.layout.height_mode = SizingMode::fixed;
    box.attributes["_layoutWidth"] = "400";   // snapshot computes 4x the authored box
    box.attributes["_layoutHeight"] = "100";
    ir.root.children.push_back(box);

    std::vector<pulp::view::FidelityIssue> report;
    CodeGenOptions opts;
    opts.fidelity_report = &report;
    (void)generate_pulp_js(ir, opts);

    bool found = false;
    for (const auto& iss : report)
        if (iss.kind == "gross-size" && iss.node_name == "Box") found = true;
    CHECK(found);
}

TEST_CASE("codegen routes a dropped vector through the vector-renderability check",
          "[view][import][fidelity][vector]") {
    // A bare vector child (no asset_path / fill / children) hits codegen's
    // generic-frame fall-through and silently degrades to an empty row. The
    // tree-level vector-renderability pass, wired into generate_pulp_js on the
    // native arm, must surface a "dropped-vector" finding for it.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 200.0f;

    IRNode glyph;
    glyph.type = "path";
    glyph.name = "Glyph";
    glyph.style.width = 64.0f;   // no asset_path / fill / children
    glyph.style.height = 64.0f;
    ir.root.children.push_back(glyph);

    std::vector<pulp::view::FidelityIssue> report;
    CodeGenOptions opts;            // defaults to bridge_native_js
    opts.fidelity_report = &report;
    (void)generate_pulp_js(ir, opts);

    bool found = false;
    for (const auto& iss : report)
        if (iss.kind == "dropped-vector" && iss.node_name == "Glyph") found = true;
    CHECK(found);
}

TEST_CASE("codegen lowers a path-data vector to a native SvgPath (not dropped)",
          "[view][import][codegen][vector]") {
    // A vector/path/svg_path node carrying path_data lowers to createSvgPath +
    // setSvgPath (+ viewBox / fill / stroke), so it RENDERS and is NOT flagged
    // by the dropped-vector invariant. Source-agnostic: keyed only on type +
    // path_data, never a layer name.
    for (const char* kind : {"path", "vector", "svg_path"}) {
        DesignIR ir;
        ir.root.type = "frame";
        ir.root.name = "Root";
        ir.root.style.width = 400.0f;
        ir.root.style.height = 200.0f;

        IRNode glyph;
        glyph.type = kind;
        glyph.name = "Glyph";
        glyph.style.width = 64.0f;
        glyph.style.height = 64.0f;
        glyph.attributes["path_data"] = "M0 0 L64 0 L32 64 Z";
        glyph.attributes["svg_fill"] = "#ff8800";
        glyph.attributes["svg_stroke"] = "#102030";
        glyph.attributes["svg_stroke_width"] = "2";
        glyph.attributes["svg_viewbox"] = "0 0 64 64";
        ir.root.children.push_back(glyph);

        std::vector<pulp::view::FidelityIssue> report;
        CodeGenOptions opts;            // defaults to bridge_native_js
        opts.fidelity_report = &report;
        const auto js = generate_pulp_js(ir, opts);

        INFO("kind=" << kind << " js=\n" << js);
        CHECK(js.find("createSvgPath('Glyph") != std::string::npos);
        CHECK(js.find("setSvgPath('Glyph") != std::string::npos);
        CHECK(js.find("M0 0 L64 0 L32 64 Z") != std::string::npos);
        CHECK(js.find("setSvgViewBox('Glyph") != std::string::npos);
        CHECK(js.find("setSvgFill('Glyph") != std::string::npos);
        CHECK(js.find("setSvgStroke('Glyph") != std::string::npos);
        // Renders → NOT flagged as a silent drop.
        bool dropped = false;
        for (const auto& iss : report)
            if (iss.kind == "dropped-vector" && iss.node_name == "Glyph") dropped = true;
        CHECK_FALSE(dropped);
    }
}

TEST_CASE("codegen synthesizes SVG paths for bare vector shape primitives",
          "[view][import][codegen][vector]") {
    // rect/line/ellipse/circle/polygon/star with no path_data, no fill, and no
    // children would otherwise drop to an empty frame. synthesize_primitive_paths
    // derives a `d` from geometry so each lowers to a native SvgPath. svg_fill is
    // forced to "none" so the SvgPathWidget's default opaque-black fill never
    // paints a phantom box. Source-agnostic: keyed only on IR type + geometry.
    struct Case { const char* type; float w; float h; const char* expect_d; };
    const Case cases[] = {
        {"rect",    100.0f, 40.0f, "M0 0 H100 V40 H0 Z"},
        {"line",     80.0f, 24.0f, "M0 0 L80 24"},
        {"ellipse",  60.0f, 60.0f, "A30 30"},     // two half-arcs
        {"circle",   60.0f, 60.0f, "A30 30"},
        {"polygon", 100.0f, 100.0f, "M50 0"},     // top vertex of a triangle
        {"star",    100.0f, 100.0f, "M50 0"},     // first spike at top
    };
    for (const auto& c : cases) {
        DesignIR ir;
        ir.root.type = "frame";
        ir.root.name = "Root";
        ir.root.style.width = 400.0f;
        ir.root.style.height = 400.0f;

        IRNode shape;
        shape.type = c.type;
        shape.name = "Shape";
        shape.style.width = c.w;
        shape.style.height = c.h;
        ir.root.children.push_back(shape);

        std::vector<pulp::view::FidelityIssue> report;
        CodeGenOptions opts;
        opts.fidelity_report = &report;
        const auto js = generate_pulp_js(ir, opts);

        INFO("type=" << c.type << " js=\n" << js);
        CHECK(js.find("createSvgPath('Shape") != std::string::npos);
        CHECK(js.find("setSvgPath('Shape") != std::string::npos);
        CHECK(js.find(c.expect_d) != std::string::npos);
        // svg_fill forced to none (codegen suffixes the id, so match the value).
        CHECK(js.find("setSvgFill('Shape") != std::string::npos);
        CHECK(js.find("', 'none')") != std::string::npos);
        bool dropped = false;
        for (const auto& iss : report)
            if (iss.kind == "dropped-vector" && iss.node_name == "Shape") dropped = true;
        CHECK_FALSE(dropped);
    }
}

TEST_CASE("synthesized rect honors border-radius and stroke-only borders",
          "[view][import][codegen][vector]") {
    // A rounded rect emits arc commands; a stroke-only shape carries its border
    // color/width onto the SvgPath stroke (and never a fill).
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 400.0f;

    IRNode rounded;
    rounded.type = "rect";
    rounded.name = "Rounded";
    rounded.style.width = 100.0f;
    rounded.style.height = 40.0f;
    rounded.style.border_radius = 8.0f;
    rounded.style.border_color = "#334455";
    rounded.style.border_width = 3.0f;
    ir.root.children.push_back(rounded);

    CodeGenOptions opts;
    const auto js = generate_pulp_js(ir, opts);
    INFO("js=\n" << js);
    CHECK(js.find("createSvgPath('Rounded") != std::string::npos);
    CHECK(js.find("A8 8") != std::string::npos);                       // rounded corner arc
    // Border → stroke; fill forced to none (codegen suffixes the bridge id).
    CHECK(js.find("setSvgStroke('Rounded") != std::string::npos);
    CHECK(js.find("'#334455')") != std::string::npos);
    CHECK(js.find("setSvgStrokeWidth('Rounded") != std::string::npos);
    CHECK(js.find("setSvgFill('Rounded") != std::string::npos);
    CHECK(js.find("', 'none')") != std::string::npos);
}

TEST_CASE("synthesis leaves filled and container primitives untouched",
          "[view][import][codegen][vector]") {
    // The synthesizer steps in ONLY for the exact drop case. A filled rect still
    // renders through the generic-frame branch; a rect with children keeps them.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 400.0f;

    IRNode filled;
    filled.type = "rect";
    filled.name = "Filled";
    filled.style.width = 50.0f;
    filled.style.height = 50.0f;
    filled.style.background_color = "#ff0000";
    ir.root.children.push_back(filled);

    IRNode container;
    container.type = "rect";
    container.name = "Container";
    container.style.width = 80.0f;
    container.style.height = 80.0f;
    IRNode label;
    label.type = "text";
    label.name = "Caption";
    label.text_content = "Hi";
    label.style.width = 40.0f;
    label.style.height = 16.0f;
    container.children.push_back(label);
    ir.root.children.push_back(container);

    CodeGenOptions opts;
    const auto js = generate_pulp_js(ir, opts);
    INFO("js=\n" << js);
    // Neither is converted to an SvgPath.
    CHECK(js.find("createSvgPath('Filled") == std::string::npos);
    CHECK(js.find("createSvgPath('Container") == std::string::npos);
    // The container's child survives.
    CHECK(js.find("Caption") != std::string::npos);
}

TEST_CASE("parse_design_ir_json normalizes Figma resize constraints",
          "[view][import][constraints]") {
    // Node-level constraints, the nested figma{} block variant, and the Figma
    // MIN/MAX/CENTER/STRETCH/SCALE spelling all normalize to the IR token set.
    const auto ir = parse_design_ir_json(R"json({
        "type": "frame", "name": "Root",
        "layout": { "direction": "column" },
        "children": [
            { "type": "frame", "name": "A",
              "constraints": { "horizontal": "CENTER", "vertical": "MAX" } },
            { "type": "frame", "name": "B",
              "constraints": { "horizontal": "SCALE", "vertical": "STRETCH" } },
            { "type": "frame", "name": "C",
              "figma": { "constraints": { "horizontal": "MIN", "vertical": "MIN" } } }
        ]
    })json");
    REQUIRE(ir.root.children.size() == 3);
    CHECK(ir.root.children[0].layout.h_constraint == "center");
    CHECK(ir.root.children[0].layout.v_constraint == "bottom");
    CHECK(ir.root.children[1].layout.h_constraint == "scale");
    CHECK(ir.root.children[1].layout.v_constraint == "stretch");
    CHECK(ir.root.children[2].layout.h_constraint == "left");
    CHECK(ir.root.children[2].layout.v_constraint == "top");
}

TEST_CASE("codegen lowers resize constraints to flex within the parent",
          "[view][import][codegen][constraints]") {
    auto emit = [](const std::string& h, const std::string& v) {
        DesignIR ir;
        ir.root.type = "frame"; ir.root.name = "Root";
        ir.root.style.width = 400.0f; ir.root.style.height = 400.0f;
        IRNode child; child.type = "frame"; child.name = "Child";
        child.style.width = 50.0f; child.style.height = 50.0f;
        if (!h.empty()) child.layout.h_constraint = h;
        if (!v.empty()) child.layout.v_constraint = v;
        ir.root.children.push_back(child);
        CodeGenOptions opts;  // bridge_native_js
        return generate_pulp_js(ir, opts);
    };
    // center → auto margins on both sides of each axis (id suffix-tolerant).
    {
        auto js = emit("center", "center");
        INFO(js);
        CHECK(js.find("'margin_left', 'auto')")   != std::string::npos);
        CHECK(js.find("'margin_right', 'auto')")  != std::string::npos);
        CHECK(js.find("'margin_top', 'auto')")    != std::string::npos);
        CHECK(js.find("'margin_bottom', 'auto')") != std::string::npos);
    }
    // right/bottom → leading auto margin only (push to the trailing edge).
    {
        auto js = emit("right", "bottom");
        INFO(js);
        CHECK(js.find("'margin_left', 'auto')") != std::string::npos);
        CHECK(js.find("'margin_top', 'auto')")  != std::string::npos);
        CHECK(js.find("margin_right")  == std::string::npos);
        CHECK(js.find("margin_bottom") == std::string::npos);
    }
    // scale → flex-grow:1.
    {
        auto js = emit("scale", "");
        INFO(js);
        CHECK(js.find("'flex_grow', 1)") != std::string::npos);
    }
    // stretch (pin both edges) → align-self:stretch.
    {
        auto js = emit("stretch", "");
        INFO(js);
        CHECK(js.find("'align_self', 'stretch')") != std::string::npos);
    }
    // left/top → flex default: nothing emitted.
    {
        auto js = emit("left", "top");
        INFO(js);
        CHECK(js.find("margin_left") == std::string::npos);
        CHECK(js.find("margin_top")  == std::string::npos);
        CHECK(js.find("align_self")  == std::string::npos);
    }
}

TEST_CASE("parse_design_ir_json reads CSS grid container + item placement",
          "[view][import][grid]") {
    auto ir = parse_design_ir_json(R"json({
        "type": "frame", "name": "Grid",
        "layout": { "display": "grid", "gridTemplateColumns": "1fr 1fr",
                    "gridTemplateRows": "auto", "gridAutoFlow": "row dense", "gap": 8 },
        "children": [
            { "type": "frame", "name": "A", "layout": { "gridColumn": "1 / 3", "gridRow": "2" } },
            { "type": "frame", "name": "B", "layout": { "grid_column": "2" } }
        ]
    })json");
    CHECK(ir.root.layout.display == "grid");
    CHECK(ir.root.layout.grid_template_columns == "1fr 1fr");
    CHECK(ir.root.layout.grid_template_rows == "auto");
    CHECK(ir.root.layout.grid_auto_flow == "row dense");
    REQUIRE(ir.root.children.size() == 2);
    CHECK(ir.root.children[0].layout.grid_column == "1 / 3");
    CHECK(ir.root.children[0].layout.grid_row == "2");
    CHECK(ir.root.children[1].layout.grid_column == "2");
}

TEST_CASE("codegen lowers a grid container to the native grid bridge",
          "[view][import][codegen][grid]") {
    DesignIR ir;
    ir.root.type = "frame"; ir.root.name = "Grid";
    ir.root.style.width = 300.0f; ir.root.style.height = 200.0f;
    ir.root.layout.display = "grid";
    ir.root.layout.grid_template_columns = "1fr 1fr";
    ir.root.layout.grid_template_rows = "auto auto";
    ir.root.layout.grid_auto_flow = "row";
    ir.root.layout.gap = 12.0f;
    ir.root.layout.justify = LayoutAlign::center;  // flex-only; must NOT be emitted for grid
    IRNode a; a.type = "frame"; a.name = "A"; a.style.width = 10.0f; a.style.height = 10.0f;
    a.layout.grid_column = "1 / 3"; a.layout.grid_row = "1";
    IRNode b; b.type = "frame"; b.name = "B"; b.style.width = 10.0f; b.style.height = 10.0f;
    b.layout.grid_row = "2";
    ir.root.children.push_back(a);
    ir.root.children.push_back(b);

    CodeGenOptions opts;  // bridge_native_js
    const auto js = generate_pulp_js(ir, opts);
    INFO(js);
    // Container lowers to createGrid (not createCol/createRow) + track template.
    CHECK(js.find("createGrid(") != std::string::npos);
    CHECK(js.find("'template_columns', '1fr 1fr')") != std::string::npos);
    CHECK(js.find("'template_rows', 'auto auto')") != std::string::npos);
    CHECK(js.find("'auto_flow', 'row')") != std::string::npos);
    CHECK(js.find("'gap', 12)") != std::string::npos);
    // Per-item line placement.
    CHECK(js.find("'column_start', 1)") != std::string::npos);
    CHECK(js.find("'column_end', 3)") != std::string::npos);
    CHECK(js.find("'row_start', 2)") != std::string::npos);
    // Flex alignment is meaningless for a grid container and must be suppressed.
    CHECK(js.find("justify_content") == std::string::npos);
}

TEST_CASE("codegen preserves radial/conic background gradients",
          "[view][import][codegen][gradient]") {
    // The IR carries the gradient as a raw CSS string; codegen emits it verbatim
    // to setBackgroundGradient, where the bridge now paints radial/conic (not a
    // flat fallback). Confirms radial/conic survive parse -> IR -> codegen.
    for (const char* g : {"radial-gradient(circle at 50% 50%, #ffffff, #000000)",
                          "conic-gradient(from 0deg at 50% 50%, #ff0000, #00ff00, #0000ff)"}) {
        DesignIR ir;
        ir.root.type = "frame"; ir.root.name = "Root";
        ir.root.style.width = 100.0f; ir.root.style.height = 100.0f;
        ir.root.style.background_gradient = g;
        CodeGenOptions opts;
        const auto js = generate_pulp_js(ir, opts);
        INFO(js);
        CHECK(js.find("setBackgroundGradient(") != std::string::npos);
        CHECK(js.find(g) != std::string::npos);  // emitted verbatim
    }
}

TEST_CASE("parse_design_ir_json reads per-range text style runs",
          "[view][import][text]") {
    auto ir = parse_design_ir_json(R"json({
        "type": "frame", "name": "Root",
        "children": [
            { "type": "text", "name": "T", "content": "Hello world",
              "runs": [ { "start": 6, "end": 11, "fontWeight": 700,
                          "color": "#ff0000", "italic": true } ] }
        ]
    })json");
    REQUIRE(ir.root.children.size() == 1);
    const auto& t = ir.root.children[0];
    REQUIRE(t.text_runs.size() == 1);
    CHECK(t.text_runs[0].start == 6);
    CHECK(t.text_runs[0].end == 11);
    CHECK(t.text_runs[0].font_weight == 700);
    CHECK(t.text_runs[0].color == "#ff0000");
    CHECK(t.text_runs[0].font_style == "italic");
}

TEST_CASE("web codegen emits per-range text style runs as nested spans",
          "[view][import][codegen][text]") {
    // Mixed-style text emits a base span whose covered range becomes a styled
    // <span> child while the gap inherits the dominant style as plain text.
    DesignIR ir;
    ir.root.type = "frame"; ir.root.name = "Root";
    ir.root.style.width = 200.0f; ir.root.style.height = 40.0f;
    IRNode t; t.type = "text"; t.name = "Caption";
    t.text_content = "Hello world";
    IRTextRun run; run.start = 6; run.end = 11;  // "world"
    run.font_weight = 700; run.color = "#ff0000";
    t.text_runs.push_back(run);
    ir.root.children.push_back(t);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    const auto js = generate_pulp_js(ir, opts);
    INFO(js);
    CHECK(js.find("createTextNode('Hello ')") != std::string::npos);  // base-styled gap
    CHECK(js.find("document.createElement('span')") != std::string::npos);
    CHECK(js.find(".style.fontWeight = '700'") != std::string::npos);
    CHECK(js.find(".style.color = '#ff0000'") != std::string::npos);
    CHECK(js.find(".textContent = 'world'") != std::string::npos);
}

TEST_CASE("single-run text keeps the plain textContent path (no regression)",
          "[view][import][codegen][text]") {
    DesignIR ir;
    ir.root.type = "frame"; ir.root.name = "Root";
    IRNode t; t.type = "text"; t.name = "Plain"; t.text_content = "Just text";
    ir.root.children.push_back(t);
    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    const auto js = generate_pulp_js(ir, opts);
    INFO(js);
    CHECK(js.find(".textContent = 'Just text'") != std::string::npos);
    CHECK(js.find("createTextNode(") == std::string::npos);  // no run splitting
}

TEST_CASE("serialize_design_ir round-trips constraints, grid, and text runs",
          "[view][import][serialization]") {
    // Regression: the new IRLayout/IRNode fields must survive a
    // serialize -> re-parse pass (frozen .pulp IR / --emit ir-json).
    DesignIR ir;
    ir.root.type = "frame"; ir.root.name = "Root";
    ir.root.layout.display = "grid";
    ir.root.layout.grid_template_columns = "1fr 1fr";
    ir.root.layout.grid_auto_flow = "row";

    IRNode child; child.type = "text"; child.name = "T"; child.text_content = "Hi world";
    child.layout.h_constraint = "center";
    child.layout.v_constraint = "bottom";
    child.layout.grid_column = "1 / 3";
    IRTextRun run; run.start = 3; run.end = 8; run.font_weight = 700; run.color = "#abcdef";
    child.text_runs.push_back(run);
    ir.root.children.push_back(child);

    const auto rt = parse_design_ir_json(serialize_design_ir(ir));
    REQUIRE(rt.root.children.size() == 1);
    const auto& c = rt.root.children[0];
    CHECK(rt.root.layout.grid_template_columns == "1fr 1fr");
    CHECK(rt.root.layout.grid_auto_flow == "row");
    CHECK(c.layout.h_constraint == "center");
    CHECK(c.layout.v_constraint == "bottom");
    CHECK(c.layout.grid_column == "1 / 3");
    REQUIRE(c.text_runs.size() == 1);
    CHECK(c.text_runs[0].start == 3);
    CHECK(c.text_runs[0].end == 8);
    CHECK(c.text_runs[0].font_weight == 700);
    CHECK(c.text_runs[0].color == "#abcdef");
}

TEST_CASE("synthesized primitive consumes a Pencil stroke_color attribute",
          "[view][import][codegen][vector]") {
    // Pencil records a shape's stroke as a stroke_color attribute, not in
    // style.border_color — the synthesizer must still paint it.
    DesignIR ir;
    ir.root.type = "frame"; ir.root.name = "Root";
    ir.root.style.width = 100.0f; ir.root.style.height = 100.0f;
    IRNode rect; rect.type = "rect"; rect.name = "Stroked";
    rect.style.width = 40.0f; rect.style.height = 40.0f;
    rect.attributes["stroke_color"] = "#13579b";
    rect.attributes["stroke_width"] = "2";
    ir.root.children.push_back(rect);
    CodeGenOptions opts;
    const auto js = generate_pulp_js(ir, opts);
    INFO(js);
    CHECK(js.find("createSvgPath('Stroked") != std::string::npos);
    CHECK(js.find("setSvgStroke('Stroked") != std::string::npos);
    CHECK(js.find("'#13579b')") != std::string::npos);
}

TEST_CASE("grid container with no explicit columns gets a default column",
          "[view][import][codegen][grid]") {
    // display:grid with no track template would drop all children in the native
    // grid engine (cols empty); emit a default single column instead.
    DesignIR ir;
    ir.root.type = "frame"; ir.root.name = "G";
    ir.root.layout.display = "grid";  // no gridTemplateColumns
    IRNode a; a.type = "frame"; a.name = "A"; a.style.width = 10.0f; a.style.height = 10.0f;
    ir.root.children.push_back(a);
    CodeGenOptions opts;
    const auto js = generate_pulp_js(ir, opts);
    INFO(js);
    CHECK(js.find("createGrid(") != std::string::npos);
    CHECK(js.find("'template_columns', '1fr')") != std::string::npos);
}

TEST_CASE("per-range run children carry the node's dominant style",
          "[view][import][codegen][text]") {
    // A run that overrides only weight must still render the base size/color —
    // web-compat Labels don't inherit, so the base style is copied onto each run
    // child (so it appears on both the base span AND the run child).
    DesignIR ir;
    ir.root.type = "frame"; ir.root.name = "Root";
    IRNode t; t.type = "text"; t.name = "Mixed"; t.text_content = "ab cd";
    t.style.font_size = 18.0f;
    t.style.color = "#222222";
    IRTextRun run; run.start = 3; run.end = 5; run.font_weight = 700;  // "cd" bold
    t.text_runs.push_back(run);
    ir.root.children.push_back(t);
    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    const auto js = generate_pulp_js(ir, opts);
    INFO(js);
    auto count = [&](const std::string& needle) {
        size_t n = 0, p = 0;
        while ((p = js.find(needle, p)) != std::string::npos) { n++; p += needle.size(); }
        return n;
    };
    CHECK(js.find(".style.fontWeight = '700'") != std::string::npos);
    CHECK(count(".style.fontSize = '18px'") >= 2);   // base span + run child
    CHECK(count(".style.color = '#222222'") >= 2);
}

TEST_CASE("STRETCH constraint fills its axis even with an explicit size",
          "[view][import][codegen][constraints]") {
    // A STRETCH constraint pins both edges = fill that dimension. min-width/
    // height:100% makes it effective even when the node also has an explicit
    // pixel size (Yoga clamps up to min), so STRETCH is no longer a no-op.
    DesignIR ir;
    ir.root.type = "frame"; ir.root.name = "Root";
    ir.root.style.width = 400.0f; ir.root.style.height = 400.0f;
    IRNode c; c.type = "frame"; c.name = "C";
    c.style.width = 50.0f; c.style.height = 50.0f;   // explicit cross-size
    c.layout.h_constraint = "stretch";
    c.layout.v_constraint = "stretch";
    ir.root.children.push_back(c);
    CodeGenOptions opts;
    const auto js = generate_pulp_js(ir, opts);
    INFO(js);
    CHECK(js.find("'min_width', '100%'") != std::string::npos);
    CHECK(js.find("'min_height', '100%'") != std::string::npos);
}

TEST_CASE("grid item 'N / span M' resolves to an end line",
          "[view][import][codegen][grid]") {
    DesignIR ir;
    ir.root.type = "frame"; ir.root.name = "G";
    ir.root.layout.display = "grid";
    ir.root.layout.grid_template_columns = "1fr 1fr 1fr";
    IRNode a; a.type = "frame"; a.name = "A"; a.style.width = 10.0f; a.style.height = 10.0f;
    a.layout.grid_column = "1 / span 2";  // start line 1, span 2 -> end line 3
    ir.root.children.push_back(a);
    CodeGenOptions opts;
    const auto js = generate_pulp_js(ir, opts);
    INFO(js);
    CHECK(js.find("'column_start', 1)") != std::string::npos);
    CHECK(js.find("'column_end', 3)") != std::string::npos);
}

TEST_CASE("native codegen emits setTextRuns for per-range text",
          "[view][import][codegen][text]") {
    // The native (bridge) arm now lowers per-range text to setTextRuns (the
    // bridge builds a canvas::AttributedString), not just the web <span> path.
    DesignIR ir;
    ir.root.type = "frame"; ir.root.name = "Root";
    IRNode t; t.type = "text"; t.name = "T"; t.text_content = "Hello world";
    t.style.font_size = 16.0f;
    IRTextRun run; run.start = 0; run.end = 5; run.font_weight = 700; run.color = "#ff0000";
    t.text_runs.push_back(run);
    ir.root.children.push_back(t);
    CodeGenOptions opts;  // bridge_native_js (default)
    const auto js = generate_pulp_js(ir, opts);
    INFO(js);
    CHECK(js.find("setTextRuns('T") != std::string::npos);
    CHECK(js.find("start: 0, end: 5") != std::string::npos);
    CHECK(js.find("fontWeight: 700") != std::string::npos);
    CHECK(js.find("color: '#ff0000'") != std::string::npos);
}

TEST_CASE("web codegen escapes clip-path / mask CSS (no JS string break)",
          "[view][import][codegen][mask]") {
    // Raw clip-path/mask CSS can carry url('...') with quotes; emit_str must
    // escape it so it can't break out of the JS string literal (#3288 P2).
    DesignIR ir;
    ir.root.type = "frame"; ir.root.name = "Root";
    ir.root.style.clip_path = "url('#a')";  // contains single quotes
    CodeGenOptions opts; opts.mode = CodeGenMode::web_compat;
    const auto js = generate_pulp_js(ir, opts);
    INFO(js);
    CHECK(js.find("\\'#a\\'") != std::string::npos);          // quotes escaped
    CHECK(js.find("clipPath = 'url('#a')'") == std::string::npos);  // not raw
}

TEST_CASE("per-range text runs slice multibyte text on byte offsets",
          "[view][import][codegen][text]") {
    // "café world" — é is 2 UTF-8 bytes, so "world" begins at BYTE offset 6.
    // The run must slice on bytes (not char index 5), leaving the "café " gap
    // intact and the run text == "world".
    DesignIR ir;
    ir.root.type = "frame"; ir.root.name = "Root";
    IRNode t; t.type = "text"; t.name = "M";
    t.text_content = "caf\xc3\xa9 world";   // "café world", é = 0xC3 0xA9
    IRTextRun run; run.start = 6; run.end = 11; run.font_weight = 700;  // "world"
    t.text_runs.push_back(run);
    ir.root.children.push_back(t);
    CodeGenOptions opts; opts.mode = CodeGenMode::web_compat;
    const auto js = generate_pulp_js(ir, opts);
    INFO(js);
    CHECK(js.find("createTextNode('caf\xc3\xa9 ')") != std::string::npos);  // gap intact
    CHECK(js.find(".textContent = 'world'") != std::string::npos);          // run text
}

TEST_CASE("codegen emits mix-blend-mode on native + web-compat paths",
          "[view][import][codegen][blend]") {
    // A node's normalized mix_blend_mode lowers to setMixBlendMode (native
    // bridge -> View::set_mix_blend_mode) and style.mixBlendMode (web-compat).
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";

    IRNode overlay;
    overlay.type = "frame";
    overlay.name = "Overlay";
    overlay.style.width = 100.0f;
    overlay.style.height = 100.0f;
    overlay.style.background_color = "#ff0000";
    overlay.style.mix_blend_mode = "multiply";
    ir.root.children.push_back(overlay);

    CodeGenOptions nopts;
    nopts.mode = CodeGenMode::bridge_native_js;
    const auto njs = generate_pulp_js(ir, nopts);
    INFO("native:\n" << njs);
    CHECK(njs.find("setMixBlendMode('Overlay") != std::string::npos);
    CHECK(njs.find("'multiply')") != std::string::npos);

    CodeGenOptions wopts;
    wopts.mode = CodeGenMode::web_compat;
    const auto wjs = generate_pulp_js(ir, wopts);
    INFO("web:\n" << wjs);
    CHECK(wjs.find("mixBlendMode = 'multiply'") != std::string::npos);
}

TEST_CASE("codegen emits clip-path + mask (native + web-compat)",
          "[view][import][codegen][mask]") {
    // clip-path / mask lower to setClipPath / setMask* (native →
    // View::set_clip_path / set_mask*) and style.clipPath / maskImage (web).
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";

    IRNode masked;
    masked.type = "frame";
    masked.name = "Masked";
    masked.style.width = 100.0f;
    masked.style.height = 100.0f;
    masked.style.background_color = "#00ff00";
    masked.style.clip_path = "inset(10px)";
    masked.style.mask_image = "linear-gradient(black, transparent)";
    ir.root.children.push_back(masked);

    CodeGenOptions nopts;
    nopts.mode = CodeGenMode::bridge_native_js;
    const auto njs = generate_pulp_js(ir, nopts);
    INFO("native:\n" << njs);
    CHECK(njs.find("setClipPath('Masked") != std::string::npos);
    CHECK(njs.find("inset(10px)") != std::string::npos);
    CHECK(njs.find("setMaskImage('Masked") != std::string::npos);

    CodeGenOptions wopts;
    wopts.mode = CodeGenMode::web_compat;
    const auto wjs = generate_pulp_js(ir, wopts);
    INFO("web:\n" << wjs);
    CHECK(wjs.find("clipPath = 'inset(10px)'") != std::string::npos);
    CHECK(wjs.find("maskImage") != std::string::npos);
}

TEST_CASE("parse + round-trip clip-path / mask", "[view][import][parse][mask]") {
    const auto ir = parse_design_ir_json(
        R"JSON({"version":1,"source":"figma","root":{"type":"frame","name":"R",
            "style":{"clipPath":"circle(40%)","mask":"url(#m)",
                     "maskImage":"linear-gradient(black,transparent)",
                     "maskSize":"cover"}}})JSON");
    REQUIRE(ir.root.style.clip_path == "circle(40%)");
    REQUIRE(ir.root.style.mask == "url(#m)");
    REQUIRE(ir.root.style.mask_image == "linear-gradient(black,transparent)");
    REQUIRE(ir.root.style.mask_size == "cover");
}

TEST_CASE("web-compat codegen also runs the image-sizing fidelity check",
          "[view][import][codegen][fidelity][web-compat]") {
    // #3267: fidelity checks previously ran only in the native-bridge
    // branch, so `--web-compat --strict-fidelity` exited 0 even for a skewed
    // bleed sprite. The web-compat <img> emits the style box directly, so the
    // image-sizing check now runs on that path too.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";

    IRNode img;
    img.type = "image";
    img.name = "Sprite";
    img.attributes["asset_bleed"]   = "1";    // bleed marker
    img.attributes["png_natural_w"] = "100";  // source aspect 1.0
    img.attributes["png_natural_h"] = "100";
    img.style.width  = 100.0f;                 // emitted aspect 2.0 → skewed
    img.style.height = 50.0f;
    ir.root.children.push_back(img);

    std::vector<pulp::view::FidelityIssue> report;
    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.fidelity_report = &report;
    (void)generate_pulp_js(ir, opts);

    bool skew = false;
    for (const auto& iss : report)
        if (iss.kind == "skew" && iss.node_name == "Sprite") skew = true;
    CHECK(skew);
}


TEST_CASE("an undersized widget reports informational (never a hard finding)",
          "[view][import][codegen][fidelity]") {
    // #3274: a below-native-minimum widget is legitimately clamped up by
    // codegen, so its finding must carry the informational flag and never be a
    // hard finding the import CLI fails on under --strict-fidelity. The xy_pad
    // branch clamps its emitted size up to the 80px native minimum (sz =
    // max(width, 80)), so a 20px source produces the widget-undersized path.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";

    IRNode pad;
    pad.type = "frame";
    pad.name = "Tiny";
    pad.audio_widget = AudioWidgetType::xy_pad;
    pad.style.width  = 20.0f;   // far below the 80px native minimum → clamped up
    pad.style.height = 20.0f;
    ir.root.children.push_back(pad);

    std::vector<pulp::view::FidelityIssue> report;
    CodeGenOptions opts;
    opts.fidelity_report = &report;
    (void)generate_pulp_js(ir, opts);

    bool saw_undersized = false;
    for (const auto& iss : report) {
        if (iss.kind == "widget-undersized") {
            saw_undersized = true;
            CHECK(iss.informational);          // the load-bearing assertion
        }
        // Whatever else surfaces, nothing about a clamp-up may be a hard finding.
        if (iss.node_name == "Tiny") CHECK(iss.informational);
    }
    CHECK(saw_undersized);
}
