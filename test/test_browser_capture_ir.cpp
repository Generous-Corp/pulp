#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "tools/import-design/browser_capture_ir.hpp"
#include "tools/import-design/browser_capture_styles.hpp"
#include "tools/import-design/browser_capture_validation.hpp"

#include <pulp/runtime/crypto.hpp>
#include <pulp/view/screenshot_compare.hpp>

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

/// Splice a value onto the envelope's `reference` block. Written as a splice
/// rather than a parameter on envelope() so the shared helper keeps producing
/// the capture every other case in this file was written against.
std::string with_reference_member(std::string capture,
                                  std::string_view member) {
    const std::string anchor = R"JSON("device_scale_factor":2)JSON";
    const auto position = capture.find(anchor);
    REQUIRE(position != std::string::npos);
    capture.replace(position, anchor.size(),
                    anchor + "," + std::string(member));
    return capture;
}

std::string with_primary_surface(std::string capture,
                                 std::string_view surface) {
    const std::string settle =
        R"JSON("settle":{"rounds":4,"stable_rounds":2,"elapsed_ms":120})JSON";
    const auto position = capture.find(settle);
    REQUIRE(position != std::string::npos);
    capture.replace(position, settle.size(),
                    settle + R"JSON(,"viewport":{"document":{)JSON" +
                        R"JSON("width":956,"height":636,"primary_surface":)JSON" +
                        std::string(surface) + "}}");
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

/// Lower a synthetic capture whose envelope `decorate` has adjusted, and hand
/// back the root's attributes. The reference PNG is written first so the
/// envelope carries its real hash: the lowering verifies it.
std::map<std::string, std::string> lowered_root_attributes(
    const TempCapture& temp,
    const std::function<std::string(std::string)>& decorate) {
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    write_valid_reports(temp);
    temp.write("capture.json",
               decorate(envelope("browser.png",
                                 pulp::runtime::sha256_hex(png))));
    auto result = pulp::import_design::lower_browser_capture_to_ir(
        temp.root / "capture.json",
        {.source = pulp::view::DesignSource::claude,
         .source_file = "/source/editor.html"});
    REQUIRE(result);
    return {result.design_ir->root.attributes.begin(),
            result.design_ir->root.attributes.end()};
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
    std::vector<const pulp::view::IRNode*> drivers;
    std::vector<const pulp::view::IRNode*> displays;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            const bool drives = node.attributes.count("pulpParamKey") != 0;
            const bool shows = node.attributes.count("pulpMeterValueKey") != 0;
            if (drives) drivers.push_back(&node);
            if (shows) displays.push_back(&node);
            if (drives || shows) controls.push_back(&node);
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);

    REQUIRE(controls.size() == 2);
    // The knob DRIVES the macro and the meter only DISPLAYS it. Both under
    // pulpParamKey reads downstream as two controls competing for one
    // parameter, which a host that requires each macro to be driven exactly
    // once rejects -- on a panel whose author did the ordinary thing.
    REQUIRE(drivers.size() == 1);
    REQUIRE(displays.size() == 1);
    REQUIRE(drivers.front()->attributes.at("pulpParamKey") == "shared");
    REQUIRE(displays.front()->attributes.at("pulpMeterValueKey") == "shared");
    REQUIRE(displays.front()->attributes.count("pulpParamKey") == 0);
    // The shared "binding" key stays on both: the JS emitter reads one key and
    // branches on the widget type (bindMeter vs bindWidgetToParam).
    REQUIRE(displays.front()->attributes.at("binding") == "shared");
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

TEST_CASE("a zero-width pointer box is placed, not dropped",
          "[import-design][browser-capture][ir][knob][indicator]") {
    // The stroke could not be recovered, so the capture forwards the client
    // rect as it stands: zero across the pointer's own axis. The projection
    // must still place it.
    //
    // Refusing here is not a smaller failure than a wrong width, it is a much
    // larger one: with no knob_ind_* the sprite pass never stamps asset_path,
    // so the knob keeps its captured body, apply_designed_body_skin reinstalls
    // DesignedControlPainter, and it returns wearing the value arc and the
    // track ring over a face whose baked pointer was never erased. That is the
    // composited chrome this lane exists to remove.
    //
    // The consumer refuses a zero knob_ind_w and keeps its skin default
    // thickness (design_import_native_common.cpp), so the result is a thin
    // pointer in the RIGHT PLACE at the right angle -- radial extent, colour
    // and sweep all still come from the design.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"knob","binding_status":"bound","name":"bare",
         "bounds":{"left":8,"top":20,"width":96,"height":96},
         "paint_bounds":{"left":8,"top":20,"width":96,"height":96},
         "indicator_bounds":{"left":56,"top":30,"width":0,"height":38},
         "indicator_color":"rgb(255, 255, 255)",
         "data_pulp":{"param":"bare"}}
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
    INFO("lowering error: " << result.error);
    REQUIRE(result);

    const pulp::view::IRNode* found = nullptr;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            const auto binding = node.attributes.find("binding");
            if (binding != node.attributes.end() && binding->second == "bare")
                found = &node;
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);
    REQUIRE(found != nullptr);

    const auto& bare = found->attributes;
    // Placed: the radial extent survives even though the width did not.
    REQUIRE(bare.count("knob_ind_r_out") == 1);
    CHECK(std::stof(bare.at("knob_ind_r_in")) ==
          Catch::Approx(0.0f).margin(0.001f));
    CHECK(std::stof(bare.at("knob_ind_r_out")) ==
          Catch::Approx(0.79167f).margin(0.001f));
    CHECK(std::stof(bare.at("knob_ind_w")) ==
          Catch::Approx(0.0f).margin(0.001f));
    // And it stays on the sprite lane, which is the part that keeps the value
    // arc and the track ring off the design's own face.
    CHECK(bare.count("browser_sprite_crop_px") == 1);
    CHECK(bare.count("browser_sprite_indicator_px") == 1);
}

TEST_CASE("a pointer box with no extent at all is still refused",
          "[import-design][browser-capture][ir][knob][indicator]") {
    // The other side of the same guard. A 0x0 box genuinely carries no
    // direction, so there is nothing to place and refusing is right -- this
    // pins that relaxing the zero-AXIS case did not relax the empty-box case
    // with it.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"knob","binding_status":"bound","name":"empty",
         "bounds":{"left":8,"top":20,"width":96,"height":96},
         "paint_bounds":{"left":8,"top":20,"width":96,"height":96},
         "indicator_bounds":{"left":56,"top":30,"width":0,"height":0},
         "data_pulp":{"param":"empty"}}
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
    INFO("lowering error: " << result.error);
    REQUIRE(result);

    const pulp::view::IRNode* found = nullptr;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            const auto binding = node.attributes.find("binding");
            if (binding != node.attributes.end() && binding->second == "empty")
                found = &node;
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);
    REQUIRE(found != nullptr);
    CHECK(found->attributes.count("knob_ind_r_out") == 0);
    CHECK(found->attributes.count("knob_ind_w") == 0);
}

TEST_CASE("a pointer aimed straight up projects to a hairline, not a slab",
          "[import-design][browser-capture][ir][knob][indicator]") {
    // Twelve o'clock, which is where every centred bipolar parameter rests.
    //
    // An SVG <line> drawn straight up has ZERO width in its client rect --
    // getBoundingClientRect() does not include stroke -- so the capture recovers
    // the painted extent from the computed stroke and hands this pass a box that
    // is 4px wide and 38px long. This pins what the projection must then make of
    // it, because both plausible failures render as a believable knob: dropping
    // the box entirely falls back to the derived tick, and mistaking the length
    // for the width paints a slab across a third of the dial.
    //
    // Dial (8,20,96,96) so centre (56,68) and half-extent 48. Pointer box
    // (54,30,4,38), centre (56,49): 19px straight up from the dial centre,
    // reaching 19px along the radius either side of that. So the radial reach is
    // its own length and the width is its own width -- r_out = 38/48, and
    // knob_ind_w = 4/48, NOT 38/48.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    // The summary mirrors the shared envelope() helper's declared counts, which
    // the lowering cross-checks against; only the one candidate below matters.
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"knob","binding_status":"bound","name":"up",
         "bounds":{"left":8,"top":20,"width":96,"height":96},
         "paint_bounds":{"left":8,"top":20,"width":96,"height":96},
         "indicator_bounds":{"left":54,"top":30,"width":4,"height":38},
         "indicator_color":"rgb(255, 255, 255)",
         "data_pulp":{"param":"up"}}
      ]
    })JSON");
    // Counts here also have to match the envelope helper: 1 colour, 1
    // dimension, 2 strings. None of them bear on the geometry under test.
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
    INFO("lowering error: " << result.error);
    REQUIRE(result);

    const pulp::view::IRNode* found = nullptr;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            const auto binding = node.attributes.find("binding");
            if (binding != node.attributes.end() && binding->second == "up")
                found = &node;
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);
    REQUIRE(found != nullptr);

    const auto& up = found->attributes;
    REQUIRE(up.count("knob_ind_r_out") == 1);
    CHECK(std::stof(up.at("knob_ind_r_in")) ==
          Catch::Approx(0.0f).margin(0.001f));
    CHECK(std::stof(up.at("knob_ind_r_out")) ==
          Catch::Approx(0.79167f).margin(0.001f));
    // The whole point of the case. Knob::paint multiplies this by the dial's
    // half-extent, so 0.0833 draws the author's 4px stroke and 0.79 would draw a
    // 38px slab.
    CHECK(std::stof(up.at("knob_ind_w")) ==
          Catch::Approx(0.08333f).margin(0.001f));
}

TEST_CASE("a rotated pointer is measured in its own space, not its footprint",
          "[import-design][browser-capture][ir][knob][indicator]") {
    // The SAME 4x38 needle as the case above, turned 38 degrees and swung round
    // the dial to match, so every number it produces must come out unchanged: a
    // rotation moves a pointer, it does not widen one.
    //
    // getBoundingClientRect() reports the box the shape SWEEPS, not the shape,
    // so this needle arrives 26.5x32.4. Projecting that footprint is a correct
    // support function of the wrong box -- it yields 0.851, ten times the truth,
    // and paints a white slab over a third of the face. The fix is not a better
    // projection but a better box: the capture also records the needle in its
    // own coordinate space (4x38) with the matrix that places it, and the same
    // projection applied to the element's own axes gives its own width back.
    //
    // Dial (8,20,96,96) so centre (56,68) and half-extent 48. The needle's
    // centre is 19px out along the 38-degree radius, and its long axis lies
    // along that radius, so r_in = 0, r_out = 38/48 and knob_ind_w = 4/48 --
    // digit for digit what the unrotated needle above produces.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"knob","binding_status":"bound","name":"turned",
         "bounds":{"left":8,"top":20,"width":96,"height":96},
         "paint_bounds":{"left":8,"top":20,"width":96,"height":96},
         "indicator_bounds":{
           "left":54.423978,"top":36.824268,
           "width":26.547179,"height":32.407055,
           "intrinsic":{"width":4,"height":38},
           "transform":[0.788010754,0.615661475,
                        -0.615661475,0.788010754,77.819613,36.824268]},
         "indicator_color":"rgb(255, 255, 255)",
         "data_pulp":{"param":"turned"}}
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
    INFO("lowering error: " << result.error);
    REQUIRE(result);

    const pulp::view::IRNode* found = nullptr;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            const auto binding = node.attributes.find("binding");
            if (binding != node.attributes.end() && binding->second == "turned")
                found = &node;
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);
    REQUIRE(found != nullptr);

    const auto& turned = found->attributes;
    REQUIRE(turned.count("knob_ind_r_out") == 1);
    CHECK(std::stof(turned.at("knob_ind_r_in")) ==
          Catch::Approx(0.0f).margin(0.001f));
    CHECK(std::stof(turned.at("knob_ind_r_out")) ==
          Catch::Approx(0.79167f).margin(0.001f));
    // The whole case. 0.0833 is the author's 4px needle; 0.8515 is the footprint
    // its diagonal sweeps, and is what this produced before the element's own
    // box was recorded.
    CHECK(std::stof(turned.at("knob_ind_w")) ==
          Catch::Approx(0.08333f).margin(0.001f));

    // The sprite hand-off keeps the FOOTPRINT, which is the opposite choice and
    // the right one: that pass crops the control out of the flat capture and
    // erases the pointer baked into the crop, so it needs every pixel the
    // rotated needle covers, not the 4px the geometry describes. DPR 2.
    CHECK(turned.at("browser_sprite_indicator_px") == "109,74,53,64");
}

TEST_CASE("an asymmetric pointer uses its transformed intrinsic centre",
          "[import-design][browser-capture][ir][knob][indicator]") {
    // A rotated triangle need not paint symmetrically inside the transformed
    // rectangle returned by getBBox(). Its client-rect centre is therefore an
    // erasure hint, not its geometry centre. The intrinsic box plus full matrix
    // places the pointer at (50,25); the painted footprint is deliberately
    // centred at (55,25). Reading the latter would tilt and widen the result.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"knob","binding_status":"bound","name":"asymmetric",
         "bounds":{"left":0,"top":0,"width":100,"height":100},
         "paint_bounds":{"left":0,"top":0,"width":100,"height":100},
         "indicator_bounds":{
           "left":50,"top":10,"width":10,"height":30,
           "intrinsic":{"x":0,"y":0,"width":10,"height":30},
           "transform":[1,0,0,1,45,10]},
         "indicator_color":"rgb(255, 255, 255)",
         "data_pulp":{"param":"asymmetric"}}
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
    INFO("lowering error: " << result.error);
    REQUIRE(result);

    const pulp::view::IRNode* found = nullptr;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            const auto binding = node.attributes.find("binding");
            if (binding != node.attributes.end() &&
                binding->second == "asymmetric")
                found = &node;
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);
    REQUIRE(found != nullptr);

    const auto& pointer = found->attributes;
    CHECK(std::stof(pointer.at("knob_ind_r_in")) ==
          Catch::Approx(0.2f).margin(0.001f));
    CHECK(std::stof(pointer.at("knob_ind_r_out")) ==
          Catch::Approx(0.8f).margin(0.001f));
    CHECK(std::stof(pointer.at("knob_ind_w")) ==
          Catch::Approx(0.2f).margin(0.001f));
    // Erasure still consumes the painted footprint, not the intrinsic box.
    CHECK(pointer.at("browser_sprite_indicator_px") == "100,20,20,60");
}

TEST_CASE("a rotated pointer in a scaled viewBox keeps its user units straight",
          "[import-design][browser-capture][ir][knob][indicator]") {
    // The same needle again, this time authored in a 24-unit viewBox painted at
    // 96px: 1 user unit wide and 9.5 long, with a matrix carrying the viewBox's
    // 4x scale as well as the rotation.
    //
    // Element space is NOT CSS px, and nothing in the numbers says so -- 1x9.5
    // reads as a plausible hairline and 4x38 reads as a plausible needle. Only
    // the matrix distinguishes them, which is why the capture records the size
    // unscaled and the matrix beside it rather than pre-multiplying: a consumer
    // that reads the size alone is off by the viewBox scale, silently, and a
    // producer that bakes the scale in double-counts it for the next consumer
    // that reads the matrix.
    //
    // Same dial and same placement as the case above, so the answer must be the
    // same too: 4/48 wide, reaching 38/48.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"knob","binding_status":"bound","name":"scaled",
         "bounds":{"left":8,"top":20,"width":96,"height":96},
         "paint_bounds":{"left":8,"top":20,"width":96,"height":96},
         "indicator_bounds":{
           "left":54.423978,"top":36.824268,
           "width":26.547179,"height":32.407055,
           "intrinsic":{"width":1,"height":9.5},
           "transform":[3.152043014,2.462645901,
                        -2.462645901,3.152043014,77.819613,36.824268]},
         "indicator_color":"rgb(255, 255, 255)",
         "data_pulp":{"param":"scaled"}}
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
    INFO("lowering error: " << result.error);
    REQUIRE(result);

    const pulp::view::IRNode* found = nullptr;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            const auto binding = node.attributes.find("binding");
            if (binding != node.attributes.end() && binding->second == "scaled")
                found = &node;
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);
    REQUIRE(found != nullptr);

    const auto& scaled = found->attributes;
    REQUIRE(scaled.count("knob_ind_r_out") == 1);
    CHECK(std::stof(scaled.at("knob_ind_r_out")) ==
          Catch::Approx(0.79167f).margin(0.001f));
    // Reading the 1-unit width as 1 CSS px would give 0.0208 -- a quarter of the
    // truth, which renders as a thin pointer rather than as a miss.
    CHECK(std::stof(scaled.at("knob_ind_w")) ==
          Catch::Approx(0.08333f).margin(0.001f));
}

TEST_CASE("an unrotated pointer answers the same with or without its own box",
          "[import-design][browser-capture][ir][knob][indicator]") {
    // The positive control for the two cases above, and the reason they are
    // trustworthy.
    //
    // An axis-aligned pointer's footprint IS its own box, so recording the
    // element space must change nothing at all. Without this, a projection that
    // quietly became "narrow axis is the width, long axis is the radial reach"
    // would satisfy every rotated case here and still be wrong, because that
    // rule discards the corner reach: an 8x8 dot at 7 o'clock spans the radius
    // along its DIAGONAL, and its 0.1682 is not 8/50.
    //
    // Same dial and same dot as "a declared knob indicator lowers to movable
    // pointer geometry", which asserts these three numbers with no element space
    // recorded. Identical values from both paths is the claim.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"knob","binding_status":"bound","name":"dot",
         "bounds":{"left":10,"top":10,"width":100,"height":100},
         "paint_bounds":{"left":10,"top":10,"width":100,"height":100},
         "indicator_bounds":{
           "left":54,"top":18,"width":8,"height":8,
           "intrinsic":{"width":8,"height":8},
           "transform":[1,0,0,1,54,18]},
         "indicator_color":"rgb(184, 248, 192)",
         "data_pulp":{"param":"dot"}}
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
    INFO("lowering error: " << result.error);
    REQUIRE(result);

    const pulp::view::IRNode* found = nullptr;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            const auto binding = node.attributes.find("binding");
            if (binding != node.attributes.end() && binding->second == "dot")
                found = &node;
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);
    REQUIRE(found != nullptr);

    const auto& dot = found->attributes;
    REQUIRE(dot.count("knob_ind_r_out") == 1);
    CHECK(std::stof(dot.at("knob_ind_r_in")) ==
          Catch::Approx(0.6770f).margin(0.001f));
    CHECK(std::stof(dot.at("knob_ind_r_out")) ==
          Catch::Approx(0.8452f).margin(0.001f));
    CHECK(std::stof(dot.at("knob_ind_w")) ==
          Catch::Approx(0.1682f).margin(0.001f));
}

TEST_CASE("a rotated line keeps the guard that its footprint would defeat",
          "[import-design][browser-capture][ir][knob][indicator]") {
    // A marked shape with no extent on either axis carries no direction to sweep
    // along and is refused. The guard has to be read off the box the projection
    // uses, because the two boxes disagree in both directions once a rotation is
    // involved.
    //
    // Here the element space is 0x0 -- an empty <g>, a <path> with no data --
    // while the footprint the browser reports is a plausible 12x9. A guard left
    // behind on the footprint passes this, and the projection then divides a
    // pointer out of nothing. The reverse trap is the one already covered
    // upstream: a rotated <line> is zero-width in its own space and fat in its
    // footprint, so a guard that demands extent on both axes drops it.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"knob","binding_status":"bound","name":"empty",
         "bounds":{"left":8,"top":20,"width":96,"height":96},
         "paint_bounds":{"left":8,"top":20,"width":96,"height":96},
         "indicator_bounds":{
           "left":54,"top":30,"width":12,"height":9,
           "intrinsic":{"width":0,"height":0},
           "transform":[0.788010754,0.615661475,
                        -0.615661475,0.788010754,0,0]},
         "indicator_color":"rgb(255, 255, 255)",
         "data_pulp":{"param":"empty"}}
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
    INFO("lowering error: " << result.error);
    REQUIRE(result);

    const pulp::view::IRNode* found = nullptr;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            const auto binding = node.attributes.find("binding");
            if (binding != node.attributes.end() && binding->second == "empty")
                found = &node;
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);
    REQUIRE(found != nullptr);

    const auto& empty = found->attributes;
    CHECK(empty.count("knob_ind_r_out") == 0);
    CHECK(empty.count("knob_ind_r_in") == 0);
    CHECK(empty.count("knob_ind_w") == 0);
    CHECK(empty.count("browser_sprite_indicator_px") == 0);
}

TEST_CASE("a declared knob indicator lowers to movable pointer geometry",
          "[import-design][browser-capture][ir][knob][indicator]") {
    // A capture is one flat picture, so a knob lowered from it owns no art and
    // the design's pointer is frozen at whatever value the screenshot caught.
    // The Figma lane already solved the RENDERING half of this: a knob carrying
    // knob_ind_* gets the design's own pointer swept along the value arc. What
    // the browser lane never produced is the geometry, because a CSS dot is not
    // a child layer to be found -- so the author declares it, exactly as they
    // declare the paint box.
    //
    // The values below are the ones a renderer can move something with:
    // fractions of the dial's half-extent, which is the unit Knob::paint scales
    // by. Recording pixels instead would break the moment the control is
    // resized, and nothing downstream would notice.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"knob","binding_status":"bound","name":"declared",
         "bounds":{"left":10,"top":10,"width":100,"height":124},
         "paint_bounds":{"left":10,"top":10,"width":100,"height":100},
         "indicator_bounds":{"left":54,"top":18,"width":8,"height":8},
         "indicator_color":"rgb(184, 248, 192)",
         "data_pulp":{"param":"declared"}},
        {"kind":"knob","binding_status":"bound","name":"silent",
         "bounds":{"left":200,"top":10,"width":100,"height":124},
         "paint_bounds":{"left":200,"top":10,"width":100,"height":100},
         "data_pulp":{"param":"silent"}}
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

    std::map<std::string, const pulp::view::IRNode*> by_binding;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            const auto binding = node.attributes.find("binding");
            if (binding != node.attributes.end())
                by_binding[binding->second] = &node;
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);
    REQUIRE(by_binding.size() == 2);

    const auto& declared = by_binding.at("declared")->attributes;
    // Dial centre (60,60), a 8x8 dot centred at (58,22): 38.05px from centre,
    // reaching 4px along the radius either side of that, over a 50px half
    // extent. The dot is nearly straight up, so the radial reach is essentially
    // its own height and the width essentially its own width.
    REQUIRE(declared.count("knob_ind_r_out") == 1);
    CHECK(std::stof(declared.at("knob_ind_r_in")) ==
          Catch::Approx(0.6770f).margin(0.001f));
    CHECK(std::stof(declared.at("knob_ind_r_out")) ==
          Catch::Approx(0.8452f).margin(0.001f));
    CHECK(std::stof(declared.at("knob_ind_w")) ==
          Catch::Approx(0.1682f).margin(0.001f));
    // The pointer's colour is the design's, resolved by the browser -- not the
    // pack's text token, which is what design_indicator carries. Normalized to
    // hex because the two consumers disagree about what they can parse: the
    // materializer reads any CSS colour, the scripted bridge reads hex only and
    // silently substitutes near-white for anything else.
    CHECK(declared.at("knob_ind_color") == "#b8f8c0");
    // Hand-off to the sprite pass: the control's own pixels and the pointer's,
    // in the capture PNG's frame at its device scale (DPR 2 here).
    CHECK(declared.at("browser_sprite_crop_px") == "20,20,200,200");
    CHECK(declared.at("browser_sprite_indicator_px") == "108,36,16,16");

    // An undeclared knob is untouched. Pulp adds no pointer of its own: with no
    // declaration there is nothing in a flat picture that says which pixels
    // move, and inventing one puts a live indicator on a design that has none.
    const auto& silent = by_binding.at("silent")->attributes;
    CHECK(silent.count("knob_ind_r_out") == 0);
    CHECK(silent.count("knob_ind_r_in") == 0);
    CHECK(silent.count("knob_ind_w") == 0);
    CHECK(silent.count("knob_ind_color") == 0);
    CHECK(silent.count("browser_sprite_crop_px") == 0);
    CHECK(silent.count("browser_sprite_indicator_px") == 0);
}

TEST_CASE("a knob indicator with no radius to sweep is refused",
          "[import-design][browser-capture][ir][knob][indicator]") {
    // A pointer centred on the dial has no radial direction, so there is no arc
    // to reproduce. Stamping one anyway yields a zero-length stroke that pivots
    // on itself: it renders, it moves nothing, and every geometric assertion
    // about it is trivially satisfied.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"knob","binding_status":"bound","name":"centred",
         "bounds":{"left":0,"top":0,"width":100,"height":100},
         "paint_bounds":{"left":0,"top":0,"width":100,"height":100},
         "indicator_bounds":{"left":45,"top":45,"width":10,"height":10},
         "indicator_color":"#fff",
         "data_pulp":{"param":"centred"}}
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
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            CHECK(node.attributes.count("knob_ind_r_out") == 0);
            CHECK(node.attributes.count("browser_sprite_crop_px") == 0);
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);
}

TEST_CASE("a fader indicator is handed to the control sprite pass",
          "[import-design][browser-capture][ir][fader][indicator]") {
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"fader","binding_status":"bound","name":"drive",
         "bounds":{"left":40,"top":10,"width":40,"height":220},
         "paint_bounds":{"left":50,"top":20,"width":20,"height":200},
         "indicator_bounds":{"left":52,"top":108,"width":16,"height":12},
         "indicator_color":"rgb(244, 231, 180)",
         "data_pulp":{"param":"drive","value":"0.5"}}
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
    INFO("lowering error: " << result.error);
    REQUIRE(result);

    const pulp::view::IRNode* found = nullptr;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            const auto binding = node.attributes.find("binding");
            if (binding != node.attributes.end() && binding->second == "drive")
                found = &node;
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);
    REQUIRE(found != nullptr);
    CHECK(found->audio_widget == pulp::view::AudioWidgetType::fader);
    CHECK(found->attributes.at("browser_sprite_crop_px") == "100,40,40,400");
    CHECK(found->attributes.at("browser_sprite_indicator_px") ==
          "104,216,32,24");
    // Rotary geometry belongs only to knobs; stamping it on a linear control
    // would make an unrelated consumer appear wired while doing nothing.
    CHECK(found->attributes.count("knob_ind_r_in") == 0);
    CHECK(found->attributes.count("knob_ind_r_out") == 0);
    CHECK(found->attributes.count("knob_ind_w") == 0);
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

// The capture is deliberately LARGER than the design: the panel's root carries
// its own padding and the harness grows the extent so drop shadows and
// absolutely positioned decoration are not clipped. Both are correct, so the
// image can never be shrunk to the design -- what it has to carry instead is
// WHERE the design sits inside it. Without that a consumer guesses, and a
// centred guess is wrong because the growth is asymmetric.
TEST_CASE("a browser capture records where the authored frame sits inside it",
          "[import-design][browser-capture][registration]") {
    SECTION("the recorded frame reaches the IR root") {
        TempCapture temp;
        const auto attributes = lowered_root_attributes(
            temp, [](std::string capture) {
                return with_reference_member(
                    std::move(capture),
                    R"JSON("authored_frame":{"x":120,"y":120,)JSON"
                    R"JSON("width":760,"height":886.0625})JSON");
            });
        REQUIRE(attributes.count("browser_authored_frame_x") == 1);
        CHECK(std::stod(attributes.at("browser_authored_frame_x")) ==
              Catch::Approx(120.0));
        CHECK(std::stod(attributes.at("browser_authored_frame_y")) ==
              Catch::Approx(120.0));
        CHECK(std::stod(attributes.at("browser_authored_frame_width")) ==
              Catch::Approx(760.0));
        CHECK(std::stod(attributes.at("browser_authored_frame_height")) ==
              Catch::Approx(886.0625));
    }

    // A capture that could not resolve the frame emits null, and null says
    // "this cannot be registered". Recording it as an origin of (0,0) would
    // turn a capture that knows it cannot be aligned into one that claims it
    // is -- the exact substitution of a plausible number for a missing one
    // that the refusal exists to prevent.
    SECTION("a null frame is absent, not an origin of zero") {
        TempCapture temp;
        const auto attributes = lowered_root_attributes(
            temp, [](std::string capture) {
                return with_reference_member(std::move(capture),
                                             R"JSON("authored_frame":null)JSON");
            });
        CHECK(attributes.count("browser_authored_frame_x") == 0);
        CHECK(attributes.count("browser_authored_frame_width") == 0);
    }

    // Captures taken before the field existed still register: the primary
    // surface is the same rect measured a different way, and it is what the
    // root's own geometry was derived from.
    SECTION("a capture predating the field registers by its primary surface") {
        TempCapture temp;
        const auto attributes = lowered_root_attributes(
            temp, [](std::string capture) {
                return with_primary_surface(
                    std::move(capture),
                    R"JSON({"left":40,"top":40,"width":576,"height":179})JSON");
            });
        REQUIRE(attributes.count("browser_authored_frame_x") == 1);
        CHECK(std::stod(attributes.at("browser_authored_frame_x")) ==
              Catch::Approx(40.0));
        CHECK(std::stod(attributes.at("browser_authored_frame_width")) ==
              Catch::Approx(576.0));
    }
}

// Registration is what makes a similarity number mean anything. Unregistered,
// the comparison reads the same pixel box out of two pictures that hold
// different content there and returns a plausible figure for it: kelvin, which
// differs from its oracle by 5.6% of its pixels, was reported at 31% similar.
TEST_CASE("the importer's comparison registers the reference before scoring",
          "[import-design][browser-capture][registration]") {
    // kelvin's own numbers: a 1760x2014 capture holding a 1520x1772 panel at
    // device (240,240), recorded as a 760x886.0625 CSS frame at (120,120).
    pulp::view::DesignIR ir;
    ir.root.attributes["browser_device_scale_factor"] = "2.000000";
    ir.root.attributes["browser_authored_frame_x"] = "120.000000";
    ir.root.attributes["browser_authored_frame_y"] = "120.000000";
    ir.root.attributes["browser_authored_frame_width"] = "760.000000";
    ir.root.attributes["browser_authored_frame_height"] = "886.062500";

    SECTION("the recorded frame is scaled by the capture's own device scale") {
        const auto registration =
            pulp::import_design::resolve_reference_registration(
                ir, {}, 1760, 2014, 1520, 1772);
        REQUIRE(registration.registered);
        CHECK(registration.x == 240);
        CHECK(registration.y == 240);
        CHECK(registration.width == 1520);
        CHECK(registration.height == 1772);
    }

    // The probe that made the reporting defect legible: a 200x120 page on a
    // 1280x800 viewport. Chrome returns the whole viewport at 2560x1600 while
    // the render is 400x240, so the comparison scored the top-left 400x240 --
    // 114 of 96000 pixels differing, 99.88% identical -- and then multiplied
    // that by an area ratio of 0.0234, printing
    //
    //     Similarity: 2% (114/96000 pixels differ, mean error: 0.127656)
    //
    // The percentage contradicted its own count because the count is measured
    // over the overlap and the percentage is scaled afterwards by how much of
    // the images that overlap covers. Registration is what closes it: a scored
    // comparison has IDENTICAL extents, so the ratio is 1 and the printed
    // percentage is exactly 1 - differing/total. That invariant is what is
    // asserted here -- the wart is in the raw comparator and is not this lane's
    // to keep alive.
    SECTION("a full-viewport capture registers to the render's own extent") {
        pulp::view::DesignIR probe;
        probe.root.attributes["browser_device_scale_factor"] = "2.000000";
        probe.root.attributes["browser_authored_frame_x"] = "0.000000";
        probe.root.attributes["browser_authored_frame_y"] = "0.000000";
        probe.root.attributes["browser_authored_frame_width"] = "200.000000";
        probe.root.attributes["browser_authored_frame_height"] = "120.000000";
        const auto registration =
            pulp::import_design::resolve_reference_registration(
                probe, {}, 2560, 1600, 400, 240);
        REQUIRE(registration.registered);
        CHECK(registration.x == 0);
        CHECK(registration.y == 0);
        CHECK(registration.width == 400);
        CHECK(registration.height == 240);
    }

    // A fractional CSS height rounds independently on the two sides, so the
    // frame and the render can land a pixel apart on a panel that is perfectly
    // registered. Refusing that would reject a good capture over arithmetic.
    SECTION("a pixel of rounding snaps to the render rather than refusing") {
        ir.root.attributes["browser_authored_frame_height"] = "886.500000";
        const auto registration =
            pulp::import_design::resolve_reference_registration(
                ir, {}, 1760, 2014, 1520, 1774);
        REQUIRE(registration.registered);
        CHECK(registration.height == 1774);
    }

    // Refuse rather than score. verify_rendered_panel.py already refuses a size
    // mismatch for the same reason -- "never scored: a size mismatch is
    // compared over the wrong pixels" -- and this lane is the one that did not.
    SECTION("an unregisterable capture is refused, not scored") {
        for (const char* key : {"browser_authored_frame_x",
                                "browser_authored_frame_y",
                                "browser_authored_frame_width",
                                "browser_authored_frame_height"}) {
            ir.root.attributes.erase(key);
        }
        const auto registration =
            pulp::import_design::resolve_reference_registration(
                ir, {}, 1760, 2014, 1520, 1772);
        CHECK_FALSE(registration.registered);
        CHECK(registration.reason.find("never scored") != std::string::npos);
    }

    // The refusal is scoped to a genuine mismatch. A capture whose reference is
    // already the render -- every uncropped one -- keeps scoring, so this does
    // not turn into a blanket new failure for every panel taken before the
    // field existed.
    SECTION("a reference that is already the render still scores") {
        for (const char* key : {"browser_authored_frame_x",
                                "browser_authored_frame_y",
                                "browser_authored_frame_width",
                                "browser_authored_frame_height"}) {
            ir.root.attributes.erase(key);
        }
        const auto registration =
            pulp::import_design::resolve_reference_registration(
                ir, {}, 1520, 1772, 1520, 1772);
        REQUIRE(registration.registered);
        CHECK(registration.x == 0);
        CHECK(registration.y == 0);
        CHECK(registration.width == 1520);
        CHECK(registration.height == 1772);
    }
}

// WHY registration is required, stated as arithmetic anyone can check.
//
// compare_screenshots scores the OVERLAP of two images and then scales the
// result by how much of them that overlap covers, so two images whose shared
// region is BYTE-IDENTICAL score 12.5% when one is 1280x600 and the other is
// the 400x240 corner of it: 96000 / 768000. The differing-pixel count printed
// beside such a number says 0.
//
// That is the whole C2 defect in one line, and it is why cropping to the
// authored frame is not a cosmetic improvement to a roughly-right number: it is
// what makes the number mean anything at all. Left unpinned, someone reads the
// scaling as the bug and "fixes" it in the comparator, and every consumer that
// legitimately compares unequal images silently changes meaning.
TEST_CASE("an unregistered comparison scores identical pixels as a near-miss",
          "[import-design][browser-capture][registration]") {
    const fs::path source_path =
        fs::path(PULP_BROWSER_CAPTURE_STYLE_FIXTURE_DIR) / "browser.png";
    std::ifstream input(source_path, std::ios::binary);
    REQUIRE(input);
    // assign() rather than an iterator-pair constructor: the latter is a most
    // vexing parse here and declares a function.
    std::vector<std::uint8_t> source;
    source.assign(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
    REQUIRE_FALSE(source.empty());

    // Both sides are produced by the same decode/encode round trip, so their
    // shared region is identical byte for byte.
    const auto whole = pulp::view::crop_png(source, 0, 0, 1280, 600);
    const auto corner = pulp::view::crop_png(source, 0, 0, 400, 240);
    if (whole.empty() || corner.empty()) {
        // This build carries no PNG pixel decoder. Reporting that as a pass is
        // wrong, but so is reporting it as a defect in the code under test.
        SUCCEED("PNG decoding is unavailable in this build");
        return;
    }

    const auto unregistered = pulp::view::compare_screenshots(whole, corner);
    REQUIRE(unregistered.valid);
    CHECK(unregistered.diff_pixels == 0);
    CHECK(unregistered.total_pixels == 400u * 240u);
    // 96000 / (1280*600) = 0.125. Zero pixels differ and it scores 12%.
    CHECK(unregistered.similarity == Catch::Approx(0.125).margin(0.001));
    CHECK_FALSE(unregistered.passes());

    // Registered -- the same region on both sides -- the score is what the
    // count says it is.
    const auto registered = pulp::view::compare_screenshots(corner, corner);
    REQUIRE(registered.valid);
    CHECK(registered.diff_pixels == 0);
    CHECK(registered.similarity == Catch::Approx(1.0));
    CHECK(registered.passes());
}

TEST_CASE("a bound switch lowers to a control rather than staying backdrop",
          "[import][browser-capture][semantics][toggle]") {
    // The semantics pass has always named a `pulp-switch` correctly; what it
    // named was then dropped, because only knob/fader/meter produced a node.
    // The panel still DREW the switch, so the failure was invisible: a shape
    // that reads as something to flip, wired to nothing.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"toggle","binding_status":"bound","name":"sync",
         "bounds":{"left":24,"top":40,"width":56,"height":28},
         "data_pulp":{"param":"sync","value":"1"}}
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

    std::vector<const pulp::view::IRNode*> toggles;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            if (node.audio_widget == pulp::view::AudioWidgetType::toggle)
                toggles.push_back(&node);
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);

    REQUIRE(toggles.size() == 1);
    const auto& sync = *toggles.front();
    // Both spellings, for the same reason a knob carries both: the script
    // emitter reads `binding` and the native binding metadata reads
    // `pulpParamKey`, and a control carrying one is half-wired.
    REQUIRE(sync.attributes.at("binding") == "sync");
    REQUIRE(sync.attributes.at("pulpParamKey") == "sync");
    REQUIRE(sync.attributes.count("pulpRouteId") == 1);
    REQUIRE(sync.stable_anchor_id);
    REQUIRE_FALSE(sync.stable_anchor_id->empty());
    // The opening state is read as a boolean by the native materializer, so a
    // switch the design drew lit has to arrive lit rather than snapping off
    // the moment the panel loads.
    REQUIRE(sync.attributes.at("checked") == "1");
}

TEST_CASE("a bound selector lowers with the segments its author declared",
          "[import][browser-capture][semantics][selector]") {
    // A choice between named alternatives is the control a synth panel needs
    // most and could not have: the semantics pass named it `select`, and the
    // lowering dropped it, so a designer's mode row was drawn and wired to
    // nothing.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"select","binding_status":"bound","name":"direction",
         "bounds":{"left":24,"top":40,"width":200,"height":28},
         "data_pulp":{"param":"shape","choices":"Up|Down|Converge|Random"}},
        {"kind":"select","binding_status":"bound","name":"unlabelled",
         "bounds":{"left":24,"top":90,"width":200,"height":28},
         "data_pulp":{"param":"nothing"}}
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

    std::vector<const pulp::view::IRNode*> selectors;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            if (node.audio_widget == pulp::view::AudioWidgetType::selector)
                selectors.push_back(&node);
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);

    // The second candidate declared no choices, so it has nothing to light and
    // stays part of the backdrop rather than arriving as an empty track.
    REQUIRE(selectors.size() == 1);
    const auto& direction = *selectors.front();
    REQUIRE(direction.attributes.at("pulpChoices") == "Up|Down|Converge|Random");
    REQUIRE(direction.attributes.at("binding") == "shape");
    REQUIRE(direction.attributes.at("pulpParamKey") == "shape");
    REQUIRE(direction.stable_anchor_id);
}

TEST_CASE("a bound stepper lowers with a declared or normalized fallback grid",
          "[import][browser-capture][semantics][stepper]") {
    // A count is the control the original request asked for ("voice number
    // parameter") and the one a knob reads worst. Its range is DECLARED
    // because nothing about the element implies it: a voices control counting
    // 1..8 and an octave control spanning -2..+2 are the same box.
    TempCapture temp;
    const auto png = png_header(1912, 1272);
    temp.write("browser.png", png);
    temp.write("semantic-report.json", R"JSON({
      "schema":"pulp-browser-semantics-v1",
      "version":1,
      "summary":{"candidates":7,"resolved":2,"unresolved":5},
      "candidates":[
        {"kind":"stepper","binding_status":"bound","name":"voices",
         "bounds":{"left":24,"top":40,"width":80,"height":28},
         "data_pulp":{"param":"voices","min":"1","max":"8","step":"1"}},
        {"kind":"stepper","binding_status":"bound","name":"normalized",
         "bounds":{"left":24,"top":90,"width":80,"height":28},
         "data_pulp":{"param":"normalized"}},
        {"kind":"stepper","binding_status":"bound","name":"partial",
         "bounds":{"left":24,"top":140,"width":80,"height":28},
         "data_pulp":{"param":"partial","min":"-2"}},
        {"kind":"stepper","binding_status":"bound","name":"bipolar",
         "bounds":{"left":24,"top":190,"width":80,"height":28},
         "data_pulp":{"param":"bipolar","min":"-2","max":"2","step":"1"}},
        {"kind":"stepper","binding_status":"bound","name":"frequency",
         "bounds":{"left":24,"top":240,"width":80,"height":28},
         "data_pulp":{"param":"frequency","min":"20","max":"20000","step":"10"}},
        {"kind":"stepper","binding_status":"bound","name":"precision",
         "bounds":{"left":24,"top":265,"width":80,"height":28},
         "data_pulp":{"param":"precision","min":"0","max":"0.000001","step":"0.0000001"}},
        {"kind":"stepper","binding_status":"bound","name":"junk",
         "bounds":{"left":24,"top":290,"width":80,"height":28},
         "data_pulp":{"param":"junk","min":"1junk","max":"8","step":"1"}},
        {"kind":"stepper","binding_status":"bound","name":"reversed",
         "bounds":{"left":24,"top":340,"width":80,"height":28},
         "data_pulp":{"param":"reversed","min":"8","max":"1","step":"1"}},
        {"kind":"stepper","binding_status":"bound","name":"nonfinite",
         "bounds":{"left":24,"top":390,"width":80,"height":28},
         "data_pulp":{"param":"nonfinite","min":"0","max":"nan","step":"1"}},
        {"kind":"stepper","binding_status":"bound","name":"zero-step",
         "bounds":{"left":24,"top":440,"width":80,"height":28},
         "data_pulp":{"param":"zero-step","min":"0","max":"1","step":"0"}}
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

    std::vector<const pulp::view::IRNode*> steppers;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            if (node.audio_widget == pulp::view::AudioWidgetType::stepper)
                steppers.push_back(&node);
            for (const auto& child : node.children) walk(child);
        };
    walk(result.design_ir->root);

    // No domain means a normalized host parameter. A partial domain is not
    // merged with an invented endpoint and therefore does not lower.
    REQUIRE(steppers.size() == 5);
    const auto find_stepper = [&](std::string_view param) -> const pulp::view::IRNode& {
        const auto it = std::find_if(steppers.begin(), steppers.end(),
            [&](const auto* node) {
                const auto found = node->attributes.find("pulpParamKey");
                return found != node->attributes.end() && found->second == param;
            });
        REQUIRE(it != steppers.end());
        return **it;
    };
    const auto& voices = find_stepper("voices");
    REQUIRE(voices.audio_min == 1.0f);
    REQUIRE(voices.audio_max == 8.0f);
    REQUIRE(std::stof(voices.attributes.at("pulpStep")) == 1.0f);
    REQUIRE(voices.attributes.at("pulpParamKey") == "voices");

    const auto& normalized = find_stepper("normalized");
    REQUIRE(normalized.audio_min == 0.0f);
    REQUIRE(normalized.audio_max == 1.0f);
    REQUIRE(std::stof(normalized.attributes.at("pulpStep")) == 0.01f);
    REQUIRE(normalized.attributes.at("pulpParamKey") == "normalized");

    const auto& bipolar = find_stepper("bipolar");
    REQUIRE(bipolar.audio_min == -2.0f);
    REQUIRE(bipolar.audio_max == 2.0f);
    REQUIRE(std::stof(bipolar.attributes.at("pulpStep")) == 1.0f);

    const auto& frequency = find_stepper("frequency");
    REQUIRE(frequency.audio_min == 20.0f);
    REQUIRE(frequency.audio_max == 20000.0f);
    REQUIRE(std::stof(frequency.attributes.at("pulpStep")) == 10.0f);

    const auto& precision = find_stepper("precision");
    REQUIRE(precision.audio_min == 0.0f);
    REQUIRE(precision.audio_max == Catch::Approx(0.000001f));
    REQUIRE(precision.attributes.at("pulpStep") == "0.0000001");
    REQUIRE(std::stof(precision.attributes.at("pulpStep")) ==
            Catch::Approx(0.0000001f));
}
