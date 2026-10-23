// Process-wide laid-out paragraph cache. See paragraph_cache.hpp for what the
// key covers and why the paint is applied at draw time instead.

#ifdef PULP_HAS_SKIA

#include "skia_paragraph_cache.hpp"

#include <bit>
#include <list>
#include <unordered_map>
#include <utility>

namespace pulp::canvas {

namespace {

std::size_t mix(std::size_t seed, std::size_t value) noexcept {
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
}

struct ParagraphKeyHash {
    std::size_t operator()(const ParagraphKey& k) const noexcept {
        std::size_t h = std::hash<std::string>{}(k.text);
        for (const auto& family : k.families)
            h = mix(h, std::hash<std::string>{}(family));
        h = mix(h, k.primary_typeface);
        h = mix(h, std::bit_cast<std::uint32_t>(k.size));
        h = mix(h, static_cast<std::size_t>(k.weight));
        h = mix(h, static_cast<std::size_t>(k.slant));
        h = mix(h, std::bit_cast<std::uint32_t>(k.letter_spacing));
        h = mix(h, k.ltr ? 1u : 0u);
        for (const auto& f : k.features)
            h = mix(mix(h, f.tag), f.value);
        h = mix(h, static_cast<std::size_t>(k.font_generation));
        return h;
    }
};

struct State {
    using Lru = std::list<ParagraphKey>;
    struct Slot {
        std::shared_ptr<CachedParagraph> entry;
        Lru::iterator position;
    };

    mutable std::mutex mutex;
    std::unordered_map<ParagraphKey, Slot, ParagraphKeyHash> map;
    Lru lru; // front = most recently used
    std::uint64_t generation = 0;
    bool enabled = true;
    std::size_t max_entries = ParagraphCache::kDefaultMaxEntries;
    std::size_t max_bytes = ParagraphCache::kDefaultMaxBytes;
    std::size_t bytes = 0;
    ParagraphCache::Stats stats;

    void erase(std::unordered_map<ParagraphKey, Slot, ParagraphKeyHash>::iterator it) {
        bytes -= it->second.entry->bytes;
        lru.erase(it->second.position);
        map.erase(it);
    }

    void evict_to_limits() {
        while (!lru.empty() && (map.size() > max_entries || bytes > max_bytes)) {
            erase(map.find(lru.back()));
            ++stats.evictions;
        }
    }

    // Entries from an older font generation can never be looked up again, so
    // drop them rather than wait for them to age out.
    void observe_generation(std::uint64_t now) {
        if (now == generation)
            return;
        stats.invalidations += map.size();
        map.clear();
        lru.clear();
        bytes = 0;
        generation = now;
    }
};

State& state() {
    static State s;
    return s;
}

} // namespace

// Entries are locked for use only after the map lock is released, so a
// thread waiting on a busy entry never blocks lookups.
ParagraphUse
find_or_build_paragraph(ParagraphKey key,
                        const std::function<PreparedParagraph(const ParagraphKey&)>& build) {
    auto& s = state();
    {
        std::shared_ptr<CachedParagraph> hit;
        {
            std::lock_guard lock(s.mutex);
            if (s.enabled) {
                s.observe_generation(key.font_generation);
                if (auto it = s.map.find(key); it != s.map.end()) {
                    s.lru.splice(s.lru.begin(), s.lru, it->second.position);
                    ++s.stats.hits;
                    hit = it->second.entry;
                }
            }
            if (!hit)
                ++s.stats.builds;
        }
        if (hit)
            return ParagraphUse(std::move(hit));
    }

    // Build outside the map lock: shaping takes the font collection's lock
    // and can be slow, and other threads' hits should not wait on it.
    auto entry = std::make_shared<CachedParagraph>();
    entry->prepared = build(key);
    if (!entry->prepared.paragraph)
        return ParagraphUse(std::move(entry));
    entry->bytes = ParagraphCache::estimated_entry_bytes(key.text.size());

    {
        std::lock_guard lock(s.mutex);
        const bool storable = s.enabled && key.font_generation == s.generation &&
                              entry->bytes <= s.max_bytes && s.max_entries > 0;
        if (storable) {
            if (auto it = s.map.find(key); it != s.map.end()) {
                entry = it->second.entry; // another thread built it first
            } else {
                s.lru.push_front(key);
                s.bytes += entry->bytes;
                s.map.emplace(std::move(key), State::Slot{entry, s.lru.begin()});
                s.evict_to_limits();
            }
        }
    }
    return ParagraphUse(std::move(entry));
}

ParagraphCache& ParagraphCache::instance() {
    static ParagraphCache cache;
    return cache;
}

ParagraphCache::Stats ParagraphCache::stats() const {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    Stats out = s.stats;
    out.entries = s.map.size();
    out.bytes = s.bytes;
    out.max_entries = s.max_entries;
    out.max_bytes = s.max_bytes;
    return out;
}

void ParagraphCache::reset_stats() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.stats = {};
}

void ParagraphCache::clear() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.map.clear();
    s.lru.clear();
    s.bytes = 0;
}

void ParagraphCache::set_enabled(bool enabled) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.enabled = enabled;
    if (!enabled) {
        s.map.clear();
        s.lru.clear();
        s.bytes = 0;
    }
}

bool ParagraphCache::enabled() const {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.enabled;
}

void ParagraphCache::set_limits(std::size_t max_entries, std::size_t max_bytes) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.max_entries = max_entries;
    s.max_bytes = max_bytes;
    s.evict_to_limits();
}

std::size_t ParagraphCache::estimated_entry_bytes(std::size_t text_bytes) noexcept {
    constexpr std::size_t kFixedBytes = 8 * 1024;
    constexpr std::size_t kBytesPerTextByte = 512;
    return kFixedBytes + text_bytes * kBytesPerTextByte;
}

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
