// DesignIR's shipped fonts (`fontFamilyAssets`): the captured @font-face
// descriptors round-trip, and documents without them, Figma exports included,
// serialize exactly as they did before the descriptors existed.
#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_codegen.hpp>
#include <pulp/view/design_ir_io.hpp>
#include <pulp/view/design_sources.hpp>
#include <pulp/view/reload_autocaps.hpp>
#include <pulp/view/reload_capabilities.hpp>

#include <string>
#include <vector>

using namespace pulp::view;

TEST_CASE("design IR JSON round-trip preserves captured @font-face descriptors",
          "[design-import][font-assets]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "R";
    IRFontAsset variable;
    variable.family = "JetBrains Mono";
    variable.weight = 100;
    variable.weight_max = 800;
    variable.font_style = "normal";
    variable.unicode_range = "U+0000-00FF, U+0131";
    variable.asset_id = "pulp-materialized-asset-aa";
    variable.resolved_path = "/x/aa.woff2";
    IRFontAsset italic;
    italic.family = "JetBrains Mono";
    italic.weight = 600;
    italic.font_style = "italic";
    italic.asset_id = "pulp-materialized-asset-bb";
    ir.font_family_assets = {variable, italic};

    const auto json = serialize_design_ir(ir);
    const auto reparsed = parse_design_ir_json(json);
    REQUIRE(reparsed.font_family_assets.size() == 2);
    const auto& a = reparsed.font_family_assets[0];
    CHECK(a.weight == 100);
    CHECK(a.weight_max == 800);
    CHECK(a.font_style == "normal");
    CHECK(a.unicode_range == "U+0000-00FF, U+0131");
    const auto& b = reparsed.font_family_assets[1];
    CHECK(b.weight == 600);
    CHECK(b.weight_max == 0);
    CHECK(b.font_style == "italic");
    CHECK(b.unicode_range.empty());
    // Order is the page's @font-face order, which unicode-range matching uses.
    CHECK(a.asset_id == "pulp-materialized-asset-aa");
    CHECK(serialize_design_ir(reparsed) == json);
}

TEST_CASE("an IR font asset without captured descriptors serializes as it did before",
          "[design-import][font-assets]") {
    // A document written before the descriptors existed parses to empty ones
    // and re-serializes byte for byte, so older readers see the same file.
    const std::string before = serialize_design_ir([] {
        DesignIR ir;
        ir.root.type = "frame";
        ir.root.name = "R";
        IRFontAsset fa;
        fa.family = "Clash Grotesk";
        fa.style = "Medium";
        fa.weight = 500;
        fa.asset_id = "userfont-1";
        fa.resolved_path = "/x/clash.ttf";
        ir.font_family_assets.push_back(fa);
        return ir;
    }());
    for (const char* key : {"fontStyle", "weightMax", "unicodeRange"})
        CHECK(before.find(key) == std::string::npos);
    const auto reparsed = parse_design_ir_json(before);
    REQUIRE(reparsed.font_family_assets.size() == 1);
    CHECK(reparsed.font_family_assets[0].font_style.empty());
    CHECK(reparsed.font_family_assets[0].weight_max == 0);
    CHECK(reparsed.font_family_assets[0].unicode_range.empty());
    CHECK(serialize_design_ir(reparsed) == before);
}

TEST_CASE("Figma plugin font assets carry no captured @font-face descriptors",
          "[design-import][font-assets][figma-plugin]") {
    auto ir = parse_figma_plugin_json(R"json({
        "format_version": "2026.05-figma-plugin-v1",
        "provenance": {"adapter": "figma-plugin"},
        "asset_manifest": {"version": 1, "assets": [
            {"asset_id": "userfont-1", "local_path": "assets/clash.ttf", "mime": "font/ttf"}]},
        "font_family_assets": [
            {"family": "Clash Grotesk", "style": "Italic", "weight": 500,
             "italic": true, "asset_id": "userfont-1"}],
        "root": {"type": "frame", "name": "R", "figma_node_id": "1:1",
                 "style": {"fontFamily": "Clash Grotesk"}}
    })json");
    REQUIRE(ir.font_family_assets.size() == 1);
    const auto& fa = ir.font_family_assets[0];
    CHECK(fa.style == "Italic");
    CHECK(fa.font_style.empty());
    CHECK(fa.weight_max == 0);
    CHECK(fa.unicode_range.empty());
    const auto json = serialize_design_ir(ir);
    for (const char* key : {"fontStyle", "weightMax", "unicodeRange"})
        CHECK(json.find(key) == std::string::npos);
}

TEST_CASE("codegen registers a font beside the script by its sealed name",
          "[design-import][fonts][security]") {
    DesignIR ir;
    ir.root.type = "frame";
    IRAssetRef asset;
    asset.asset_id = "userfont-1";
    asset.local_path = "assets/0123abcd.ttf"; // what the export pass localizes to
    ir.asset_manifest.assets.push_back(asset);
    IRFontAsset fa;
    fa.family = "Sealed Grotesk";
    fa.weight = 400;
    fa.weight_max = 700;
    fa.asset_id = "userfont-1";
    fa.resolved_path = "assets/0123abcd.ttf";
    ir.font_family_assets.push_back(fa);

    CHECK(sealed_font_asset_name(ir, fa) == "assets/0123abcd.ttf");
    const auto js = generate_pulp_js(ir);
    CHECK(js.find("if (typeof registerEmbeddedFont === 'function') registerEmbeddedFont("
                  "'Sealed Grotesk', 'assets/0123abcd.ttf', 400, 700);") != std::string::npos);
    // The manifest id is not an embedded file name and never reaches the call.
    CHECK(js.find("'userfont-1'") == std::string::npos);
    // A sealed face emits no Filesystem-gated call, so the panel infers an
    // empty capability set and runs under an empty grant.
    CHECK(js.find("registerFont") == std::string::npos);
    CHECK(infer_capability_names_from_js(js).empty());

    // A face reachable only by an absolute path keeps the gated fallback, and
    // the panel honestly declares Filesystem for it.
    DesignIR absolute = ir;
    absolute.asset_manifest.assets[0].local_path = "/design/pack/clash.ttf";
    absolute.font_family_assets[0].resolved_path = "/design/pack/clash.ttf";
    CHECK(sealed_font_asset_name(absolute, absolute.font_family_assets[0]).empty());
    const auto absolute_js = generate_pulp_js(absolute);
    CHECK(absolute_js.find("registerEmbeddedFont") == std::string::npos);
    CHECK(absolute_js.find("registerFont('Sealed Grotesk', '/design/pack/clash.ttf', 400, 700)") !=
          std::string::npos);
    CHECK(infer_capability_names_from_js(absolute_js) ==
          std::vector<std::string>{std::string(capability_name(ReloadCapability::Filesystem))});

    // Names that are not script-relative are never offered as sealed.
    for (const char* unsafe : {"../assets/0123abcd.ttf", "assets/../../x.ttf",
                               "file://assets/x.ttf", "/etc/hosts", "C:/fonts/x.ttf"}) {
        DesignIR probe = ir;
        probe.asset_manifest.assets.clear();
        probe.font_family_assets[0].resolved_path = unsafe;
        INFO(unsafe);
        CHECK(sealed_font_asset_name(probe, probe.font_family_assets[0]).empty());
    }
    // A leading ./ normalizes to the embedded name.
    DesignIR dotted = ir;
    dotted.asset_manifest.assets[0].local_path = "./assets/0123abcd.ttf";
    CHECK(sealed_font_asset_name(dotted, dotted.font_family_assets[0]) == "assets/0123abcd.ttf");
}
