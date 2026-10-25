// DesignIR's shipped fonts (`fontFamilyAssets`): the captured @font-face
// descriptors round-trip, and documents without them, Figma exports included,
// serialize exactly as they did before the descriptors existed.
#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_ir_io.hpp>
#include <pulp/view/design_sources.hpp>

#include <string>

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
