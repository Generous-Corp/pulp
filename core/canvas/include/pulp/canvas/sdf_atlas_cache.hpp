#pragma once

// Runtime SDF atlas cache with LRU eviction.
//
// `SdfAtlas` builds a fixed atlas up-front; many UIs want a growing
// atlas that inserts glyphs lazily and drops least-recently-used
// glyphs when the texture budget is hit. `SdfAtlasCache` sits on top
// of `SdfAtlas` and tracks access recency per glyph so the GPU upload
// path can invalidate only the pages that changed.

#include <pulp/canvas/sdf_atlas.hpp>

#include <cstdint>
#include <list>
#include <unordered_map>

namespace pulp::canvas {

// Records a glyph's position in the cache and when it was last used.
// `frame_last_used` tracks frame count so the cache can evict glyphs
// that have not been drawn for N frames regardless of capacity.
struct CachedGlyph {
    SdfGlyph glyph;
    std::uint64_t frame_last_used = 0;
    bool          dirty = true;   // needs upload to GPU
};

class SdfAtlasCache {
public:
    SdfAtlasCache() = default;

    // Initialize the cache with a preallocated atlas for `initial_chars`
    // at `base_size`. Returns false if the initial build fails.
    bool initialize(const std::string& font_family,
                    const std::vector<char32_t>& initial_chars,
                    int base_size = 48,
                    int padding   = 8,
                    int max_atlas_size = 2048);

    // Touch a codepoint: update its frame_last_used, allocate space in
    // the atlas if the glyph is missing and there is room. Returns a
    // pointer to the cache record or nullptr when the atlas is full.
    const CachedGlyph* touch(char32_t codepoint);

    // Evict glyphs not used in the last `max_age_frames` frames. Returns
    // the number of glyphs removed.
    std::size_t evict_older_than(std::uint64_t max_age_frames);

    // Dynamic atlas growth: ensure `codepoint` has a slot in the atlas,
    // rebuilding with the union of currently cached codepoints + the
    // new one if necessary. Returns true when the codepoint is resident
    // on return (whether it was already present or newly added). Every
    // newly inserted glyph lands with `dirty = true` so the GPU upload
    // path knows to re-transfer the affected region.
    bool ensure(char32_t codepoint);

    // Advance the frame counter; call once per presented frame.
    void next_frame() { ++current_frame_; }

    std::uint64_t current_frame() const { return current_frame_; }
    std::size_t   size() const { return entries_.size(); }

    const SdfAtlas& atlas() const { return atlas_; }
    SdfAtlas&       atlas()       { return atlas_; }

private:
    SdfAtlas atlas_;
    std::unordered_map<char32_t, CachedGlyph> entries_;
    std::uint64_t current_frame_ = 0;
    std::string   font_family_;
    int           base_size_   = 48;
    int           padding_     = 8;
    int           max_size_    = 2048;
};

}  // namespace pulp::canvas
