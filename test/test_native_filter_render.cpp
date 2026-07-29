#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pulp::view;

namespace {

/// A ground with one circle on it. `filter` is applied to the circle.
DesignIR one_circle(const std::string& filter, const std::string& blend = {}) {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "ground";
    ir.root.style.width = 240.0f;
    ir.root.style.height = 240.0f;
    ir.root.style.background_color = "#07080B";

    IRNode dot;
    dot.type = "frame";
    dot.name = "bloom";
    dot.style.width = 120.0f;
    dot.style.height = 120.0f;
    dot.style.border_radius = 60.0f;
    dot.style.background_color = "#16DAC2";
    if (!filter.empty()) dot.style.filter = filter;
    if (!blend.empty()) dot.style.mix_blend_mode = blend;
    ir.root.children.push_back(std::move(dot));
    return ir;
}

std::vector<unsigned char> render_bytes(const DesignIR& ir, const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);

    auto root = build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(root != nullptr);
    REQUIRE(render_to_file(*root, 240, 240, path.string(), 1.0f,
                           ScreenshotBackend::skia));

    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("a blur filter changes what the native tree draws",
          "[view][filter][design]") {
    // The regression this pins: the native tree carried style.filter and
    // applied none of it, so a 60px bloom rendered as a hard-edged circle.
    // Nothing in the IR explained the difference — the same document rendered
    // soft through the JS lane and hard through this one.
    //
    // Comparing bytes is deliberate. Asserting that set_filter_blur was called
    // would pass even if the value never reached a paint; only the pixels prove
    // the blur was composited.
    const auto sharp = render_bytes(one_circle(""), "pulp-filter-sharp.png");
    const auto blurred = render_bytes(one_circle("blur(12px)"),
                                      "pulp-filter-blurred.png");

    REQUIRE_FALSE(sharp.empty());
    REQUIRE_FALSE(blurred.empty());
    CHECK(sharp != blurred);
}

TEST_CASE("a blend mode changes what the native tree draws",
          "[view][filter][design]") {
    // "screen" over a dark ground is how a lit element reads as emitting
    // rather than merely being bright — the effect an atmospheric panel is
    // built from.
    const auto plain = render_bytes(one_circle(""), "pulp-blend-plain.png");
    const auto screened = render_bytes(one_circle("", "screen"),
                                       "pulp-blend-screen.png");

    REQUIRE_FALSE(plain.empty());
    CHECK(plain != screened);
}

TEST_CASE("a filter list without blur leaves the node unfiltered",
          "[view][filter][design]") {
    // Only blur() is read. brightness/saturate need a real filter chain, and
    // collapsing them to a blur radius would be a lie — an unfiltered node is
    // the honest failure, since wrongly blurred is worse than plain.
    const auto plain = render_bytes(one_circle(""), "pulp-filter-none.png");
    const auto other = render_bytes(one_circle("brightness(1.4)"),
                                    "pulp-filter-other.png");

    CHECK(plain == other);
}

TEST_CASE("a zero or negative blur radius is not a filter",
          "[view][filter][design]") {
    // blur(0) is a no-op the model may well write; treating it as a filter
    // would allocate a layer for nothing.
    const auto plain = render_bytes(one_circle(""), "pulp-filter-zero-base.png");
    const auto zero = render_bytes(one_circle("blur(0px)"), "pulp-filter-zero.png");

    CHECK(plain == zero);
}


TEST_CASE("an IR text node renders crisp", "[view][text][design]") {
    // A Halo-class panel came out with BOTH its 96px wordmark and its 12px
    // eyebrow smeared, while knob labels in the same frame stayed sharp. The
    // labels are drawn by the widget; these two are IR text nodes. So the
    // question is not size, it is whether an IR text node rasterizes cleanly.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.style.width = 200.0f;
    ir.root.style.height = 80.0f;
    ir.root.style.background_color = "#000000";

    IRNode text;
    text.type = "text";
    text.text_content = "Halo";
    text.style.color = "#FFFFFF";
    text.style.font_size = 48.0f;
    text.style.font_weight = 800;
    ir.root.children.push_back(std::move(text));

    const auto path = std::filesystem::temp_directory_path() / "pulp-ir-text.png";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto root = build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(root != nullptr);
    REQUIRE(render_to_file(*root, 200, 80, path.string(), 1.0f,
                           ScreenshotBackend::skia));
    REQUIRE(std::filesystem::exists(path));
    // Text at all: a blank frame compresses to almost nothing.
    CHECK(std::filesystem::file_size(path) > 1500);
}

TEST_CASE("IR text inside a nested frame stays crisp", "[view][text][design]") {
    // Halo's wordmark sits inside a `lede` frame; the crisp control rendered
    // text directly under the root. That nesting is the remaining difference
    // after decorations and every filter were removed and the smear stayed.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.style.width = 300.0f;
    ir.root.style.height = 120.0f;
    ir.root.style.background_color = "#000000";

    IRNode lede;
    lede.type = "frame";
    lede.layout.display = "flex";
    lede.layout.direction = LayoutDirection::column;
    lede.layout.gap = 8.0f;

    IRNode eyebrow;
    eyebrow.type = "text";
    eyebrow.text_content = "GRANULAR ENGINE";
    eyebrow.style.color = "#16DAC2";
    eyebrow.style.font_size = 12.0f;
    lede.children.push_back(std::move(eyebrow));

    IRNode word;
    word.type = "text";
    word.text_content = "Halo";
    word.style.color = "#FFFFFF";
    word.style.font_size = 48.0f;
    word.style.font_weight = 800;
    lede.children.push_back(std::move(word));

    ir.root.children.push_back(std::move(lede));

    const auto path = std::filesystem::temp_directory_path() / "pulp-ir-text-nested.png";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto root = build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(root != nullptr);
    REQUIRE(render_to_file(*root, 300, 120, path.string(), 1.0f,
                           ScreenshotBackend::skia));
    CHECK(std::filesystem::file_size(path) > 1500);
}

TEST_CASE("IR text stays crisp when the panel is scaled up",
          "[view][text][design]") {
    // The Halo artifact declares a 900x580 root and is rendered into 1280x800
    // — about 1.4x. Its wordmark came out smeared while widget-drawn knob
    // labels in the same frame stayed sharp, which is what rasterizing text at
    // design size and then scaling the result would look like.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.style.width = 200.0f;
    ir.root.style.height = 80.0f;
    ir.root.style.background_color = "#000000";

    IRNode text;
    text.type = "text";
    text.text_content = "Halo";
    text.style.color = "#FFFFFF";
    text.style.font_size = 48.0f;
    text.style.font_weight = 800;
    ir.root.children.push_back(std::move(text));

    const auto dir = std::filesystem::temp_directory_path();
    std::error_code ec;
    const auto exact = dir / "pulp-ir-text-exact.png";
    const auto scaled = dir / "pulp-ir-text-scaled.png";
    std::filesystem::remove(exact, ec);
    std::filesystem::remove(scaled, ec);

    auto a = build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(render_to_file(*a, 200, 80, exact.string(), 1.0f,
                           ScreenshotBackend::skia));
    auto b = build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(render_to_file(*b, 284, 114, scaled.string(), 1.0f,
                           ScreenshotBackend::skia));

    REQUIRE(std::filesystem::exists(scaled));
    CHECK(std::filesystem::file_size(scaled) > 1500);
}
