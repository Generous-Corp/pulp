#include <pulp/view/design_import.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/widgets.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace pulp::view;
namespace fs = std::filesystem;

namespace {

struct TemporaryDirectory {
    fs::path path =
        fs::temp_directory_path() /
        ("pulp-design-ir-asset-base-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));

    TemporaryDirectory() { fs::create_directories(path); }
    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

struct CurrentPathGuard {
    fs::path original = fs::current_path();
    ~CurrentPathGuard() {
        std::error_code error;
        fs::current_path(original, error);
    }
};

}  // namespace

TEST_CASE("native materializer renders faithful_capture through the shared image path",
          "[view][import][native-materializer][faithful-capture]") {
    DesignIR ir;
    ir.source = DesignSource::claude;
    ir.root.type = "frame";
    ir.root.name = "Browser capture";
    ir.root.render_mode = NodeRenderMode::faithful_capture;
    ir.root.capture_asset_id = "reference:browser";
    ir.root.style.width = 956.0f;
    ir.root.style.height = 636.0f;

    // Real bytes on disk: the materializer only emits a `file://` image source
    // for a file that exists, so a synthetic capture path would materialize the
    // unresolved-asset placeholder instead of the shared image path under test.
    TemporaryDirectory capture_directory;
    const auto capture = capture_directory.path / "browser.png";
    {
        std::ofstream output(capture, std::ios::binary);
        output << "pulp-test-capture-bytes";
    }

    IRAssetRef asset;
    asset.asset_id = "reference:browser";
    asset.original_uri = "pulp-capture:///browser.png";
    asset.local_path = capture.string();
    asset.mime = "image/png";
    asset.width = 1912;
    asset.height = 1272;
    ir.asset_manifest.assets.push_back(asset);

    auto root = build_native_view_tree(ir, {});
    REQUIRE(root != nullptr);
    auto* image = dynamic_cast<ImageView*>(root.get());
    REQUIRE(image != nullptr);
    REQUIRE(image->image_source() ==
            "file://" + capture.lexically_normal().generic_string());
}

TEST_CASE("serialized DesignIR resolves portable capture assets from its document directory",
          "[view][import][native-materializer][faithful-capture][portable]") {
    TemporaryDirectory temporary;
    const auto document_directory = temporary.path / "document";
    const auto other_directory = temporary.path / "other-cwd";
    fs::create_directories(document_directory / "assets");
    fs::create_directories(other_directory);

    View source;
    source.set_background_color(Color::rgba8(204, 32, 64, 255));
    const auto source_png = render_to_png(
        source, 16, 16, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(source_png.empty());
    {
        std::ofstream output(
            document_directory / "assets/browser.png",
            std::ios::binary);
        output.write(
            reinterpret_cast<const char*>(source_png.data()),
            static_cast<std::streamsize>(source_png.size()));
    }

    DesignIR ir;
    ir.source = DesignSource::claude;
    ir.root.type = "frame";
    ir.root.name = "Portable browser capture";
    ir.root.render_mode = NodeRenderMode::faithful_capture;
    ir.root.capture_asset_id = "reference:browser";
    ir.root.style.width = 16.0f;
    ir.root.style.height = 16.0f;
    IRAssetRef asset;
    asset.asset_id = "reference:browser";
    asset.local_path = "assets/browser.png";
    asset.mime = "image/png";
    asset.width = 32;
    asset.height = 32;
    ir.asset_manifest.assets.push_back(std::move(asset));

    const auto document_path = document_directory / "ui.json";
    {
        std::ofstream output(document_path);
        output << serialize_design_ir(ir);
    }
    std::ifstream input(document_path);
    const std::string json{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    const auto reloaded = parse_design_ir_json(json);

    CurrentPathGuard current_path;
    fs::current_path(other_directory);
    auto root = build_native_view_tree(
        reloaded, reloaded.asset_manifest,
        {.asset_base_directory = document_path.parent_path()});
    REQUIRE(root);
    auto* image = dynamic_cast<ImageView*>(root.get());
    REQUIRE(image);
    CHECK(
        image->image_source() ==
        "file://" +
            (document_directory / "assets/browser.png")
                .lexically_normal()
                .generic_string());
    const auto reloaded_png = render_to_png(
        *root, 16, 16, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(reloaded_png.empty());
    const auto comparison =
        compare_screenshots(source_png, reloaded_png);
    REQUIRE(comparison.valid);
    CHECK(comparison.similarity == 1.0f);
}
