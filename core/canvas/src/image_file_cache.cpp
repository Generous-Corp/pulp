// Process-wide decoded/GPU-uploaded file-image cache. See
// image_file_cache.hpp for the design rationale and the deliberate
// path-keyed (not content-keyed) semantic change.

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/image_file_cache.hpp>

#include "include/core/SkImage.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <limits>
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

constexpr std::size_t kSaturated = std::numeric_limits<std::size_t>::max();

// Failure records kept per pool before expired ones are pruned.
constexpr std::size_t kMaxFailureRecords = 256;

// The time source both pools read for the failure memo. Tests replace it so
// the retry window can be crossed without sleeping.
struct Clock {
    std::mutex mutex;
    std::function<std::chrono::steady_clock::time_point()> override_now;

    std::chrono::steady_clock::time_point now() {
        std::lock_guard<std::mutex> lock(mutex);
        return override_now ? override_now() : std::chrono::steady_clock::now();
    }
};

// Per-backend frame generations, advanced by ImageFileCache::begin_frame.
// An entry stamped with its backend's current generation was drawn in the
// frame being painted. A backend that never announces frames stays at 0, and
// none of its entries count as drawn this frame.
struct Frames {
    std::mutex mutex;
    std::unordered_map<const void*, std::uint64_t> generation;

    std::uint64_t current(const void* backend) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = generation.find(backend);
        return it == generation.end() ? 0 : it->second;
    }
};

// What an over-budget build does to the pool.
enum class OverBudget {
    build_uncached, // return it, retain nothing (the inline pool)
    keep_newest,    // retain it, evicting only entries not drawn this frame (the path pool)
};

// What a file looked like when a build of it failed. A failure is retried as
// soon as the file changes, so an asset that is written or fixed shows on the
// next repaint rather than after the retry window.
struct FileSignature {
    bool exists = false;
    std::filesystem::file_time_type mtime{};
    std::uintmax_t size = 0;
    bool operator==(const FileSignature&) const = default;
};

// Synthetic keys (embedded data, inline digests) start with NUL and name no file.
bool names_a_file(const std::string& key) {
    return !key.empty() && key.front() != '\0';
}

FileSignature file_signature(const std::string& path) {
    FileSignature sig;
    std::error_code ec;
    const std::filesystem::path fs_path(path);
    sig.size = std::filesystem::file_size(fs_path, ec);
    if (ec)
        return FileSignature{};
    sig.mtime = std::filesystem::last_write_time(fs_path, ec);
    if (ec)
        return FileSignature{};
    sig.exists = true;
    return sig;
}

// One LRU pool, bounded by entry count and bytes. The pools are separate so
// many distinct inline images cannot evict file images or pin unbounded
// encoded copies and textures.
struct Pool {
    std::mutex mutex;
    std::list<ImageKey> lru;  // front = most recently used
    struct Entry {
        std::string path;  // stored for collision defense
        sk_sp<SkImage> image;
        std::list<ImageKey>::iterator lru_it;
        std::size_t cost = 0;
        std::uint64_t frame = 0; // backend frame generation of the last draw
    };
    std::unordered_map<ImageKey, Entry, ImageKeyHash> map;
    // Keys currently being decoded/uploaded. Keeping this separate from map
    // lets concurrent painters wait for one build instead of duplicating the
    // expensive decode and GPU upload.
    std::unordered_set<ImageKey, ImageKeyHash> building;
    std::condition_variable build_cv;
    // Keys whose last build returned null, with when it failed. A lookup
    // inside the retry window returns null without building, so a corrupt or
    // missing asset is not decoded again on every paint.
    struct Failure {
        std::string path; // stored for collision defense
        std::chrono::steady_clock::time_point at;
        FileSignature file; // the file at failure time, when the key names one
    };
    std::unordered_map<ImageKey, Failure, ImageKeyHash> failures;
    Clock& clock;
    Frames& frames;
    std::size_t capacity;
    std::size_t byte_budget;
    OverBudget over_budget_rule;
    std::size_t bytes = 0;
    bool enabled = true;
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> builds{0};
    std::atomic<std::uint64_t> over_budget{0};

    Pool(Clock& c, Frames& f, std::size_t cap, std::size_t budget, OverBudget rule)
        : clock(c), frames(f), capacity(cap), byte_budget(budget), over_budget_rule(rule) {}

    // Failures in the path pool are keyed on the file's state as well as time.
    bool tracks_files() const {
        return over_budget_rule == OverBudget::keep_newest;
    }

    void erase(std::unordered_map<ImageKey, Entry, ImageKeyHash>::iterator it) {
        bytes -= it->second.cost;
        lru.erase(it->second.lru_it);
        map.erase(it);
    }

    // A saturated cost (SIZE_MAX) never fits: it means the true size is
    // unknown and at least as large as anything the budget could hold.
    bool fits(std::size_t cost) const {
        return cost != kSaturated && cost <= byte_budget;
    }

    // Whether a built image of @p cost is retained. Under keep_newest an
    // image larger than the whole budget is still kept, so a large backdrop
    // drawn every frame is not decoded and uploaded every frame. A saturated
    // cost is never kept: its true size is unknown.
    bool retains(std::size_t cost) const {
        return fits(cost) || (over_budget_rule == OverBudget::keep_newest && cost != kSaturated);
    }

    bool drawn_this_frame(const ImageKey& key, const Entry& entry) const {
        const std::uint64_t generation = frames.current(key.backend);
        return generation != 0 && entry.frame == generation;
    }

    // Evict least recently used entries until the pool is within its limits.
    // The inline pool evicts strictly by recency. The path pool keeps its most
    // recent entry, and never evicts for bytes an entry drawn in the frame
    // being painted: a frame whose images exceed the budget overshoots it
    // instead of evicting the image it will draw next, which would decode and
    // upload every image on every frame. The entry cap still applies.
    void evict_to_limits() {
        if (over_budget_rule == OverBudget::build_uncached) {
            while ((map.size() > capacity || bytes > byte_budget) && !lru.empty())
                erase(map.find(lru.back()));
            return;
        }
        auto it = lru.end();
        while ((map.size() > capacity || bytes > byte_budget) && it != lru.begin()) {
            --it;
            if (it == lru.begin())
                break;
            auto entry = map.find(*it);
            if (map.size() <= capacity && drawn_this_frame(entry->first, entry->second))
                continue;
            it = std::next(it);
            erase(entry);
        }
    }

    bool failed_recently(const ImageKey& key, const std::string& path,
                         std::chrono::steady_clock::time_point now) {
        auto it = failures.find(key);
        if (it == failures.end() || it->second.path != path)
            return false;
        if (now - it->second.at < ImageFileCache::kFailureRetryDelay &&
            (!tracks_files() || !names_a_file(path) || file_signature(path) == it->second.file))
            return true;
        failures.erase(it);
        return false;
    }

    void record_failure(const ImageKey& key, const std::string& path,
                        std::chrono::steady_clock::time_point now, const FileSignature& file) {
        if (failures.size() >= kMaxFailureRecords) {
            for (auto it = failures.begin(); it != failures.end();) {
                if (now - it->second.at >= ImageFileCache::kFailureRetryDelay)
                    it = failures.erase(it);
                else
                    ++it;
            }
            if (failures.size() >= kMaxFailureRecords)
                failures.erase(failures.begin());
        }
        failures.insert_or_assign(key, Failure{path, now, file});
    }

    sk_sp<SkImage> get_or_build(const std::string& path, const void* backend_key,
                                std::size_t encoded_cost,
                                const std::function<sk_sp<SkImage>()>& build);
    void clear();
    void clear_backend(const void* backend_key);
    void set_enabled(bool on);
};

// Bytes an image holds once built: its pixels, decoded or uploaded. Skia
// reports SIZE_MAX when the size does not fit in size_t.
std::size_t image_cost(const SkImage& image) {
    return image.imageInfo().computeMinByteSize();
}

// Entry costs add an encoded size to a pixel size; clamp at SIZE_MAX rather
// than wrap to a small value that would admit a huge entry.
std::size_t saturating_add(std::size_t a, std::size_t b) {
    return a > kSaturated - b ? kSaturated : a + b;
}

sk_sp<SkImage> Pool::get_or_build(const std::string& path, const void* backend_key,
                                  std::size_t encoded_cost,
                                  const std::function<sk_sp<SkImage>()>& build) {
    const ImageKey key{fnv1a(path), path.size(), backend_key};
    bool cache_this_build = false;
    const auto lookup_time = clock.now();
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
                it->second.frame = frames.current(backend_key);
                hits.fetch_add(1, std::memory_order_relaxed);
                return it->second.image;
            }
            // A build of this key failed inside the retry window (possibly
            // the one this painter just waited for): do not decode it again.
            if (failed_recently(key, path, lookup_time))
                return nullptr;
            // Bytes that already exceed the budget cannot be retained; build
            // uncached without claiming the key.
            if (!retains(encoded_cost)) {
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

    // A failed build is remembered even when it was not claimed (an encoded
    // size over the budget), so the memo covers every enabled lookup.
    if (cache_this_build || !image) {
        const auto failure_time = image ? lookup_time : clock.now();
        const FileSignature failed_file =
            !image && tracks_files() && names_a_file(path) ? file_signature(path) : FileSignature{};
        const std::uint64_t frame = frames.current(backend_key);
        std::lock_guard<std::mutex> lock(mutex);
        if (cache_this_build)
            building.erase(key);
        // Re-check enabled under the mutex before publishing. If disabled
        // while the build was running, do not retain the result.
        const std::size_t cost = image ? saturating_add(encoded_cost, image_cost(*image)) : 0;
        if (enabled && !image) {
            record_failure(key, path, failure_time, failed_file);
        } else if (enabled && cache_this_build && !retains(cost)) {
            over_budget.fetch_add(1, std::memory_order_relaxed);
        } else if (enabled && cache_this_build) {
            failures.erase(key);
            auto existing = map.find(key);
            if (existing != map.end())
                erase(existing);
            lru.push_front(key);
            map.emplace(key, Entry{path, image, lru.begin(), cost, frame});
            bytes = saturating_add(bytes, cost);
            evict_to_limits();
        }
        if (cache_this_build)
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
    failures.clear();
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
    for (auto it = failures.begin(); it != failures.end();) {
        if (it->first.backend == backend_key)
            it = failures.erase(it);
        else
            ++it;
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
    Clock clock;
    Frames frames;
    // A handful of distinct file images per frame.
    Pool files{clock, frames, 32, ImageFileCache::kDefaultFileByteBudget, OverBudget::keep_newest};
    Pool inline_bytes{clock, frames, 1024, ImageFileCache::kDefaultInlineByteBudget,
                      OverBudget::build_uncached};
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
                 d.map.size(), d.bytes};
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
    Frames& f = impl().frames;
    std::lock_guard<std::mutex> lock(f.mutex);
    f.generation.clear();
}

void ImageFileCache::clear_backend(const void* backend_key) {
    if (!backend_key)
        return;
    impl().files.clear_backend(backend_key);
    impl().inline_bytes.clear_backend(backend_key);
    Frames& f = impl().frames;
    std::lock_guard<std::mutex> lock(f.mutex);
    f.generation.erase(backend_key);
}

void ImageFileCache::begin_frame(const void* backend_key) {
    Frames& f = impl().frames;
    std::lock_guard<std::mutex> lock(f.mutex);
    ++f.generation[backend_key];
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

void ImageFileCache::set_byte_budget(std::size_t bytes) {
    Pool& d = impl().files;
    std::lock_guard<std::mutex> lock(d.mutex);
    d.byte_budget = bytes == 0 ? 1 : bytes;
    d.evict_to_limits();
}

std::size_t ImageFileCache::byte_budget() const {
    Pool& d = impl().files;
    std::lock_guard<std::mutex> lock(d.mutex);
    return d.byte_budget;
}

void ImageFileCache::set_inline_byte_budget(std::size_t bytes) {
    Pool& d = impl().inline_bytes;
    std::lock_guard<std::mutex> lock(d.mutex);
    d.byte_budget = bytes == 0 ? 1 : bytes;
    d.evict_to_limits();
}

std::size_t ImageFileCache::inline_byte_budget() const {
    Pool& d = impl().inline_bytes;
    std::lock_guard<std::mutex> lock(d.mutex);
    return d.byte_budget;
}

void ImageFileCache::set_clock_for_testing(
    std::function<std::chrono::steady_clock::time_point()> now) {
    Clock& c = impl().clock;
    std::lock_guard<std::mutex> lock(c.mutex);
    c.override_now = std::move(now);
}

}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
