#include "test_design_import_shared.hpp"
#include <pulp/view/widgets.hpp>

TEST_CASE("captured fader thumb travel stays inside the content box",
          "[view][widgets][fader][geometry]") {
    CHECK(Fader::thumb_axis_position(100.0f, 1.0f, 1.0f, 10.0f, 0.0f) == Approx(6.0f));
    CHECK(Fader::thumb_axis_position(100.0f, 1.0f, 1.0f, 10.0f, 1.0f) == Approx(94.0f));
    CHECK(Fader::thumb_axis_position(100.0f, 1.0f, 1.0f, 10.0f, 0.5f) == Approx(50.0f));
}

TEST_CASE("persisted browser faders retain split indicator asset metadata",
          "[view][import][ir-v1][fader][assets]") {
    const auto temp = pulp::test::make_unique_temp_dir("fader-persist");
    const auto indicator = temp / "fader-indicator.png";
    {
        std::ofstream out(indicator, std::ios::binary);
        REQUIRE(out.good());
        out << "captured-indicator";
    }
    DesignIR ir;
    ir.root.type = "frame";
    IRNode fader;
    fader.type = "frame";
    fader.audio_widget = AudioWidgetType::fader;
    fader.attributes["fader_indicator_asset_path"] = indicator.string();
    fader.attributes["fader_indicator_natural_w"] = "18";
    fader.attributes["fader_indicator_natural_h"] = "10";
    ir.root.children.push_back(std::move(fader));
    refresh_design_ir_asset_manifest(ir, {.base_directory = temp});
    REQUIRE(ir.asset_manifest.assets.size() == 1);
    REQUIRE(ir.root.children.front().attributes.count("fader_indicator_asset_pathAssetId") == 1);
    const auto reparsed = parse_design_ir_json(serialize_design_ir(ir));
    const auto& attrs = reparsed.root.children.front().attributes;
    CHECK(attrs.at("fader_indicator_asset_path") == indicator.string());
    CHECK(attrs.at("fader_indicator_natural_w") == "18");
    CHECK(attrs.at("fader_indicator_natural_h") == "10");
}

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

TEST_CASE("faithful_capture JS uses persisted manifest bytes without a scratch path",
          "[view][import][codegen][faithful-capture][assets]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.style.width = 780.0f;
    ir.root.style.height = 496.0f;
    // Browser-import provenance may put the capture's stable asset identity on
    // the wrapper too. It must not reclassify that wrapper as an image and
    // discard the live controls layered beside its captured backdrop.
    ir.root.attributes["asset_ref"] = "reference:browser";

    IRNode capture;
    capture.type = "frame";
    capture.render_mode = NodeRenderMode::faithful_capture;
    capture.capture_asset_id = "reference:browser";
    capture.style.width = 780.0f;
    capture.style.height = 496.0f;
    ir.root.children.push_back(capture);

    IRNode knob;
    knob.type = "frame";
    knob.name = "Rate";
    knob.audio_widget = AudioWidgetType::knob;
    knob.style.width = 64.0f;
    knob.style.height = 64.0f;
    ir.root.children.push_back(knob);

    IRAssetRef asset;
    asset.asset_id = "reference:browser";
    asset.original_uri = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAAB";
    asset.mime = "image/png";
    ir.asset_manifest.assets.push_back(asset);

    const auto native_js = generate_pulp_js(ir);
    REQUIRE(native_js.find("createImage(") != std::string::npos);
    REQUIRE(native_js.find("setImageSource(") != std::string::npos);
    REQUIRE(native_js.find("createKnob(") != std::string::npos);
    REQUIRE(native_js.find(asset.original_uri) != std::string::npos);
    REQUIRE(native_js.find("file://") == std::string::npos);

    CodeGenOptions web_options;
    web_options.mode = CodeGenMode::web_compat;
    const auto web_js = generate_pulp_js(ir, web_options);
    REQUIRE(web_js.find("document.createElement('img')") != std::string::npos);
    REQUIRE(web_js.find("document.createElement('knob')") != std::string::npos);
    REQUIRE(web_js.find(".src = '") != std::string::npos);
    REQUIRE(web_js.find(asset.original_uri) != std::string::npos);

    asset.original_uri = "https://example.invalid/capture.png";
    ir.asset_manifest.assets.clear();
    ir.asset_manifest.assets.push_back(asset);

    const auto unresolved_native_js = generate_pulp_js(ir);
    REQUIRE(unresolved_native_js.find("setImageSource(") == std::string::npos);
    REQUIRE(unresolved_native_js.find(asset.original_uri) == std::string::npos);

    const auto unresolved_web_js = generate_pulp_js(ir, web_options);
    REQUIRE(unresolved_web_js.find(".src = '") == std::string::npos);
    REQUIRE(unresolved_web_js.find(asset.original_uri) == std::string::npos);
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

TEST_CASE("DesignIR JSON keeps Chromium's sub-pixel layout on large boxes",
          "[view][import][ir-v1]") {
    // Chromium lays out in 1/64 px. A stream's default six significant digits
    // wrote 1006.703125 as 1006.7, so the panel no longer matched its capture.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.style.width = 1280.015625f;
    ir.root.style.height = 1006.703125f;
    ir.root.style.opacity = 0.1f;
    const auto json = serialize_design_ir(ir);
    REQUIRE(json.find("\"height\":1006.7,") == std::string::npos);
    REQUIRE(json.find("\"height\":1006.7031") != std::string::npos);
    REQUIRE(json.find("\"width\":1280.0156") != std::string::npos);
    // Shortest text that reads back as the same float, not a padded expansion.
    REQUIRE(json.find("\"opacity\":0.1") != std::string::npos);
    const auto parsed = parse_design_ir_json(json);
    REQUIRE(parsed.root.style.width == ir.root.style.width);
    REQUIRE(parsed.root.style.height == ir.root.style.height);
}
