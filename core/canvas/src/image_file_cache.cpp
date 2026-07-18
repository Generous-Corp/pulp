// Process-wide decoded/GPU-uploaded file-image cache. See
// image_file_cache.hpp for the design rationale and the deliberate
// path-keyed (not content-keyed) semantic change.

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/image_file_cache.hpp>

#include "include/core/SkImage.h"

#include <atomic>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
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
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        if (d.enabled) {
            auto it = d.map.find(key);
            // Honor a hit only when the stored path matches exactly (defends
            // against a hash collision producing the wrong image).
            if (it != d.map.end() && it->second.path == path) {
                d.lru.splice(d.lru.begin(), d.lru, it->second.lru_it);
                d.hits.fetch_add(1, std::memory_order_relaxed);
                return it->second.image;
            }
        }
    }

    // Build outside the lock — the decode + GPU upload is the expensive part
    // and another painter thread should not block on it.
    sk_sp<SkImage> image = build();
    d.builds.fetch_add(1, std::memory_order_relaxed);

    if (d.enabled && image) {
        std::lock_guard<std::mutex> lock(d.mutex);
        auto existing = d.map.find(key);
        if (existing != d.map.end() && existing->second.path == path) {
            existing->second.image = image;
            d.lru.splice(d.lru.begin(), d.lru, existing->second.lru_it);
        } else {
            // Either a fresh key or a colliding key from a different path; in
            // the collision case erase the stale mapping first to avoid a
            // dangling lru iterator.
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
    std::lock_guard<std::mutex> lock(d.mutex);
    d.map.clear();
    d.lru.clear();
}

void ImageFileCache::set_enabled(bool enabled) {
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.enabled = enabled;
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
