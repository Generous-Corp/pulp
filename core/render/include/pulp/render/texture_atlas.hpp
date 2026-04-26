#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace pulp::render {

/// Simple rectangle packer for atlas allocation.
/// Uses a shelf-based algorithm (row-by-row packing).
class AtlasPacker {
public:
    struct Region {
        int x = 0, y = 0, w = 0, h = 0;
    };

    explicit AtlasPacker(int width, int height)
        : width_(width), height_(height), shelf_y_(0), shelf_h_(0), shelf_x_(0) {}

    /// Try to allocate a region of the given size. Returns false if full.
    bool allocate(int w, int h, Region& out) {
        if (w <= 0 || h <= 0 || w > width_ || h > height_) return false;

        // Try current shelf
        if (shelf_x_ + w <= width_ && shelf_y_ + std::max(h, shelf_h_) <= height_) {
            out = {shelf_x_, shelf_y_, w, h};
            shelf_x_ += w;
            shelf_h_ = std::max(shelf_h_, h);
            return true;
        }

        // Start new shelf
        shelf_y_ += shelf_h_;
        shelf_x_ = 0;
        shelf_h_ = 0;

        if (shelf_x_ + w <= width_ && shelf_y_ + h <= height_) {
            out = {shelf_x_, shelf_y_, w, h};
            shelf_x_ += w;
            shelf_h_ = h;
            return true;
        }

        return false; // Atlas full
    }

    void reset() { shelf_y_ = 0; shelf_h_ = 0; shelf_x_ = 0; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    int width_, height_;
    int shelf_y_, shelf_h_, shelf_x_;
};

/// Image atlas — packs small images into a shared GPU texture.
/// Images under the size threshold are packed; larger ones get their own texture.
class ImageAtlas {
public:
    explicit ImageAtlas(int size = 2048) : packer_(size, size) {}

    struct Entry {
        AtlasPacker::Region region;
        uint64_t ref_count = 1;
        uint64_t last_used_frame = 0;
    };

    /// Try to allocate space for an image. Returns the region, or empty on failure.
    bool allocate(uint64_t key, int w, int h, AtlasPacker::Region& out) {
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            it->second.ref_count++;
            out = it->second.region;
            return true;
        }
        if (!packer_.allocate(w, h, out)) return false;
        entries_[key] = {out, 1, 0};
        return true;
    }

    void release(uint64_t key) {
        auto it = entries_.find(key);
        if (it != entries_.end() && it->second.ref_count > 0) {
            --it->second.ref_count;
        }
    }

    /// Evict entries not used since the given frame.
    size_t evict_stale(uint64_t current_frame, uint64_t max_age = 120) {
        size_t evicted = 0;
        for (auto it = entries_.begin(); it != entries_.end(); ) {
            if (it->second.ref_count == 0 &&
                current_frame > it->second.last_used_frame &&
                current_frame - it->second.last_used_frame > max_age) {
                it = entries_.erase(it);
                ++evicted;
            } else {
                ++it;
            }
        }
        return evicted;
    }

    void mark_used(uint64_t key, uint64_t frame) {
        auto it = entries_.find(key);
        if (it != entries_.end()) it->second.last_used_frame = frame;
    }

    size_t entry_count() const { return entries_.size(); }

private:
    AtlasPacker packer_;
    std::unordered_map<uint64_t, Entry> entries_;
};

/// Gradient atlas — caches evaluated gradient ramps as GPU textures.
/// Gradients are keyed by a hash of their stops/colors.
class GradientAtlas {
public:
    struct Entry {
        int row = 0;         ///< Row in the atlas texture
        uint64_t last_used = 0;
    };

    bool has(uint64_t key) const { return entries_.find(key) != entries_.end(); }

    bool allocate(uint64_t key, int& row) {
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            row = it->second.row;
            return true;
        }
        if (next_row_ >= max_rows_) return false;
        row = next_row_++;
        entries_[key] = {row, 0};
        return true;
    }

    void mark_used(uint64_t key, uint64_t frame) {
        auto it = entries_.find(key);
        if (it != entries_.end()) it->second.last_used = frame;
    }

    size_t evict_stale(uint64_t current_frame, uint64_t max_age = 120) {
        size_t evicted = 0;
        for (auto it = entries_.begin(); it != entries_.end(); ) {
            if (current_frame > it->second.last_used &&
                current_frame - it->second.last_used > max_age) {
                it = entries_.erase(it);
                ++evicted;
            } else {
                ++it;
            }
        }
        return evicted;
    }

    size_t entry_count() const { return entries_.size(); }

private:
    std::unordered_map<uint64_t, Entry> entries_;
    int next_row_ = 0;
    int max_rows_ = 512;
};

/// Glyph atlas — per-font-size glyph cache as GPU texture.
/// Skia manages its own internal glyph cache via SkStrike, so this atlas
/// is a supplementary cache for custom text rendering paths (e.g., SDF text).
class GlyphAtlas {
public:
    struct Entry {
        AtlasPacker::Region region;
        uint64_t last_used = 0;
    };

    explicit GlyphAtlas(int size = 1024) : packer_(size, size) {}

    bool allocate(uint64_t glyph_key, int w, int h, AtlasPacker::Region& out) {
        auto it = entries_.find(glyph_key);
        if (it != entries_.end()) { out = it->second.region; return true; }
        if (!packer_.allocate(w, h, out)) return false;
        entries_[glyph_key] = {out, 0};
        return true;
    }

    void mark_used(uint64_t key, uint64_t frame) {
        auto it = entries_.find(key);
        if (it != entries_.end()) it->second.last_used = frame;
    }

    size_t evict_stale(uint64_t current_frame, uint64_t max_age = 300) {
        size_t evicted = 0;
        for (auto it = entries_.begin(); it != entries_.end(); ) {
            if (current_frame > it->second.last_used &&
                current_frame - it->second.last_used > max_age) {
                it = entries_.erase(it); ++evicted;
            } else { ++it; }
        }
        return evicted;
    }

    size_t entry_count() const { return entries_.size(); }

private:
    AtlasPacker packer_;
    std::unordered_map<uint64_t, Entry> entries_;
};

/// Path atlas — caches rasterized vector paths (e.g., complex SVG icons).
/// Paths are keyed by a hash of path data + transform.
class PathAtlas {
public:
    struct Entry {
        AtlasPacker::Region region;
        uint64_t last_used = 0;
    };

    explicit PathAtlas(int size = 2048) : packer_(size, size) {}

    bool allocate(uint64_t path_hash, int w, int h, AtlasPacker::Region& out) {
        auto it = entries_.find(path_hash);
        if (it != entries_.end()) { out = it->second.region; return true; }
        if (!packer_.allocate(w, h, out)) return false;
        entries_[path_hash] = {out, 0};
        return true;
    }

    void mark_used(uint64_t key, uint64_t frame) {
        auto it = entries_.find(key);
        if (it != entries_.end()) it->second.last_used = frame;
    }

    size_t evict_stale(uint64_t current_frame, uint64_t max_age = 600) {
        size_t evicted = 0;
        for (auto it = entries_.begin(); it != entries_.end(); ) {
            if (current_frame > it->second.last_used &&
                current_frame - it->second.last_used > max_age) {
                it = entries_.erase(it); ++evicted;
            } else { ++it; }
        }
        return evicted;
    }

    size_t entry_count() const { return entries_.size(); }

private:
    AtlasPacker packer_;
    std::unordered_map<uint64_t, Entry> entries_;
};

/// Buffer pool — reuses std::vector allocations in hot rendering paths.
template <typename T>
class BufferPool {
public:
    std::vector<T> acquire() {
        if (!pool_.empty()) {
            auto v = std::move(pool_.back());
            pool_.pop_back();
            v.clear();
            return v;
        }
        return {};
    }

    void release(std::vector<T> v) {
        if (pool_.size() < max_pool_size_) {
            pool_.push_back(std::move(v));
        }
    }

    size_t pool_size() const { return pool_.size(); }

private:
    std::vector<std::vector<T>> pool_;
    size_t max_pool_size_ = 32;
};

} // namespace pulp::render
