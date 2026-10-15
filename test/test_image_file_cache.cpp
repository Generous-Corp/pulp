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
#include <condition_variable>
#include <mutex>
#include <thread>

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

TEST_CASE("ImageFileCache: clear_backend removes only one GPU backend",
          "[canvas][image-cache][embedded]") {
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);

    std::atomic<int> builds{0};
    auto build = [&]() -> sk_sp<SkImage> {
        ++builds;
        return make_dummy_image();
    };
    const void* gpu_a = reinterpret_cast<const void*>(0xCAFE);
    const void* gpu_b = reinterpret_cast<const void*>(0xBABE);

    cache.get_or_build("/tmp/shared.png", gpu_a, build);
    cache.get_or_build("/tmp/shared.png", gpu_b, build);
    REQUIRE(cache.stats().size == 2);
    cache.clear_backend(gpu_a);
    REQUIRE(cache.stats().size == 1);
    cache.get_or_build("/tmp/shared.png", gpu_b, build);
    REQUIRE(builds.load() == 2); // backend B entry remains a hit
    cache.get_or_build("/tmp/shared.png", gpu_a, build);
    REQUIRE(builds.load() == 3); // backend A was invalidated
}

TEST_CASE("ImageFileCache: same-key concurrent builds are coalesced",
          "[canvas][image-cache][embedded]") {
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    const void* backend = reinterpret_cast<const void*>(0xDADA);
    std::atomic<int> builds{0};
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    auto build = [&]() -> sk_sp<SkImage> {
        ++builds;
        started.store(true, std::memory_order_release);
        gate_cv.notify_all();
        std::unique_lock<std::mutex> lock(gate_mutex);
        gate_cv.wait_for(lock, std::chrono::seconds(10),
                         [&] { return release.load(std::memory_order_acquire); });
        return make_dummy_image();
    };
    sk_sp<SkImage> first, second;
    std::thread t1([&] { first = cache.get_or_build("/tmp/concurrent.png", backend, build); });
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        REQUIRE(gate_cv.wait_for(lock, std::chrono::seconds(10),
                                 [&] { return started.load(std::memory_order_acquire); }));
    }
    std::thread t2([&] { second = cache.get_or_build("/tmp/concurrent.png", backend, build); });
    release.store(true, std::memory_order_release);
    gate_cv.notify_all();
    t1.join();
    t2.join();
    REQUIRE(first != nullptr);
    REQUIRE(second == first);
    REQUIRE(builds.load() == 1);
    REQUIRE(cache.stats().hits == 1);
}

TEST_CASE("ImageFileCache: clear_backend waits for an in-flight build",
          "[canvas][image-cache][embedded]") {
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    const void* backend = reinterpret_cast<const void*>(0xD0D0);
    std::atomic<int> builds{0};
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    auto build = [&]() -> sk_sp<SkImage> {
        ++builds;
        started.store(true, std::memory_order_release);
        gate_cv.notify_all();
        std::unique_lock<std::mutex> lock(gate_mutex);
        gate_cv.wait_for(lock, std::chrono::seconds(10),
                         [&] { return release.load(std::memory_order_acquire); });
        return make_dummy_image();
    };
    std::thread painter([&] { cache.get_or_build("/tmp/teardown.png", backend, build); });
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        REQUIRE(gate_cv.wait_for(lock, std::chrono::seconds(10),
                                 [&] { return started.load(std::memory_order_acquire); }));
    }
    std::atomic<bool> invalidated{false};
    std::thread invalidator([&] {
        cache.clear_backend(backend);
        invalidated.store(true, std::memory_order_release);
    });
    for (int i = 0; i < 1000 && !invalidated.load(std::memory_order_acquire); ++i)
        std::this_thread::yield();
    REQUIRE(!invalidated.load(std::memory_order_acquire));
    release.store(true, std::memory_order_release);
    gate_cv.notify_all();
    painter.join();
    invalidator.join();
    REQUIRE(cache.stats().size == 0);
    cache.get_or_build("/tmp/teardown.png", backend, [&] {
        ++builds;
        return make_dummy_image();
    });
    REQUIRE(builds.load() == 2);
}

TEST_CASE("ImageFileCache: disabling wakes a coalesced waiter",
          "[canvas][image-cache][embedded]") {
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    const void* backend = reinterpret_cast<const void*>(0xD1D1);
    std::atomic<int> builds{0};
    std::atomic<bool> first_started{false};
    std::atomic<bool> release_first{false};
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    auto build = [&]() -> sk_sp<SkImage> {
        const int n = ++builds;
        if (n == 1) {
            first_started.store(true, std::memory_order_release);
            gate_cv.notify_all();
            std::unique_lock<std::mutex> lock(gate_mutex);
            gate_cv.wait_for(lock, std::chrono::seconds(10), [&] {
                return release_first.load(std::memory_order_acquire);
            });
        }
        return make_dummy_image();
    };
    std::thread first([&] { cache.get_or_build("/tmp/disable.png", backend, build); });
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        REQUIRE(gate_cv.wait_for(lock, std::chrono::seconds(10), [&] {
            return first_started.load(std::memory_order_acquire);
        }));
    }
    std::atomic<bool> second_done{false};
    std::thread second([&] {
        cache.get_or_build("/tmp/disable.png", backend, build);
        second_done.store(true, std::memory_order_release);
    });
    cache.set_enabled(false);
    for (int i = 0; i < 1000 && !second_done.load(std::memory_order_acquire); ++i)
        std::this_thread::yield();
    REQUIRE(second_done.load(std::memory_order_acquire));
    release_first.store(true, std::memory_order_release);
    gate_cv.notify_all();
    first.join();
    second.join();
    REQUIRE(builds.load() == 2);
    cache.set_enabled(true);
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
