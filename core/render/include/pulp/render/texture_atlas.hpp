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
    bool allocate(int w, int h, Region& out);

    void reset() { shelf_y_ = 0; shelf_h_ = 0; shelf_x_ = 0; }
    int width() const { return width_; }
    int height() const { return height_; }

    /// Area of the atlas surface (width × height), in texels.
    /// Read-only introspection — used by the inspector's atlas viewer.
    std::size_t capacity() const {
        if (width_ <= 0 || height_ <= 0) return 0;
        return static_cast<std::size_t>(width_) *
               static_cast<std::size_t>(height_);
    }

    /// Texels consumed by the shelf allocator so far. This is the
    /// shelf-packed *footprint* — every completed shelf counts its full
    /// height across the atlas width, plus the in-progress shelf counts
    /// up to its current write cursor. It is an upper bound on the live
    /// pixel area (shelf packing leaves gaps), which is exactly the
    /// number that answers "how close is this atlas to full?".
    std::size_t used_area() const {
        if (width_ <= 0 || height_ <= 0 ||
            (shelf_y_ <= 0 && shelf_x_ <= 0)) {
            return 0;
        }
        const std::size_t completed =
            static_cast<std::size_t>(shelf_y_) *
            static_cast<std::size_t>(width_);
        const std::size_t in_progress =
            static_cast<std::size_t>(shelf_x_) *
            static_cast<std::size_t>(shelf_h_);
        return completed + in_progress;
    }

    /// Fraction of the atlas the shelf allocator has consumed, in
    /// [0, 1]. Returns 0 for a degenerate (zero-area) atlas.
    float occupancy() const {
        const std::size_t cap = capacity();
        if (cap == 0) return 0.0f;
        return static_cast<float>(used_area()) / static_cast<float>(cap);
    }

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
    bool allocate(uint64_t key, int w, int h, AtlasPacker::Region& out);

    void release(uint64_t key);

    /// Evict entries not used since the given frame.
    size_t evict_stale(uint64_t current_frame, uint64_t max_age = 120);

    void mark_used(uint64_t key, uint64_t frame);

    size_t entry_count() const { return entries_.size(); }

    /// Read-only introspection for the inspector's atlas viewer.
    int width() const { return packer_.width(); }
    int height() const { return packer_.height(); }
    float occupancy() const { return packer_.occupancy(); }

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

    bool allocate(uint64_t key, int& row);

    void mark_used(uint64_t key, uint64_t frame);

    size_t evict_stale(uint64_t current_frame, uint64_t max_age = 120);

    size_t entry_count() const { return entries_.size(); }

    /// Read-only introspection for the inspector's atlas viewer. A
    /// GradientAtlas is row-packed (one ramp per row) rather than
    /// shelf-packed, so occupancy is simply allocated rows over the
    /// row budget.
    int row_capacity() const { return max_rows_; }
    int rows_used() const { return next_row_; }
    float occupancy() const {
        if (max_rows_ <= 0) return 0.0f;
        return static_cast<float>(next_row_) / static_cast<float>(max_rows_);
    }

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

    bool allocate(uint64_t glyph_key, int w, int h, AtlasPacker::Region& out);

    void mark_used(uint64_t key, uint64_t frame);

    size_t evict_stale(uint64_t current_frame, uint64_t max_age = 300);

    size_t entry_count() const { return entries_.size(); }

    /// Read-only introspection for the inspector's atlas viewer.
    int width() const { return packer_.width(); }
    int height() const { return packer_.height(); }
    float occupancy() const { return packer_.occupancy(); }

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

    bool allocate(uint64_t path_hash, int w, int h, AtlasPacker::Region& out);

    void mark_used(uint64_t key, uint64_t frame);

    size_t evict_stale(uint64_t current_frame, uint64_t max_age = 600);

    size_t entry_count() const { return entries_.size(); }

    /// Read-only introspection for the inspector's atlas viewer.
    int width() const { return packer_.width(); }
    int height() const { return packer_.height(); }
    float occupancy() const { return packer_.occupancy(); }

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
