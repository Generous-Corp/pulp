#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "tools/import-design/browser_capture_ir.hpp"
#include "tools/import-design/browser_capture_styles.hpp"

#include <pulp/runtime/crypto.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct TempCapture {
    fs::path root;

    TempCapture() {
        root = fs::temp_directory_path() /
               ("pulp-browser-capture-ir-test-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        fs::create_directories(root);
    }
    ~TempCapture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(std::string_view name, std::string_view bytes) const {
        std::ofstream out(root / name, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
};

std::string png_header(int width, int height) {
    std::string bytes(24, '\0');
    const unsigned char signature[] =
        {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::copy(std::begin(signature), std::end(signature), bytes.begin());
    bytes[12] = 'I';
    bytes[13] = 'H';
    bytes[14] = 'D';
    bytes[15] = 'R';
    auto write_be32 = [&](int offset, int value) {
        bytes[offset] = static_cast<char>((value >> 24) & 0xff);
        bytes[offset + 1] = static_cast<char>((value >> 16) & 0xff);
        bytes[offset + 2] = static_cast<char>((value >> 8) & 0xff);
        bytes[offset + 3] = static_cast<char>(value & 0xff);
    };
    write_be32(16, width);
    write_be32(20, height);
    return bytes;
}

std::string envelope(
    std::string_view reference_path = "browser.png",
    std::string_view png_hash =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") {
    return std::string(R"JSON({
  "schema":"pulp-browser-capture-v1",
  "version":1,
  "provenance":{
    "capture_method":"chromium-cdp",
    "source":{"entry":"editor.html","sha256":"sourcehash"},
    "settle":{"rounds":4,"stable_rounds":2,"elapsed_ms":120}
  },
  "documents":[{
    "id":"document:main",
    "url":"http://127.0.0.1/editor.html",
    "node_count":10,
    "layout_count":10,
    "paint_order":[],
    "snapshot_asset":"snapshot:main"
  }],
  "assets":[{
    "id":"reference:browser",
    "kind":"screenshot",
    "mime_type":"image/png",
    "path":"browser.png",
    "sha256":")JSON") + std::string(png_hash) + R"JSON(",
    "width_px":1912,
    "height_px":1272
  }],
  "semantics":{
    "schema":"pulp-browser-semantics-v1",
    "report":"semantic-report.json",
    "candidate_count":7,
    "resolved_count":2,
    "unresolved_count":5
  },
  "tokens":{
    "schema":"pulp-browser-tokens-v1",
    "report":"tokens.json",
    "color_count":1,
    "dimension_count":1,
    "string_count":2
  },
  "states":[{
    "name":"default",
    "reference_asset_id":"reference:browser"
  }],
  "reference":{
    "asset_id":"reference:browser",
    "path":")JSON" + std::string(reference_path) + R"JSON(",
    "logical_width":956,
    "logical_height":636,
    "device_scale_factor":2
  }
})JSON";
}

constexpr std::string_view kInteractionPlanHash =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

std::string interaction_report(
    std::string_view plan_hash = kInteractionPlanHash) {
    return std::string(R"JSON({
  "schema":"pulp-browser-interactions-v1",
  "version":1,
  "plan_sha256":")JSON") + std::string(plan_hash) + R"JSON(",
  "action_count":2,
  "actions":[
    {
      "action":"click",
      "selector":"#open",
      "timeout_ms":5000,
      "status":"completed"
    },
    {
      "action":"type",
      "selector":"input",
      "timeout_ms":5000,
      "text_length":7,
      "status":"completed"
    }
  ]
})JSON";
}

std::string with_interaction_provenance(
    std::string capture,
    std::string_view report_hash,
    std::string_view report_path = "interaction-report.json",
    std::string_view plan_hash = kInteractionPlanHash,
    int action_count = 2) {
    const std::string settle =
        R"JSON("settle":{"rounds":4,"stable_rounds":2,"elapsed_ms":120})JSON";
    const auto position = capture.find(settle);
    REQUIRE(position != std::string::npos);
    const std::string replacement = settle +
        R"JSON(,"interactions":{"schema":"pulp-browser-interactions-v1","version":1,"report":")JSON" +
        std::string(report_path) +
        R"JSON(","report_sha256":")JSON" + std::string(report_hash) +
        R"JSON(","plan_sha256":")JSON" + std::string(plan_hash) +
        R"JSON(","action_count":)JSON" + std::to_string(action_count) + "}";
    capture.replace(position, settle.size(), replacement);
    return capture;
}

void write_valid_reports(const TempCapture& temp) {
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[]
    })JSON");
    temp.write("tokens.json", R"JSON({
      "schema":"pulp-browser-tokens-v1",
      "version":1,
      "colors":{"css/accent":"#7c5cff"},
      "dimensions":{"css/radius":12},
      "strings":{"css/width":"100%","css/space":"calc(1rem + 2px)"},
      "source_identity":{}
    })JSON");
}

}  // namespace

TEST_CASE("browser capture envelope lowers to an honest faithful_capture DesignIR",
          "[import-design][browser-capture][ir]") {
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[]
    })JSON");
    temp.write("tokens.json", R"JSON({
      "schema":"pulp-browser-tokens-v1",
      "version":1,
      "colors":{"css/accent":"#7c5cff"},
      "dimensions":{"css/radius":12},
      "strings":{"css/width":"100%","css/space":"calc(1rem + 2px)"},
      "source_identity":{
        "css/accent":{
          "source_id":"--accent",
          "source_collection":"css-custom-properties",
          "source_mode":"computed-capture-light",
          "source_adapter":"browser-capture"
        }
      }
    })JSON");
    temp.write("capture.json", envelope(
        "browser.png", pulp::runtime::sha256_hex(png)));

    auto result = pulp::import_design::lower_browser_capture_to_ir(
        temp.root / "capture.json",
        {.source = pulp::view::DesignSource::claude,
         .source_file = "/source/editor.html"});

    REQUIRE(result);
    REQUIRE_FALSE(result.interaction_report);
    REQUIRE(result.reference_png == fs::weakly_canonical(temp.root / "browser.png"));
    REQUIRE(result.design_ir->capture_method == "chromium-cdp");
    REQUIRE(result.design_ir->settle_rounds == 4);
    REQUIRE(result.design_ir->root.render_mode ==
            pulp::view::NodeRenderMode::faithful_capture);
    REQUIRE(result.design_ir->root.capture_asset_id == "reference:browser");
    REQUIRE(result.design_ir->root.style.width == 956.0f);
    REQUIRE(result.design_ir->root.style.height == 636.0f);
    REQUIRE(result.design_ir->root.attributes.at(
                "browser_semantic_candidates") == "7");
    REQUIRE(result.design_ir->asset_manifest.assets.size() == 1);
    REQUIRE(result.design_ir->asset_manifest.assets[0].width == 1912);
    REQUIRE(result.design_ir->asset_manifest.assets[0].height == 1272);
    REQUIRE(result.design_ir->tokens.colors.at("css/accent") == "#7c5cff");
    REQUIRE(result.design_ir->tokens.dimensions.at("css/radius") == 12.0f);
    REQUIRE(result.design_ir->tokens.strings.at("css/width") == "100%");
    REQUIRE(result.design_ir->tokens.strings.at("css/space") ==
            "calc(1rem + 2px)");
    REQUIRE(result.design_ir->tokens.source_identity.at("css/accent").source_id ==
            "--accent");
    REQUIRE(result.design_ir->root.attributes.at("browser_capture_envelope") ==
            "pulp-capture:///capture.json");
    REQUIRE(result.design_ir->root.attributes.at("browser_semantic_report") ==
            "pulp-capture:///semantic-report.json");
}

TEST_CASE("browser interaction evidence is contained parsed and integrity-bound",
          "[import-design][browser-capture][ir][interactions][security]") {
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    const auto report = interaction_report();
    temp.write("browser.png", png);
    temp.write("interaction-report.json", report);
    write_valid_reports(temp);
    temp.write("capture.json", with_interaction_provenance(
        envelope("browser.png", pulp::runtime::sha256_hex(png)),
        pulp::runtime::sha256_hex(report)));
    pulp::import_design::BrowserCaptureIrOptions options;
    options.require_interaction_report = true;

    const auto result = pulp::import_design::lower_browser_capture_to_ir(
        temp.root / "capture.json", options);

    REQUIRE(result);
    REQUIRE(result.interaction_report);
    CHECK(*result.interaction_report ==
          fs::weakly_canonical(temp.root / "interaction-report.json"));
    CHECK(result.design_ir->root.attributes.at(
              "browser_interaction_report") ==
          "pulp-capture:///interaction-report.json");
    CHECK(result.design_ir->root.attributes.at(
              "browser_interaction_report_sha256") ==
          pulp::runtime::sha256_hex(report));
    CHECK(result.design_ir->root.attributes.at(
              "browser_interaction_plan_sha256") ==
          kInteractionPlanHash);
    CHECK(result.design_ir->root.attributes.at(
              "browser_interaction_action_count") == "2");
}

TEST_CASE("browser interaction lowering fails closed on missing or unsafe evidence",
          "[import-design][browser-capture][ir][interactions][security]") {
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    const auto report = interaction_report();
    temp.write("browser.png", png);
    write_valid_reports(temp);
    pulp::import_design::BrowserCaptureIrOptions options;
    options.require_interaction_report = true;

    SECTION("requested interactions require provenance") {
        temp.write("capture.json", envelope(
            "browser.png", pulp::runtime::sha256_hex(png)));
        const auto result =
            pulp::import_design::lower_browser_capture_to_ir(
                temp.root / "capture.json", options);
        REQUIRE_FALSE(result);
        CHECK(result.error.find("omitted interaction provenance") !=
              std::string::npos);
    }

    SECTION("interaction report cannot escape the capture directory") {
        temp.write("capture.json", with_interaction_provenance(
            envelope("browser.png", pulp::runtime::sha256_hex(png)),
            pulp::runtime::sha256_hex(report), "../interaction-report.json"));
        const auto result =
            pulp::import_design::lower_browser_capture_to_ir(
                temp.root / "capture.json", options);
        REQUIRE_FALSE(result);
        CHECK(result.error.find("escapes the capture directory") !=
              std::string::npos);
    }

    SECTION("interaction report bytes must match provenance") {
        temp.write("interaction-report.json", report + " ");
        temp.write("capture.json", with_interaction_provenance(
            envelope("browser.png", pulp::runtime::sha256_hex(png)),
            pulp::runtime::sha256_hex(report)));
        const auto result =
            pulp::import_design::lower_browser_capture_to_ir(
                temp.root / "capture.json", options);
        REQUIRE_FALSE(result);
        CHECK(result.error.find("report hash does not match") !=
              std::string::npos);
    }

    SECTION("interaction report must parse") {
        const std::string malformed = "{not-json";
        temp.write("interaction-report.json", malformed);
        temp.write("capture.json", with_interaction_provenance(
            envelope("browser.png", pulp::runtime::sha256_hex(png)),
            pulp::runtime::sha256_hex(malformed)));
        const auto result =
            pulp::import_design::lower_browser_capture_to_ir(
                temp.root / "capture.json", options);
        REQUIRE_FALSE(result);
        CHECK(result.error.find("invalid browser interaction JSON") !=
              std::string::npos);
    }

    SECTION("interaction plan identity must match the report") {
        const auto mismatched_report = interaction_report(
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        temp.write("interaction-report.json", mismatched_report);
        temp.write("capture.json", with_interaction_provenance(
            envelope("browser.png", pulp::runtime::sha256_hex(png)),
            pulp::runtime::sha256_hex(mismatched_report)));
        const auto result =
            pulp::import_design::lower_browser_capture_to_ir(
                temp.root / "capture.json", options);
        REQUIRE_FALSE(result);
        CHECK(result.error.find("does not match capture provenance") !=
              std::string::npos);
    }
}

TEST_CASE("browser capture lowering rejects sidecars outside the capture directory",
          "[import-design][browser-capture][security]") {
    TempCapture temp;
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5}
    })JSON");
    temp.write("tokens.json", R"JSON({
      "schema":"pulp-browser-tokens-v1",
      "version":1,
      "colors":{},"dimensions":{},"strings":{},"source_identity":{}
    })JSON");
    temp.write("capture.json", envelope("../outside.png"));

    auto result = pulp::import_design::lower_browser_capture_to_ir(
        temp.root / "capture.json");

    REQUIRE_FALSE(result);
    REQUIRE(result.error.find("escapes the capture directory") != std::string::npos);
}

TEST_CASE("browser capture lowering rejects unsafe v1 reference geometry",
          "[import-design][browser-capture][protocol]") {
    struct GeometryCase {
        std::string_view width;
        std::string_view height;
        std::string_view dpr;
        std::string_view expected_error;
    };
    const GeometryCase cases[] = {
        {"956", "636", "1", "requires DPR 2"},
        {"956.5", "636", "2", "finite positive integer"},
        {"1e999", "636", "2", "finite positive integer"},
        {"8193", "1", "2", "no larger than 8192"},
        {"8192", "2049", "2", "64 megapixel"},
    };

    for (const auto& geometry : cases) {
        TempCapture temp;
        auto capture = envelope();
        const auto replace_member =
            [&](std::string_view key, std::string_view original,
                std::string_view replacement) {
                const auto needle =
                    "\"" + std::string(key) + "\":" + std::string(original);
                const auto position = capture.find(needle);
                REQUIRE(position != std::string::npos);
                capture.replace(
                    position, needle.size(),
                    "\"" + std::string(key) + "\":" +
                        std::string(replacement));
            };
        replace_member("logical_width", "956", geometry.width);
        replace_member("logical_height", "636", geometry.height);
        replace_member("device_scale_factor", "2", geometry.dpr);
        temp.write("capture.json", capture);

        const auto result = pulp::import_design::lower_browser_capture_to_ir(
            temp.root / "capture.json");

        INFO("geometry " << geometry.width << "x" << geometry.height
                         << " DPR " << geometry.dpr);
        REQUIRE_FALSE(result);
        REQUIRE(result.error.find(geometry.expected_error) !=
                std::string::npos);
    }
}

TEST_CASE("browser capture lowering requires asset pixels to match logical geometry",
          "[import-design][browser-capture][protocol]") {
    TempCapture temp;
    const auto png = png_header(1910, 1272);
    temp.write("browser.png", png);
    write_valid_reports(temp);
    auto capture = envelope(
        "browser.png", pulp::runtime::sha256_hex(png));
    const auto width = capture.find("\"width_px\":1912");
    REQUIRE(width != std::string::npos);
    capture.replace(
        width, std::string("\"width_px\":1912").size(),
        "\"width_px\":1910");
    temp.write("capture.json", capture);

    const auto result = pulp::import_design::lower_browser_capture_to_ir(
        temp.root / "capture.json");

    REQUIRE_FALSE(result);
    REQUIRE(result.error.find(
                "pixel dimensions do not match logical dimensions and DPR") !=
            std::string::npos);
}

TEST_CASE("browser capture lowering rejects semantic count drift",
          "[import-design][browser-capture][protocol]") {
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":6,"resolved":2,"unresolved":4}
    })JSON");
    temp.write("tokens.json", R"JSON({
      "schema":"pulp-browser-tokens-v1",
      "version":1,
      "colors":{},"dimensions":{},"strings":{},"source_identity":{}
    })JSON");
    temp.write("capture.json", envelope(
        "browser.png", pulp::runtime::sha256_hex(png)));

    const auto result = pulp::import_design::lower_browser_capture_to_ir(
        temp.root / "capture.json");

    REQUIRE_FALSE(result);
    REQUIRE(result.error.find("does not match capture envelope") !=
            std::string::npos);
}

TEST_CASE("browser capture lowering rejects token count drift",
          "[import-design][browser-capture][protocol]") {
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5}
    })JSON");
    temp.write("tokens.json", R"JSON({
      "schema":"pulp-browser-tokens-v1",
      "version":1,
      "colors":{},"dimensions":{},"strings":{},"source_identity":{}
    })JSON");
    auto capture = envelope(
        "browser.png", pulp::runtime::sha256_hex(png));
    const auto count = capture.find("\"color_count\":1");
    REQUIRE(count != std::string::npos);
    capture.replace(count, std::string("\"color_count\":1").size(),
                    "\"color_count\":2");
    temp.write("capture.json", capture);

    const auto result = pulp::import_design::lower_browser_capture_to_ir(
        temp.root / "capture.json");

    REQUIRE_FALSE(result);
    REQUIRE(result.error.find("token counts do not match") !=
            std::string::npos);
}

TEST_CASE("browser capture lowering rejects state reference drift",
          "[import-design][browser-capture][protocol]") {
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5}
    })JSON");
    temp.write("tokens.json", R"JSON({
      "schema":"pulp-browser-tokens-v1",
      "version":1,
      "colors":{"css/accent":"#7c5cff"},
      "dimensions":{"css/radius":12},
      "strings":{"css/width":"100%","css/space":"calc(1rem + 2px)"},
      "source_identity":{}
    })JSON");
    auto capture = envelope(
        "browser.png", pulp::runtime::sha256_hex(png));
    const auto state_reference =
        capture.find("\"reference_asset_id\":\"reference:browser\"");
    REQUIRE(state_reference != std::string::npos);
    capture.replace(
        state_reference,
        std::string("\"reference_asset_id\":\"reference:browser\"").size(),
        "\"reference_asset_id\":\"reference:stale\"");
    temp.write("capture.json", capture);

    const auto result = pulp::import_design::lower_browser_capture_to_ir(
        temp.root / "capture.json");

    REQUIRE_FALSE(result);
    REQUIRE(result.error.find("canonical asset") != std::string::npos);
}

TEST_CASE("browser capture lowering rejects image dimension drift",
          "[import-design][browser-capture][protocol]") {
    TempCapture temp;
    temp.write("browser.png", png_header(100, 100));
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5}
    })JSON");
    temp.write("tokens.json", R"JSON({
      "schema":"pulp-browser-tokens-v1",
      "version":1,
      "colors":{},"dimensions":{},"strings":{},"source_identity":{}
    })JSON");
    temp.write("capture.json", envelope(
        "browser.png",
        pulp::runtime::sha256_hex(png_header(100, 100))));

    const auto result = pulp::import_design::lower_browser_capture_to_ir(
        temp.root / "capture.json");

    REQUIRE_FALSE(result);
    REQUIRE(result.error.find("PNG dimensions") != std::string::npos);
}

TEST_CASE("a control's own accent outranks the pack token",
          "[import-design][browser-capture][ir]") {
    // The pack token is a DEFAULT, not what a control ended up. A panel that
    // scopes its palette -- which a good one does -- leaves the token set
    // describing a colour no control on screen uses, and painting the value
    // layer from it puts a teal arc on an orange knob. Nothing catches that:
    // the arc is drawn by us and by nobody in the reference, so the A/B
    // comparison is identical either way.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"knob","binding_status":"bound","name":"stock",
         "accent":"",
         "bounds":{"left":0,"top":0,"width":64,"height":80},
         "paint_bounds":{"left":0,"top":0,"width":64,"height":64},
         "data_pulp":{"param":"a"}},
        {"kind":"knob","binding_status":"bound","name":"scoped",
         "accent":"#ff7a1a",
         "bounds":{"left":80,"top":0,"width":64,"height":80},
         "paint_bounds":{"left":80,"top":0,"width":64,"height":64},
         "data_pulp":{"param":"b"}}
      ]
    })JSON");
    temp.write("tokens.json", R"JSON({
      "schema":"pulp-browser-tokens-v1",
      "version":1,
      "colors":{"css/accent":"#16dac2"},
      "dimensions":{"css/radius":12},
      "strings":{"css/width":"100%","css/space":"1rem"},
      "source_identity":{}
    })JSON");
    temp.write("capture.json", envelope(
        "browser.png", pulp::runtime::sha256_hex(png)));

    const auto result = pulp::import_design::lower_browser_capture_to_ir(
        temp.root / "capture.json");
    REQUIRE(result);

    std::map<std::string, std::string> accent_by_binding;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            const auto binding = node.attributes.find("binding");
            if (binding != node.attributes.end()) {
                const auto accent = node.attributes.find("design_accent");
                accent_by_binding[binding->second] =
                    accent == node.attributes.end() ? "" : accent->second;
            }
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);

    REQUIRE(accent_by_binding.size() == 2);
    // The scoped control keeps its OWN colour...
    REQUIRE(accent_by_binding.at("b") == "#ff7a1a");
    // ...and the one that declared none still falls back to the pack token,
    // so the fix adds per-control accuracy without dropping the default.
    REQUIRE(accent_by_binding.at("a") == "#16dac2");
    // The real regression is the two collapsing to one value: that is what
    // reading the pack token for every control looks like from here.
    REQUIRE(accent_by_binding.at("a") != accent_by_binding.at("b"));
}

TEST_CASE("a lowered control carries what the binding-helper gate requires",
          "[import-design][browser-capture][ir]") {
    // A param key alone puts a control in the binding MANIFEST but emits no
    // binding code: collect_resolved_binding_plan admits a helper route only
    // when the node ALSO has a route id and a stable anchor, because the
    // emitted helper finds its widget by anchor and claims it by route id.
    // Carrying only the key produces a manifest full of bindings that nothing
    // applies -- a panel that reads as wired and moves nothing.
    //
    // Anchors must also be DISTINCT. The generated lookup requires exactly one
    // match and treats two as no match, so two controls sharing an anchor
    // silently bind neither -- which is why the key alone is not enough of an
    // anchor: a meter may legitimately share its macro with the control that
    // drives it.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"knob","binding_status":"bound","name":"drive",
         "bounds":{"left":0,"top":0,"width":64,"height":80},
         "paint_bounds":{"left":0,"top":0,"width":64,"height":64},
         "data_pulp":{"param":"shared"}},
        {"kind":"meter","binding_status":"bound","name":"readout",
         "bounds":{"left":80,"top":0,"width":10,"height":64},
         "data_pulp":{"meter":"shared"}}
      ]
    })JSON");
    temp.write("tokens.json", R"JSON({
      "schema":"pulp-browser-tokens-v1",
      "version":1,
      "colors":{"css/accent":"#16dac2"},
      "dimensions":{"css/radius":12},
      "strings":{"css/width":"100%","css/space":"1rem"},
      "source_identity":{}
    })JSON");
    temp.write("capture.json", envelope(
        "browser.png", pulp::runtime::sha256_hex(png)));

    const auto result = pulp::import_design::lower_browser_capture_to_ir(
        temp.root / "capture.json");
    REQUIRE(result);

    std::vector<const pulp::view::IRNode*> controls;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            if (node.attributes.count("pulpParamKey") != 0) controls.push_back(&node);
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);

    REQUIRE(controls.size() == 2);
    std::set<std::string> anchors;
    for (const auto* control : controls) {
        const auto route = control->attributes.find("pulpRouteId");
        REQUIRE(route != control->attributes.end());
        REQUIRE_FALSE(route->second.empty());
        REQUIRE(control->stable_anchor_id.has_value());
        REQUIRE_FALSE(control->stable_anchor_id->empty());
        // The helper claims by route id the view it found by anchor, so the two
        // must agree or the claim rejects the widget the lookup just resolved.
        REQUIRE(route->second == *control->stable_anchor_id);
        anchors.insert(*control->stable_anchor_id);
    }
    // Both controls name the SAME macro, so a macro-keyed anchor would collide
    // here and bind neither.
    REQUIRE(anchors.size() == 2);
}

#ifdef PULP_BROWSER_CAPTURE_STYLE_FIXTURE_DIR
// Chrome solves the page; the lowering's job is to carry that solved appearance
// into the IR rather than leave the native nodes as transparent hit targets over
// a screenshot. The fixture is a real capture, so these assertions run against
// what Chrome actually serializes — including the `/` alpha separator in
// `oklab(L a b / A)`, which a path-redaction pass in the capture runtime used to
// rewrite to "<local-path>" and make every translucent colour unparseable.
TEST_CASE("a lowered control carries the appearance Chrome solved",
          "[import-design][browser-capture][computed-style]") {
    const fs::path fixture{PULP_BROWSER_CAPTURE_STYLE_FIXTURE_DIR};
    REQUIRE(fs::exists(fixture / "capture.json"));

    const auto result = pulp::import_design::lower_browser_capture_to_ir(
        fixture / "capture.json");
    REQUIRE(result);

    std::vector<const pulp::view::IRNode*> controls;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            if (node.attributes.count("pulpParamKey") != 0)
                controls.push_back(&node);
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);
    REQUIRE(controls.size() == 2);

    const auto& styled = result.design_ir->root.attributes.at(
        "controls_with_captured_style");
    CHECK(styled == "2");

    for (const auto* control : controls) {
        const auto& style = control->style;

        // The knob face is painted by a radial gradient, not a flat fill. The
        // gradient is the single clearest proof that the appearance survived:
        // it exists only in the computed value, never in the semantic report.
        REQUIRE(style.background_gradient.has_value());
        CHECK(style.background_gradient->find("radial-gradient") !=
              std::string::npos);
        // color-mix() resolves to oklab() in Chrome's computed value. Its alpha
        // arrives after a literal "/" — the exact byte the redaction pass ate.
        CHECK(style.background_gradient->find("oklab(") != std::string::npos);
        CHECK(style.background_gradient->find("<local-path>") ==
              std::string::npos);

        // `border-radius: 50%` on the face is a circle, so the percentage has
        // to be resolved against the element's own box to survive as px. The
        // reference is the 98px border box (96px content plus a 1px border per
        // side) — the same box the paint bounds report — not the content box.
        REQUIRE(style.border_radius.has_value());
        CHECK(*style.border_radius == Catch::Approx(49.0f).margin(0.5));

        // A uniform radius populates all four corners as well as the single
        // slot. Asserting the corners here — not only that a circle renders —
        // is the point: the four fields existed and had a consumer long before
        // anything wrote to them, so a render test passed while the IR carried
        // nothing. Absence and agreement are different states.
        REQUIRE(style.border_top_left_radius.has_value());
        REQUIRE(style.border_top_right_radius.has_value());
        REQUIRE(style.border_bottom_right_radius.has_value());
        REQUIRE(style.border_bottom_left_radius.has_value());
        CHECK(*style.border_top_left_radius == Catch::Approx(49.0f).margin(0.5));
        CHECK(*style.border_bottom_right_radius ==
              Catch::Approx(49.0f).margin(0.5));

        // Three layered shadows, two of them inset. Collapsing the declaration
        // to one string would drop every layer past the first.
        REQUIRE(style.box_shadow.size() == 3);
        int inset_layers = 0;
        for (const auto& layer : style.box_shadow) {
            CHECK_FALSE(layer.color.empty());
            CHECK(layer.color.find("<local-path>") == std::string::npos);
            if (layer.inset) ++inset_layers;
        }
        CHECK(inset_layers == 2);
        // Ordered, and the offsets are real numbers rather than a parse that
        // silently produced zeroes.
        CHECK(style.box_shadow[0].offset_y == Catch::Approx(2.0f));
        CHECK(style.box_shadow[1].offset_y == Catch::Approx(-14.0f));
        CHECK(style.box_shadow[1].blur == Catch::Approx(22.0f));

        // A translucent accent border, carried per side and as the uniform
        // shorthand.
        REQUIRE(style.border_top_color.has_value());
        CHECK(style.border_top_color->find("oklab(") != std::string::npos);
        CHECK(style.border_top_color->find('/') != std::string::npos);
        REQUIRE(style.border_width.has_value());
        CHECK(*style.border_width == Catch::Approx(1.0f));

        REQUIRE(style.filter.has_value());
        CHECK(style.filter->find("saturate") != std::string::npos);

        // Appearance only: the design's paint box stays the placement
        // authority, so the page's own layout values must not leak in.
        REQUIRE(style.position.has_value());
        CHECK(*style.position == "absolute");
        REQUIRE(style.width.has_value());
        CHECK(*style.width == Catch::Approx(98.0f));
    }

    // The full-frame capture node is untouched by style lowering: it still
    // backs the panel, so nothing regresses visually while the controls gain
    // appearance of their own.
    const auto& root = result.design_ir->root;
    REQUIRE_FALSE(root.children.empty());
    const auto& capture = root.children.front();
    CHECK(capture.style.object_fit.has_value());
    CHECK_FALSE(capture.style.background_gradient.has_value());
}
#endif  // PULP_BROWSER_CAPTURE_STYLE_FIXTURE_DIR

// Direct cover for the CSS→IR mapping, independent of a capture: these are the
// shapes Chrome's computed values actually take.
TEST_CASE("computed style mapping folds CSS onto the IR",
          "[import-design][browser-capture][computed-style]") {
    using pulp::import_design::apply_computed_styles;
    using pulp::import_design::CapturedBox;
    using pulp::import_design::parse_box_shadow;

    SECTION("shadow layers keep their order, inset flag, and colour commas") {
        const auto layers = parse_box_shadow(
            "rgba(0, 0, 0, 0.5) 0px 1px 2px 0px inset, "
            "oklab(0.87 -0.2 0.13 / 0.34) 4px -8px 16px 2px");
        REQUIRE(layers.size() == 2);
        // A naive comma split would shred rgba() into four bogus layers.
        CHECK(layers[0].color == "rgba(0, 0, 0, 0.5)");
        CHECK(layers[0].inset);
        CHECK(layers[0].offset_y == Catch::Approx(1.0f));
        CHECK(layers[0].blur == Catch::Approx(2.0f));
        CHECK(layers[1].color == "oklab(0.87 -0.2 0.13 / 0.34)");
        CHECK_FALSE(layers[1].inset);
        CHECK(layers[1].offset_x == Catch::Approx(4.0f));
        CHECK(layers[1].offset_y == Catch::Approx(-8.0f));
        CHECK(layers[1].spread == Catch::Approx(2.0f));
    }

    SECTION("a per-corner radius keeps all four corners") {
        // The card idiom: a media area rounded at the top and square where it
        // meets its caption. Taking only the first corner of the shorthand
        // renders a plain rectangle inside a rounded border — the corners are
        // where the two disagree, so a uniform-radius fixture cannot catch it.
        CapturedBox box{0.0, 0.0, 240.0, 160.0};
        pulp::view::IRStyle style;
        apply_computed_styles({{"border-radius", "12px 12px 0px 0px"}}, box,
                              style);
        REQUIRE(style.border_top_left_radius.has_value());
        REQUIRE(style.border_top_right_radius.has_value());
        REQUIRE(style.border_bottom_right_radius.has_value());
        REQUIRE(style.border_bottom_left_radius.has_value());
        CHECK(*style.border_top_left_radius == Catch::Approx(12.0f));
        CHECK(*style.border_top_right_radius == Catch::Approx(12.0f));
        CHECK(*style.border_bottom_right_radius == Catch::Approx(0.0f));
        CHECK(*style.border_bottom_left_radius == Catch::Approx(0.0f));
        // The single slot must stay empty when the corners disagree: a
        // consumer reading it alone would otherwise round all four.
        CHECK_FALSE(style.border_radius.has_value());
    }

    SECTION("two- and three-value radius shorthands expand per CSS") {
        // 2 values: TL/BR then TR/BL. 3 values: TL, TR/BL, BR. Both are
        // diagonal pairings that a left-to-right reading gets wrong.
        CapturedBox box{0.0, 0.0, 240.0, 160.0};
        pulp::view::IRStyle two;
        apply_computed_styles({{"border-radius", "4px 16px"}}, box, two);
        CHECK(*two.border_top_left_radius == Catch::Approx(4.0f));
        CHECK(*two.border_bottom_right_radius == Catch::Approx(4.0f));
        CHECK(*two.border_top_right_radius == Catch::Approx(16.0f));
        CHECK(*two.border_bottom_left_radius == Catch::Approx(16.0f));

        pulp::view::IRStyle three;
        apply_computed_styles({{"border-radius", "2px 6px 10px"}}, box, three);
        CHECK(*three.border_top_left_radius == Catch::Approx(2.0f));
        CHECK(*three.border_top_right_radius == Catch::Approx(6.0f));
        CHECK(*three.border_bottom_left_radius == Catch::Approx(6.0f));
        CHECK(*three.border_bottom_right_radius == Catch::Approx(10.0f));
    }

    SECTION("an elliptical radius keeps its horizontal axis") {
        // Nothing downstream carries two radii per corner. Keeping the
        // horizontal axis is wrong-but-round; dropping the declaration would
        // paint a square corner, which is further from the design.
        CapturedBox box{0.0, 0.0, 240.0, 160.0};
        pulp::view::IRStyle style;
        apply_computed_styles({{"border-radius", "10px 10px 10px 10px / 20px"}},
                              box, style);
        REQUIRE(style.border_top_left_radius.has_value());
        CHECK(*style.border_top_left_radius == Catch::Approx(10.0f));
    }

    SECTION("a zero-width border contributes no colour") {
        // Chrome reports border-*-color on every element whether or not a
        // border is drawn, so an ungated mapping invents a border here.
        CapturedBox box{0.0, 0.0, 40.0, 20.0};
        pulp::view::IRStyle style;
        apply_computed_styles({{"border-top-width", "0px"},
                               {"border-right-width", "0px"},
                               {"border-bottom-width", "0px"},
                               {"border-left-width", "0px"},
                               {"border-top-color", "rgb(184, 248, 192)"},
                               {"border-right-color", "rgb(184, 248, 192)"},
                               {"border-bottom-color", "rgb(184, 248, 192)"},
                               {"border-left-color", "rgb(184, 248, 192)"},
                               {"border-top-style", "solid"}},
                              box, style);
        CHECK_FALSE(style.border_top_color.has_value());
        CHECK_FALSE(style.border_color.has_value());
        CHECK_FALSE(style.border.has_value());
        CHECK_FALSE(style.border_style.has_value());
    }

    SECTION("a real border survives as sides and shorthand") {
        CapturedBox box{0.0, 0.0, 40.0, 20.0};
        pulp::view::IRStyle style;
        apply_computed_styles({{"border-top-width", "2px"},
                               {"border-right-width", "2px"},
                               {"border-bottom-width", "2px"},
                               {"border-left-width", "2px"},
                               {"border-top-color", "rgb(1, 2, 3)"},
                               {"border-right-color", "rgb(1, 2, 3)"},
                               {"border-bottom-color", "rgb(1, 2, 3)"},
                               {"border-left-color", "rgb(1, 2, 3)"},
                               {"border-top-style", "solid"}},
                              box, style);
        REQUIRE(style.border_color.has_value());
        CHECK(*style.border_color == "rgb(1, 2, 3)");
        REQUIRE(style.border.has_value());
        CHECK(*style.border == "2px solid rgb(1, 2, 3)");
    }

    SECTION("a style on an edge other than the top is not dropped") {
        // Chrome computes a style per edge, and the capture records all four.
        // Reading only `border-top-style` lost the style of any border that is
        // not on the top edge: this box has a dashed LEFT border and a top
        // edge of zero width, so the only style it has was never read and it
        // painted solid.
        CapturedBox box{0.0, 0.0, 40.0, 20.0};
        pulp::view::IRStyle style;
        apply_computed_styles({{"border-top-width", "0px"},
                               {"border-right-width", "0px"},
                               {"border-bottom-width", "0px"},
                               {"border-left-width", "3px"},
                               {"border-left-color", "rgb(1, 2, 3)"},
                               {"border-top-style", "none"},
                               {"border-right-style", "none"},
                               {"border-bottom-style", "none"},
                               {"border-left-style", "dashed"}},
                              box, style);
        REQUIRE(style.border_style.has_value());
        CHECK(*style.border_style == "dashed");
        // The edge that has no width contributes no style either: `none` on
        // the other three is Chrome's report for "there is no border here",
        // and taking it would turn the one real edge off.
        REQUIRE(style.border_left_width.has_value());
        CHECK(*style.border_left_width == Catch::Approx(3.0f));
    }

    SECTION("edges that disagree about style emit no shorthand") {
        // The shorthand is a single `<width> <style> <color>` string, so a box
        // whose edges carry different styles cannot be described by one. Left
        // ungated, it took the first style it found and asserted it of all
        // four — a solid frame reported as dashed, or the reverse.
        CapturedBox box{0.0, 0.0, 40.0, 20.0};
        pulp::view::IRStyle style;
        apply_computed_styles({{"border-top-width", "2px"},
                               {"border-right-width", "2px"},
                               {"border-bottom-width", "2px"},
                               {"border-left-width", "2px"},
                               {"border-top-color", "rgb(1, 2, 3)"},
                               {"border-right-color", "rgb(1, 2, 3)"},
                               {"border-bottom-color", "rgb(1, 2, 3)"},
                               {"border-left-color", "rgb(1, 2, 3)"},
                               {"border-top-style", "solid"},
                               {"border-right-style", "solid"},
                               {"border-bottom-style", "dashed"},
                               {"border-left-style", "solid"}},
                              box, style);
        CHECK_FALSE(style.border.has_value());
        // The uniform width and colour still hold; only the one-string
        // shorthand is refused.
        REQUIRE(style.border_width.has_value());
        CHECK(*style.border_width == Catch::Approx(2.0f));
    }

    SECTION("transparent and absent values are not recorded as appearance") {
        CapturedBox box{0.0, 0.0, 40.0, 20.0};
        pulp::view::IRStyle style;
        apply_computed_styles({{"background-color", "rgba(0, 0, 0, 0)"},
                               {"background-image", "none"},
                               {"filter", "none"},
                               {"letter-spacing", "normal"},
                               {"opacity", "1"},
                               {"mix-blend-mode", "normal"},
                               {"transform", "matrix(2, 0, 0, 2, 0, 0)"}},
                              box, style);
        CHECK_FALSE(style.background_color.has_value());
        CHECK_FALSE(style.background_gradient.has_value());
        CHECK_FALSE(style.filter.has_value());
        CHECK_FALSE(style.letter_spacing.has_value());
        CHECK_FALSE(style.opacity.has_value());
        CHECK_FALSE(style.mix_blend_mode.has_value());
        // The node is placed by its already-transformed paint box, so carrying
        // the matrix too would apply it twice.
        CHECK_FALSE(style.transform.has_value());
    }

    SECTION("percentage radius resolves against the element's own box") {
        CapturedBox box{0.0, 0.0, 80.0, 40.0};
        pulp::view::IRStyle style;
        apply_computed_styles({{"border-radius", "50%"}}, box, style);
        REQUIRE(style.border_radius.has_value());
        CHECK(*style.border_radius == Catch::Approx(20.0f));
    }
}
