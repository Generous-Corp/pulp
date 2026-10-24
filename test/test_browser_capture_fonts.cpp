#include <catch2/catch_test_macros.hpp>

#include "tools/import-design/browser_capture_fonts.hpp"

#include <pulp/view/design_ir_io.hpp>
#include <pulp/view/design_sources.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace pulp::view;
using pulp::import_design::attach_captured_font_faces;
namespace fs = std::filesystem;

namespace {

constexpr const char* kFirst =
    "pulp-materialized-asset-93a17c7b5a173be2da95f76cb62a26ae30c0e13ce230acd243be63023258bf82";
constexpr const char* kSecond =
    "pulp-materialized-asset-42bf82c0cf61a1760a16ee92e88db58757128437f524c86a47c9659614c64e71";

// A captured page that serves one variable face at two weights and as a
// weight range, plus a second subset split off by unicode-range.
const std::string kSidecar = std::string(R"JSON({
  "schema":"pulp-materialized-browser-document-v1","version":1,
  "html":"<html><body><div id=\"root\"></div></body></html>",
  "assets":[
    {"id":")JSON") + kFirst + R"JSON(","mime_type":"font/woff2","byte_length":20,
     "data_base64":"Z2xvYmFsVGhpcy5va1RydWU9MTs=",
     "sha256":"93a17c7b5a173be2da95f76cb62a26ae30c0e13ce230acd243be63023258bf82"},
    {"id":")JSON" + kSecond + R"JSON(","mime_type":"font/ttf","byte_length":26,
     "data_base64":"c2Vjb25kIGNhcHR1cmVkIGZhY2UgYnl0ZXM=",
     "sha256":"42bf82c0cf61a1760a16ee92e88db58757128437f524c86a47c9659614c64e71"}],
  "font_bindings":[
    {"family":"Captured Mono","asset_id":")JSON" +
                             kFirst + R"JSON(",
     "weight":"400","style":"normal","unicode_range":"U+0000-00FF"},
    {"family":"Captured Mono","asset_id":")JSON" +
                             kFirst + R"JSON(",
     "weight":"bold","style":"normal","unicode_range":"U+0000-00FF"},
    {"family":"Captured Mono","asset_id":")JSON" +
                             kSecond + R"JSON(",
     "weight":" 100  800 ","style":"italic","unicode_range":" U+0400-045F "},
    {"family":"Captured Sans","asset_id":")JSON" +
                             kSecond + R"JSON(",
     "weight":"normal","style":"normal"}]
})JSON";

struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() /
               ("pulp-captured-fonts-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(counter++));
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("captured @font-face rules become IR font assets in page order",
          "[import-design][browser-capture][font-assets]") {
    const auto bundle = parse_materialized_browser_document(kSidecar);
    REQUIRE(bundle.has_value());
    ScratchDir scratch;
    DesignIR ir;
    std::string error;
    REQUIRE(attach_captured_font_faces(ir, *bundle, scratch.path / "fonts", error));
    CHECK(error.empty());

    REQUIRE(ir.font_family_assets.size() == 4);
    const auto& regular = ir.font_family_assets[0];
    CHECK(regular.family == "Captured Mono");
    CHECK(regular.weight == 400);
    CHECK(regular.weight_max == 0);
    CHECK(regular.font_style == "normal");
    CHECK(regular.unicode_range == "U+0000-00FF");
    CHECK(regular.asset_id == kFirst);
    CHECK(ir.font_family_assets[1].weight == 700);
    CHECK(ir.font_family_assets[1].asset_id == kFirst);
    const auto& range = ir.font_family_assets[2];
    CHECK(range.weight == 100);
    CHECK(range.weight_max == 800);
    CHECK(range.font_style == "italic");
    CHECK(range.unicode_range == "U+0400-045F");
    CHECK(ir.font_family_assets[3].family == "Captured Sans");
    CHECK(ir.font_family_assets[3].weight == 400);
    CHECK(ir.font_family_assets[3].unicode_range.empty());

    // One manifest entry and one file per asset, however many rules share it,
    // named by content hash and holding the captured bytes.
    REQUIRE(ir.asset_manifest.assets.size() == 2);
    const auto* first = ir.asset_manifest.resolve(kFirst);
    REQUIRE(first != nullptr);
    CHECK(first->mime == "font/woff2");
    CHECK(first->font_family == std::optional<std::string>("Captured Mono"));
    CHECK(first->content_hash ==
          "93a17c7b5a173be2da95f76cb62a26ae30c0e13ce230acd243be63023258bf82");
    REQUIRE(first->local_path.has_value());
    CHECK(fs::path(*first->local_path).filename() ==
          "93a17c7b5a173be2da95f76cb62a26ae30c0e13ce230acd243be63023258bf82.woff2");
    CHECK(read_file(*first->local_path) == "globalThis.okTrue=1;");
    CHECK(regular.resolved_path == *first->local_path);
    const auto* second = ir.asset_manifest.resolve(kSecond);
    REQUIRE(second != nullptr);
    CHECK(fs::path(*second->local_path).extension() == ".ttf");
    CHECK(read_file(*second->local_path) == "second captured face bytes");
    std::size_t files = 0;
    for (const auto& entry : fs::directory_iterator(scratch.path / "fonts")) {
        (void)entry;
        ++files;
    }
    CHECK(files == 2);

    // The faces survive the IR's own round trip.
    const auto reparsed = parse_design_ir_json(serialize_design_ir(ir));
    REQUIRE(reparsed.font_family_assets.size() == 4);
    CHECK(reparsed.font_family_assets[2].weight_max == 800);
    CHECK(reparsed.font_family_assets[2].unicode_range == "U+0400-045F");
    REQUIRE(reparsed.asset_manifest.resolve(kSecond) != nullptr);
}

TEST_CASE("a materialized document without font bindings leaves the IR unchanged",
          "[import-design][browser-capture][font-assets]") {
    const auto bundle = parse_materialized_browser_document(
        R"JSON({"schema":"pulp-materialized-browser-document-v1","version":1,
        "html":"<html><body></body></html>","assets":[]})JSON");
    REQUIRE(bundle.has_value());
    ScratchDir scratch;
    DesignIR ir;
    ir.root.type = "frame";
    const auto before = serialize_design_ir(ir);
    std::string error;
    REQUIRE(attach_captured_font_faces(ir, *bundle, scratch.path / "fonts", error));
    CHECK(serialize_design_ir(ir) == before);
    CHECK_FALSE(fs::exists(scratch.path / "fonts"));
}

TEST_CASE("captured fonts that cannot be written fail the import",
          "[import-design][browser-capture][font-assets]") {
    const auto bundle = parse_materialized_browser_document(kSidecar);
    REQUIRE(bundle.has_value());
    ScratchDir scratch;
    // A regular file where the font directory has to go.
    const auto blocked = scratch.path / "fonts";
    std::ofstream(blocked) << "not a directory";
    DesignIR ir;
    std::string error;
    CHECK_FALSE(attach_captured_font_faces(ir, *bundle, blocked, error));
    CHECK_FALSE(error.empty());
}
