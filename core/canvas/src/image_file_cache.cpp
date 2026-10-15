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

}  // namespace

struct ImageFileCache::Impl {
    std::mutex mutex;
    std::list<ImageKey> lru;  // front = most recently used
    struct Entry {
        std::string path;  // stored for collision defense
        sk_sp<SkImage> image;
        std::list<ImageKey>::iterator lru_it;
    };
    std::unordered_map<ImageKey, Entry, ImageKeyHash> map;
    // Keys currently being decoded/uploaded. Keeping this separate from map
    // lets concurrent painters wait for one build instead of duplicating the
    // expensive decode and GPU upload.
    std::unordered_set<ImageKey, ImageKeyHash> building;
    std::condition_variable build_cv;
    std::size_t capacity = 32;  // a handful of distinct file images per frame
    bool enabled = true;
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> builds{0};
};

ImageFileCache::Impl& ImageFileCache::impl() {
    static Impl s_impl;
    return s_impl;
}

ImageFileCache& ImageFileCache::instance() {
    static ImageFileCache s_cache;
    return s_cache;
}

sk_sp<SkImage> ImageFileCache::get_or_build(
    const std::string& path,
    const void* backend_key,
    const std::function<sk_sp<SkImage>()>& build) {
    Impl& d = impl();
    const ImageKey key{fnv1a(path), path.size(), backend_key};
    bool cache_this_build = false;
    {
        std::unique_lock<std::mutex> lock(d.mutex);
        for (;;) {
            // Snapshot enabled while holding the mutex. The old post-build
            // read raced with set_enabled().
            if (!d.enabled)
                break;
            auto it = d.map.find(key);
            if (it != d.map.end() && it->second.path == path) {
                d.lru.splice(d.lru.begin(), d.lru, it->second.lru_it);
                d.hits.fetch_add(1, std::memory_order_relaxed);
                return it->second.image;
            }
            // Another painter is decoding/uploading this exact key. Wait for
            // it and retry; a failed build removes the marker and lets us take
            // over, while disabling the cache falls through to an uncached
            // build.
            if (d.building.find(key) == d.building.end()) {
                d.building.insert(key);
                cache_this_build = true;
                break;
            }
            d.build_cv.wait(lock, [&] {
                return !d.enabled || d.map.find(key) != d.map.end() ||
                       d.building.find(key) == d.building.end();
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
            std::lock_guard<std::mutex> lock(d.mutex);
            d.building.erase(key);
            d.build_cv.notify_all();
        }
        throw;
    }
    d.builds.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(d.mutex);
        // Re-check enabled under the mutex before publishing. If disabled
        // while the build was running, do not retain the result.
        if (cache_this_build) {
            d.building.erase(key);
            if (d.enabled && image) {
                auto existing = d.map.find(key);
                if (existing != d.map.end() && existing->second.path == path) {
                    existing->second.image = image;
                    d.lru.splice(d.lru.begin(), d.lru, existing->second.lru_it);
                } else {
                    if (existing != d.map.end()) {
                        d.lru.erase(existing->second.lru_it);
                        d.map.erase(existing);
                    }
                    d.lru.push_front(key);
                    d.map.emplace(key, Impl::Entry{path, image, d.lru.begin()});
                    while (d.map.size() > d.capacity && !d.lru.empty()) {
                        const ImageKey victim = d.lru.back();
                        d.map.erase(victim);
                        d.lru.pop_back();
                    }
                }
            }
            d.build_cv.notify_all();
        }
    }
    return image;
}

ImageFileCache::Stats ImageFileCache::stats() const {
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    return Stats{d.hits.load(std::memory_order_relaxed),
                 d.builds.load(std::memory_order_relaxed), d.map.size()};
}

void ImageFileCache::reset_stats() {
    Impl& d = impl();
    d.hits.store(0, std::memory_order_relaxed);
    d.builds.store(0, std::memory_order_relaxed);
}

void ImageFileCache::clear() {
    Impl& d = impl();
    std::unique_lock<std::mutex> lock(d.mutex);
    // Do not let an in-flight decode repopulate the cache after invalidation.
    d.build_cv.wait(lock, [&] { return d.building.empty(); });
    d.map.clear();
    d.lru.clear();
}

void ImageFileCache::clear_backend(const void* backend_key) {
    if (!backend_key)
        return;
    Impl& d = impl();
    std::unique_lock<std::mutex> lock(d.mutex);
    // Context teardown must wait until all uploads for this backend have
    // finished; otherwise a builder could publish a stale image after this
    // invalidation returns and after the context has been destroyed.
    d.build_cv.wait(lock, [&] {
        for (const auto& key : d.building) {
            if (key.backend == backend_key)
                return false;
        }
        return true;
    });
    for (auto it = d.map.begin(); it != d.map.end();) {
        if (it->first.backend == backend_key) {
            d.lru.erase(it->second.lru_it);
            it = d.map.erase(it);
        } else {
            ++it;
        }
    }
}

void ImageFileCache::set_enabled(bool enabled) {
    Impl& d = impl();
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.enabled = enabled;
    }
    // A disabled cache lets waiters take the uncached path. Wake them now
    // instead of making them wait for an unrelated in-flight build to finish.
    d.build_cv.notify_all();
}

bool ImageFileCache::enabled() const {
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    return d.enabled;
}

void ImageFileCache::set_capacity(std::size_t capacity) {
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.capacity = capacity == 0 ? 1 : capacity;
}

}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
