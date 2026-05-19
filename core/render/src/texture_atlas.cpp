#include <pulp/render/texture_atlas.hpp>

#include <algorithm>

namespace pulp::render {

// ---------------------------------------------------------------------------
// AtlasPacker
// ---------------------------------------------------------------------------

bool AtlasPacker::allocate(int w, int h, Region& out) {
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

// ---------------------------------------------------------------------------
// ImageAtlas
// ---------------------------------------------------------------------------

bool ImageAtlas::allocate(uint64_t key, int w, int h, AtlasPacker::Region& out) {
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

void ImageAtlas::release(uint64_t key) {
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second.ref_count > 0) {
        --it->second.ref_count;
    }
}

size_t ImageAtlas::evict_stale(uint64_t current_frame, uint64_t max_age) {
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

void ImageAtlas::mark_used(uint64_t key, uint64_t frame) {
    auto it = entries_.find(key);
    if (it != entries_.end()) it->second.last_used_frame = frame;
}

// ---------------------------------------------------------------------------
// GradientAtlas
// ---------------------------------------------------------------------------

bool GradientAtlas::allocate(uint64_t key, int& row) {
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

void GradientAtlas::mark_used(uint64_t key, uint64_t frame) {
    auto it = entries_.find(key);
    if (it != entries_.end()) it->second.last_used = frame;
}

size_t GradientAtlas::evict_stale(uint64_t current_frame, uint64_t max_age) {
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

// ---------------------------------------------------------------------------
// GlyphAtlas
// ---------------------------------------------------------------------------

bool GlyphAtlas::allocate(uint64_t glyph_key, int w, int h, AtlasPacker::Region& out) {
    auto it = entries_.find(glyph_key);
    if (it != entries_.end()) { out = it->second.region; return true; }
    if (!packer_.allocate(w, h, out)) return false;
    entries_[glyph_key] = {out, 0};
    return true;
}

void GlyphAtlas::mark_used(uint64_t key, uint64_t frame) {
    auto it = entries_.find(key);
    if (it != entries_.end()) it->second.last_used = frame;
}

size_t GlyphAtlas::evict_stale(uint64_t current_frame, uint64_t max_age) {
    size_t evicted = 0;
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if (current_frame > it->second.last_used &&
            current_frame - it->second.last_used > max_age) {
            it = entries_.erase(it); ++evicted;
        } else { ++it; }
    }
    return evicted;
}

// ---------------------------------------------------------------------------
// PathAtlas
// ---------------------------------------------------------------------------

bool PathAtlas::allocate(uint64_t path_hash, int w, int h, AtlasPacker::Region& out) {
    auto it = entries_.find(path_hash);
    if (it != entries_.end()) { out = it->second.region; return true; }
    if (!packer_.allocate(w, h, out)) return false;
    entries_[path_hash] = {out, 0};
    return true;
}

void PathAtlas::mark_used(uint64_t key, uint64_t frame) {
    auto it = entries_.find(key);
    if (it != entries_.end()) it->second.last_used = frame;
}

size_t PathAtlas::evict_stale(uint64_t current_frame, uint64_t max_age) {
    size_t evicted = 0;
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if (current_frame > it->second.last_used &&
            current_frame - it->second.last_used > max_age) {
            it = entries_.erase(it); ++evicted;
        } else { ++it; }
    }
    return evicted;
}

} // namespace pulp::render
