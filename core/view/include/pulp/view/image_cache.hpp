#pragma once

// Image cache — workstream 07 slice 7.4.
//
// The plan calls out `ImageView` as `partial` because it renders a
// placeholder instead of decoding actual bytes. The real Skia decode
// path lives behind the existing PULP_HAS_SKIA gate; this slice adds
// the cache that both Skia-backed and stub paths will consume. A
// memory-pressure trim hook (workstream 05 slice 5.3) lets the host
// evict everything rebuildable when the OS asks.
//
// Keys:
//   string URI ("file:///…", "resource://…", "memory://sha256=…")
//   combined with mtime/size for file URIs so a rebuilt image
//   invalidates automatically.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::view {

struct DecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    /// Platform-owned handle. Opaque — consumers pass it back to the
    /// draw path. Cleared on eviction.
    void* native_handle = nullptr;
    /// Best-effort estimate of the resident memory this entry costs
    /// (e.g. width * height * 4 for RGBA8). Used by the LRU eviction
    /// when the total cache exceeds the soft cap.
    std::size_t bytes = 0;
};

struct ImageCacheStats {
    std::size_t entry_count = 0;
    std::size_t total_bytes = 0;
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;
};

/// Pluggable decode callback. Returns nullopt on error. Separates the
/// cache from the Skia code path so unit tests drive it with a fake
/// decoder.
using ImageDecodeFn = std::function<std::optional<DecodedImage>(const std::string& uri)>;

/// Release the platform handle when an entry is evicted. Cache invokes
/// this on LRU trim + clear(). May be called from any thread.
using ImageReleaseFn = std::function<void(DecodedImage&)>;

class ImageCache {
public:
    ImageCache() = default;

    void set_decoder(ImageDecodeFn fn) { decoder_ = std::move(fn); }
    void set_releaser(ImageReleaseFn fn) { releaser_ = std::move(fn); }

    /// Soft byte cap. When insertions push `total_bytes` past this,
    /// LRU entries are evicted until it fits. A cap of 0 disables
    /// trimming (useful in tests).
    void set_byte_budget(std::size_t bytes) { budget_ = bytes; try_trim_(); }

    /// Look up (and decode if absent) the image at `uri`. Returns
    /// nullptr on decode failure.
    const DecodedImage* get(const std::string& uri);

    /// Drop everything rebuildable. Wired to
    /// Processor::on_memory_pressure(Critical) at the host layer.
    void clear();

    ImageCacheStats stats() const;

private:
    struct Entry {
        DecodedImage image;
        uint64_t last_used = 0;
    };

    void evict_one_();
    void try_trim_();

    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> entries_;
    ImageDecodeFn decoder_;
    ImageReleaseFn releaser_;
    std::size_t total_bytes_ = 0;
    std::size_t budget_ = 0;
    uint64_t tick_ = 0;
    ImageCacheStats stats_{};
};

}  // namespace pulp::view
