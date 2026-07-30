#include "test_design_import_shared.hpp"

TEST_CASE("DesignIR round-trips faithful browser capture backing distinctly from SVG",
          "[view][import][ir-v1][faithful-capture]") {
    DesignIR ir;
    ir.source = DesignSource::claude;
    ir.capture_method = "chromium-cdp";
    ir.root.type = "frame";
    ir.root.name = "Browser-evaluated HTML";
    ir.root.render_mode = NodeRenderMode::faithful_capture;
    ir.root.capture_asset_id = "reference:browser";
    ir.root.style.width = 956.0f;
    ir.root.style.height = 636.0f;

    const auto canonical = serialize_design_ir(ir);
    const auto parsed = parse_design_ir_json(canonical);

    REQUIRE(parsed.root.render_mode == NodeRenderMode::faithful_capture);
    REQUIRE(parsed.root.capture_asset_id == "reference:browser");
    REQUIRE_FALSE(parsed.root.svg_asset_id.has_value());
    REQUIRE(canonical.find("\"render_mode\":\"faithful_capture\"") !=
            std::string::npos);
    REQUIRE(canonical.find("\"capture_asset_id\":\"reference:browser\"") !=
            std::string::npos);
}

TEST_CASE("faithful_capture lowers to the captured image in JS and baked C++",
          "[view][import][codegen][faithful-capture]") {
    DesignIR ir;
    ir.source = DesignSource::claude;
    ir.root.type = "frame";
    ir.root.name = "Browser capture";
    ir.root.render_mode = NodeRenderMode::faithful_capture;
    ir.root.capture_asset_id = "reference:browser";
    // Browser capture is visual authority even when source semantics identify
    // a knob. Recognition must never replace authored knob art with Pulp's
    // silver/vector fallback.
    ir.root.audio_widget = AudioWidgetType::knob;
    ir.root.style.width = 956.0f;
    ir.root.style.height = 636.0f;
    ir.root.attributes["asset_path"] = "/capture/browser.png";

    IRAssetRef asset;
    asset.asset_id = "reference:browser";
    asset.local_path = "/capture/browser.png";
    asset.mime = "image/png";
    ir.asset_manifest.assets.push_back(asset);

    const auto js = generate_pulp_js(ir);
    REQUIRE(js.find("createImage(") != std::string::npos);
    REQUIRE(js.find("setImageSource(") != std::string::npos);
    REQUIRE(js.find("/capture/browser.png") != std::string::npos);
    REQUIRE(js.find("createKnob") == std::string::npos);

    const auto cpp = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(cpp.source.find("std::make_unique<pulp::view::ImageView>()") !=
            std::string::npos);
    REQUIRE(cpp.source.find(
        "set_image_source(\"file:///capture/browser.png\")") !=
            std::string::npos);
    REQUIRE(cpp.source.find("DesignFrameView") == std::string::npos);
    REQUIRE(cpp.source.find("Knob") == std::string::npos);
}

TEST_CASE("browser capture wire contract rejects unknown or incomplete render modes",
          "[view][import][ir-v1][faithful-capture]") {
    try {
        (void)parse_design_ir_json(
            R"({"version":"1.0","root":{"type":"frame","render_mode":"future_capture"}})");
        FAIL("unknown render mode must be rejected");
    } catch (const std::exception& error) {
        REQUIRE(std::string(error.what()) ==
                "unknown DesignIR render_mode: future_capture");
    }

    DesignIR ir;
    ir.root.type = "frame";
    ir.root.render_mode = NodeRenderMode::faithful_capture;
    try {
        (void)generate_pulp_js(ir);
        FAIL("faithful capture without an asset must be rejected");
    } catch (const std::exception& error) {
        REQUIRE(std::string(error.what()) ==
                "faithful_capture node requires capture_asset_id");
    }
}
