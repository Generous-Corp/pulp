#include <pulp/view/design_import.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/widgets.hpp>

#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/canvas/text_shaper.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/unique_temp_dir.hpp"
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using namespace pulp::view;
namespace fs = std::filesystem;

namespace {

struct TemporaryDirectory {
    fs::path path = pulp::test::make_unique_temp_dir("pulp-design-ir-asset-base");

    TemporaryDirectory() = default;
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

namespace {

IRNode text_in_slot(std::string text, std::optional<std::string> valign, float height,
                    float font_size, float top) {
    IRNode node;
    node.type = "text";
    node.text_content = std::move(text);
    node.style.position = "absolute";
    node.style.left = 10.0f;
    node.style.top = top;
    node.style.width = 120.0f;
    node.style.height = height;
    node.style.font_size = font_size;
    node.style.font_family = "Arial";
    node.style.white_space = "nowrap";
    node.style.vertical_align = std::move(valign);
    return node;
}

} // namespace

TEST_CASE("an imported label's vertical alignment is the same in the native and baked lanes",
          "[view][import][native-materializer][text-vertical-align]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.style.width = 300.0f;
    ir.root.style.height = 300.0f;
    // A browser capture's button-input value: the 15px content box is not 1.15x
    // the 13.3333px font, so only the recorded `middle` centres it, as Chromium
    // centres the line in the control.
    ir.root.children.push_back(text_in_slot("Save preset", "middle", 15.0f, 13.3333f, 10.0f));
    // An explicit `top` wins over the tall-slot rule.
    ir.root.children.push_back(text_in_slot("Top", "top", 40.0f, 12.0f, 40.0f));
    ir.root.children.push_back(text_in_slot("Bottom", "bottom", 40.0f, 12.0f, 90.0f));
    // With no recorded alignment, a slot well taller than its font is centred,
    // and one that barely fits keeps the default.
    ir.root.children.push_back(text_in_slot("Slot", std::nullopt, 40.0f, 12.0f, 140.0f));
    ir.root.children.push_back(text_in_slot("Tight", std::nullopt, 13.0f, 12.0f, 190.0f));
    using V = pulp::canvas::TextVerticalAlign;
    const V expected[] = {V::center, V::top, V::bottom, V::center, V::top};

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    root->layout_children();
    REQUIRE(root->child_count() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        auto* label = dynamic_cast<Label*>(root->child_at(i));
        REQUIRE(label != nullptr);
        INFO(label->text());
        CHECK(label->vertical_align() == expected[i]);
    }

    // Centred, the value's baseline sits half the leftover ink below the box
    // top, not one full ascent down as a top-aligned label paints it.
    const auto face = pulp::canvas::global_text_shaper().prepare(" ", "Arial", 13.3333f, 400, 0);
    if (face.metrics_are_real()) {
        pulp::canvas::RecordingCanvas canvas;
        root->child_at(0)->paint(canvas);
        std::optional<float> baseline;
        for (const auto& command : canvas.commands())
            if (command.type == pulp::canvas::DrawCommand::Type::fill_text)
                baseline = command.f[1];
        REQUIRE(baseline.has_value());
        const float ink = face.ascent() + face.descent();
        CHECK(*baseline == Catch::Approx((15.0f - ink) * 0.5f + face.ascent()).margin(0.01f));
    }

    const auto source = generate_pulp_cpp(ir, {}, {}).source;
    const auto count = [&](const std::string& needle) {
        std::size_t n = 0;
        for (auto at = source.find(needle); at != std::string::npos;
             at = source.find(needle, at + needle.size()))
            ++n;
        return n;
    };
    CHECK(count("->set_vertical_align(pulp::canvas::TextVerticalAlign::center);") == 2);
    CHECK(count("->set_vertical_align(pulp::canvas::TextVerticalAlign::bottom);") == 1);
    CHECK(count("->set_vertical_align(") == 3);
}
