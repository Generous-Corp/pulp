// ImageFileCache — path-keyed decoded/GPU-uploaded file-image cache. [#6223 S35]
//
// Proves the cache that stops SkiaCanvas::draw_image_from_file from re-reading +
// re-decoding + re-uploading a file image every frame: a hit skips the build
// closure entirely, the backend token partitions GPU-context-bound textures,
// and clear() is the invalidation hook for the deliberate path-keyed (not
// content-keyed) semantics.

#ifdef PULP_HAS_SKIA

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <pulp/canvas/image_file_cache.hpp>
#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/runtime/crypto.hpp>

#include "include/core/SkImage.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkImageGenerator.h"
#include "include/core/SkSurface.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::canvas;

namespace {

// A tiny non-null raster SkImage so get_or_build has something to store.
sk_sp<SkImage> make_dummy_image() {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(1, 1));
    return surface ? surface->makeImageSnapshot() : nullptr;
}

sk_sp<SkImage> make_image(int w, int h) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
    return surface ? surface->makeImageSnapshot() : nullptr;
}

// Restores the process-wide cache defaults however a test case exits.
struct CacheDefaults {
    ~CacheDefaults() {
        auto& cache = ImageFileCache::instance();
        cache.set_enabled(true);
        cache.set_capacity(32);
        cache.set_byte_budget(ImageFileCache::kDefaultFileByteBudget);
        cache.set_inline_byte_budget(ImageFileCache::kDefaultInlineByteBudget);
        cache.set_clock_for_testing(nullptr);
        cache.clear();
    }
};

// A clock the test advances by hand, installed as the cache's time source.
struct ManualClock {
    std::chrono::steady_clock::time_point now{};
    ManualClock() {
        ImageFileCache::instance().set_clock_for_testing([this] { return now; });
    }
    void advance(std::chrono::milliseconds by) {
        now += by;
    }
};

constexpr std::size_t kMiB = 1024u * 1024u;

// One frame of a panel: ten file images, then @p inline_count distinct
// inline images. With @p inline_in_path_pool the inline images go through
// the path pool instead, which is the sharing the separate pool prevents.
void paint_mixed_frame(int inline_count, bool inline_in_path_pool) {
    auto& cache = ImageFileCache::instance();
    const void* backend = reinterpret_cast<const void*>(0x5EED);
    for (int i = 0; i < 10; ++i)
        cache.get_or_build("/tmp/file-" + std::to_string(i) + ".png", backend,
                           [] { return make_image(8, 8); });
    for (int i = 0; i < inline_count; ++i) {
        const std::string key = "inline-" + std::to_string(i);
        if (inline_in_path_pool)
            cache.get_or_build(key, backend, [] { return make_image(8, 8); });
        else
            cache.get_or_build_inline(key, backend, 64, [] { return make_image(8, 8); });
    }
}

} // namespace

TEST_CASE("Many inline images leave file-image hits untouched",
          "[canvas][image-cache][inline-image]") {
    CacheDefaults restore;
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);

    for (int frame = 0; frame < 3; ++frame)
        paint_mixed_frame(30, false);
    CHECK(cache.stats().builds == 10);
    CHECK(cache.stats().hits == 20);
    CHECK(cache.inline_stats().builds == 30);
    CHECK(cache.inline_stats().hits == 60);

    // Control: the same frames with inline images in the 32-entry path pool
    // cycle it, so the file images stop hitting. This is what the counters
    // above would show if the pools were shared.
    cache.clear();
    cache.reset_stats();
    for (int frame = 0; frame < 3; ++frame)
        paint_mixed_frame(30, true);
    CHECK(cache.stats().hits == 0);
}

TEST_CASE("Inline pool stays within its byte budget, evicting least recently used",
          "[canvas][image-cache][inline-image]") {
    CacheDefaults restore;
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    // Each entry: 16x16 N32 pixels (1024 bytes) + 100 encoded bytes.
    constexpr std::size_t kEntry = 16 * 16 * 4 + 100;
    cache.set_inline_byte_budget(3 * kEntry);
    const void* backend = nullptr;
    auto build = [] { return make_image(16, 16); };

    for (int i = 0; i < 5; ++i)
        cache.get_or_build_inline("k" + std::to_string(i), backend, 100, build);
    auto stats = cache.inline_stats();
    CHECK(stats.size == 3);
    CHECK(stats.bytes == 3 * kEntry);
    CHECK(stats.bytes <= cache.inline_byte_budget());
    CHECK(stats.over_budget == 0);

    // k4, k3, k2 survive; k0 was evicted first.
    cache.get_or_build_inline("k4", backend, 100, build);
    CHECK(cache.inline_stats().hits == 1);
    cache.get_or_build_inline("k0", backend, 100, build);
    CHECK(cache.inline_stats().builds == 6);

    // Shrinking the budget evicts down to it at once.
    cache.set_inline_byte_budget(kEntry);
    CHECK(cache.inline_stats().size == 1);
    CHECK(cache.inline_stats().bytes <= kEntry);
}

TEST_CASE("An inline image larger than the byte budget draws correctly uncached",
          "[canvas][image-cache][inline-image]") {
    CacheDefaults restore;
    // The 2x1 red/blue PNG from the cases above.
    static constexpr std::uint8_t red_blue_png[] = {
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
    // Room for the small resident entry, not for the PNG's encoded copy plus
    // its decoded pixels.
    cache.set_inline_byte_budget(sizeof(red_blue_png) + 4);
    cache.get_or_build_inline("resident", nullptr, 0, [] { return make_image(1, 1); });
    REQUIRE(cache.inline_stats().size == 1);

    const auto key = pulp::runtime::sha256_hex(red_blue_png, sizeof(red_blue_png));
    for (int frame = 0; frame < 2; ++frame) {
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(2, 1));
        REQUIRE(surface != nullptr);
        SkiaCanvas canvas(surface->getCanvas());
        REQUIRE(
            canvas.draw_image_from_keyed_data(key, red_blue_png, sizeof(red_blue_png), 0, 0, 2, 1));
        SkBitmap pixels;
        REQUIRE(pixels.tryAllocN32Pixels(2, 1));
        REQUIRE(surface->readPixels(pixels, 0, 0));
        CHECK(SkColorGetR(pixels.getColor(0, 0)) > 200);
        CHECK(SkColorGetB(pixels.getColor(1, 0)) > 200);
    }
    const auto stats = cache.inline_stats();
    CHECK(stats.over_budget == 2); // built every draw, never retained
    CHECK(stats.size == 1);        // and the resident entry was not evicted
    cache.get_or_build_inline("resident", nullptr, 0, [] { return make_image(1, 1); });
    CHECK(cache.inline_stats().hits == 1);
}

TEST_CASE("Inline entry cost saturates instead of wrapping to a small value",
          "[canvas][image-cache][inline-image]") {
    CacheDefaults restore;
    // A lazily generated image with the largest valid dimensions: it is never
    // decoded, but its pixel size (about 2^60 bytes) is what the pool charges.
    struct HugeGenerator : SkImageGenerator {
        HugeGenerator()
            : SkImageGenerator(SkImageInfo::MakeN32Premul((1 << 29) - 1, (1 << 29) - 1)) {}
    };
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    cache.set_inline_byte_budget(max - 1);
    std::size_t pixel_bytes = 0;
    auto build = [&] {
        auto image = SkImages::DeferredFromGenerator(std::make_unique<HugeGenerator>());
        if (image)
            pixel_bytes = image->imageInfo().computeMinByteSize();
        return image;
    };

    // An encoded size that passes the budget on its own but overflows size_t
    // once the pixel bytes are added.
    const std::size_t encoded = max - (std::size_t{1} << 59);
    auto image = cache.get_or_build_inline("huge", nullptr, encoded, build);
    REQUIRE(image != nullptr);
    REQUIRE(pixel_bytes > (std::size_t{1} << 59)); // the sum really overflows
    const auto stats = cache.inline_stats();
    CHECK(stats.over_budget == 1);
    CHECK(stats.size == 0);
    CHECK(stats.bytes == 0);
}

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

TEST_CASE("Keyed inline bytes decode once per content key on a raster canvas",
          "[canvas][image-cache][inline-image]") {
    // The same 2x1 red/blue PNG as above, held in a short-lived heap buffer:
    // the cached image must own its bytes, and the key is the content digest.
    static constexpr std::uint8_t red_blue_png[] = {
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
    const auto key = pulp::runtime::sha256_hex(red_blue_png, sizeof(red_blue_png));

    auto draw = [&](const std::string& k) {
        std::vector<std::uint8_t> bytes(std::begin(red_blue_png), std::end(red_blue_png));
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(2, 1));
        REQUIRE(surface != nullptr);
        SkiaCanvas canvas(surface->getCanvas());
        REQUIRE(canvas.draw_image_from_keyed_data(k, bytes.data(), bytes.size(), 0, 0, 2, 1));
        bytes.assign(bytes.size(), 0); // the cache must not read this buffer later
        SkBitmap pixels;
        REQUIRE(pixels.tryAllocN32Pixels(2, 1));
        REQUIRE(surface->readPixels(pixels, 0, 0));
        return std::array<SkColor, 2>{pixels.getColor(0, 0), pixels.getColor(1, 0)};
    };
    for (int frame = 0; frame < 3; ++frame) {
        const auto px = draw(key);
        REQUIRE(SkColorGetR(px[0]) > 200);
        REQUIRE(SkColorGetB(px[1]) > 200);
    }
    REQUIRE(cache.inline_stats().builds == 1);
    REQUIRE(cache.inline_stats().hits == 2);

    // An empty key draws through the uncached path and never touches the cache.
    const auto px = draw(std::string{});
    REQUIRE(SkColorGetR(px[0]) > 200);
    REQUIRE(cache.inline_stats().builds == 1);
    REQUIRE(cache.inline_stats().hits == 2);
}

TEST_CASE("Recording canvas records keyed inline bytes like unkeyed bytes",
          "[canvas][image-cache][inline-image]") {
    const std::uint8_t bytes[] = {1, 2, 3, 4};
    RecordingCanvas keyed, plain;
    REQUIRE(keyed.draw_image_from_keyed_data("k", bytes, sizeof(bytes), 1, 2, 3, 4));
    REQUIRE(plain.draw_image_from_data(bytes, sizeof(bytes), 1, 2, 3, 4));
    REQUIRE(
        keyed.draw_image_from_keyed_data_rect("k", bytes, sizeof(bytes), 0, 0, 1, 1, 1, 2, 3, 4));
    REQUIRE(plain.draw_image_from_data_rect(bytes, sizeof(bytes), 0, 0, 1, 1, 1, 2, 3, 4));
    REQUIRE(keyed.commands().size() == plain.commands().size());
    for (size_t i = 0; i < keyed.commands().size(); ++i) {
        REQUIRE(keyed.commands()[i].type == plain.commands()[i].type);
        REQUIRE(keyed.commands()[i].text == plain.commands()[i].text);
        REQUIRE(keyed.commands()[i].floats == plain.commands()[i].floats);
    }
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

TEST_CASE("ImageFileCache: clear waits for an in-flight build", "[canvas][image-cache][embedded]") {
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    const void* backend = reinterpret_cast<const void*>(0xC1EA);
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
    std::thread painter([&] { cache.get_or_build("/tmp/clear.png", backend, build); });
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        REQUIRE(gate_cv.wait_for(lock, std::chrono::seconds(10),
                                 [&] { return started.load(std::memory_order_acquire); }));
    }
    std::atomic<bool> cleared{false};
    std::thread invalidator([&] {
        cache.clear();
        cleared.store(true, std::memory_order_release);
    });
    for (int i = 0; i < 1000 && !cleared.load(std::memory_order_acquire); ++i)
        std::this_thread::yield();
    REQUIRE(!cleared.load(std::memory_order_acquire));
    release.store(true, std::memory_order_release);
    gate_cv.notify_all();
    painter.join();
    invalidator.join();
    REQUIRE(cleared.load(std::memory_order_acquire));
    REQUIRE(cache.stats().size == 0);
}

TEST_CASE("ImageFileCache: disabling wakes a coalesced waiter", "[canvas][image-cache][embedded]") {
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
            gate_cv.wait_for(lock, std::chrono::seconds(10),
                             [&] { return release_first.load(std::memory_order_acquire); });
        }
        return make_dummy_image();
    };
    std::thread first([&] { cache.get_or_build("/tmp/disable.png", backend, build); });
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        REQUIRE(gate_cv.wait_for(lock, std::chrono::seconds(10),
                                 [&] { return first_started.load(std::memory_order_acquire); }));
    }
    std::atomic<bool> second_done{false};
    std::condition_variable second_done_cv;
    std::thread second([&] {
        cache.get_or_build("/tmp/disable.png", backend, build);
        second_done.store(true, std::memory_order_release);
        second_done_cv.notify_one();
    });
    cache.set_enabled(false);
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        REQUIRE(second_done_cv.wait_for(lock, std::chrono::seconds(10), [&] {
            return second_done.load(std::memory_order_acquire);
        }));
    }
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
    CacheDefaults restore;
    ManualClock clock;
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

    // Once the failure memo expires, the next lookup builds again.
    clock.advance(ImageFileCache::kFailureRetryDelay + std::chrono::milliseconds(1));
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

TEST_CASE("File pool stays within its byte budget, evicting least recently used",
          "[canvas][image-cache][byte-budget]") {
    CacheDefaults restore;
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    cache.set_byte_budget(16 * kMiB);
    // 2048x1024 N32 rasters: 8 MiB of pixels each.
    auto build = [] { return make_image(2048, 1024); };

    for (const char* path : {"/tmp/a.png", "/tmp/b.png", "/tmp/c.png"})
        REQUIRE(cache.get_or_build(path, nullptr, build) != nullptr);
    auto stats = cache.stats();
    CHECK(stats.size == 2);
    CHECK(stats.bytes == 16 * kMiB);
    CHECK(stats.bytes <= cache.byte_budget());

    // The first key was evicted and rebuilds; the newest two still hit.
    cache.get_or_build("/tmp/c.png", nullptr, build);
    CHECK(cache.stats().hits == 1);
    cache.get_or_build("/tmp/a.png", nullptr, build);
    CHECK(cache.stats().builds == 4);
    CHECK(cache.stats().bytes <= 16 * kMiB);
}

TEST_CASE("File pool keeps a lone image larger than its byte budget",
          "[canvas][image-cache][byte-budget]") {
    CacheDefaults restore;
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    cache.set_byte_budget(16 * kMiB);
    cache.get_or_build("/tmp/small.png", nullptr, [] { return make_image(16, 16); });
    REQUIRE(cache.stats().size == 1);

    // A 24 MiB backdrop under a 16 MiB budget is returned and retained alone,
    // so repainting it every frame is a hit rather than a decode per frame.
    // The inline pool's rule (build uncached) is covered below.
    int backdrop_builds = 0;
    auto backdrop = [&] {
        ++backdrop_builds;
        return make_image(2048, 3072);
    };
    for (int frame = 0; frame < 3; ++frame)
        REQUIRE(cache.get_or_build("/tmp/backdrop.png", nullptr, backdrop) != nullptr);
    CHECK(backdrop_builds == 1);
    const auto stats = cache.stats();
    CHECK(stats.size == 1);
    CHECK(stats.hits == 2);
    CHECK(stats.bytes == 24 * kMiB);

    // Shrinking the budget still keeps the newest entry.
    cache.set_byte_budget(1);
    CHECK(cache.stats().size == 1);
    cache.get_or_build("/tmp/backdrop.png", nullptr, backdrop);
    CHECK(backdrop_builds == 1);
}

TEST_CASE("Inline pool still builds an over-budget image uncached",
          "[canvas][image-cache][byte-budget][inline-image]") {
    CacheDefaults restore;
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    cache.set_byte_budget(16 * kMiB);
    cache.set_inline_byte_budget(16 * kMiB);

    // The same 24 MiB image as the path-pool case: the inline pool's rule is
    // unchanged by the path pool's keep-newest rule.
    for (int frame = 0; frame < 2; ++frame)
        REQUIRE(cache.get_or_build_inline("backdrop", nullptr, 0,
                                          [] { return make_image(2048, 3072); }) != nullptr);
    const auto stats = cache.inline_stats();
    CHECK(stats.over_budget == 2);
    CHECK(stats.builds == 2);
    CHECK(stats.size == 0);
    CHECK(stats.bytes == 0);
}

TEST_CASE("A failed build is remembered for the retry window",
          "[canvas][image-cache][failure-memo]") {
    CacheDefaults restore;
    ManualClock clock;
    auto& cache = ImageFileCache::instance();
    cache.set_enabled(true);
    const bool inline_pool = GENERATE(false, true);
    CAPTURE(inline_pool);

    int calls = 0;
    bool succeed = false;
    auto build = [&]() -> sk_sp<SkImage> {
        ++calls;
        return succeed ? make_dummy_image() : nullptr;
    };
    auto lookup = [&] {
        return inline_pool ? cache.get_or_build_inline("corrupt", nullptr, 16, build)
                           : cache.get_or_build("/tmp/corrupt.png", nullptr, build);
    };
    cache.clear();
    cache.reset_stats();

    // A second of frames at 60 Hz builds the failing key once.
    for (int frame = 0; frame < 60; ++frame)
        CHECK(lookup() == nullptr);
    CHECK(calls == 1);

    // After the retry window the key builds again, and fails again.
    clock.advance(std::chrono::milliseconds(1100));
    CHECK(lookup() == nullptr);
    CHECK(calls == 2);
    CHECK(lookup() == nullptr);
    CHECK(calls == 2);

    // clear() drops the memo.
    cache.clear();
    CHECK(lookup() == nullptr);
    CHECK(calls == 3);

    // A build that succeeds after the window is cached; the next lookup hits.
    succeed = true;
    clock.advance(std::chrono::milliseconds(1100));
    CHECK(lookup() != nullptr);
    CHECK(calls == 4);
    CHECK(lookup() != nullptr);
    CHECK(calls == 4);
}

TEST_CASE("clear_backend drops only that backend's failure records",
          "[canvas][image-cache][failure-memo]") {
    CacheDefaults restore;
    ManualClock clock;
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    const void* gpu_a = reinterpret_cast<const void*>(0xFA11);
    const void* gpu_b = reinterpret_cast<const void*>(0xFA12);

    int calls = 0;
    auto failing = [&]() -> sk_sp<SkImage> {
        ++calls;
        return nullptr;
    };
    for (const void* backend : {gpu_a, gpu_b}) {
        cache.get_or_build("/tmp/broken.png", backend, failing);
        cache.get_or_build_inline("broken", backend, 16, failing);
    }
    REQUIRE(calls == 4);

    cache.clear_backend(gpu_a);
    for (const void* backend : {gpu_a, gpu_b}) {
        cache.get_or_build("/tmp/broken.png", backend, failing);
        cache.get_or_build_inline("broken", backend, 16, failing);
    }
    CHECK(calls == 6); // only backend A's two keys built again
}

TEST_CASE("File images drawn every frame stay cached when a frame exceeds the byte budget",
          "[canvas][image-cache][byte-budget]") {
    CacheDefaults restore;
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    // Four 6 MiB layers (30% of the budget each) drawn in the same order every
    // frame, the shape of an imported panel's full-frame chrome and canvases.
    cache.set_byte_budget(20 * kMiB);
    const void* backend = reinterpret_cast<const void*>(0xF8A3E);
    auto build = [] { return make_image(1024, 1536); };

    for (int frame = 0; frame < 10; ++frame) {
        cache.begin_frame(backend);
        for (int layer = 0; layer < 4; ++layer)
            REQUIRE(cache.get_or_build("/tmp/layer-" + std::to_string(layer) + ".png", backend,
                                       build) != nullptr);
    }
    const auto stats = cache.stats();
    CHECK(stats.builds == 4);
    CHECK(stats.hits == 36);
    CHECK(stats.size == 4);
    CHECK(stats.bytes == 24 * kMiB); // over budget for the frame, not evicting it
}

TEST_CASE("File images not drawn this frame are evicted to the byte budget",
          "[canvas][image-cache][byte-budget]") {
    CacheDefaults restore;
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    cache.set_byte_budget(16 * kMiB);
    const void* backend = reinterpret_cast<const void*>(0xF8A3F);
    auto build = [] { return make_image(2048, 1024); }; // 8 MiB

    cache.begin_frame(backend);
    cache.get_or_build("/tmp/a.png", backend, build);
    cache.get_or_build("/tmp/b.png", backend, build);
    // The next frame draws b and a new c: a, from the earlier frame only,
    // is evicted and the pool is back within its budget.
    cache.begin_frame(backend);
    cache.get_or_build("/tmp/b.png", backend, build);
    cache.get_or_build("/tmp/c.png", backend, build);
    auto stats = cache.stats();
    CHECK(stats.size == 2);
    CHECK(stats.bytes == 16 * kMiB);
    CHECK(stats.builds == 3);
    cache.get_or_build("/tmp/b.png", backend, build);
    CHECK(cache.stats().builds == 3);
    cache.get_or_build("/tmp/a.png", backend, build);
    CHECK(cache.stats().builds == 4);

    // A frame that drew three images overshoots; the next frame's first
    // build trims what that frame no longer draws.
    cache.begin_frame(backend);
    for (const char* path : {"/tmp/a.png", "/tmp/b.png", "/tmp/c.png"})
        cache.get_or_build(path, backend, build);
    CHECK(cache.stats().bytes == 24 * kMiB);
    cache.begin_frame(backend);
    cache.get_or_build("/tmp/d.png", backend, build);
    CHECK(cache.stats().size == 2);
    CHECK(cache.stats().bytes == 16 * kMiB);
}

TEST_CASE("A failed file build is retried as soon as the file changes",
          "[canvas][image-cache][failure-memo]") {
    CacheDefaults restore;
    ManualClock clock;
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    const auto path =
        (std::filesystem::temp_directory_path() / "pulp-image-cache-memo-test.png").string();
    std::filesystem::remove(path);

    // Stands in for a decode: a missing file or one reading "corrupt" fails.
    int calls = 0;
    auto build = [&]() -> sk_sp<SkImage> {
        ++calls;
        std::ifstream in(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return !in || bytes == "corrupt" ? nullptr : make_dummy_image();
    };
    auto write = [&](const std::string& bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << bytes;
    };

    CHECK(cache.get_or_build(path, nullptr, build) == nullptr); // missing
    CHECK(cache.get_or_build(path, nullptr, build) == nullptr);
    CHECK(calls == 1);

    write("corrupt"); // appears inside the window: retried at once
    CHECK(cache.get_or_build(path, nullptr, build) == nullptr);
    CHECK(calls == 2);
    CHECK(cache.get_or_build(path, nullptr, build) == nullptr);
    CHECK(calls == 2);

    write("a fixed image"); // fixed inside the window: retried and cached
    CHECK(cache.get_or_build(path, nullptr, build) != nullptr);
    CHECK(calls == 3);
    CHECK(cache.get_or_build(path, nullptr, build) != nullptr);
    CHECK(calls == 3);
    std::filesystem::remove(path);
}

TEST_CASE("CPU raster frames are independent of GPU backend frames",
          "[canvas][image-cache][byte-budget]") {
    CacheDefaults restore;
    auto& cache = ImageFileCache::instance();
    cache.clear();
    cache.reset_stats();
    cache.set_enabled(true);
    cache.set_byte_budget(20 * kMiB);
    const void* gpu = reinterpret_cast<const void*>(0xF8A40);
    auto build = [] { return make_image(1024, 1536); }; // 6 MiB

    // A GPU window starting its frames between the raster frame's draws does
    // not end the raster frame: the null key has its own generation.
    for (int frame = 0; frame < 10; ++frame) {
        cache.begin_frame(nullptr);
        for (int layer = 0; layer < 4; ++layer) {
            cache.begin_frame(gpu);
            cache.get_or_build("/tmp/raster-layer-" + std::to_string(layer) + ".png", nullptr,
                               build);
        }
    }
    CHECK(cache.stats().builds == 4);
    CHECK(cache.stats().hits == 36);
}

#endif // PULP_HAS_SKIA
