#include <pulp/canvas/sdf_atlas_cache.hpp>

namespace pulp::canvas {

bool SdfAtlasCache::initialize(const std::string& font_family,
                               const std::vector<char32_t>& initial_chars,
                               int base_size,
                               int padding,
                               int max_atlas_size) {
    font_family_ = font_family;
    base_size_   = base_size;
    padding_     = padding;
    max_size_    = max_atlas_size;
    entries_.clear();

    if (!atlas_.build(font_family, initial_chars,
                      base_size, padding, max_atlas_size)) {
        return false;
    }
    for (auto c : initial_chars) {
        const SdfGlyph* g = atlas_.glyph(c);
        if (!g) continue;
        CachedGlyph ce;
        ce.glyph = *g;
        ce.frame_last_used = current_frame_;
        ce.dirty = true;
        entries_.emplace(c, ce);
    }
    return true;
}

const CachedGlyph* SdfAtlasCache::touch(char32_t codepoint) {
    auto it = entries_.find(codepoint);
    if (it != entries_.end()) {
        it->second.frame_last_used = current_frame_;
        return &it->second;
    }
    // Glyph is not in the atlas. The current `SdfAtlas` builds are
    // one-shot, so a runtime insert requires a rebuild. Signal "miss"
    // so the caller can schedule a background rebuild with the expanded
    // glyph set; the SDF draw path can fall back to Skia path rendering
    // for the frame in which the miss occurs.
    return nullptr;
}

bool SdfAtlasCache::ensure(char32_t codepoint) {
    if (entries_.find(codepoint) != entries_.end()) {
        return true;  // already resident
    }
    // Rebuild the atlas with the union of cached codepoints + the new one.
    std::vector<char32_t> all;
    all.reserve(entries_.size() + 1);
    for (const auto& kv : entries_) all.push_back(kv.first);
    all.push_back(codepoint);

    SdfAtlas rebuilt;
    if (!rebuilt.build(font_family_, all, base_size_, padding_, max_size_)) {
        return false;
    }
    atlas_ = std::move(rebuilt);

    // Reseed the entries map against the rebuilt glyph layout, preserving
    // prior frame_last_used values so LRU ordering is intact. Every glyph
    // is marked dirty because the rebuilt atlas texture is a new allocation.
    std::unordered_map<char32_t, CachedGlyph> next;
    next.reserve(all.size());
    for (auto c : all) {
        const SdfGlyph* g = atlas_.glyph(c);
        if (!g) continue;
        CachedGlyph ce;
        ce.glyph = *g;
        auto prior = entries_.find(c);
        ce.frame_last_used =
            prior != entries_.end() ? prior->second.frame_last_used : current_frame_;
        ce.dirty = true;
        next.emplace(c, ce);
    }
    entries_ = std::move(next);
    return entries_.find(codepoint) != entries_.end();
}

std::size_t SdfAtlasCache::evict_older_than(std::uint64_t max_age_frames) {
    std::size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        const auto age = current_frame_ - it->second.frame_last_used;
        if (age > max_age_frames) {
            it = entries_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

}  // namespace pulp::canvas
