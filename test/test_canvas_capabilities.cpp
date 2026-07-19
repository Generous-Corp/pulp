// Canvas capability queries — `Canvas::supports(CanvasCapability)` (WI-28).
//
// `supports(c) == false` means the corresponding verb DEGRADES (silent no-op,
// color/mask/blur op dropped, filename placeholder) rather than rendering
// faithfully. These tests pin the honest per-backend map:
//   - SkiaCanvas advertises the full faithful set (scene_cache is future work).
//   - CoreGraphicsCanvas advertises ONLY images — its real gap surface.
//   - RecordingCanvas advertises nothing (it records intent, does not render)
//     yet still records the verb, proving "capability false != verb unusable".
// The alias `supports_image_draw()` forwards to `supports(images)`.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/view/view.hpp>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#endif

#ifdef __APPLE__
#include <pulp/canvas/cg_canvas.hpp>
#include <CoreGraphics/CoreGraphics.h>
#include <vector>
#endif

using namespace pulp::canvas;

namespace {

// Every enumerator, so a test can assert "all false" without listing them.
constexpr CanvasCapability kAllCaps[] = {
    CanvasCapability::images,
    CanvasCapability::svg,
    CanvasCapability::clip_path_svg,
    CanvasCapability::filter_chain,
    CanvasCapability::mask_layer,
    CanvasCapability::backdrop_filter,
    CanvasCapability::bloom_layer,
    CanvasCapability::sksl_draw,
    CanvasCapability::sksl_post_effect,
    CanvasCapability::box_shadow_gaussian,
    CanvasCapability::scene_cache,
};

}  // namespace

TEST_CASE("RecordingCanvas advertises no capabilities but still records verbs",
          "[canvas][capabilities][issue-6262]") {
    RecordingCanvas rec;

    // Records intent; renders nothing → every capability is false.
    for (CanvasCapability cap : kAllCaps) {
        INFO("capability ordinal " << static_cast<int>(cap));
        REQUIRE_FALSE(rec.supports(cap));
    }
    // The alias tracks supports(images).
    REQUIRE_FALSE(rec.supports_image_draw());

    // capability false != verb unusable: the mask verb still lands in the
    // command stream (as the distinct save_layer_mask intent), it simply is
    // not rasterized. This is why adoption logs-once but never drops the verb.
    rec.save_layer_with_mask(0, 0, 32, 32, 1.0f, "url(#m)", "cover");
    rec.restore();
    bool recorded_mask = false;
    for (const auto& c : rec.commands())
        if (c.type == DrawCommand::Type::save_layer_mask) recorded_mask = true;
    REQUIRE(recorded_mask);
}

TEST_CASE("Masked View paints its mask verb into a RecordingCanvas, balanced",
          "[canvas][capabilities][view][issue-6262]") {
    // Non-tautology guard: drive the REAL paint_all mask branch (not a direct
    // canvas call) and prove that on a backend reporting supports(mask_layer)
    // == false the verb is still emitted and the save stack stays balanced.
    pulp::view::View v;
    v.set_bounds({0, 0, 40, 40});
    v.set_mask_image("linear-gradient(black, transparent)");

    RecordingCanvas rec;
    REQUIRE_FALSE(rec.supports(CanvasCapability::mask_layer));
    v.paint_all(rec);

    int mask_layers = 0, depth = 0;
    for (const auto& c : rec.commands()) {
        if (c.type == DrawCommand::Type::save_layer_mask) ++mask_layers;
        if (c.type == DrawCommand::Type::save ||
            c.type == DrawCommand::Type::save_layer ||
            c.type == DrawCommand::Type::save_layer_blend ||
            c.type == DrawCommand::Type::save_layer_filters ||
            c.type == DrawCommand::Type::save_layer_mask ||
            c.type == DrawCommand::Type::save_layer_bloom)
            ++depth;
        else if (c.type == DrawCommand::Type::restore)
            --depth;
        REQUIRE(depth >= 0);  // never a restore without a matching save
    }
    REQUIRE(mask_layers == 1);  // the masked subtree opened exactly one layer
    REQUIRE(depth == 0);        // balanced
}

#ifdef PULP_HAS_SKIA
TEST_CASE("SkiaCanvas advertises the full faithful capability set",
          "[canvas][capabilities][skia][issue-6262]") {
    SkImageInfo info = SkImageInfo::Make(8, 8, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    SkiaCanvas canvas(surface->getCanvas());

    REQUIRE(canvas.supports(CanvasCapability::images));
    REQUIRE(canvas.supports(CanvasCapability::svg));
    REQUIRE(canvas.supports(CanvasCapability::clip_path_svg));
    REQUIRE(canvas.supports(CanvasCapability::filter_chain));
    REQUIRE(canvas.supports(CanvasCapability::mask_layer));
    REQUIRE(canvas.supports(CanvasCapability::backdrop_filter));
    REQUIRE(canvas.supports(CanvasCapability::bloom_layer));
    REQUIRE(canvas.supports(CanvasCapability::sksl_draw));
    REQUIRE(canvas.supports(CanvasCapability::sksl_post_effect));
    REQUIRE(canvas.supports(CanvasCapability::box_shadow_gaussian));
    // The alias agrees with the enum query.
    REQUIRE(canvas.supports_image_draw());

    // scene_cache is FU-3 work; Skia honestly reports false until record_scene
    // lands, so callers still paint directly today.
    REQUIRE_FALSE(canvas.supports(CanvasCapability::scene_cache));
}
#endif

#ifdef __APPLE__
TEST_CASE("CoreGraphicsCanvas advertises only image capability",
          "[canvas][capabilities][cg][issue-6262]") {
    std::vector<uint8_t> px(8 * 8 * 4, 0u);
    auto cs = CGColorSpaceCreateDeviceRGB();
    REQUIRE(cs != nullptr);
    const uint32_t bitmap_info =
        static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
        static_cast<uint32_t>(kCGBitmapByteOrder32Big);
    CGContextRef ctx = CGBitmapContextCreate(px.data(), 8, 8, 8, 8 * 4u, cs,
                                             bitmap_info);
    CGColorSpaceRelease(cs);
    REQUIRE(ctx != nullptr);

    CoreGraphicsCanvas canvas(ctx, 8, 8);
    // CG has a real ImageIO decoder — images faithful...
    REQUIRE(canvas.supports(CanvasCapability::images));
    REQUIRE(canvas.supports_image_draw());  // alias agrees
    // ...but every effect verb inherits the base degradation. These are the
    // real CG gaps the honest map must report false.
    REQUIRE_FALSE(canvas.supports(CanvasCapability::mask_layer));
    REQUIRE_FALSE(canvas.supports(CanvasCapability::filter_chain));
    REQUIRE_FALSE(canvas.supports(CanvasCapability::backdrop_filter));
    REQUIRE_FALSE(canvas.supports(CanvasCapability::sksl_post_effect));
    REQUIRE_FALSE(canvas.supports(CanvasCapability::sksl_draw));
    REQUIRE_FALSE(canvas.supports(CanvasCapability::svg));
    REQUIRE_FALSE(canvas.supports(CanvasCapability::clip_path_svg));
    REQUIRE_FALSE(canvas.supports(CanvasCapability::bloom_layer));
    REQUIRE_FALSE(canvas.supports(CanvasCapability::box_shadow_gaussian));
    REQUIRE_FALSE(canvas.supports(CanvasCapability::scene_cache));

    CGContextRelease(ctx);
}
#endif
