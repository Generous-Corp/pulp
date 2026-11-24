#include <catch2/catch_test_macros.hpp>

#include "support/unique_temp_dir.hpp"
#include "tools/import-design/browser_capture_fonts.hpp"
#include "tools/import-design/sealed_font_files.hpp"
#include "tools/import-design/sprite_skins.hpp"

#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_codegen.hpp>
#include <pulp/view/reload_autocaps.hpp>
#include <pulp/view/reload_capabilities.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/view/design_ir_io.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>
#include <string>
#include <vector>

using namespace pulp::view;
using pulp::import_design::attach_page_font_faces;
namespace fs = std::filesystem;

namespace {

constexpr const char* kFirst =
    "pulp-materialized-asset-43cab887e2aee6658706301d92c0ba103fb7294f2811c66666545f0117bf4e79";
constexpr const char* kSecond =
    "pulp-materialized-asset-62cbf9ecaa14ed3cf258993be6fdcd299fe8c60a78238c9ea1c4a51c297a0a6a";

// A captured bundle that serves one variable face at two weights and as a
// weight range, plus a second subset split off by unicode-range, from Blob
// URLs, as font-faces.json records it.
const std::string kSidecar = std::string(R"JSON({
  "schema":"pulp-browser-capture-font-faces-v1",
  "faces":[
    {"family":"Captured Mono","weight":"400","style":"normal","unicode_range":"U+0000-00FF",
     "format":"woff2","source":"blob:","asset_id":")JSON") +
                             kFirst + R"JSON(",
     "sha256":"43cab887e2aee6658706301d92c0ba103fb7294f2811c66666545f0117bf4e79"},
    {"family":"Captured Mono","weight":"bold","style":"normal","unicode_range":"U+0000-00FF",
     "format":"woff2","source":"blob:","asset_id":")JSON" +
                             kFirst + R"JSON(",
     "sha256":"43cab887e2aee6658706301d92c0ba103fb7294f2811c66666545f0117bf4e79"},
    {"family":"Captured Mono","weight":" 100  800 ","style":"italic",
     "unicode_range":" U+0400-045F ","format":"","source":"blob:","asset_id":")JSON" +
                             kSecond + R"JSON(",
     "sha256":"62cbf9ecaa14ed3cf258993be6fdcd299fe8c60a78238c9ea1c4a51c297a0a6a"},
    {"family":"Captured Sans","weight":"normal","style":"normal","unicode_range":"",
     "format":"","source":"blob:","asset_id":")JSON" +
                             kSecond + R"JSON(",
     "sha256":"62cbf9ecaa14ed3cf258993be6fdcd299fe8c60a78238c9ea1c4a51c297a0a6a"}],
  "assets":[
    {"sha256":"43cab887e2aee6658706301d92c0ba103fb7294f2811c66666545f0117bf4e79",
     "mime_type":"font/woff2","byte_length":27,"data_base64":"d09GMiBjYXB0dXJlZCB2YXJpYWJsZSBmYWNl"},
    {"sha256":"62cbf9ecaa14ed3cf258993be6fdcd299fe8c60a78238c9ea1c4a51c297a0a6a",
     "mime_type":"font/ttf","byte_length":25,
     "data_base64":"dHJ1ZSBzZWNvbmQgY2FwdHVyZWQgZmFjZQ=="}]
})JSON";

struct ScratchDir {
    fs::path path = pulp::test::make_unique_temp_dir("pulp-captured-fonts");
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
    ScratchDir scratch;
    const auto record = scratch.path / "font-faces.json";
    std::ofstream(record, std::ios::binary) << kSidecar;
    DesignIR ir;
    std::vector<std::string> warnings;
    std::string error;
    REQUIRE(attach_page_font_faces(ir, record, scratch.path / "fonts", warnings, error));
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
          "43cab887e2aee6658706301d92c0ba103fb7294f2811c66666545f0117bf4e79");
    REQUIRE(first->local_path.has_value());
    CHECK(fs::path(*first->local_path).filename() ==
          "43cab887e2aee6658706301d92c0ba103fb7294f2811c66666545f0117bf4e79.woff2");
    CHECK(read_file(*first->local_path) == "wOF2 captured variable face");
    CHECK(regular.resolved_path == *first->local_path);
    const auto* second = ir.asset_manifest.resolve(kSecond);
    REQUIRE(second != nullptr);
    CHECK(fs::path(*second->local_path).extension() == ".ttf");
    CHECK(read_file(*second->local_path) == "true second captured face");
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

TEST_CASE("a capture that loaded no font faces leaves the IR unchanged",
          "[import-design][browser-capture][font-assets]") {
    ScratchDir scratch;
    const auto record = scratch.path / "font-faces.json";
    std::ofstream(record, std::ios::binary)
        << R"JSON({"schema":"pulp-browser-capture-font-faces-v1","faces":[],"assets":[]})JSON";
    DesignIR ir;
    ir.root.type = "frame";
    const auto before = serialize_design_ir(ir);
    std::vector<std::string> warnings;
    std::string error;
    REQUIRE(attach_page_font_faces(ir, record, scratch.path / "fonts", warnings, error));
    CHECK(serialize_design_ir(ir) == before);
    CHECK_FALSE(fs::exists(scratch.path / "fonts"));
}

TEST_CASE("captured fonts that cannot be written fail the import",
          "[import-design][browser-capture][font-assets]") {
    ScratchDir scratch;
    const auto record = scratch.path / "font-faces.json";
    std::ofstream(record, std::ios::binary) << kSidecar;
    // A regular file where the font directory has to go.
    const auto blocked = scratch.path / "fonts";
    std::ofstream(blocked) << "not a directory";
    DesignIR ir;
    std::vector<std::string> warnings;
    std::string error;
    CHECK_FALSE(attach_page_font_faces(ir, record, blocked, warnings, error));
    CHECK_FALSE(error.empty());
}

namespace {

// A digest no carried bytes have.
constexpr const char* kWrongSha =
    "2cbb1c0ae72e1b35e1a1ec3c1c6f99d3a3f3a0b8e3e9e7d0b5ec2e3e5ed0d0f1";

// The bytes of a plain page's two url() faces, each opening with its
// container's signature, as the capture carries them.
const std::string kRegularBytes = std::string("true") + "regular bytes";
const std::string kBoldBytes = std::string("wOF2") + "bold bytes";

// The font-faces.json a capture writes for that page, with the bytes'
// recorded digests and, optionally, faces it dropped.
std::string page_font_faces(const std::string& regular_sha, const std::string& bold_sha,
                            const std::string& regular_bytes = kRegularBytes,
                            const std::string& dropped = "[]") {
    const auto b64 = [](const std::string& bytes) {
        return pulp::runtime::base64_encode(reinterpret_cast<const uint8_t*>(bytes.data()),
                                            bytes.size());
    };
    return R"JSON({"schema":"pulp-browser-capture-font-faces-v1",
  "faces":[
    {"family":"Pulp Page Face","weight":"400","style":"normal","unicode_range":"",
     "format":"truetype","source":"pulp-capture:///fonts/Jost-Regular.ttf","sha256":")JSON" +
           regular_sha + R"JSON("},
    {"family":"Pulp Page Face","weight":"300 800","style":"italic","unicode_range":"U+0-FF",
     "format":"woff2","source":"pulp-capture:///fonts/Jost-Bold.woff2","sha256":")JSON" +
           bold_sha + R"JSON("}],
  "dropped":)JSON" +
           dropped + R"JSON(,
  "assets":[
    {"sha256":")JSON" +
           regular_sha + R"JSON(","mime_type":"font/ttf","data_base64":")JSON" +
           b64(regular_bytes) + R"JSON("},
    {"sha256":")JSON" +
           bold_sha + R"JSON(","mime_type":"font/woff2","data_base64":")JSON" + b64(kBoldBytes) +
           R"JSON("}]
})JSON";
}

std::string sha(const std::string& bytes) {
    return pulp::runtime::sha256_hex(std::string_view(bytes));
}

} // namespace

TEST_CASE("a plain page's url() @font-face files become IR font assets",
          "[import-design][browser-capture][font-assets]") {
    const auto regular = sha(kRegularBytes);
    const auto bold = sha(kBoldBytes);
    ScratchDir scratch;
    const auto record = scratch.path / "font-faces.json";
    std::ofstream(record, std::ios::binary)
        << page_font_faces(regular, bold, kRegularBytes,
                           R"([{"family":"Old Face","weight":"400","style":"normal",
            "source":"pulp-capture:///fonts/old.woff","reason":"it is WOFF 1.0"}])");

    DesignIR ir;
    std::vector<std::string> warnings;
    std::string error;
    const bool attached =
        attach_page_font_faces(ir, record, scratch.path / "fonts", warnings, error);
    INFO(error);
    REQUIRE(attached);
    REQUIRE(ir.font_family_assets.size() == 2);
    const auto& first = ir.font_family_assets[0];
    CHECK(first.family == "Pulp Page Face");
    CHECK(first.weight == 400);
    CHECK(first.weight_max == 0);
    CHECK(first.font_style == "normal");
    CHECK(first.asset_id == "pulp-page-font-" + regular);
    CHECK(read_file(first.resolved_path) == kRegularBytes);
    CHECK(fs::path(first.resolved_path).extension() == ".ttf");
    const auto& second = ir.font_family_assets[1];
    CHECK(second.weight == 300);
    CHECK(second.weight_max == 800);
    CHECK(second.font_style == "italic");
    CHECK(second.unicode_range == "U+0-FF");
    CHECK(fs::path(second.resolved_path).extension() == ".woff2");
    REQUIRE(ir.asset_manifest.assets.size() == 2);
    CHECK(ir.asset_manifest.assets[1].mime == "font/woff2");
    CHECK(ir.asset_manifest.assets[1].content_hash == bold);
    // A face the capture dropped is named in the import's warnings.
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("\"Old Face\" (pulp-capture:///fonts/old.woff)") != std::string::npos);
    CHECK(warnings[0].find("WOFF 1.0") != std::string::npos);
}

TEST_CASE("page font bytes that are not a native font fail the import",
          "[import-design][browser-capture][font-assets]") {
    using pulp::import_design::attach_page_font_faces;
    for (const std::string& bytes : {std::string("<!doctype html>"), std::string("wOFFold woff")}) {
        ScratchDir scratch;
        const auto record = scratch.path / "font-faces.json";
        std::ofstream(record, std::ios::binary)
            << page_font_faces(sha(bytes), sha(kBoldBytes), bytes);
        DesignIR ir;
        std::vector<std::string> warnings;
        std::string error;
        CHECK_FALSE(attach_page_font_faces(ir, record, scratch.path / "fonts", warnings, error));
        CHECK(error.find("is not a TrueType, OpenType or WOFF2 file") != std::string::npos);
        CHECK(ir.font_family_assets.empty());
    }
}

TEST_CASE("a page font record whose bytes do not match fails the import",
          "[import-design][browser-capture][font-assets]") {
    const auto bold = sha(kBoldBytes);
    ScratchDir scratch;
    const auto record = scratch.path / "font-faces.json";
    std::ofstream(record, std::ios::binary) << page_font_faces(kWrongSha, bold);

    DesignIR ir;
    std::vector<std::string> warnings;
    std::string error;
    CHECK_FALSE(attach_page_font_faces(ir, record, scratch.path / "fonts", warnings, error));
    CHECK(error.find("do not match their SHA-256") != std::string::npos);
    CHECK(ir.font_family_assets.empty());

    // No record means the page loaded no url() face.
    DesignIR untouched;
    CHECK(attach_page_font_faces(untouched, scratch.path / "absent.json", scratch.path / "fonts",
                                 warnings, error));
    CHECK(untouched.font_family_assets.empty());
}

TEST_CASE("a face whose asset id does not name its font fails the import",
          "[import-design][browser-capture][font-assets]") {
    ScratchDir scratch;
    const auto record = scratch.path / "font-faces.json";
    std::string text = kSidecar;
    const std::string first = kFirst;
    text.replace(text.find(first), first.size(), "pulp-materialized-asset-other");
    std::ofstream(record, std::ios::binary) << text;
    DesignIR ir;
    std::vector<std::string> warnings;
    std::string error;
    CHECK_FALSE(attach_page_font_faces(ir, record, scratch.path / "fonts", warnings, error));
    CHECK(error.find("does not name its font") != std::string::npos);
}

TEST_CASE("a bundle's Blob font bytes that are not a native font fail the import",
          "[import-design][browser-capture][font-assets]") {
    // A Blob font is page input like any other: its materialized id earns it no trust.
    const std::string bytes = "<!doctype html>";
    const auto digest = sha(bytes);
    ScratchDir scratch;
    const auto record = scratch.path / "font-faces.json";
    std::ofstream(record, std::ios::binary)
        << R"JSON({"schema":"pulp-browser-capture-font-faces-v1","faces":[
             {"family":"Bundled","weight":"400","style":"normal","unicode_range":"",
              "source":"blob:","asset_id":"pulp-materialized-asset-)JSON" +
               digest + R"JSON(","sha256":")JSON" + digest + R"JSON("}],
           "assets":[{"sha256":")JSON" +
               digest + R"JSON(","mime_type":"font/woff2","data_base64":")JSON" +
               pulp::runtime::base64_encode(reinterpret_cast<const uint8_t*>(bytes.data()),
                                            bytes.size()) +
               R"JSON("}]})JSON";
    DesignIR ir;
    std::vector<std::string> warnings;
    std::string error;
    CHECK_FALSE(attach_page_font_faces(ir, record, scratch.path / "fonts", warnings, error));
    CHECK(error.find("is not a TrueType, OpenType or WOFF2 file") != std::string::npos);
    CHECK(ir.font_family_assets.empty());
}

namespace {

// The files a pulp_add_plugin(UI_SCRIPT) build embeds beside `script`, as the
// `relative|absolute` pairs PulpUiAssets.cmake computes. The real CMake rule is
// evaluated in a throwaway configure so the embedded names under test are the
// ones a shipped plug-in carries, not a reimplementation of them.
std::vector<std::pair<std::string, fs::path>> ui_script_embed_pairs(const fs::path& script,
                                                                    const fs::path& scratch) {
    const auto project = scratch / "embed-rule";
    fs::create_directories(project);
    const auto pairs_file = scratch / "embed-pairs.txt";
    std::ofstream(project / "CMakeLists.txt", std::ios::binary)
        << "cmake_minimum_required(VERSION 3.24)\nproject(sealed_font_embed_rule NONE)\n"
        << "include(\"" << PULP_SOURCE_DIR << "/tools/cmake/PulpUiAssets.cmake\")\n"
        << "_pulp_ui_embed_sibling_files(_pairs \"" << script.generic_string() << "\" \"\")\n"
        << "file(WRITE \"" << pairs_file.generic_string() << "\" \"${_pairs}\")\n";
    const std::string command = std::string("\"") + PULP_CMAKE_COMMAND + "\" -S \"" +
                                project.string() + "\" -B \"" + (project / "build").string() +
                                "\" > \"" + (scratch / "embed-rule.log").string() + "\" 2>&1";
#ifdef _WIN32
    // cmd.exe drops the first and last quote of a line that starts with one.
    REQUIRE(std::system(("\"" + command + "\"").c_str()) == 0);
#else
    REQUIRE(std::system(command.c_str()) == 0);
#endif
    std::vector<std::pair<std::string, fs::path>> pairs;
    std::stringstream all(read_file(pairs_file));
    for (std::string entry; std::getline(all, entry, ';');) {
        const auto bar = entry.find('|');
        REQUIRE(bar != std::string::npos);
        pairs.emplace_back(entry.substr(0, bar), fs::path(entry.substr(bar + 1)));
    }
    return pairs;
}

} // namespace

TEST_CASE("a captured page font registers in a shipped panel under an empty grant",
          "[import-design][browser-capture][font-assets][security]") {
    const auto jost = read_file(fs::path(PULP_SOURCE_DIR) / "external/fonts/Jost-Regular.ttf");
    REQUIRE_FALSE(jost.empty());
    const auto digest = pulp::runtime::sha256_hex(std::string_view(jost));
    const std::string family =
        "Sealed Capture " +
        pulp::test::make_unique_temp_dir("pulp-sealed-family").filename().string();

    ScratchDir scratch;
    const auto record = scratch.path / "font-faces.json";
    std::ofstream(record, std::ios::binary)
        << R"JSON({"schema":"pulp-browser-capture-font-faces-v1","faces":[{"family":")JSON"
        << family
        << R"JSON(","weight":"400 700","style":"normal","unicode_range":"","format":"truetype",
            "source":"pulp-capture:///fonts/Jost-Regular.ttf","sha256":")JSON"
        << digest << R"JSON("}],"assets":[{"sha256":")JSON" << digest
        << R"JSON(","mime_type":"font/ttf","data_base64":")JSON"
        << pulp::runtime::base64_encode(reinterpret_cast<const uint8_t*>(jost.data()), jost.size())
        << R"JSON("}]})JSON";

    // Capture, then the export pass that copies the face beside the script.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.style.font_family = family;
    std::vector<std::string> warnings;
    std::string error;
    REQUIRE(
        attach_page_font_faces(ir, record, scratch.path / "capture" / "fonts", warnings, error));
    const auto script = scratch.path / "plugin" / "ui" / "ui.js";
    fs::create_directories(script.parent_path());
    REQUIRE(pulp::import_design::localize_ir_assets(ir, script.string(), &error));
    INFO(error);

    const auto js = generate_pulp_js(ir);
    std::ofstream(script, std::ios::binary) << js;
    // The shipped script asks for no capability at all.
    CHECK(infer_capability_names_from_js(js).empty());

    // Seal the files exactly as the plug-in build does, then remove them from
    // disk: a face that registers now came from sealed bytes, not a path.
    std::vector<std::string> sealed_bytes;
    std::vector<WidgetBridge::EmbeddedAsset> embedded;
    const auto pairs = ui_script_embed_pairs(script, scratch.path);
    REQUIRE_FALSE(pairs.empty());
    sealed_bytes.reserve(pairs.size());
    for (const auto& [name, path] : pairs)
        sealed_bytes.push_back(read_file(path));
    for (std::size_t i = 0; i < pairs.size(); ++i)
        embedded.push_back({pairs[i].first,
                            reinterpret_cast<const unsigned char*>(sealed_bytes[i].data()),
                            sealed_bytes[i].size()});
    fs::remove_all(script.parent_path() / "assets");
    REQUIRE_FALSE(pulp::canvas::is_font_registered(family));

    pulp::view::ScriptEngine engine;
    View root;
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store, nullptr, pulp::view::CapabilitySet{});
    bridge.set_embedded_assets(embedded);
    REQUIRE(engine.evaluate("typeof registerFont").toString() == "undefined");
    bridge.load_script(js);
    CHECK(pulp::canvas::is_font_registered(family));
}

TEST_CASE("a tool seals only font files that resolve inside the script directory",
          "[import-design][font-assets][security]") {
    ScratchDir scratch;
    const auto script_dir = scratch.path / "panel";
    fs::create_directories(script_dir / "assets");
    fs::create_directories(scratch.path / "outside");
    const auto jost = fs::path(PULP_SOURCE_DIR) / "external/fonts/Jost-Regular.ttf";
    fs::copy_file(jost, script_dir / "assets" / "inside.ttf");
    fs::copy_file(jost, scratch.path / "outside" / "elsewhere.ttf");
    std::error_code ec;
    fs::create_symlink(scratch.path / "outside" / "elsewhere.ttf",
                       script_dir / "assets" / "escape.ttf", ec);
    if (ec)
        SKIP("this host cannot create a symlink: " + ec.message());

    DesignIR ir;
    for (const char* name : {"assets/inside.ttf", "assets/escape.ttf"}) {
        IRFontAsset face;
        face.family = name;
        face.resolved_path = name;
        ir.font_family_assets.push_back(face);
    }
    const auto sealed = pulp::import_design::read_sealed_font_files(ir, script_dir);
    REQUIRE(sealed.assets.size() == 1);
    CHECK(sealed.assets[0].name == "assets/inside.ttf");
}
