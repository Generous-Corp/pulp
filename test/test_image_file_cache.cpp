// ImageFileCache — path-keyed decoded/GPU-uploaded file-image cache. [#6223 S35]
//
// Proves the cache that stops SkiaCanvas::draw_image_from_file from re-reading +
// re-decoding + re-uploading a file image every frame: a hit skips the build
// closure entirely, the backend token partitions GPU-context-bound textures,
// and clear() is the invalidation hook for the deliberate path-keyed (not
// content-keyed) semantics.

#ifdef PULP_HAS_SKIA

#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/image_file_cache.hpp>
#include <pulp/canvas/skia_canvas.hpp>

#include "include/core/SkImage.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkSurface.h"

#include <atomic>
#include <array>

using namespace pulp::canvas;

namespace {

// A tiny non-null raster SkImage so get_or_build has something to store.
sk_sp<SkImage> make_dummy_image() {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(1, 1));
    return surface ? surface->makeImageSnapshot() : nullptr;
}

} // namespace

TEST_CASE("Static embedded sprite decodes once across canvas frames",
          "[canvas][image-cache][embedded]") {
    // 2x1 RGBA PNG: red, blue. Static storage is part of this API's contract.
    static constexpr std::uint8_t png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
        0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0xf4,
        0x22, 0x7f, 0x8a, 0x00, 0x00, 0x00, 0x0e, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8,
        0xcf, 0xc0, 0x00, 0x42, 0xff, 0x01, 0x0f, 0xf9, 0x03, 0xfd, 0x85, 0x11, 0x99, 0x76, 0x00,
        0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);

    for (int frame = 0; frame < 2; ++frame) {
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(4, 2));
        REQUIRE(surface != nullptr);
        SkiaCanvas canvas(surface->getCanvas());
        REQUIRE(canvas.draw_image_from_static_data_rect(png, sizeof(png), 1, 0, 1, 1, 0, 0, 4, 2));
        SkBitmap pixels;
        REQUIRE(pixels.tryAllocN32Pixels(4, 2));
        REQUIRE(surface->readPixels(pixels, 0, 0));
        const auto color = pixels.getColor(2, 1);
        REQUIRE(SkColorGetB(color) > 200);
        REQUIRE(SkColorGetR(color) < 50);
    }
    REQUIRE(cache.stats().builds == 1);
    REQUIRE(cache.stats().hits == 1);
}

TEST_CASE("Static embedded sprites key distinct bytes with the same size",
          "[canvas][image-cache][embedded]") {
    // Same 2x1 dimensions and size as the fixture above, but different pixels
    // and a distinct address. This catches keys that accidentally omit the
    // source address or size.
    static constexpr std::uint8_t green_yellow_png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
        0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0xf4,
        0x22, 0x7f, 0x8a, 0x00, 0x00, 0x00, 0x0e, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60,
        0x60, 0x60, 0xf8, 0x0f, 0x02, 0x00, 0x0e, 0xfa, 0x04, 0xfc, 0xa2, 0xab, 0xed, 0xce, 0x00,
        0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };

    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);

    auto draw_and_read = [](const std::uint8_t* png, size_t size) {
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(2, 1));
        REQUIRE(surface != nullptr);
        SkiaCanvas canvas(surface->getCanvas());
        REQUIRE(canvas.draw_image_from_static_data_rect(png, size, 0, 0, 2, 1, 0, 0, 2, 1));
        SkBitmap pixels;
        REQUIRE(pixels.tryAllocN32Pixels(2, 1));
        REQUIRE(surface->readPixels(pixels, 0, 0));
        return std::array<SkColor, 2>{pixels.getColor(0, 0), pixels.getColor(1, 0)};
    };

    static constexpr std::uint8_t red_blue_png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
        0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0xf4,
        0x22, 0x7f, 0x8a, 0x00, 0x00, 0x00, 0x0e, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8,
        0xcf, 0xc0, 0x00, 0x42, 0xff, 0x01, 0x0f, 0xf9, 0x03, 0xfd, 0x85, 0x11, 0x99, 0x76, 0x00,
        0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
    const auto first = draw_and_read(red_blue_png, sizeof(red_blue_png));
    const auto second = draw_and_read(green_yellow_png, sizeof(green_yellow_png));
    REQUIRE(first[0] != second[0]);
    REQUIRE(first[1] != second[1]);
    REQUIRE(cache.stats().builds == 2);
    REQUIRE(cache.stats().hits == 0);
}

TEST_CASE("ImageFileCache: miss builds, hit reuses", "[canvas][image-cache][issue-6223]") {
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);

    std::atomic<int> builds{0};
    auto build = [&]() -> sk_sp<SkImage> {
        ++builds;
        return make_dummy_image();
    };

    const void* backend = reinterpret_cast<const void*>(0x1);
    auto a = cache.get_or_build("/tmp/knob.png", backend, build);
    REQUIRE(a != nullptr);
    REQUIRE(builds.load() == 1);
    REQUIRE(cache.stats().builds == 1);

    // Second draw of the same path + backend is a hit: closure NOT invoked.
    auto b = cache.get_or_build("/tmp/knob.png", backend, build);
    REQUIRE(b == a);            // same shared image
    REQUIRE(builds.load() == 1);
    REQUIRE(cache.stats().hits == 1);
}

TEST_CASE("ImageFileCache: distinct paths and backends key separately",
          "[canvas][image-cache][issue-6223]") {
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);

    std::atomic<int> builds{0};
    auto build = [&]() -> sk_sp<SkImage> { ++builds; return make_dummy_image(); };

    const void* gpu_a = reinterpret_cast<const void*>(0xA);
    const void* gpu_b = reinterpret_cast<const void*>(0xB);

    cache.get_or_build("/tmp/one.png", gpu_a, build);
    cache.get_or_build("/tmp/two.png", gpu_a, build);   // different path
    cache.get_or_build("/tmp/one.png", gpu_b, build);   // same path, other backend
    // Three distinct keys → three builds, no hits.
    REQUIRE(builds.load() == 3);
    REQUIRE(cache.stats().builds == 3);
    REQUIRE(cache.stats().hits == 0);

    // A GPU texture uploaded for backend A must never be reused for backend B.
    auto a = cache.get_or_build("/tmp/one.png", gpu_a, build);
    auto b = cache.get_or_build("/tmp/one.png", gpu_b, build);
    REQUIRE(a != b);
    REQUIRE(builds.load() == 3);  // both were hits
    REQUIRE(cache.stats().hits == 2);
}

TEST_CASE("ImageFileCache: clear() forces a rebuild (invalidation hook)",
          "[canvas][image-cache][issue-6223]") {
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);

    std::atomic<int> builds{0};
    auto build = [&]() -> sk_sp<SkImage> { ++builds; return make_dummy_image(); };
    const void* backend = nullptr;  // CPU raster

    cache.get_or_build("/tmp/bg.png", backend, build);
    REQUIRE(builds.load() == 1);
    cache.get_or_build("/tmp/bg.png", backend, build);
    REQUIRE(builds.load() == 1);  // hit

    // clear() is the documented hook for on-disk mutation / context teardown.
    cache.clear();
    cache.get_or_build("/tmp/bg.png", backend, build);
    REQUIRE(builds.load() == 2);  // rebuilt after clear
}

TEST_CASE("ImageFileCache: disabled always rebuilds and stores nothing",
          "[canvas][image-cache][issue-6223]") {
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(false);

    std::atomic<int> builds{0};
    auto build = [&]() -> sk_sp<SkImage> { ++builds; return make_dummy_image(); };
    const void* backend = reinterpret_cast<const void*>(0x2);

    cache.get_or_build("/tmp/x.png", backend, build);
    cache.get_or_build("/tmp/x.png", backend, build);
    REQUIRE(builds.load() == 2);       // no caching while disabled
    REQUIRE(cache.stats().size == 0);

    cache.set_enabled(true);  // restore for other tests / process default
}

TEST_CASE("ImageFileCache: null build result is not stored (no poisoning)",
          "[canvas][image-cache][issue-6223]") {
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);

    std::atomic<int> builds{0};
    // Simulate a transient read/decode failure that later succeeds.
    auto failing = [&]() -> sk_sp<SkImage> { ++builds; return nullptr; };
    const void* backend = nullptr;

    auto miss = cache.get_or_build("/tmp/flaky.png", backend, failing);
    REQUIRE(miss == nullptr);
    REQUIRE(cache.stats().size == 0);   // failure not cached

    std::atomic<int> ok_builds{0};
    auto ok = [&]() -> sk_sp<SkImage> { ++ok_builds; return make_dummy_image(); };
    auto good = cache.get_or_build("/tmp/flaky.png", backend, ok);
    REQUIRE(good != nullptr);
    REQUIRE(ok_builds.load() == 1);     // rebuilt, not a poisoned hit
}

TEST_CASE("ImageFileCache: LRU capacity evicts oldest",
          "[canvas][image-cache][issue-6223]") {
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    cache.set_capacity(2);

    std::atomic<int> builds{0};
    auto build = [&]() -> sk_sp<SkImage> { ++builds; return make_dummy_image(); };
    const void* backend = nullptr;

    cache.get_or_build("/a.png", backend, build);  // build 1
    cache.get_or_build("/b.png", backend, build);  // build 2
    cache.get_or_build("/c.png", backend, build);  // build 3, evicts /a.png
    REQUIRE(cache.stats().size == 2);

    cache.get_or_build("/a.png", backend, build);  // build 4 (was evicted)
    REQUIRE(builds.load() == 4);

    cache.set_capacity(32);  // restore default
    cache.clear();
}

#endif // PULP_HAS_SKIA
