// CSS filter chain coverage has two sub-clusters that pin the same backend
// contract:
//
//   1. Skia-only pixel readback. contrast(0) renders ~mid-gray;
//      invert(1) maps black → white; opacity ordering with
//      drop-shadow changes pixel output; set_filter parses
//      drop-shadow with hex color + parses fallback null.
//
//   2. Portable filter-chain matrix math (no Skia required).
//      contrast(0) bias matrix lands at mid-gray (~128); invert(1)
//      maps black→white via the matrix; invert(0) is identity;
//      opacity(α) scales alpha and preserves RGB.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <array>
#include <vector>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#endif

using namespace pulp::canvas;

#ifdef PULP_HAS_SKIA
// ── CSS filter chain pixel readback ────────────────────────────────────
// Regression: SkColorFilters::Matrix translation column is in 0..255
// space. `contrast(0)` must produce mid-gray (~128) and `invert(1)`
// must map black to white pixel-for-pixel.
// Regression: opacity() must remain in the composed chain at its
// source-order position so subsequent filters (drop-shadow) see the
// reduced alpha as their input.
TEST_CASE("SkiaCanvas filter chain: contrast(0) renders ~mid-gray",
          "[canvas][skia][filter-chain]") {  // was [!mayfail]: the 255x-bias bug
                                             // rendered white; now enforced.
    constexpr int kW = 16;
    constexpr int kH = 16;
    SkImageInfo info = SkImageInfo::Make(kW, kH, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    REQUIRE(sk_canvas != nullptr);
    sk_canvas->clear(SK_ColorWHITE);

    SkiaCanvas canvas(sk_canvas);

    Canvas::FilterChainEntry contrast{};
    contrast.kind = Canvas::FilterChainEntry::Kind::contrast;
    contrast.amount = 0.0f;  // contrast(0) -> all colors map to mid-gray

    canvas.save_layer_with_filters(0, 0, kW, kH, 1.0f, &contrast, 1);
    canvas.set_fill_color(Color::rgba(1.0f, 0.0f, 0.0f, 1.0f));  // red input
    canvas.fill_rect(0, 0, kW, kH);
    canvas.restore();

    SkPixmap pm;
    REQUIRE(surface->peekPixels(&pm));
    SkColor c = pm.getColor(kW / 2, kH / 2);
    // contrast(0) collapses any input color to 0.5 * 255 = 128 on every
    // RGB channel. This pins the bias in 8-bit color space, with tolerance
    // for premultiplied round-tripping.
    REQUIRE(SkColorGetR(c) >= 120);
    REQUIRE(SkColorGetR(c) <= 136);
    REQUIRE(SkColorGetG(c) >= 120);
    REQUIRE(SkColorGetG(c) <= 136);
    REQUIRE(SkColorGetB(c) >= 120);
    REQUIRE(SkColorGetB(c) <= 136);
}

TEST_CASE("SkiaCanvas filter chain: invert(1) maps black to white",
          "[canvas][skia][filter-chain]") {
    constexpr int kW = 16;
    constexpr int kH = 16;
    SkImageInfo info = SkImageInfo::Make(kW, kH, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    REQUIRE(sk_canvas != nullptr);
    sk_canvas->clear(SK_ColorWHITE);

    SkiaCanvas canvas(sk_canvas);

    Canvas::FilterChainEntry invert{};
    invert.kind = Canvas::FilterChainEntry::Kind::invert;
    invert.amount = 1.0f;  // full invert

    canvas.save_layer_with_filters(0, 0, kW, kH, 1.0f, &invert, 1);
    canvas.set_fill_color(Color::rgba(0.0f, 0.0f, 0.0f, 1.0f));  // black input
    canvas.fill_rect(0, 0, kW, kH);
    canvas.restore();

    SkPixmap pm;
    REQUIRE(surface->peekPixels(&pm));
    SkColor c = pm.getColor(kW / 2, kH / 2);
    // invert(1) on black = white (t=1 normalized: -1*0 + 1 = 1). NOTE: this
    // black-only case passes under BOTH the correct [0,1] bias and the old
    // 255x-scaled bias (both clamp to white), which is exactly why it never
    // caught the bias bug — the white->black test below is the one that does.
    REQUIRE(SkColorGetR(c) >= 250);
    REQUIRE(SkColorGetG(c) >= 250);
    REQUIRE(SkColorGetB(c) >= 250);
}

// The smoking-gun for the bias bug: invert(1) on WHITE must render BLACK
// (k=-1, normalized bias 1 → -1 + 1 = 0). The prior 255x-scaled bias computed
// -1 + 255 → clamp to white, i.e. white stayed uninverted. Pixel-readback,
// mirroring the bloom test's rigor.
TEST_CASE("SkiaCanvas filter chain: invert(1) maps white to black",
          "[canvas][skia][filter-chain]") {
    constexpr int kW = 16;
    constexpr int kH = 16;
    SkImageInfo info = SkImageInfo::Make(kW, kH, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    sk_canvas->clear(SK_ColorWHITE);
    SkiaCanvas canvas(sk_canvas);

    Canvas::FilterChainEntry invert{};
    invert.kind = Canvas::FilterChainEntry::Kind::invert;
    invert.amount = 1.0f;

    canvas.save_layer_with_filters(0, 0, kW, kH, 1.0f, &invert, 1);
    canvas.set_fill_color(Color::rgba(1.0f, 1.0f, 1.0f, 1.0f));  // white input
    canvas.fill_rect(0, 0, kW, kH);
    canvas.restore();

    SkPixmap pm;
    REQUIRE(surface->peekPixels(&pm));
    SkColor c = pm.getColor(kW / 2, kH / 2);
    REQUIRE(SkColorGetR(c) <= 8);
    REQUIRE(SkColorGetG(c) <= 8);
    REQUIRE(SkColorGetB(c) <= 8);
}

TEST_CASE("SkiaCanvas filter chain: opacity ordering changes pixel output "
          "with drop-shadow",
          "[canvas][skia][filter-chain]") {
    // CSS spec: filters apply in source order. `opacity(0.5) drop-shadow(...)`
    // must reduce the source alpha BEFORE the shadow generates, while
    // `drop-shadow(...) opacity(0.5)` reduces the alpha of the already-
    // shadowed image. The two orderings produce different pixels; if
    // opacity were applied as final layer-alpha (ignoring source order)
    // both outputs would be identical.
    constexpr int kW = 32;
    constexpr int kH = 32;
    auto render_chain = [&](const Canvas::FilterChainEntry* chain, int n) {
        SkImageInfo info = SkImageInfo::Make(kW, kH, kN32_SkColorType,
                                             kPremul_SkAlphaType,
                                             SkColorSpace::MakeSRGB());
        auto surface = SkSurfaces::Raster(info);
        REQUIRE(surface != nullptr);
        auto* sk_canvas = surface->getCanvas();
        REQUIRE(sk_canvas != nullptr);
        sk_canvas->clear(SK_ColorWHITE);
        SkiaCanvas canvas(sk_canvas);
        canvas.save_layer_with_filters(0, 0, kW, kH, 1.0f, chain, n);
        canvas.set_fill_color(Color::rgba(0.0f, 0.0f, 0.0f, 1.0f));
        canvas.fill_rect(8, 8, 16, 16);
        canvas.restore();
        return surface;
    };

    Canvas::FilterChainEntry op{};
    op.kind = Canvas::FilterChainEntry::Kind::opacity;
    op.amount = 0.5f;
    Canvas::FilterChainEntry ds{};
    ds.kind = Canvas::FilterChainEntry::Kind::drop_shadow;
    ds.ds_offset_x = 4.0f;
    ds.ds_offset_y = 4.0f;
    ds.ds_blur = 0.0f;
    ds.ds_color = Color::rgba(0.0f, 0.0f, 0.0f, 1.0f);

    Canvas::FilterChainEntry chain_a[2] = {op, ds};   // opacity, then ds
    Canvas::FilterChainEntry chain_b[2] = {ds, op};   // ds, then opacity

    auto surf_a = render_chain(chain_a, 2);
    auto surf_b = render_chain(chain_b, 2);

    SkPixmap pa, pb;
    REQUIRE(surf_a->peekPixels(&pa));
    REQUIRE(surf_b->peekPixels(&pb));

    // Sample a point inside the shadow (offset 4,4 from the rect bottom-
    // right corner, well outside the original 8..24 fill).
    SkColor a = pa.getColor(26, 26);
    SkColor b = pb.getColor(26, 26);

    // Order matters: the two pixels must differ. If opacity were folded
    // into the final layer alpha instead of staying in the chain, the
    // shadow would be generated from the same fully-opaque source in
    // both orderings and the output would be identical.
    bool any_channel_differs =
        SkColorGetR(a) != SkColorGetR(b) ||
        SkColorGetG(a) != SkColorGetG(b) ||
        SkColorGetB(a) != SkColorGetB(b) ||
        SkColorGetA(a) != SkColorGetA(b);
    REQUIRE(any_channel_differs);
}

// Regression test: `parse_filter_chain("drop-shadow(dx dy blur color)")`
// must return non-null and the resulting SkImageFilter must produce a
// visible offset shadow when rendered through SkiaCanvas::set_filter().
//
// Kept [!mayfail]: this fails for a reason UNRELATED to the color-matrix bias
// fix — the drop-shadow filter is created (the getImageFilter() assertion
// passes) but no darkened pixel appears at the expected shadow offset (28,28),
// i.e. the set_filter/apply_filter drop-shadow render/position path, not the
// contrast/invert bias. Left masked so this closeout doesn't silently flip a
// test that is red for a separate, unaddressed reason.
TEST_CASE("SkiaCanvas set_filter parses drop-shadow and renders shadow",
          "[canvas][skia][filter-chain][drop-shadow][!mayfail]") {
    constexpr int kW = 32;
    constexpr int kH = 32;
    SkImageInfo info = SkImageInfo::Make(kW, kH, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    REQUIRE(sk_canvas != nullptr);
    sk_canvas->clear(SK_ColorWHITE);

    SkiaCanvas canvas(sk_canvas);
    canvas.set_filter("drop-shadow(4px 4px 0px black)");

    SkPaint paint;
    canvas.apply_filter(paint);
    REQUIRE(paint.getImageFilter() != nullptr);

    // Render a 16x16 filled rect at (8,8). The drop-shadow should
    // produce a black pixel at the shadow offset position (8+16+4, 8+16+4)
    // = (28,28) that is NOT fully white.
    sk_canvas->saveLayer(nullptr, &paint);
    SkPaint rect_paint;
    rect_paint.setColor(SK_ColorRED);
    rect_paint.setAntiAlias(false);
    sk_canvas->drawRect(SkRect::MakeXYWH(8, 8, 16, 16), rect_paint);
    sk_canvas->restore();

    SkPixmap pm;
    REQUIRE(surface->peekPixels(&pm));
    SkColor shadow_pixel = pm.getColor(28, 28);
    // Black drop-shadow on white → at least one channel < 200.
    bool has_shadow =
        SkColorGetR(shadow_pixel) < 200 ||
        SkColorGetG(shadow_pixel) < 200 ||
        SkColorGetB(shadow_pixel) < 200;
    REQUIRE(has_shadow);
}

TEST_CASE("SkiaCanvas set_filter parsers drop-shadow with hex color",
          "[canvas][skia][filter-chain][drop-shadow]") {
    constexpr int kW = 32;
    constexpr int kH = 32;
    SkImageInfo info = SkImageInfo::Make(kW, kH, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    REQUIRE(sk_canvas != nullptr);
    sk_canvas->clear(SK_ColorWHITE);

    SkiaCanvas canvas(sk_canvas);
    canvas.set_filter("drop-shadow(2px 2px 2px #ff0000)");

    SkPaint paint;
    canvas.apply_filter(paint);
    REQUIRE(paint.getImageFilter() != nullptr);
}

TEST_CASE("SkiaCanvas set_filter non-existent filter returns null",
          "[canvas][skia][filter-chain]") {
    SkiaCanvas canvas(nullptr);  // null canvas for this test
    canvas.set_filter("none");
    SkPaint paint;
    canvas.apply_filter(paint);
    REQUIRE(paint.getImageFilter() == nullptr);
}

#endif  // PULP_HAS_SKIA

// ── Portable filter-chain matrix math ──────────────────────────────────
// These tests run on every platform (no Skia required) and dry-run the
// SAME float math the production save_layer_with_filters() switch uses
// to populate SkColorMatrix entries. They guard against the two
// regressions independently of whether Skia is linked into the test
// binary:
//   - contrast / invert bias must be in 0..255 space.
//   - opacity() must be a per-position color matrix.
//
// Helpers below mirror the matrix construction in
// core/canvas/src/skia_canvas.cpp; if those formulas drift here without
// drifting in the production switch (or vice versa) the tests fail.
namespace {

// Apply a 4x5 SkColorMatrix-style row-major matrix to a (R,G,B,A) tuple in
// 0..255 space and return the post-clamp output channel as a uint8. Models what
// SkColorFilters::Matrix ACTUALLY does: RGBA multipliers act on the channel
// value, but the TRANSLATION column (m[4],9,14,19) is NORMALIZED [0,1] — Skia
// scales it by 255 for 8-bit output. (A prior version added the bias directly
// in 0..255 space; combined with the production code's 255x-scaled bias it was
// self-consistent but modeled the WRONG convention, which is exactly why the
// [!mayfail] raster contrast test disagreed with these unit tests.)
struct Px { float r, g, b, a; };  // 0..255

Px apply_matrix(const float m[20], Px in) {
    auto clamp = [](float v) {
        if (v < 0.0f) return 0.0f;
        if (v > 255.0f) return 255.0f;
        return v;
    };
    Px out;
    out.r = clamp(m[ 0]*in.r + m[ 1]*in.g + m[ 2]*in.b + m[ 3]*in.a + m[ 4]*255.0f);
    out.g = clamp(m[ 5]*in.r + m[ 6]*in.g + m[ 7]*in.b + m[ 8]*in.a + m[ 9]*255.0f);
    out.b = clamp(m[10]*in.r + m[11]*in.g + m[12]*in.b + m[13]*in.a + m[14]*255.0f);
    out.a = clamp(m[15]*in.r + m[16]*in.g + m[17]*in.b + m[18]*in.a + m[19]*255.0f);
    return out;
}

// Mirrors the contrast(c) matrix construction in skia_canvas_opacity.cpp: the
// bias is NORMALIZED [0,1] (0.5*(1-c)), not 8-bit.
void build_contrast_matrix(float c, float m[20]) {
    const float t = 0.5f * (1.0f - c);
    float src[20] = {
        c, 0, 0, 0, t,
        0, c, 0, 0, t,
        0, 0, c, 0, t,
        0, 0, 0, 1, 0,
    };
    for (int i = 0; i < 20; ++i) m[i] = src[i];
}

// Mirrors the invert(amount) matrix construction: normalized [0,1] bias `a`.
void build_invert_matrix(float amount, float m[20]) {
    const float a = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
    const float k = 1.0f - 2.0f * a;
    const float t = a;
    float src[20] = {
        k, 0, 0, 0, t,
        0, k, 0, 0, t,
        0, 0, k, 0, t,
        0, 0, 0, 1, 0,
    };
    for (int i = 0; i < 20; ++i) m[i] = src[i];
}

// Mirrors the opacity(amount) matrix construction.
void build_opacity_matrix(float amount, float m[20]) {
    const float a = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
    float src[20] = {
        1, 0, 0, 0, 0,
        0, 1, 0, 0, 0,
        0, 0, 1, 0, 0,
        0, 0, 0, a, 0,
    };
    for (int i = 0; i < 20; ++i) m[i] = src[i];
}

} // namespace

TEST_CASE("Filter chain: contrast(0) bias lands at mid-gray (~128)",
          "[canvas][filter-chain]") {
    float m[20];
    build_contrast_matrix(0.0f, m);
    // Any input -> 128 because slope=0, intercept=128.
    Px white{255, 255, 255, 255};
    Px black{  0,   0,   0, 255};
    Px red  {255,   0,   0, 255};

    Px ow = apply_matrix(m, white);
    Px ob = apply_matrix(m, black);
    Px orr = apply_matrix(m, red);
    REQUIRE(ow.r == Catch::Approx(127.5f).margin(0.5f));
    REQUIRE(ow.g == Catch::Approx(127.5f).margin(0.5f));
    REQUIRE(ow.b == Catch::Approx(127.5f).margin(0.5f));
    REQUIRE(ob.r == Catch::Approx(127.5f).margin(0.5f));
    REQUIRE(ob.g == Catch::Approx(127.5f).margin(0.5f));
    REQUIRE(ob.b == Catch::Approx(127.5f).margin(0.5f));
    REQUIRE(orr.r == Catch::Approx(127.5f).margin(0.5f));
    REQUIRE(orr.g == Catch::Approx(127.5f).margin(0.5f));
    REQUIRE(orr.b == Catch::Approx(127.5f).margin(0.5f));
    // The normalized [0,1] bias 0.5, scaled to 8-bit by apply_matrix, lands at
    // 127.5 — matching what real Skia renders (see the raster contrast test).
}

TEST_CASE("Filter chain: invert(1) maps black->white via the matrix",
          "[canvas][filter-chain]") {
    float m[20];
    build_invert_matrix(1.0f, m);
    Px black{0, 0, 0, 255};
    Px white{255, 255, 255, 255};
    Px ob = apply_matrix(m, black);
    Px ow = apply_matrix(m, white);
    // black -> white
    REQUIRE(ob.r == Catch::Approx(255.0f).margin(0.5f));
    REQUIRE(ob.g == Catch::Approx(255.0f).margin(0.5f));
    REQUIRE(ob.b == Catch::Approx(255.0f).margin(0.5f));
    // white -> black (k=-1, normalized bias 1 → 8-bit 255: -255 + 255 = 0)
    REQUIRE(ow.r == Catch::Approx(0.0f).margin(0.5f));
    REQUIRE(ow.g == Catch::Approx(0.0f).margin(0.5f));
    REQUIRE(ow.b == Catch::Approx(0.0f).margin(0.5f));
    // The [0,1] bias is what real Skia uses; the 255x-scaled bias the production
    // code previously carried left white UNINVERTED (255) — see the raster test.
}

TEST_CASE("Filter chain: invert(0) is identity",
          "[canvas][filter-chain]") {
    float m[20];
    build_invert_matrix(0.0f, m);
    Px in{42, 137, 200, 255};
    Px out = apply_matrix(m, in);
    REQUIRE(out.r == Catch::Approx(42.0f));
    REQUIRE(out.g == Catch::Approx(137.0f));
    REQUIRE(out.b == Catch::Approx(200.0f));
    REQUIRE(out.a == Catch::Approx(255.0f));
}

TEST_CASE("Filter chain: opacity(a) scales alpha and preserves RGB",
          "[canvas][filter-chain]") {
    // Opacity is a color matrix in the chain (alpha *= a), not a
    // final-layer alpha multiplier. RGB channels pass through unchanged
    // so subsequent filters operate on the same color.
    float m[20];
    build_opacity_matrix(0.5f, m);
    Px in{200, 100, 50, 255};
    Px out = apply_matrix(m, in);
    REQUIRE(out.r == Catch::Approx(200.0f));
    REQUIRE(out.g == Catch::Approx(100.0f));
    REQUIRE(out.b == Catch::Approx(50.0f));
    REQUIRE(out.a == Catch::Approx(127.5f).margin(0.5f));

    // opacity(0) -> alpha 0.
    build_opacity_matrix(0.0f, m);
    Px out0 = apply_matrix(m, in);
    REQUIRE(out0.a == Catch::Approx(0.0f).margin(0.5f));

    // opacity(1) -> identity on alpha.
    build_opacity_matrix(1.0f, m);
    Px out1 = apply_matrix(m, in);
    REQUIRE(out1.a == Catch::Approx(255.0f).margin(0.5f));
}
