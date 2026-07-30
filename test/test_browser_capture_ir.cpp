#include <catch2/catch_test_macros.hpp>

#include "tools/import-design/browser_capture_ir.hpp"

#include <pulp/runtime/crypto.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

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
