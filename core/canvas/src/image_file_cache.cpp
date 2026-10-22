// Process-wide decoded/GPU-uploaded file-image cache. See
// image_file_cache.hpp for the design rationale and the deliberate
// path-keyed (not content-keyed) semantic change.

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/image_file_cache.hpp>

#include "include/core/SkImage.h"

#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulp::canvas {

namespace {

// Key = (path hash, path length, backend token). The length + a stored copy of
// the path defend against a hash collision handing back the wrong image; the
// backend token keeps GPU-context-bound textures from leaking across contexts.
struct ImageKey {
    std::uint64_t hash = 0;
    std::size_t length = 0;
    const void* backend = nullptr;
    bool operator==(const ImageKey& o) const noexcept {
        return hash == o.hash && length == o.length && backend == o.backend;
    }
};
struct ImageKeyHash {
    std::size_t operator()(const ImageKey& k) const noexcept {
        std::uint64_t h = k.hash ^ (k.length * 1099511628211ull);
        h ^= reinterpret_cast<std::uintptr_t>(k.backend) * 1099511628211ull;
        return static_cast<std::size_t>(h);
    }
};
std::uint64_t fnv1a(const std::string& s) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// One LRU pool. The path pool is bounded by entry count; the inline pool is
// bounded by bytes, so many distinct inline images cannot evict file images
// or pin unbounded encoded copies and textures.
struct Pool {
    std::mutex mutex;
    std::list<ImageKey> lru;  // front = most recently used
    struct Entry {
        std::string path;  // stored for collision defense
        sk_sp<SkImage> image;
        std::list<ImageKey>::iterator lru_it;
        std::size_t cost = 0;
    };
    std::unordered_map<ImageKey, Entry, ImageKeyHash> map;
    // Keys currently being decoded/uploaded. Keeping this separate from map
    // lets concurrent painters wait for one build instead of duplicating the
    // expensive decode and GPU upload.
    std::unordered_set<ImageKey, ImageKeyHash> building;
    std::condition_variable build_cv;
    std::size_t capacity;
    std::size_t byte_budget; // 0 = bounded by entry count only
    std::size_t bytes = 0;
    bool enabled = true;
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> builds{0};
    std::atomic<std::uint64_t> over_budget{0};

    Pool(std::size_t cap, std::size_t budget) : capacity(cap), byte_budget(budget) {}

    void erase(std::unordered_map<ImageKey, Entry, ImageKeyHash>::iterator it) {
        bytes -= it->second.cost;
        lru.erase(it->second.lru_it);
        map.erase(it);
    }

    bool fits(std::size_t cost) const {
        return byte_budget == 0 || cost <= byte_budget;
    }

    sk_sp<SkImage> get_or_build(const std::string& path, const void* backend_key,
                                std::size_t encoded_cost,
                                const std::function<sk_sp<SkImage>()>& build);
    void clear();
    void clear_backend(const void* backend_key);
    void set_enabled(bool on);
};

// Bytes an image holds once built: its pixels, decoded or uploaded.
std::size_t image_cost(const SkImage& image) {
    return image.imageInfo().computeMinByteSize();
}

sk_sp<SkImage> Pool::get_or_build(const std::string& path, const void* backend_key,
                                  std::size_t encoded_cost,
                                  const std::function<sk_sp<SkImage>()>& build) {
    const ImageKey key{fnv1a(path), path.size(), backend_key};
    bool cache_this_build = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            // Snapshot enabled while holding the mutex. The old post-build
            // read raced with set_enabled().
            if (!enabled)
                break;
            auto it = map.find(key);
            if (it != map.end() && it->second.path == path) {
                lru.splice(lru.begin(), lru, it->second.lru_it);
                hits.fetch_add(1, std::memory_order_relaxed);
                return it->second.image;
            }
            // Bytes that already exceed the budget cannot be retained; build
            // uncached without claiming the key.
            if (!fits(encoded_cost)) {
                over_budget.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            // Another painter is decoding/uploading this exact key. Wait for
            // it and retry; a failed build removes the marker and lets us take
            // over, while disabling the cache falls through to an uncached
            // build.
            if (building.find(key) == building.end()) {
                building.insert(key);
                cache_this_build = true;
                break;
            }
            build_cv.wait(lock, [&] {
                return !enabled || map.find(key) != map.end() ||
                       building.find(key) == building.end();
            });
        }
    }

    // Build outside the lock — decodes/uploads are expensive. At most one
    // build for a cache-enabled key is in flight at a time.
    sk_sp<SkImage> image;
    try {
        image = build();
    } catch (...) {
        if (cache_this_build) {
            std::lock_guard<std::mutex> lock(mutex);
            building.erase(key);
            build_cv.notify_all();
        }
        throw;
    }
    builds.fetch_add(1, std::memory_order_relaxed);

    if (cache_this_build) {
        std::lock_guard<std::mutex> lock(mutex);
        building.erase(key);
        // Re-check enabled under the mutex before publishing. If disabled
        // while the build was running, do not retain the result.
        const std::size_t cost = image ? encoded_cost + image_cost(*image) : 0;
        if (enabled && image && !fits(cost)) {
            over_budget.fetch_add(1, std::memory_order_relaxed);
        } else if (enabled && image) {
            auto existing = map.find(key);
            if (existing != map.end())
                erase(existing);
            lru.push_front(key);
            map.emplace(key, Entry{path, image, lru.begin(), cost});
            bytes += cost;
            while ((map.size() > capacity || (byte_budget != 0 && bytes > byte_budget)) &&
                   lru.size() > 1)
                erase(map.find(lru.back()));
        }
        build_cv.notify_all();
    }
    return image;
}

void Pool::clear() {
    std::unique_lock<std::mutex> lock(mutex);
    // Do not let an in-flight decode repopulate the cache after invalidation.
    build_cv.wait(lock, [&] { return building.empty(); });
    map.clear();
    lru.clear();
    bytes = 0;
}

void Pool::clear_backend(const void* backend_key) {
    std::unique_lock<std::mutex> lock(mutex);
    // Context teardown must wait until all uploads for this backend have
    // finished; otherwise a builder could publish a stale image after this
    // invalidation returns and after the context has been destroyed.
    build_cv.wait(lock, [&] {
        for (const auto& key : building) {
            if (key.backend == backend_key)
                return false;
        }
        return true;
    });
    for (auto it = map.begin(); it != map.end();) {
        if (it->first.backend == backend_key) {
            auto victim = it++;
            erase(victim);
        } else {
            ++it;
        }
    }
}

void Pool::set_enabled(bool on) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        enabled = on;
    }
    // A disabled cache lets waiters take the uncached path. Wake them now
    // instead of making them wait for an unrelated in-flight build to finish.
    build_cv.notify_all();
}

} // namespace

struct ImageFileCache::Impl {
    Pool files{32, 0}; // a handful of distinct file images per frame
    Pool inline_bytes{1024, ImageFileCache::kDefaultInlineByteBudget};
};

ImageFileCache::Impl& ImageFileCache::impl() {
    static Impl s_impl;
    return s_impl;
}

ImageFileCache& ImageFileCache::instance() {
    static ImageFileCache s_cache;
    return s_cache;
}

sk_sp<SkImage> ImageFileCache::get_or_build(const std::string& path, const void* backend_key,
                                            const std::function<sk_sp<SkImage>()>& build) {
    return impl().files.get_or_build(path, backend_key, 0, build);
}

sk_sp<SkImage> ImageFileCache::get_or_build_inline(const std::string& content_key,
                                                   const void* backend_key,
                                                   std::size_t encoded_size,
                                                   const std::function<sk_sp<SkImage>()>& build) {
    return impl().inline_bytes.get_or_build(content_key, backend_key, encoded_size, build);
}

ImageFileCache::Stats ImageFileCache::stats() const {
    Pool& d = impl().files;
    std::lock_guard<std::mutex> lock(d.mutex);
    return Stats{d.hits.load(std::memory_order_relaxed), d.builds.load(std::memory_order_relaxed),
                 d.map.size()};
}

ImageFileCache::InlineStats ImageFileCache::inline_stats() const {
    Pool& d = impl().inline_bytes;
    std::lock_guard<std::mutex> lock(d.mutex);
    return InlineStats{d.hits.load(std::memory_order_relaxed),
                       d.builds.load(std::memory_order_relaxed),
                       d.over_budget.load(std::memory_order_relaxed), d.map.size(), d.bytes};
}

void ImageFileCache::reset_stats() {
    for (Pool* d : {&impl().files, &impl().inline_bytes}) {
        d->hits.store(0, std::memory_order_relaxed);
        d->builds.store(0, std::memory_order_relaxed);
        d->over_budget.store(0, std::memory_order_relaxed);
    }
}

void ImageFileCache::clear() {
    impl().files.clear();
    impl().inline_bytes.clear();
}

void ImageFileCache::clear_backend(const void* backend_key) {
    if (!backend_key)
        return;
    impl().files.clear_backend(backend_key);
    impl().inline_bytes.clear_backend(backend_key);
}

void ImageFileCache::set_enabled(bool enabled) {
    impl().files.set_enabled(enabled);
    impl().inline_bytes.set_enabled(enabled);
}

bool ImageFileCache::enabled() const {
    Pool& d = impl().files;
    std::lock_guard<std::mutex> lock(d.mutex);
    return d.enabled;
}

void ImageFileCache::set_capacity(std::size_t capacity) {
    Pool& d = impl().files;
    std::lock_guard<std::mutex> lock(d.mutex);
    d.capacity = capacity == 0 ? 1 : capacity;
}

void ImageFileCache::set_inline_byte_budget(std::size_t bytes) {
    Pool& d = impl().inline_bytes;
    std::lock_guard<std::mutex> lock(d.mutex);
    d.byte_budget = bytes == 0 ? 1 : bytes;
    while (d.bytes > d.byte_budget && !d.lru.empty())
        d.erase(d.map.find(d.lru.back()));
}

std::size_t ImageFileCache::inline_byte_budget() const {
    Pool& d = impl().inline_bytes;
    std::lock_guard<std::mutex> lock(d.mutex);
    return d.byte_budget;
}

}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
