// Content-keyed inline image cache on a live offscreen Dawn + Skia Graphite
// surface.
//
// An inline image (an ImageView with a `data:image/png` source, or a
// SceneImage holding an encoded blob) keeps its encoded bytes in memory and
// draws them every paint. On Graphite each uncached draw decodes the PNG and
// uploads a fresh texture. The keyed draw path routes those bytes through
// ImageFileCache under a SHA-256 content key, so the decode and upload happen
// once per (content, backend). These cases prove the reuse with the cache's own
// build/hit counters, prove the counters can see the failure by disabling the
// cache, and prove the cached frames are pixel-identical to the uncached ones.
//
// The timing case is tagged [bench]: it prints CPU wall time per frame for a
// retina-sized inline PNG with the cache on and off, and asserts only that both
// modes produced samples. Every case soft-skips without a Dawn adapter.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/scene/scene_image.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)
#include <pulp/canvas/image_file_cache.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#endif

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)

using namespace pulp;

namespace {

struct Fixture {
    std::unique_ptr<render::GpuSurface> gpu;
    std::unique_ptr<render::SkiaSurface> skia;
};

Fixture make_fixture(uint32_t w, uint32_t h) {
    Fixture f;
    f.gpu = render::GpuSurface::create_dawn();
    if (!f.gpu)
        return f;
    render::GpuSurface::Config gpu_config{};
    gpu_config.width = w;
    gpu_config.height = h;
    gpu_config.native_surface_handle = nullptr;
    if (!f.gpu->initialize(gpu_config)) {
        f.gpu.reset();
        return f;
    }
    render::SkiaSurface::Config skia_config{};
    skia_config.width = w;
    skia_config.height = h;
    skia_config.scale_factor = 1.0f;
    f.skia = render::SkiaSurface::create(*f.gpu, skia_config);
    return f;
}

bool usable(const Fixture& f) {
    return f.gpu && f.skia && f.skia->is_available();
}

// A PNG with four coloured quadrants and a diagonal band, so a wrong or
// missing draw changes many pixels. With @p grain, low-amplitude per-pixel
// noise keeps the encoded size near that of a photographic backdrop rather
// than a flat-colour PNG that compresses to almost nothing.
std::vector<uint8_t> make_png(int w, int h, SkColor tint, bool grain = false) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
    if (!surface)
        return {};
    auto* c = surface->getCanvas();
    c->clear(SK_ColorBLACK);
    SkPaint p;
    p.setColor(SK_ColorRED);
    c->drawRect(SkRect::MakeXYWH(0, 0, w / 2.0f, h / 2.0f), p);
    p.setColor(SK_ColorGREEN);
    c->drawRect(SkRect::MakeXYWH(w / 2.0f, 0, w / 2.0f, h / 2.0f), p);
    p.setColor(SK_ColorBLUE);
    c->drawRect(SkRect::MakeXYWH(0, h / 2.0f, w / 2.0f, h / 2.0f), p);
    p.setColor(tint);
    p.setStrokeWidth(w / 16.0f);
    c->drawLine(0, 0, static_cast<float>(w), static_cast<float>(h), p);
    if (grain) {
        SkPixmap pm;
        if (!surface->peekPixels(&pm))
            return {};
        uint32_t state = 0x9e3779b9u;
        for (int y = 0; y < h; ++y) {
            auto* row = pm.writable_addr32(0, y);
            for (int x = 0; x < w; ++x) {
                state = state * 1664525u + 1013904223u;
                row[x] ^= (state >> 8) & 0x0f0f0fu; // colour bytes only, alpha untouched
            }
        }
    }
    auto image = surface->makeImageSnapshot();
    auto data = SkPngEncoder::Encode(nullptr, image.get(), SkPngEncoder::Options{});
    if (!data)
        return {};
    const auto* bytes = static_cast<const uint8_t*>(data->data());
    return std::vector<uint8_t>(bytes, bytes + data->size());
}

std::string data_uri(const std::vector<uint8_t>& png) {
    return "data:image/png;base64," + runtime::base64_encode(png.data(), png.size());
}

template <typename PaintFn>
bool paint_frame(Fixture& f, PaintFn&& paint, std::vector<uint8_t>* px = nullptr) {
    if (!f.gpu->begin_frame())
        return false;
    auto* canvas = f.skia->begin_frame();
    if (!canvas) {
        f.gpu->end_frame();
        return false;
    }
    canvas->set_fill_color(canvas::Color::rgba8(16, 18, 26, 255));
    canvas->fill_rect(0, 0, 10000, 10000);
    paint(*canvas);
    f.skia->end_frame();
    f.gpu->end_frame();
    if (!px)
        return true;
    uint32_t pw = 0, ph = 0;
    return f.skia->read_current_rgba(*px, pw, ph);
}

int max_channel_delta(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size())
        return 256;
    int worst = 0;
    for (size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, std::abs(int(a[i]) - int(b[i])));
    return worst;
}

size_t red_pixels(const std::vector<uint8_t>& px) {
    size_t n = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4)
        if (px[i] > 200 && px[i + 1] < 50 && px[i + 2] < 50)
            ++n;
    return n;
}

void reset_cache(bool enabled) {
    auto& cache = canvas::ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(enabled);
}

// Re-enables the process-wide cache however a test case exits, so a SKIP or
// a failed REQUIRE cannot leave later cases running uncached.
struct CacheEnabledRestore {
    ~CacheEnabledRestore() {
        canvas::ImageFileCache::instance().set_enabled(true);
    }
};

constexpr int kFrames = 6;

} // namespace

TEST_CASE("Keyed inline image bytes build once and hit on later Graphite frames",
          "[gpu][skia][image-cache][inline-image]") {
    CacheEnabledRestore restore_cache;
    auto f = make_fixture(256, 256);
    if (!usable(f))
        SKIP("Dawn/Graphite unavailable on this host.");
    const auto png = make_png(128, 128, SK_ColorYELLOW);
    REQUIRE_FALSE(png.empty());
    const auto key = runtime::sha256_hex(png.data(), png.size());

    // The uncached reference frame, drawn with the cache disabled.
    reset_cache(false);
    std::vector<uint8_t> reference;
    if (!paint_frame(
            f,
            [&](canvas::Canvas& c) {
                REQUIRE(c.draw_image_from_data(png.data(), png.size(), 0, 0, 256, 256));
            },
            &reference))
        SKIP("GPU readback failed (no adapter).");
    if (red_pixels(reference) == 0)
        SKIP("Offscreen GPU composited nothing (no real adapter).");

    reset_cache(true);
    std::vector<uint8_t> cached;
    for (int frame = 0; frame < kFrames; ++frame) {
        REQUIRE(paint_frame(
            f,
            [&](canvas::Canvas& c) {
                REQUIRE(c.draw_image_from_keyed_data(key, png.data(), png.size(), 0, 0, 256, 256));
            },
            &cached));
        INFO("frame " << frame);
        CHECK(max_channel_delta(reference, cached) == 0);
    }
    const auto stats = canvas::ImageFileCache::instance().inline_stats();
    CHECK(stats.builds == 1);
    CHECK(stats.hits == kFrames - 1);
}

TEST_CASE("Disabling the image cache rebuilds keyed inline bytes every Graphite frame",
          "[gpu][skia][image-cache][inline-image]") {
    CacheEnabledRestore restore_cache;
    // Planted negative for the counters above: with caching off the same keyed
    // draws must report one build per draw and no hits.
    auto f = make_fixture(256, 256);
    if (!usable(f))
        SKIP("Dawn/Graphite unavailable on this host.");
    const auto png = make_png(128, 128, SK_ColorYELLOW);
    const auto key = runtime::sha256_hex(png.data(), png.size());
    reset_cache(false);
    for (int frame = 0; frame < kFrames; ++frame) {
        if (!paint_frame(f, [&](canvas::Canvas& c) {
                REQUIRE(c.draw_image_from_keyed_data_rect(key, png.data(), png.size(), 0, 0, 64, 64,
                                                          0, 0, 256, 256));
            }))
            SKIP("GPU frame failed (no adapter).");
    }
    const auto stats = canvas::ImageFileCache::instance().inline_stats();
    CHECK(stats.builds == kFrames);
    CHECK(stats.hits == 0);
}

TEST_CASE("ImageView data URI decodes once across Graphite paints and matches the uncached frame",
          "[gpu][skia][image-cache][inline-image][image-view]") {
    CacheEnabledRestore restore_cache;
    auto f = make_fixture(200, 120);
    if (!usable(f))
        SKIP("Dawn/Graphite unavailable on this host.");
    const auto png = make_png(100, 60, SK_ColorYELLOW);
    view::ImageView image;
    image.set_bounds({0, 0, 200, 120});
    image.set_image_source(data_uri(png));

    reset_cache(false);
    std::vector<uint8_t> reference;
    if (!paint_frame(f, [&](canvas::Canvas& c) { image.paint(c); }, &reference))
        SKIP("GPU readback failed (no adapter).");
    if (red_pixels(reference) == 0)
        SKIP("Offscreen GPU composited nothing (no real adapter).");

    reset_cache(true);
    std::vector<uint8_t> cached;
    for (int frame = 0; frame < kFrames; ++frame) {
        REQUIRE(paint_frame(f, [&](canvas::Canvas& c) { image.paint(c); }, &cached));
        CHECK(max_channel_delta(reference, cached) == 0);
    }
    auto stats = canvas::ImageFileCache::instance().inline_stats();
    CHECK(stats.builds == 1);
    CHECK(stats.hits == kFrames - 1);

    // New bytes under a new source must miss, never reuse the old texture.
    const auto other = make_png(100, 60, SK_ColorMAGENTA);
    image.set_image_source(data_uri(other));
    std::vector<uint8_t> swapped;
    REQUIRE(paint_frame(f, [&](canvas::Canvas& c) { image.paint(c); }, &swapped));
    stats = canvas::ImageFileCache::instance().inline_stats();
    CHECK(stats.builds == 2);
    CHECK(max_channel_delta(reference, swapped) > 100);
}

TEST_CASE("SceneImage encoded bytes decode once across Graphite paints",
          "[gpu][skia][image-cache][inline-image][scene]") {
    CacheEnabledRestore restore_cache;
    auto f = make_fixture(128, 128);
    if (!usable(f))
        SKIP("Dawn/Graphite unavailable on this host.");
    const auto png = make_png(64, 64, SK_ColorYELLOW);
    canvas::SceneImage node;
    node.set_rect(0, 0, 128, 128);
    node.set_encoded_bytes(png);
    REQUIRE(node.encoded_bytes_key() == runtime::sha256_hex(png.data(), png.size()));

    reset_cache(false);
    std::vector<uint8_t> reference;
    if (!paint_frame(f, [&](canvas::Canvas& c) { node.paint_geometry(c); }, &reference))
        SKIP("GPU readback failed (no adapter).");
    if (red_pixels(reference) == 0)
        SKIP("Offscreen GPU composited nothing (no real adapter).");

    reset_cache(true);
    std::vector<uint8_t> cached;
    for (int frame = 0; frame < kFrames; ++frame) {
        REQUIRE(paint_frame(f, [&](canvas::Canvas& c) { node.paint_geometry(c); }, &cached));
        CHECK(max_channel_delta(reference, cached) == 0);
    }
    const auto stats = canvas::ImageFileCache::instance().inline_stats();
    CHECK(stats.builds == 1);
    CHECK(stats.hits == kFrames - 1);
}

TEST_CASE("Inline image paint cost per Graphite frame, cached and uncached",
          "[gpu][skia][image-cache][inline-image][bench]") {
    CacheEnabledRestore restore_cache;
    // A 2x-retina backdrop drawn by an ImageView with a data URI, the shape of
    // an imported panel's inline background. CPU wall time per frame covers the
    // paint, the Graphite recording snap and submit, and a readback that waits
    // for the GPU; decode and texture staging happen on the CPU inside the
    // paint, so they are what the uncached mode pays for.
    constexpr uint32_t kW = 1320, kH = 860;
    auto f = make_fixture(kW, kH);
    if (!usable(f))
        SKIP("Dawn/Graphite unavailable on this host.");
    const auto png =
        make_png(static_cast<int>(kW * 2), static_cast<int>(kH * 2), SK_ColorYELLOW, true);
    view::ImageView image;
    image.set_bounds({0, 0, static_cast<float>(kW), static_cast<float>(kH)});
    image.set_image_source(data_uri(png));

    std::vector<uint8_t> px;
    // `direct` is the unkeyed draw an inline image made before the keyed path:
    // it borrows the bytes instead of copying them, so it is the fair baseline.
    auto run = [&](bool cached, bool direct = false) {
        reset_cache(cached);
        std::vector<double> ms;
        for (int i = 0; i < 25; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = paint_frame(
                f,
                [&](canvas::Canvas& c) {
                    if (direct)
                        c.draw_image_from_data(png.data(), png.size(), 0, 0, static_cast<float>(kW),
                                               static_cast<float>(kH));
                    else
                        image.paint(c);
                },
                &px);
            if (!ok)
                return ms;
            const auto t1 = std::chrono::steady_clock::now();
            if (i >= 5)
                ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return ms;
    };
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2];
    };
    const auto direct = run(false, true);
    const auto uncached = run(false);
    const auto cached = run(true);
    const auto stats = canvas::ImageFileCache::instance().inline_stats();
    if (direct.empty() || uncached.empty() || cached.empty())
        SKIP("GPU frame failed (no adapter).");
    std::printf("inline PNG %ux%u (%zu bytes): unkeyed draw median %.3f ms/frame, keyed with "
                "cache disabled %.3f ms/frame, keyed cached %.3f ms/frame, cached builds %llu "
                "hits %llu\n",
                kW * 2, kH * 2, png.size(), median(direct), median(uncached), median(cached),
                static_cast<unsigned long long>(stats.builds),
                static_cast<unsigned long long>(stats.hits));
    CHECK(stats.builds == 1);
}

#else

TEST_CASE("Inline image cache GPU proof needs Skia on macOS", "[gpu][inline-image]") {
    SKIP("Skia Graphite offscreen surface unavailable in this build.");
}

#endif
