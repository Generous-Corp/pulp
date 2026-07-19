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

#include "include/core/SkImage.h"
#include "include/core/SkSurface.h"

#include <atomic>

using namespace pulp::canvas;

namespace {

// A tiny non-null raster SkImage so get_or_build has something to store.
sk_sp<SkImage> make_dummy_image() {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(1, 1));
    return surface ? surface->makeImageSnapshot() : nullptr;
}

} // namespace

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
