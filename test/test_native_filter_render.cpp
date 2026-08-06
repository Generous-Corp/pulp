#include <catch2/catch_test_macros.hpp>

#include "paint_probe.hpp"

#include <pulp/view/css_effect_parse.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

/// Two flat opaque rectangles, the upper one carrying `blend`. Flat colours
/// and full cover make the composite arithmetic checkable by hand: the child
/// fills the ground exactly, so every interior pixel is one blend of one pair.
DesignIR flat_over_flat(const std::string& blend) {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "ground";
    ir.root.style.width = 64.0f;
    ir.root.style.height = 64.0f;
    ir.root.style.background_color = "#483221";  // (72, 50, 33)

    IRNode bar;
    bar.type = "frame";
    bar.name = "bar";
    bar.style.width = 64.0f;
    bar.style.height = 64.0f;
    bar.style.background_color = "#C86B37";  // (200, 107, 55)
    if (!blend.empty()) bar.style.mix_blend_mode = blend;
    ir.root.children.push_back(std::move(bar));
    return ir;
}

/// A dark ground with one small opaque square in the middle, carrying a box
/// shadow and optionally a blend mode. Small box, wide shadow: almost all of
/// the shadow's ink lands OUTSIDE the box, which is exactly the ink a layer
/// sized to the box throws away.
DesignIR dot_with_shadow(const std::string& blend, bool inset = false) {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "ground";
    ir.root.style.width = 96.0f;
    ir.root.style.height = 96.0f;
    ir.root.style.background_color = "#07080B";

    IRNode dot;
    dot.type = "frame";
    dot.name = "dot";
    dot.style.width = 24.0f;
    dot.style.height = 24.0f;
    dot.style.position = "absolute";
    dot.style.left = 36.0f;
    dot.style.top = 36.0f;
    dot.style.background_color = "#C86B37";
    IRBoxShadow glow;
    glow.blur = 16.0f;
    glow.color = "rgba(200, 107, 55, 0.9)";
    glow.inset = inset;
    dot.style.box_shadow.push_back(glow);
    if (!blend.empty()) dot.style.mix_blend_mode = blend;
    ir.root.children.push_back(std::move(dot));
    return ir;
}

/// How many pixels OUTSIDE the dot's own 24x24 box carry ink the flat ground
/// does not. Counted rather than compared: the point is whether the halo
/// exists at all, and a count says that without depending on what a blend mode
/// does to the halo's colour.
///
/// Reads the Skia raster buffer, so a case calling this must first call
/// PULP_SKIP_WITHOUT_PAINT. Without it the case fails on a no-Skia lane at the
/// REQUIRE below, as a paint bug that is not there.
int halo_pixels(const DesignIR& ir) {
    auto root = build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(root != nullptr);
    uint32_t w = 0, h = 0;
    const auto rgba = render_to_rgba(*root, 96, 96, 1.0f, &w, &h);
    REQUIRE_FALSE(rgba.empty());
    REQUIRE(w == 96u);
    REQUIRE(h == 96u);
    int lit = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const bool inside_dot = x >= 36u && x < 60u && y >= 36u && y < 60u;
            if (inside_dot) continue;
            const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
            // The ground is (7, 8, 11). Anything meaningfully above it in red
            // is the warm glow, and nothing else in this document paints.
            if (static_cast<int>(rgba[i]) > 7 + 6) ++lit;
        }
    }
    return lit;
}

struct Rgb {
    int r = 0, g = 0, b = 0;
};

/// The centre pixel of the rendered tree, from the Skia raster path's own
/// buffer. No PNG encode/decode round-trip, so the value read is the value
/// composited.
///
/// Reads the Skia raster buffer, so a case calling this must first call
/// PULP_SKIP_WITHOUT_PAINT. Without it the case fails on a no-Skia lane at the
/// REQUIRE below, as a paint bug that is not there.
Rgb render_centre(const DesignIR& ir) {
    auto root = build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(root != nullptr);
    uint32_t w = 0, h = 0;
    const auto rgba = render_to_rgba(*root, 64, 64, 1.0f, &w, &h);
    // An empty buffer would read as (0,0,0) and quietly fail every assertion
    // for the wrong reason.
    REQUIRE_FALSE(rgba.empty());
    REQUIRE(w == 64u);
    REQUIRE(h == 64u);
    const size_t i = (static_cast<size_t>(32) * w + 32u) * 4u;
    return Rgb{rgba[i], rgba[i + 1], rgba[i + 2]};
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

TEST_CASE("the CSS blend parser maps the additive keywords",
          "[view][blend][design]") {
    using BM = pulp::canvas::Canvas::BlendMode;
    // The parser is the native lane's only mapping from the IR keyword to a
    // canvas blend mode. A keyword it does not know is not an error anywhere
    // downstream — the importer simply never calls set_mix_blend_mode and the
    // node composites source-over — so an absent entry is invisible except in
    // pixels.
    CHECK(css_blend_mode("plus-lighter") == BM::lighter);
    CHECK(css_blend_mode("plus-darker") == BM::lighter);
    CHECK(css_blend_mode("screen") == BM::screen);
    CHECK(css_blend_mode("multiply") == BM::multiply);
    // Still nothing for a keyword with no faithful lowering, which is what
    // keeps an unhonored mode visible as absent rather than as normal.
    CHECK_FALSE(css_blend_mode("linear-burn").has_value());
}

TEST_CASE("plus-lighter composites additively in the native tree",
          "[view][blend][design]") {
    PULP_SKIP_WITHOUT_PAINT("additive compositing in the native tree");
    // A bar of (200,107,55) over a ground of (72,50,33). Additive compositing
    // gives (272,157,88), which clamps to (255,157,88) — brighter than either
    // input in every channel. Source-over leaves the bar's own colour.
    //
    // Measured numbers, not invented ones: on the lattice fixture's velocity
    // row the native render drew (200,107,55) where the browser drew
    // (241,143,82) over the same (72,50,33) backdrop, with the bar geometry an
    // exact match. Nothing but the compositing was wrong, and it was wrong
    // because the keyword had no entry in the parser above — so the importer
    // never called set_mix_blend_mode and the View stayed at
    // BlendMode::normal.
    const Rgb source_over = render_centre(flat_over_flat(""));
    // Control first: the reader must be able to report the UN-blended state,
    // or an additive assertion below would only be agreeing with itself.
    CHECK(std::abs(source_over.r - 200) <= 1);
    CHECK(std::abs(source_over.g - 107) <= 1);
    CHECK(std::abs(source_over.b - 55) <= 1);

    const Rgb additive = render_centre(flat_over_flat("plus-lighter"));
    INFO("plus-lighter centre = " << additive.r << "," << additive.g << ","
                                  << additive.b);
    // The sum is taken on the 8-bit sRGB-encoded values, which is what Skia's
    // kPlus does on this raster surface and what the browser does in device
    // space. A blend performed in linear light would land near (255,118,65)
    // instead and fail these bounds loudly rather than silently passing.
    CHECK(additive.r >= 250);
    CHECK(std::abs(additive.g - 157) <= 4);
    CHECK(std::abs(additive.b - 88) <= 4);
}

TEST_CASE("a blend mode does not delete the node's outset shadow",
          "[view][blend][shadow][design]") {
    PULP_SKIP_WITHOUT_PAINT("a blend layer's effect on an outset shadow");
    // A compositing layer's bounds are a CLIP, and an outset shadow paints
    // INSIDE that layer. Sized to the border box, the layer threw the shadow
    // away — every pixel of it, because a 16px glow on a 24px box is almost
    // entirely outside the box.
    //
    // The bug is older than the blend support that exposed it: nothing in the
    // fixture corpus opened a layer AND carried a shadow until `plus-lighter`
    // started opening one. It does now — kelvin's three envelope vertex dots
    // and lattice's fifteen velocity bars all carry `plus-lighter` and an
    // outset glow — so this is a live defect, not a hypothetical.
    //
    // Control first. Without a blend mode no layer opens, so this is the halo
    // the renderer has always drawn and the number the assertion below is
    // measured against. If it is zero the instrument is broken and the blend
    // assertion would be agreeing with itself.
    const int plain = halo_pixels(dot_with_shadow(""));
    INFO("halo without a blend mode: " << plain);
    REQUIRE(plain > 500);

    const int blended = halo_pixels(dot_with_shadow("plus-lighter"));
    INFO("halo with plus-lighter: " << blended);
    // Before the fix this is exactly 0 — the layer is 24x24 and the glow has
    // nowhere to land. After it, the halo is the same silhouette composited
    // additively, so it covers at least as much ground as the plain one.
    CHECK(blended >= plain);
}

TEST_CASE("an inset shadow does not grow a blend node's layer",
          "[view][blend][shadow][design]") {
    PULP_SKIP_WITHOUT_PAINT("an inset shadow's effect on a blend layer");
    // The other half of the contract, and the reason the extent is computed
    // from the shadow list rather than padded by a constant. An inset shadow
    // paints inside the padding box by definition, so it must contribute
    // nothing to the layer — a constant pad would enlarge the layer for every
    // node that has one and admit ink that does not exist.
    //
    // Asserted where it is observable: no ink outside the box, blend or not.
    CHECK(halo_pixels(dot_with_shadow("", /*inset=*/true)) == 0);
    CHECK(halo_pixels(dot_with_shadow("plus-lighter", /*inset=*/true)) == 0);
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
