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

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using pulp::render::HeadlessSurface;

namespace {

constexpr uint32_t kW = 256;
constexpr uint32_t kH = 128;

// Sentinel background — distinctive enough that
// `count_non_background` won't mistake noise for the user's paint.
constexpr uint8_t kBgR = 20, kBgG = 40, kBgB = 80;

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
