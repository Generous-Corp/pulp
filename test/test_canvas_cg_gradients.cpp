// Canvas — CoreGraphics gradient + pattern fixtures (pulp #1524 cluster).
//
// Extracted from test/test_canvas.cpp as part of P5-2 to keep individual
// fixture TUs under the maintainability threshold. The cluster is
// Apple-only because every test path drives CoreGraphics directly to
// prove the CoreGraphicsCanvas fallback honours the canvas2d gradient
// + pattern spec (no degradations).

#ifdef __APPLE__

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/cg_canvas.hpp>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <array>
#include <cstdio>
#include <unistd.h>
#include <vector>

using namespace pulp::canvas;

// ── pulp #1524 — CG-degraded gradient/pattern cluster ────────────────────────
//
// Promotes 3 DIVERGE entries → PASS on the CoreGraphics fallback path:
//   * canvas2d/createConicGradient     — software-rasterised CGImage sweep
//   * canvas2d/createRadialGradient    — full two-circle CGContextDrawRadialGradient
//   * canvas2d/createPattern           — CGPatternCreate + image-tile callback
//
// Strategy: build a CGBitmapContext, route a draw through CoreGraphicsCanvas,
// read the pixels back, and assert the output reflects the spec (not the
// pre-#1524 single-stop / outer-circle-only / solid-fallback degradations).

namespace {
struct CgPixelGrid {
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    // Sample a pixel as straight-RGBA in [0, 255]. Origin convention matches
    // the CoreGraphicsCanvas constructor: canvas y=0 is the *top* of the
    // bitmap. The bitmap memory is bottom-up, so flip y here.
    std::array<int, 4> at(int x, int y) const {
        const int by = (h - 1) - y;
        const size_t off = (static_cast<size_t>(by) * w + x) * 4u;
        return {pixels[off + 0], pixels[off + 1], pixels[off + 2], pixels[off + 3]};
    }
};

CGContextRef cg_make_bitmap(int w, int h, std::vector<uint8_t>& pixels) {
    pixels.assign(static_cast<size_t>(w) * h * 4u, 0u);
    auto cs = CGColorSpaceCreateDeviceRGB();
    if (!cs) return nullptr;
    const uint32_t bitmap_info =
        static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
        static_cast<uint32_t>(kCGBitmapByteOrder32Big);
    CGContextRef ctx = CGBitmapContextCreate(
        pixels.data(), w, h, 8, w * 4u, cs, bitmap_info);
    CGColorSpaceRelease(cs);
    return ctx;
}
} // namespace

// pulp #1524 — `createConicGradient` on CG must produce angle-varying colour
// instead of the pre-#1524 first-stop solid fallback. We fill a 64x64 square
// with a 4-stop conic spanning red / green / blue / red and sample the four
// cardinal directions from the centre. Each cardinal must hit a different
// dominant channel — proving the sweep actually rotated through the stops.
TEST_CASE("CoreGraphicsCanvas createConicGradient sweeps angle through stops",
          "[canvas][cg][issue-1524]") {
    constexpr int W = 64, H = 64;
    std::vector<uint8_t> pixels;
    CGContextRef ctx = cg_make_bitmap(W, H, pixels);
    REQUIRE(ctx != nullptr);
    {
        CoreGraphicsCanvas canvas(ctx, static_cast<float>(W),
                                  static_cast<float>(H));
        const Color stops[] = {
            Color::rgba(1.0f, 0.0f, 0.0f, 1.0f),  // 0     (right, +x)  red
            Color::rgba(0.0f, 1.0f, 0.0f, 1.0f),  // 0.25  (down,  +y)  green
            Color::rgba(0.0f, 0.0f, 1.0f, 1.0f),  // 0.5   (left,  -x)  blue
            Color::rgba(0.0f, 1.0f, 0.0f, 1.0f),  // 0.75  (up,    -y)  green
            Color::rgba(1.0f, 0.0f, 0.0f, 1.0f)   // 1.0   wrap to red
        };
        const float pos[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
        canvas.set_fill_gradient_conic(W * 0.5f, H * 0.5f, 0.0f, stops, pos, 5);
        canvas.fill_rect(0, 0, W, H);
    }
    CGContextRelease(ctx);

    CgPixelGrid grid{pixels, W, H};
    // East cardinal — far right, vertically centred — must be red-dominant.
    auto east  = grid.at(W - 4, H / 2);
    auto south = grid.at(W / 2, H - 4);   // canvas y down = +y → green stop
    auto west  = grid.at(3,     H / 2);
    INFO("east="  << east[0]  << "," << east[1]  << "," << east[2]);
    INFO("south=" << south[0] << "," << south[1] << "," << south[2]);
    INFO("west="  << west[0]  << "," << west[1]  << "," << west[2]);
    REQUIRE(east[0]  > east[1]);   REQUIRE(east[0]  > east[2]);   // red dominant
    REQUIRE(south[1] > south[0]);  REQUIRE(south[1] > south[2]);  // green dominant
    REQUIRE(west[2]  > west[0]);   REQUIRE(west[2]  > west[1]);   // blue dominant

    // Sanity: all sampled pixels must be opaque (alpha 255). Catches a
    // regression where the rasteriser writes 0-alpha and CG composites
    // nothing onto the destination.
    REQUIRE(east[3]  == 255);
    REQUIRE(south[3] == 255);
    REQUIRE(west[3]  == 255);
}

// pulp #1524 — `createRadialGradient` two-circle form must honour the inner
// circle. Pre-#1524 the JS shim dropped (x0,y0,r0) and forwarded only the
// outer circle, so a gradient with an *offset* inner circle painted as if
// inner_centre==outer_centre — visually the same as a single-circle radial.
//
// Strategy: build a 64x64 with a two-circle radial whose inner circle sits
// well to the right of the outer centre and whose stops are red→blue.
// With both circles wired, the red-dominant region must shift toward the
// inner-circle centre. With the bug, red lands at the outer centre instead.
TEST_CASE("CoreGraphicsCanvas radial two-circle form honours inner circle",
          "[canvas][cg][issue-1524]") {
    constexpr int W = 64, H = 64;
    std::vector<uint8_t> pixels;
    CGContextRef ctx = cg_make_bitmap(W, H, pixels);
    REQUIRE(ctx != nullptr);
    {
        CoreGraphicsCanvas canvas(ctx, static_cast<float>(W),
                                  static_cast<float>(H));
        const Color stops[] = {
            Color::rgba(1.0f, 0.0f, 0.0f, 1.0f),  // red at inner circle (t=0)
            Color::rgba(0.0f, 0.0f, 1.0f, 1.0f)   // blue at outer circle (t=1)
        };
        const float pos[] = {0.0f, 1.0f};
        // Inner circle: centre (50, 32), radius 0 (point) — far to the right.
        // Outer circle: centre (32, 32), radius 30 — covers most of the canvas.
        // Spec semantics: red is concentrated near (50, 32); blue is at the
        // outer circle's ring. Pre-fix: shim collapsed to (32, 32) and red
        // would land at the centre instead.
        canvas.set_fill_gradient_radial_two_circles(
            50.0f, 32.0f, 0.0f,
            32.0f, 32.0f, 30.0f,
            stops, pos, 2);
        canvas.fill_rect(0, 0, W, H);
    }
    CGContextRelease(ctx);

    CgPixelGrid grid{pixels, W, H};
    // Sample near the inner-circle centre (50, 32) — must be red-dominant.
    auto near_inner = grid.at(50, 32);
    // Sample near the outer-circle centre (32, 32) — must NOT be red-dominant
    // (this is what would fail pre-fix where the inner collapsed onto outer).
    auto at_outer_centre = grid.at(32, 32);
    INFO("near_inner=" << near_inner[0] << "," << near_inner[1] << "," << near_inner[2]);
    INFO("at_outer_centre=" << at_outer_centre[0] << "," << at_outer_centre[1] << "," << at_outer_centre[2]);
    REQUIRE(near_inner[0] > near_inner[2]);             // red > blue near inner
    REQUIRE(at_outer_centre[0] < near_inner[0]);        // outer-centre is less red than inner
    // Defensive: gradient must have actually painted (alpha 255 inside the
    // outer circle).
    REQUIRE(near_inner[3] == 255);
}

// pulp #1524 — `createPattern` on CG must install a real CGPattern instead
// of falling back to the active solid colour. A 1x2 image (red top half,
// blue bottom half) tiled with `repeat` should produce alternating red/blue
// horizontal bands when filled across a tall rectangle. Pre-#1524 the
// canvas painted a single solid colour.
TEST_CASE("CoreGraphicsCanvas set_fill_pattern installs a real CGPattern",
          "[canvas][cg][issue-1524]") {
    constexpr int W = 32, H = 32;

    // Step 1 — write a tiny PNG-shaped tile to a temp file. Using a 4x4
    // bitmap (red top half, blue bottom half) keeps the disk decode path
    // exercised end-to-end (ImageIO → CGImageRef → CGPattern callback).
    // Use only CoreFoundation + ImageIO (no Cocoa) so this stays C++ and
    // the test file doesn't need to be compiled as Objective-C++.
    char tmp_template[] = "/tmp/pulp_1524_patternXXXXXX.png";
    int fd = mkstemps(tmp_template, 4);
    REQUIRE(fd >= 0);
    close(fd);
    std::string tile_path_str = tmp_template;
    {
        const int TW = 4, TH = 4;
        std::vector<uint8_t> tile(static_cast<size_t>(TW) * TH * 4u, 0u);
        for (int y = 0; y < TH; ++y) {
            for (int x = 0; x < TW; ++x) {
                const size_t off = (static_cast<size_t>(y) * TW + x) * 4u;
                if (y < TH / 2) {
                    tile[off + 0] = 255; tile[off + 1] = 0;   tile[off + 2] = 0;
                } else {
                    tile[off + 0] = 0;   tile[off + 1] = 0;   tile[off + 2] = 255;
                }
                tile[off + 3] = 255;
            }
        }
        auto cs = CGColorSpaceCreateDeviceRGB();
        REQUIRE(cs != nullptr);
        CGContextRef bmp = CGBitmapContextCreate(
            tile.data(), TW, TH, 8, TW * 4u, cs,
            static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
            static_cast<uint32_t>(kCGBitmapByteOrder32Big));
        CGColorSpaceRelease(cs);
        REQUIRE(bmp != nullptr);
        CGImageRef img = CGBitmapContextCreateImage(bmp);
        CGContextRelease(bmp);
        REQUIRE(img != nullptr);
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(
            nullptr,
            reinterpret_cast<const UInt8*>(tile_path_str.c_str()),
            static_cast<CFIndex>(tile_path_str.size()),
            false);
        REQUIRE(url != nullptr);
        CFStringRef png_uti = CFSTR("public.png");
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL(
            url, png_uti, 1, nullptr);
        CFRelease(url);
        REQUIRE(dst != nullptr);
        CGImageDestinationAddImage(dst, img, nullptr);
        REQUIRE(CGImageDestinationFinalize(dst));
        CFRelease(dst);
        CGImageRelease(img);
    }

    // Step 2 — fill a 32x32 destination canvas with the pattern (`repeat`
    // both axes) and verify the image content shows BOTH red and blue
    // bands. Pre-fix: only the active solid `set_fill_color` painted, so
    // exactly one colour would appear.
    std::vector<uint8_t> pixels;
    CGContextRef ctx = cg_make_bitmap(W, H, pixels);
    REQUIRE(ctx != nullptr);
    {
        CoreGraphicsCanvas canvas(ctx, static_cast<float>(W),
                                  static_cast<float>(H));
        canvas.set_fill_color(Color::rgba(0.0f, 1.0f, 0.0f, 1.0f));  // green sentinel
        canvas.set_fill_pattern(tile_path_str,
                                Canvas::PatternTileMode::repeat,
                                Canvas::PatternTileMode::repeat);
        canvas.fill_rect(0, 0, W, H);
    }
    CGContextRelease(ctx);
    std::remove(tile_path_str.c_str());

    // Verify the pattern actually rendered both colours from the tile —
    // not the green sentinel (which would mean the pattern fell back to
    // solid fill_color_) and not a single colour from one half (which
    // would mean the tile didn't repeat correctly across the canvas).
    CgPixelGrid grid{pixels, W, H};
    int red_count = 0, blue_count = 0, green_count = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            auto p = grid.at(x, y);
            if (p[0] > 200 && p[1] < 64  && p[2] < 64) ++red_count;
            if (p[2] > 200 && p[0] < 64  && p[1] < 64) ++blue_count;
            if (p[1] > 200 && p[0] < 64  && p[2] < 64) ++green_count;
        }
    }
    INFO("red=" << red_count << " blue=" << blue_count << " green=" << green_count);
    // Pattern fired: both red and blue must be present from the tile.
    REQUIRE(red_count > 16);
    REQUIRE(blue_count > 16);
    // Sentinel green must NOT appear (its colour would survive only if the
    // pattern silently degraded to set_fill_color, which is the pre-fix bug).
    REQUIRE(green_count == 0);
}

// pulp #1524 — `set_fill_pattern("")` clears any active pattern back to the
// solid fill colour. Mirrors clear_fill_gradient's reset semantics so a JS
// fillStyle string assignment after a pattern fillStyle reverts cleanly.
TEST_CASE("CoreGraphicsCanvas set_fill_pattern clears on empty src",
          "[canvas][cg][issue-1524]") {
    constexpr int W = 16, H = 16;
    std::vector<uint8_t> pixels;
    CGContextRef ctx = cg_make_bitmap(W, H, pixels);
    REQUIRE(ctx != nullptr);
    {
        CoreGraphicsCanvas canvas(ctx, static_cast<float>(W),
                                  static_cast<float>(H));
        canvas.set_fill_color(Color::rgba(0.0f, 1.0f, 0.0f, 1.0f));
        canvas.set_fill_pattern("",
                                Canvas::PatternTileMode::repeat,
                                Canvas::PatternTileMode::repeat);
        canvas.fill_rect(0, 0, W, H);
    }
    CGContextRelease(ctx);
    // The first three bytes of any pixel inside the canvas should be the
    // green solid fill we set before the empty-src pattern call cleared.
    REQUIRE(pixels.size() >= 4u);
    REQUIRE(pixels[0] == 0);    // R
    REQUIRE(pixels[1] == 255);  // G
    REQUIRE(pixels[2] == 0);    // B
}

// pulp #1666 — CoreGraphicsCanvas::fill_text must honour an active fill
// gradient. Pre-fix, fill_text used kCGTextFill with apply_fill_color()
// which routed solid `fill_color_` into the glyphs and silently dropped
// the gradient set via set_fill_gradient_linear. Post-fix, fill_text
// switches to kCGTextClip on the glyph fills then paints the active
// gradient inside the clip. Test renders glyphs with a horizontal
// red→blue linear gradient and probes left vs right pixels to confirm
// the colour ramp lands on glyph pixels (not solid stroke colour).
TEST_CASE("CoreGraphicsCanvas::fill_text honours active fill gradient",
          "[canvas][cg][issue-1666]") {
    constexpr int W = 128;
    constexpr int H = 32;
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 4u, 0u);
    auto cs = CGColorSpaceCreateDeviceRGB();
    REQUIRE(cs != nullptr);
    const uint32_t bitmap_info =
        static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
        static_cast<uint32_t>(kCGBitmapByteOrder32Big);
    CGContextRef ctx = CGBitmapContextCreate(
        pixels.data(), W, H, 8, W * 4u, cs, bitmap_info);
    CGColorSpaceRelease(cs);
    REQUIRE(ctx != nullptr);

    {
        CoreGraphicsCanvas canvas(ctx, static_cast<float>(W),
                                  static_cast<float>(H));
        // Horizontal gradient: red at x=0, blue at x=W.
        Color stops[2] = {
            Color::rgba(1.0f, 0.0f, 0.0f, 1.0f),
            Color::rgba(0.0f, 0.0f, 1.0f, 1.0f),
        };
        float positions[2] = {0.0f, 1.0f};
        canvas.set_fill_gradient_linear(0.0f, 0.0f,
                                        static_cast<float>(W), 0.0f,
                                        stops, positions, 2);
        canvas.set_font("Helvetica", 28);
        canvas.fill_text("WWWWWWWW", 0, 26);
    }
    CGContextRelease(ctx);

    auto sample = [&](int x, int y, int chan) -> int {
        return pixels[(static_cast<size_t>(y) * W + x) * 4u + chan];
    };

    // Find a glyph pixel near the left edge (high red, low blue) and a
    // glyph pixel near the right edge (low red, high blue). Scan the
    // baseline-ish row range [10, 25] across the column.
    auto any_glyph_with_dominant_channel = [&](int x_lo, int x_hi,
                                                int dominant, int other) {
        for (int x = x_lo; x < x_hi; ++x) {
            for (int y = 6; y < 28; ++y) {
                int d = sample(x, y, dominant);
                int o = sample(x, y, other);
                int a = sample(x, y, 3);
                if (a > 32 && d > 100 && d > o + 30) return true;
            }
        }
        return false;
    };
    INFO("Left edge of gradient text should have red-dominant glyph pixels.");
    REQUIRE(any_glyph_with_dominant_channel(0, 16, /*r*/0, /*b*/2));
    INFO("Right edge of gradient text should have blue-dominant glyph pixels.");
    REQUIRE(any_glyph_with_dominant_channel(W - 16, W, /*b*/2, /*r*/0));
}

// pulp #1666 — CoreGraphicsCanvas stroke ops must honour an active
// stroke gradient set via set_stroke_gradient_linear. Pre-fix, every
// stroke_* method called apply_stroke_color() which routed solid
// `stroke_color_` and silently dropped the gradient. Post-fix, stroke
// methods short-circuit to stroke_with_active_paint() which clips to
// the stroked outline and draws the gradient. Probe a wide stroke_rect
// rendered with a horizontal red→blue gradient: the left vertical edge
// must be red-dominant; the right vertical edge must be blue-dominant.
TEST_CASE("CoreGraphicsCanvas::stroke_rect honours active stroke gradient",
          "[canvas][cg][issue-1666]") {
    constexpr int W = 128;
    constexpr int H = 64;
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 4u, 0u);
    auto cs = CGColorSpaceCreateDeviceRGB();
    REQUIRE(cs != nullptr);
    const uint32_t bitmap_info =
        static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
        static_cast<uint32_t>(kCGBitmapByteOrder32Big);
    CGContextRef ctx = CGBitmapContextCreate(
        pixels.data(), W, H, 8, W * 4u, cs, bitmap_info);
    CGColorSpaceRelease(cs);
    REQUIRE(ctx != nullptr);

    {
        CoreGraphicsCanvas canvas(ctx, static_cast<float>(W),
                                  static_cast<float>(H));
        Color stops[2] = {
            Color::rgba(1.0f, 0.0f, 0.0f, 1.0f),
            Color::rgba(0.0f, 0.0f, 1.0f, 1.0f),
        };
        float positions[2] = {0.0f, 1.0f};
        canvas.set_stroke_gradient_linear(0.0f, 0.0f,
                                          static_cast<float>(W), 0.0f,
                                          stops, positions, 2);
        canvas.set_line_width(6.0f);
        canvas.stroke_rect(8, 8, W - 16, H - 16);
    }
    CGContextRelease(ctx);

    auto sample = [&](int x, int y, int chan) -> int {
        return pixels[(static_cast<size_t>(y) * W + x) * 4u + chan];
    };

    auto any_stroked_with_channel = [&](int x_lo, int x_hi,
                                         int dominant, int other) {
        for (int x = x_lo; x < x_hi; ++x) {
            for (int y = 8; y < H - 8; ++y) {
                int d = sample(x, y, dominant);
                int o = sample(x, y, other);
                int a = sample(x, y, 3);
                if (a > 32 && d > 100 && d > o + 30) return true;
            }
        }
        return false;
    };
    INFO("Left edge of gradient-stroked rect should be red-dominant.");
    REQUIRE(any_stroked_with_channel(4, 16, /*r*/0, /*b*/2));
    INFO("Right edge of gradient-stroked rect should be blue-dominant.");
    REQUIRE(any_stroked_with_channel(W - 16, W - 4, /*b*/2, /*r*/0));
}

// pulp #1747 (P1 Codex review on #1736) — stroke_text gradient endpoints
// must be evaluated in canvas space, NOT in text-local space. Pre-fix
// stroke_text mutated the CTM (translate(draw_x, y) + scale(1, -1)) and
// then called stroke_with_active_paint(), so the gradient was sampled
// in text-local coords: same createLinearGradient stops produced
// different colors as text x-position changed, and vertical gradients
// rendered upside-down. Regression test: render the same horizontal
// red→blue gradient over identical glyphs at two different text
// positions; assert the stroked-glyph pixels at canvas-x=center are
// the same purple (gradient midpoint) in both. Pre-fix the colors
// would diverge because the gradient shifted with the text.
TEST_CASE("CoreGraphicsCanvas::stroke_text gradient is canvas-space anchored",
          "[canvas][cg][issue-1747][issue-1666]") {
    auto render_at = [](float text_x, std::vector<uint8_t>& pixels,
                        int W, int H) {
        pixels.assign(static_cast<size_t>(W) * H * 4u, 0u);
        auto cs = CGColorSpaceCreateDeviceRGB();
        REQUIRE(cs != nullptr);
        const uint32_t bitmap_info =
            static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
            static_cast<uint32_t>(kCGBitmapByteOrder32Big);
        CGContextRef ctx = CGBitmapContextCreate(
            pixels.data(), W, H, 8, W * 4u, cs, bitmap_info);
        CGColorSpaceRelease(cs);
        REQUIRE(ctx != nullptr);
        {
            CoreGraphicsCanvas canvas(ctx, static_cast<float>(W),
                                      static_cast<float>(H));
            // Horizontal red→blue gradient spanning the full canvas width.
            // Endpoints are in CANVAS coordinates regardless of where
            // the text is positioned.
            Color stops[2] = {
                Color::rgba(1.0f, 0.0f, 0.0f, 1.0f),
                Color::rgba(0.0f, 0.0f, 1.0f, 1.0f),
            };
            float positions[2] = {0.0f, 1.0f};
            canvas.set_stroke_gradient_linear(0.0f, 0.0f,
                                              static_cast<float>(W), 0.0f,
                                              stops, positions, 2);
            canvas.set_line_width(2.5f);
            canvas.set_font("Helvetica", 32);
            // Long string of W glyphs covers most of the canvas width
            // so we always have stroked pixels to sample at the centre.
            canvas.stroke_text("WWWWWWWW", text_x, 36);
        }
        CGContextRelease(ctx);
    };

    constexpr int W = 256;
    constexpr int H = 64;

    std::vector<uint8_t> pixels_a, pixels_b;
    render_at(0.0f,   pixels_a, W, H);
    render_at(40.0f,  pixels_b, W, H);

    auto sample = [&](const std::vector<uint8_t>& p, int x, int y, int chan) {
        return p[(static_cast<size_t>(y) * W + x) * 4u + chan];
    };

    // For each render, scan a vertical strip near canvas-x=W/2 (the
    // midpoint of the gradient) for the strongest stroked pixel and
    // assert it is purple-ish — neither red-dominant nor blue-dominant.
    // If the gradient is canvas-space anchored, BOTH renders should
    // hit purple at canvas-x=W/2. Pre-fix render_b would shift the
    // gradient with the text and the sample at x=W/2 would be either
    // pure red or pure blue (depending on which glyph landed there).
    auto best_stroked_pixel = [&](const std::vector<uint8_t>& p) {
        int best_alpha = 0; int best_x = 0; int best_y = 0;
        for (int x = W/2 - 8; x < W/2 + 8; ++x) {
            for (int y = 0; y < H; ++y) {
                int a = sample(p, x, y, 3);
                if (a > best_alpha) { best_alpha = a; best_x = x; best_y = y; }
            }
        }
        return std::tuple<int,int,int>{best_x, best_y, best_alpha};
    };

    auto [ax, ay, aa] = best_stroked_pixel(pixels_a);
    auto [bx, by, ba] = best_stroked_pixel(pixels_b);
    INFO("render_a strongest stroked pixel near x=W/2: (" << ax << "," << ay
         << ") alpha=" << aa);
    INFO("render_b strongest stroked pixel near x=W/2: (" << bx << "," << by
         << ") alpha=" << ba);
    REQUIRE(aa > 32);
    REQUIRE(ba > 32);

    int ar = sample(pixels_a, ax, ay, 0), ab = sample(pixels_a, ax, ay, 2);
    int br = sample(pixels_b, bx, by, 0), bb = sample(pixels_b, bx, by, 2);
    INFO("render_a (text_x=0) midpoint sample R=" << ar << " B=" << ab);
    INFO("render_b (text_x=40) midpoint sample R=" << br << " B=" << bb);
    // Pre-fix at least one of these would be heavily skewed (e.g.
    // R>200 with B<50 if the gradient shifted). With the fix both
    // should be in the purple band: red+blue both substantial,
    // neither dominating by more than ~80 (out of 255). Use a loose
    // tolerance to absorb anti-aliasing + glyph-shape noise.
    REQUIRE(std::abs(ar - br) < 80);
    REQUIRE(std::abs(ab - bb) < 80);
    // Both samples must be PURPLE, not pure-red and not pure-blue.
    // (Either channel below 30 would mean the gradient missed.)
    REQUIRE(ar > 30); REQUIRE(ab > 30);
    REQUIRE(br > 30); REQUIRE(bb > 30);
}

// pulp #1898 — CoreGraphicsCanvas::set_line_dash must produce visible gaps.
//
// Background. The base Canvas virtual is a no-op `(void)`, so before this
// override Spectr's 0 dB rail (and ~5 other dashed-stroke callsites) drew
// solid on the macOS CG CPU paint path. SkiaCanvas applies the dash via
// SkDashPathEffect on the per-call SkPaint; CG applies via CGContextSetLineDash
// on the GState. The test renders a horizontal line at y=4 with a coarse
// [4, 4] dash and asserts the resulting bitmap has alternating filled +
// empty samples — i.e. real gaps, not a solid stroke.
TEST_CASE("CoreGraphicsCanvas::set_line_dash produces gaps in stroke",
          "[canvas][cg][issue-1898]") {
    constexpr int W = 32;
    constexpr int H = 8;
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 4u, 0u);
    auto cs = CGColorSpaceCreateDeviceRGB();
    REQUIRE(cs != nullptr);
    const uint32_t bitmap_info =
        static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
        static_cast<uint32_t>(kCGBitmapByteOrder32Big);
    CGContextRef ctx = CGBitmapContextCreate(
        pixels.data(), W, H, 8, W * 4u, cs, bitmap_info);
    CGColorSpaceRelease(cs);
    REQUIRE(ctx != nullptr);

    // Disable antialiasing so the dash-gap boundaries are pixel-accurate.
    CGContextSetShouldAntialias(ctx, false);
    CGContextSetAllowsAntialiasing(ctx, false);

    {
        CoreGraphicsCanvas canvas(ctx, static_cast<float>(W),
                                  static_cast<float>(H));
        canvas.set_stroke_color(Color::rgba(1.0f, 0.0f, 0.0f, 1.0f));
        canvas.set_line_width(1.0f);
        // Pattern: 4 px ON, 4 px OFF, phase 0. Span 32 px ⇒ 4 dashes.
        const float intervals[2] = {4.0f, 4.0f};
        canvas.set_line_dash(intervals, 2, 0.0f);
        // Use stroke_line to exercise the GState dash path (mirrors the
        // Spectr 0 dB rail call site, which uses ctx.stroke() over a
        // beginPath+moveTo+lineTo built path).
        canvas.stroke_line(0.0f, 4.0f, static_cast<float>(W), 4.0f);
    }
    CGContextRelease(ctx);

    // Sample pixel row y=3 (the line is drawn at y=4 but butt cap + line
    // width 1 with AA off can paint either y=3 or y=4 depending on rounding
    // — we union-sample both rows). A red pixel has R>200 + G<60 + B<60.
    auto sample_red = [&](int x, int y) -> bool {
        const size_t i = (static_cast<size_t>(y) * W + x) * 4u;
        return pixels[i] > 200 && pixels[i + 1] < 60 && pixels[i + 2] < 60;
    };
    auto union_red = [&](int x) -> bool {
        return sample_red(x, 3) || sample_red(x, 4);
    };

    int filled = 0;
    int empty = 0;
    for (int x = 0; x < W; ++x) {
        if (union_red(x)) ++filled;
        else ++empty;
    }
    INFO("dashed line filled=" << filled << " empty=" << empty << " of W=" << W);

    // A [4,4] dash over a 32 px span lands on roughly 16 filled + 16 empty
    // pixels. Accept any split with at least 6 filled + at least 6 empty
    // — the requirement is just "alternating", not "exactly half".
    // The pre-fix bug rendered SOLID, so empty would be ~0; the assertion
    // catches that regression specifically.
    REQUIRE(filled >= 6);
    REQUIRE(empty >= 6);

    // Sanity: assert the FIRST 4 pixels are filled and pixels 4..7 are
    // empty (the on/off boundary). This is the strong test — pre-fix every
    // x in [0..W) would be filled.
    int first_segment_filled = 0;
    int first_gap_empty = 0;
    for (int x = 0; x < 4; ++x)  if (union_red(x))  ++first_segment_filled;
    for (int x = 4; x < 8; ++x)  if (!union_red(x)) ++first_gap_empty;
    INFO("first 4 px filled=" << first_segment_filled
         << " gap [4..8) empty=" << first_gap_empty);
    REQUIRE(first_segment_filled >= 3);
    REQUIRE(first_gap_empty >= 3);
}

// pulp #1898 — clearing the dash (empty intervals) must restore solid strokes
// on subsequent draws. Mirrors the JS bridge contract: ctx.setLineDash([])
// reverts to solid.
TEST_CASE("CoreGraphicsCanvas::set_line_dash clears back to solid",
          "[canvas][cg][issue-1898]") {
    constexpr int W = 32;
    constexpr int H = 8;
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 4u, 0u);
    auto cs = CGColorSpaceCreateDeviceRGB();
    REQUIRE(cs != nullptr);
    const uint32_t bitmap_info =
        static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
        static_cast<uint32_t>(kCGBitmapByteOrder32Big);
    CGContextRef ctx = CGBitmapContextCreate(
        pixels.data(), W, H, 8, W * 4u, cs, bitmap_info);
    CGColorSpaceRelease(cs);
    REQUIRE(ctx != nullptr);
    CGContextSetShouldAntialias(ctx, false);
    CGContextSetAllowsAntialiasing(ctx, false);

    {
        CoreGraphicsCanvas canvas(ctx, static_cast<float>(W),
                                  static_cast<float>(H));
        canvas.set_stroke_color(Color::rgba(1.0f, 0.0f, 0.0f, 1.0f));
        canvas.set_line_width(1.0f);
        const float intervals[2] = {4.0f, 4.0f};
        canvas.set_line_dash(intervals, 2, 0.0f);
        // Re-clear: empty pattern reverts to solid.
        canvas.set_line_dash(nullptr, 0, 0.0f);
        canvas.stroke_line(0.0f, 4.0f, static_cast<float>(W), 4.0f);
    }
    CGContextRelease(ctx);

    auto sample_red = [&](int x, int y) -> bool {
        const size_t i = (static_cast<size_t>(y) * W + x) * 4u;
        return pixels[i] > 200 && pixels[i + 1] < 60 && pixels[i + 2] < 60;
    };
    auto union_red = [&](int x) -> bool {
        return sample_red(x, 3) || sample_red(x, 4);
    };

    int filled = 0;
    for (int x = 0; x < W; ++x)
        if (union_red(x)) ++filled;
    INFO("solid line filled=" << filled << " of W=" << W);
    // After clearing the dash, every pixel in the span must be filled.
    REQUIRE(filled >= W - 2);  // allow 2 px slack for end caps
}


#endif  // __APPLE__
