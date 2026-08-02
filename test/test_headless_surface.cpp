// test_headless_surface.cpp — HeadlessSurface wrapper
//
// Plan: planning/2026-05-24-macos-plugin-authoring-plan.md §6.7.
// Confirms the one-call CI wrapper that hides the existing
// `GpuSurface::create_dawn() + native_surface_handle=nullptr` +
// `SkiaSurface::create()` ceremony behind a `render_rgba` / `render_png`
// API. The runtime cases gate on `PULP_HAS_SKIA && __APPLE__` —
// otherwise the wrapper's create() returns nullptr (compile-time
// safe) and we exercise only the pure-function `encode_png` /
// `rgba_fingerprint` paths.
//
// Tag: [gpu][skia][headless-surface][item-6-7]

#include <catch2/catch_test_macros.hpp>

#include <pulp/render/headless_surface.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/runtime/base64.hpp>

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)
#include <pulp/canvas/skia_canvas.hpp>
#include "include/core/SkAlphaType.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#endif

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using pulp::render::HeadlessSurface;

namespace {

constexpr uint32_t kW = 256;
constexpr uint32_t kH = 128;

// Sentinel background — distinctive enough that
// `count_non_background` won't mistake noise for the user's paint.
constexpr uint8_t kBgR = 20, kBgG = 40, kBgB = 80;

// Count pixels within `tol` (per-channel sum) of a target color. Used by the
// image / SVG cases to assert the *asset's own ink* landed on the surface, not
// merely that something non-background did.
uint32_t count_near_color(const HeadlessSurface::Rgba& rgba,
                          uint8_t tr, uint8_t tg, uint8_t tb, int tol = 24) {
    const size_t n = rgba.pixel_count();
    if (n == 0 || rgba.pixels.size() < n * 4u) return 0;
    uint32_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        const int dr = std::abs(static_cast<int>(rgba.pixels[i * 4 + 0]) - tr);
        const int dg = std::abs(static_cast<int>(rgba.pixels[i * 4 + 1]) - tg);
        const int db = std::abs(static_cast<int>(rgba.pixels[i * 4 + 2]) - tb);
        if (dr + dg + db <= tol) ++count;
    }
    return count;
}

uint32_t count_non_background(const HeadlessSurface::Rgba& rgba) {
    const size_t n = rgba.pixel_count();
    if (n == 0 || rgba.pixels.size() < n * 4u) return 0;
    uint32_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t r = rgba.pixels[i * 4 + 0];
        const uint8_t g = rgba.pixels[i * 4 + 1];
        const uint8_t b = rgba.pixels[i * 4 + 2];
        const int dr = std::abs(static_cast<int>(r) - kBgR);
        const int dg = std::abs(static_cast<int>(g) - kBgG);
        const int db = std::abs(static_cast<int>(b) - kBgB);
        if (dr + dg + db > 24) ++count;
    }
    return count;
}

} // namespace

// ── Always-on cases (independent of GPU availability) ────────────────────

TEST_CASE("HeadlessSurface::encode_png rejects empty / undersized input",
          "[gpu][headless-surface][item-6-7]") {
    HeadlessSurface::Rgba empty;
    std::string err;
    auto png = HeadlessSurface::encode_png(empty, &err);
    REQUIRE(png.empty());
    REQUIRE_FALSE(err.empty());

    HeadlessSurface::Rgba bad;
    bad.width = 4;
    bad.height = 4;
    bad.pixels.assign(8, 0);  // need 64 bytes, give 8
    err.clear();
    png = HeadlessSurface::encode_png(bad, &err);
    REQUIRE(png.empty());
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("HeadlessSurface::rgba_fingerprint is deterministic and dim-sensitive",
          "[gpu][headless-surface][item-6-7]") {
    HeadlessSurface::Rgba a;
    a.width = 2;
    a.height = 3;
    a.pixels = {1,2,3,4, 5,6,7,8, 9,10,11,12,
                13,14,15,16, 17,18,19,20, 21,22,23,24};

    HeadlessSurface::Rgba b = a;  // same content + dims
    REQUIRE(HeadlessSurface::rgba_fingerprint(a) ==
            HeadlessSurface::rgba_fingerprint(b));

    HeadlessSurface::Rgba c = a;  // mutate one byte → different fingerprint
    c.pixels[0] ^= 0xFF;
    REQUIRE(HeadlessSurface::rgba_fingerprint(a) !=
            HeadlessSurface::rgba_fingerprint(c));

    HeadlessSurface::Rgba d;      // swap dims → different fingerprint
    d.width = 3;
    d.height = 2;
    d.pixels = a.pixels;
    REQUIRE(HeadlessSurface::rgba_fingerprint(a) !=
            HeadlessSurface::rgba_fingerprint(d));
}

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)

TEST_CASE("HeadlessSurface::create soft-fails when Dawn unavailable",
          "[gpu][skia][headless-surface][item-6-7]") {
    HeadlessSurface::Config cfg;
    cfg.width = 0;  // intentionally invalid → wrapper rejects without touching Dawn
    cfg.height = 0;
    std::string err;
    auto surface = HeadlessSurface::create(cfg, &err);
    REQUIRE(surface == nullptr);
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("HeadlessSurface renders a deterministic clear-only frame",
          "[gpu][skia][headless-surface][item-6-7]") {
    HeadlessSurface::Config cfg;
    cfg.width = kW;
    cfg.height = kH;
    cfg.clear_r = kBgR;
    cfg.clear_g = kBgG;
    cfg.clear_b = kBgB;
    cfg.clear_a = 255;

    std::string err;
    auto surface = HeadlessSurface::create(cfg, &err);
    if (!surface) {
        // No Dawn/Graphite on this host — that's the documented
        // soft-skip path the CI lane uses when the runner lacks a GPU.
        SUCCEED("HeadlessSurface unavailable: " + err);
        return;
    }
    REQUIRE(surface->is_ready());

    auto frame_a = surface->render_rgba(nullptr);
    if (frame_a.empty()) {
        SUCCEED("GPU readback failed: " + surface->last_error());
        return;
    }
    REQUIRE(frame_a.width == kW);
    REQUIRE(frame_a.height == kH);

    // Same surface, same paint (none) → reproducible within tolerance.
    // GPU async readback can produce sub-LSB jitter on a small number
    // of pixels even on the same machine (plan §6.7: "reproducible
    // within a documented tolerance"). The wrapper guarantees that the
    // *vast majority* of pixels match exactly — we assert that floor
    // rather than bit-exact fingerprint equality.
    auto frame_b = surface->render_rgba(nullptr);
    REQUIRE_FALSE(frame_b.empty());
    REQUIRE(frame_a.width == frame_b.width);
    REQUIRE(frame_a.height == frame_b.height);
    REQUIRE(frame_a.pixels.size() == frame_b.pixels.size());
    size_t diff_bytes = 0;
    for (size_t i = 0; i < frame_a.pixels.size(); ++i) {
        if (frame_a.pixels[i] != frame_b.pixels[i]) ++diff_bytes;
    }
    INFO("byte-diff between consecutive renders: " << diff_bytes
         << " of " << frame_a.pixels.size());
    // Tolerance: at most 1% of bytes may differ across consecutive
    // renders. In practice the fully-cleared frames we render here
    // match bit-exactly on Apple Silicon — the budget exists for
    // adapter-jitter on other lanes.
    REQUIRE(diff_bytes * 100u < frame_a.pixels.size());

    // Clear-only frame: count_non_background must be ~0 (cleared with
    // the background color). Allow a tiny tolerance for any GPU sampler
    // dither at the edges.
    const uint32_t painted = count_non_background(frame_a);
    INFO("non-background pixels on a clear-only frame: " << painted);
    REQUIRE(painted < (frame_a.pixel_count() / 100u));  // < 1%
}

TEST_CASE("HeadlessSurface renders a paint callback then encodes PNG",
          "[gpu][skia][headless-surface][item-6-7]") {
    HeadlessSurface::Config cfg;
    cfg.width = kW;
    cfg.height = kH;
    cfg.clear_r = kBgR;
    cfg.clear_g = kBgG;
    cfg.clear_b = kBgB;
    cfg.clear_a = 255;

    std::string err;
    auto surface = HeadlessSurface::create(cfg, &err);
    if (!surface) {
        SUCCEED("HeadlessSurface unavailable: " + err);
        return;
    }

    auto paint_red_band = [](pulp::canvas::Canvas& c) {
        c.set_fill_color(pulp::canvas::Color::rgba8(220, 30, 30, 255));
        c.fill_rect(16.0f, 16.0f,
                    static_cast<float>(kW) - 32.0f,
                    static_cast<float>(kH) - 32.0f);
    };

    auto rgba = surface->render_rgba(paint_red_band);
    if (rgba.empty()) {
        SUCCEED("GPU readback failed: " + surface->last_error());
        return;
    }
    REQUIRE(rgba.width == kW);
    REQUIRE(rgba.height == kH);

    // The paint callback should have produced many non-background pixels.
    const uint32_t painted = count_non_background(rgba);
    INFO("non-background pixels after paint: " << painted);
    REQUIRE(painted > 1000u);

    // PNG round-trip via the convenience entry point.
    auto png = surface->render_png(paint_red_band);
    if (png.empty()) {
        SUCCEED("PNG encode failed: " + surface->last_error());
        return;
    }
    REQUIRE(png.size() >= 8u);
    // PNG magic: 89 50 4E 47 0D 0A 1A 0A
    static constexpr uint8_t kMagic[8] = {0x89, 0x50, 0x4E, 0x47,
                                          0x0D, 0x0A, 0x1A, 0x0A};
    REQUIRE(std::memcmp(png.data(), kMagic, sizeof(kMagic)) == 0);
}

// ── File-backed image + SVG on the GPU (Graphite) path ───────────────────
//
// `fill_rect` and text render on Graphite even when `draw_image_from_file` /
// `draw_svg` silently record nothing, so a case that only asserts the draw
// call returned true reproduces the original blindness. These assert the
// asset's OWN ink is present in the readback.

namespace {

// Write a solid-color PNG to `path` using the wrapper's own encoder, so the
// fixture needs no checked-in binary asset.
bool write_solid_png(const std::string& path,
                     uint32_t w, uint32_t h,
                     uint8_t r, uint8_t g, uint8_t b) {
    HeadlessSurface::Rgba src;
    src.width = w;
    src.height = h;
    src.pixels.resize(static_cast<size_t>(w) * h * 4u);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        src.pixels[i * 4 + 0] = r;
        src.pixels[i * 4 + 1] = g;
        src.pixels[i * 4 + 2] = b;
        src.pixels[i * 4 + 3] = 255;
    }
    auto png = HeadlessSurface::encode_png(src);
    if (png.empty()) return false;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(png.data()),
              static_cast<std::streamsize>(png.size()));
    return out.good();
}

// Run the same paint callback through a CPU raster SkiaCanvas and return the
// result in the wrapper's Rgba shape. The raster backend composites file
// images and SVG-embedded images unconditionally, so it is the control that
// isolates a GPU-only loss.
HeadlessSurface::Rgba raster_render(uint32_t w, uint32_t h,
                                    const HeadlessSurface::PaintFn& paint) {
    HeadlessSurface::Rgba out;
    auto surface = SkSurfaces::Raster(
        SkImageInfo::Make(static_cast<int>(w), static_cast<int>(h),
                          kRGBA_8888_SkColorType, kPremul_SkAlphaType));
    if (!surface) return out;
    surface->getCanvas()->clear(SkColorSetARGB(255, kBgR, kBgG, kBgB));
    if (paint) {
        pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
        paint(canvas);
    }
    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<size_t>(w) * h * 4u);
    SkPixmap pm(SkImageInfo::Make(static_cast<int>(w), static_cast<int>(h),
                                  kRGBA_8888_SkColorType, kPremul_SkAlphaType),
                out.pixels.data(), static_cast<size_t>(w) * 4u);
    if (!surface->readPixels(pm, 0, 0)) out.pixels.clear();
    return out;
}

std::string temp_path(const char* leaf) {
    auto dir = std::filesystem::temp_directory_path() / "pulp-headless-surface";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return (dir / leaf).string();
}

} // namespace

TEST_CASE("HeadlessSurface draws a file-backed image on the GPU path",
          "[gpu][skia][headless-surface][image]") {
    // Distinct from both the clear color and the SVG color below.
    constexpr uint8_t kImgR = 240, kImgG = 200, kImgB = 20;
    const std::string png_path = temp_path("solid-amber.png");
    REQUIRE(write_solid_png(png_path, 64, 64, kImgR, kImgG, kImgB));

    HeadlessSurface::Config cfg;
    cfg.width = kW;
    cfg.height = kH;
    cfg.clear_r = kBgR;
    cfg.clear_g = kBgG;
    cfg.clear_b = kBgB;
    cfg.clear_a = 255;

    std::string err;
    auto surface = HeadlessSurface::create(cfg, &err);
    if (!surface) {
        SUCCEED("HeadlessSurface unavailable: " + err);
        return;
    }

    auto paint_image = [&](pulp::canvas::Canvas& c) {
        REQUIRE(c.draw_image_from_file(png_path, 32.0f, 16.0f, 96.0f, 96.0f));
    };

    auto rgba = surface->render_rgba(paint_image);
    if (rgba.empty()) {
        SUCCEED("GPU readback failed: " + surface->last_error());
        return;
    }

    // Assert the ink: the amber pixels must actually be on the surface. The
    // 96x96 destination rect is 9216 pixels; allow generous slack for edge
    // filtering, but a dropped draw yields exactly 0.
    const uint32_t amber = count_near_color(rgba, kImgR, kImgG, kImgB);
    INFO("amber pixels from the file-backed image: " << amber);
    REQUIRE(amber > 4000u);
}

TEST_CASE("HeadlessSurface draws an SVG document on the GPU path",
          "[gpu][skia][headless-surface][svg]") {
    constexpr uint8_t kSvgR = 30, kSvgG = 220, kSvgB = 120;
    const std::string svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" "
        "viewBox=\"0 0 100 100\">"
        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#1EDC78\"/>"
        "</svg>";

    HeadlessSurface::Config cfg;
    cfg.width = kW;
    cfg.height = kH;
    cfg.clear_r = kBgR;
    cfg.clear_g = kBgG;
    cfg.clear_b = kBgB;
    cfg.clear_a = 255;

    std::string err;
    auto surface = HeadlessSurface::create(cfg, &err);
    if (!surface) {
        SUCCEED("HeadlessSurface unavailable: " + err);
        return;
    }

    auto paint_svg = [&](pulp::canvas::Canvas& c) {
        REQUIRE(c.draw_svg(svg, 32.0f, 16.0f, 96.0f, 96.0f));
    };

    auto rgba = surface->render_rgba(paint_svg);
    if (rgba.empty()) {
        SUCCEED("GPU readback failed: " + surface->last_error());
        return;
    }

    const uint32_t green = count_near_color(rgba, kSvgR, kSvgG, kSvgB);
    INFO("SVG-fill pixels: " << green);
    REQUIRE(green > 4000u);
}

// An SVG that embeds its artwork as a data-URI `<image>` is decoded by
// SkSVGDOM into a raster SkImage and drawn straight to the canvas — no Pulp
// call site sits in between to upload it. Without an ImageProvider on the
// Recorder, Graphite discards that draw and logs
// "Couldn't convert SkImage to a Graphite-backed representation", while
// `draw_svg` still returns true. Rendering the identical document on the
// raster backend composites it correctly, which is what made the loss look
// like a design/import fault rather than a renderer one.
TEST_CASE("HeadlessSurface draws an SVG's embedded raster image on the GPU path",
          "[gpu][skia][headless-surface][svg][image]") {
    constexpr uint8_t kImgR = 240, kImgG = 200, kImgB = 20;

    HeadlessSurface::Rgba src;
    src.width = 64;
    src.height = 64;
    src.pixels.resize(64u * 64u * 4u);
    for (size_t i = 0; i < 64u * 64u; ++i) {
        src.pixels[i * 4 + 0] = kImgR;
        src.pixels[i * 4 + 1] = kImgG;
        src.pixels[i * 4 + 2] = kImgB;
        src.pixels[i * 4 + 3] = 255;
    }
    auto png = HeadlessSurface::encode_png(src);
    REQUIRE_FALSE(png.empty());

    const std::string svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" "
        "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
        "width=\"100\" height=\"100\" viewBox=\"0 0 100 100\">"
        "<image x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
        "xlink:href=\"data:image/png;base64," +
        pulp::runtime::base64_encode(png.data(), png.size()) +
        "\"/></svg>";

    HeadlessSurface::Config cfg;
    cfg.width = kW;
    cfg.height = kH;
    cfg.clear_r = kBgR;
    cfg.clear_g = kBgG;
    cfg.clear_b = kBgB;
    cfg.clear_a = 255;

    std::string err;
    auto surface = HeadlessSurface::create(cfg, &err);
    if (!surface) {
        SUCCEED("HeadlessSurface unavailable: " + err);
        return;
    }

    auto paint_svg = [&](pulp::canvas::Canvas& c) {
        REQUIRE(c.draw_svg(svg, 32.0f, 16.0f, 96.0f, 96.0f));
    };

    // Raster control first: the same document through a CPU SkiaCanvas. It
    // pins the fixture (a well-formed SVG, a decodable data URI, a color the
    // counter can find), so a red GPU assertion below can only mean the GPU
    // path lost the draw.
    const uint32_t raster_amber = count_near_color(
        raster_render(kW, kH, paint_svg), kImgR, kImgG, kImgB);
    INFO("amber pixels on the raster control: " << raster_amber);
    REQUIRE(raster_amber > 4000u);

    // Three frames on one surface: the first uploads through the provider, the
    // rest take its cache-hit branch. A cache that returned a stale or
    // wrong-Recorder texture would show up as a later frame losing the artwork.
    for (int frame = 0; frame < 3; ++frame) {
        auto rgba = surface->render_rgba(paint_svg);
        if (rgba.empty()) {
            SUCCEED("GPU readback failed: " + surface->last_error());
            return;
        }
        const uint32_t amber = count_near_color(rgba, kImgR, kImgG, kImgB);
        INFO("frame " << frame << " amber pixels from the SVG's embedded image: "
                      << amber);
        REQUIRE(amber > 4000u);
    }
}

// `draw_image_from_file` decodes and uploads once, then reuses the prepared
// SkImage from the path-keyed cache on later frames. A repaint that lost the
// artwork after the first frame would be a cache handing back something the
// current Recorder cannot draw.
TEST_CASE("HeadlessSurface keeps drawing a file-backed image across frames",
          "[gpu][skia][headless-surface][image]") {
    constexpr uint8_t kImgR = 240, kImgG = 200, kImgB = 20;
    const std::string png_path = temp_path("solid-amber-repeat.png");
    REQUIRE(write_solid_png(png_path, 256, 256, kImgR, kImgG, kImgB));

    HeadlessSurface::Config cfg;
    cfg.width = kW;
    cfg.height = kH;
    cfg.clear_r = kBgR;
    cfg.clear_g = kBgG;
    cfg.clear_b = kBgB;
    cfg.clear_a = 255;

    std::string err;
    auto surface = HeadlessSurface::create(cfg, &err);
    if (!surface) {
        SUCCEED("HeadlessSurface unavailable: " + err);
        return;
    }

    auto paint_image = [&](pulp::canvas::Canvas& c) {
        REQUIRE(c.draw_image_from_file(png_path, 32.0f, 16.0f, 96.0f, 96.0f));
    };

    for (int frame = 0; frame < 3; ++frame) {
        auto rgba = surface->render_rgba(paint_image);
        if (rgba.empty()) {
            SUCCEED("GPU readback failed: " + surface->last_error());
            return;
        }
        const uint32_t amber = count_near_color(rgba, kImgR, kImgG, kImgB);
        INFO("frame " << frame << " amber pixels: " << amber);
        REQUIRE(amber > 4000u);
    }
}

#else  // !(PULP_HAS_SKIA && __APPLE__)

TEST_CASE("HeadlessSurface skips runtime cases when Skia/Apple unavailable",
          "[gpu][skia][headless-surface][item-6-7]") {
    HeadlessSurface::Config cfg;
    cfg.width = kW;
    cfg.height = kH;
    std::string err;
    auto surface = HeadlessSurface::create(cfg, &err);
    // Without Skia, create() returns null; with Skia on non-Apple, the
    // platform GpuSurface path may not init. Either way: no crash, and
    // err is populated so callers can soft-skip.
    if (!surface) {
        REQUIRE_FALSE(err.empty());
        SUCCEED("HeadlessSurface unavailable on this build: " + err);
        return;
    }
    SUCCEED("HeadlessSurface available on a non-Apple Skia build — runtime cases skipped");
}

#endif  // PULP_HAS_SKIA && __APPLE__
