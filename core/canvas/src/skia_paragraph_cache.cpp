// Process-wide laid-out paragraph cache. See paragraph_cache.hpp for what the
// key covers, why the paint is applied at draw time instead, and how eviction
// keeps stable labels through text that changes every frame.

#ifdef PULP_HAS_SKIA

#include "skia_paragraph_cache.hpp"

#include <bit>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::canvas {

namespace {

std::size_t mix(std::size_t seed, std::size_t value) noexcept {
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
}

// Keys reach the map with canonical floats (make_paragraph folds -0 to +0 and
// replaces non-finite values), so equal keys hash equally.
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
        h = mix(h, k.hinting ? 1u : 0u);
        for (const auto& f : k.features)
            h = mix(mix(h, f.tag), f.value);
        h = mix(h, static_cast<std::size_t>(k.font_generation));
        return h;
    }
};

using Entry = std::shared_ptr<CachedParagraph>;

struct State {
    using Lru = std::list<ParagraphKey>; // front = most recently used
    struct Slot {
        Entry entry;
        bool is_protected = false;
        Lru::iterator position;
    };
    using Map = std::unordered_map<ParagraphKey, Slot, ParagraphKeyHash>;

    mutable std::mutex mutex;
    Map map;
    Lru probation;
    Lru protected_lru;
    std::size_t protected_bytes = 0;
    std::uint64_t generation = 0;
    bool enabled = true;
    std::size_t max_entries = ParagraphCache::kDefaultMaxEntries;
    std::size_t max_bytes = ParagraphCache::kDefaultMaxBytes;
    std::size_t bytes = 0;
    ParagraphCache::Stats stats;
    ParagraphCache::BuildObserver observer;
    // Entries dropped under the lock, destroyed by the caller after it
    // releases the lock (see Unlocked below).
    std::vector<Entry> graveyard;

    Lru& list_of(const Slot& slot) {
        return slot.is_protected ? protected_lru : probation;
    }

    // The protected segment keeps at most four fifths of either bound, so
    // probation always has room for a new entry to earn a second lookup.
    std::size_t protected_max_entries() const {
        return max_entries - max_entries / 5;
    }
    std::size_t protected_max_bytes() const {
        return max_bytes - max_bytes / 5;
    }

    void erase(Map::iterator it) {
        bytes -= it->second.entry->bytes;
        if (it->second.is_protected)
            protected_bytes -= it->second.entry->bytes;
        list_of(it->second).erase(it->second.position);
        graveyard.push_back(std::move(it->second.entry));
        map.erase(it);
    }

    void evict_to_limits() {
        while (map.size() > max_entries || bytes > max_bytes) {
            Lru& victims = probation.empty() ? protected_lru : probation;
            if (victims.empty())
                break;
            auto it = map.find(victims.back());
            if (it == map.end()) {
                // A list key the map cannot find would be a broken invariant;
                // drop it rather than erase through end().
                victims.pop_back();
                continue;
            }
            erase(it);
            ++stats.evictions;
        }
    }

    void demote_protected_overflow() {
        while (!protected_lru.empty() && (protected_lru.size() > protected_max_entries() ||
                                          protected_bytes > protected_max_bytes())) {
            auto it = map.find(protected_lru.back());
            if (it == map.end()) {
                protected_lru.pop_back();
                continue;
            }
            Slot& slot = it->second;
            protected_bytes -= slot.entry->bytes;
            probation.splice(probation.begin(), protected_lru, slot.position);
            slot.is_protected = false;
        }
    }

    void touch(Slot& slot) {
        if (slot.is_protected) {
            protected_lru.splice(protected_lru.begin(), protected_lru, slot.position);
            return;
        }
        protected_lru.splice(protected_lru.begin(), probation, slot.position);
        slot.is_protected = true;
        protected_bytes += slot.entry->bytes;
        demote_protected_overflow();
    }

    void drop_all() {
        for (auto& [key, slot] : map)
            graveyard.push_back(std::move(slot.entry));
        map.clear();
        probation.clear();
        protected_lru.clear();
        bytes = 0;
        protected_bytes = 0;
    }

    // Entries from an older font generation can never be looked up again, so
    // drop them rather than wait for them to age out. A lookup that read the
    // generation before a newer one arrived neither clears nor stores.
    void observe_generation(std::uint64_t now) {
        if (now <= generation)
            return;
        stats.invalidations += map.size();
        drop_all();
        generation = now;
    }
};

State& state() {
    static State s;
    return s;
}

// The map lock, plus the entries it dropped: they are destroyed after the
// lock is released, so freeing many paragraphs (a font registration drops
// all of them) never stalls another thread's lookup.
class Unlocked {
  public:
    explicit Unlocked(State& s) : s_(s), lock_(s.mutex) {}
    ~Unlocked() {
        std::vector<Entry> doomed;
        doomed.swap(s_.graveyard);
        lock_.unlock();
    }
    Unlocked(const Unlocked&) = delete;
    Unlocked& operator=(const Unlocked&) = delete;

  private:
    State& s_;
    std::unique_lock<std::mutex> lock_;
};

} // namespace

// Entries are locked for use only after the map lock is released, so a
// thread waiting on a busy entry never blocks lookups.
ParagraphUse
find_or_build_paragraph(ParagraphKey key,
                        const std::function<PreparedParagraph(const ParagraphKey&)>& build) {
    auto& s = state();
    ParagraphCache::BuildObserver observer;
    {
        Entry hit;
        {
            Unlocked guard(s);
            if (s.enabled) {
                s.observe_generation(key.font_generation);
                if (auto it = s.map.find(key); it != s.map.end()) {
                    s.touch(it->second);
                    ++s.stats.hits;
                    hit = it->second.entry;
                }
            }
            if (!hit) {
                ++s.stats.builds;
                observer = s.observer;
            }
        }
        if (hit)
            return ParagraphUse(std::move(hit));
    }

    // Build outside the map lock: shaping takes the font collection's lock
    // and can be slow, and other threads' hits should not wait on it.
    if (observer)
        observer(key.text);
    auto entry = std::make_shared<CachedParagraph>();
    entry->prepared = build(key);
    if (!entry->prepared.paragraph)
        return ParagraphUse(std::move(entry));
    entry->bytes = ParagraphCache::estimated_entry_bytes(key.text.size());

    {
        Unlocked guard(s);
        const bool storable = s.enabled && key.font_generation == s.generation &&
                              entry->bytes <= s.max_bytes && s.max_entries > 0;
        if (storable) {
            if (auto it = s.map.find(key); it != s.map.end()) {
                entry = it->second.entry; // another thread built it first
            } else {
                s.probation.push_front(key);
                s.bytes += entry->bytes;
                s.map.emplace(std::move(key), State::Slot{entry, false, s.probation.begin()});
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
    out.protected_entries = s.protected_lru.size();
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
    Unlocked guard(s);
    s.drop_all();
}

void ParagraphCache::set_enabled(bool enabled) {
    auto& s = state();
    Unlocked guard(s);
    s.enabled = enabled;
    if (!enabled)
        s.drop_all();
}

bool ParagraphCache::enabled() const {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.enabled;
}

void ParagraphCache::set_limits(std::size_t max_entries, std::size_t max_bytes) {
    auto& s = state();
    Unlocked guard(s);
    s.max_entries = max_entries;
    s.max_bytes = max_bytes;
    s.demote_protected_overflow();
    s.evict_to_limits();
}

void ParagraphCache::set_build_observer(BuildObserver observer) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.observer = std::move(observer);
}

// Measured with malloc-zone statistics on macOS arm64 (Inter 13px): about
// 8.4 KB for a 19-byte Latin label, 8.9 KB for 27 bytes of CJK, 13 KB for a
// 25-byte label with two emoji, 39 KB for 166 bytes of Latin. The fit is about
// 4.5 KB plus 210 bytes per text byte; these constants keep 10-70% headroom
// over every one of them.
std::size_t ParagraphCache::estimated_entry_bytes(std::size_t text_bytes) noexcept {
    constexpr std::size_t kFixedBytes = 9 * 1024;
    constexpr std::size_t kBytesPerTextByte = 224;
    return kFixedBytes + text_bytes * kBytesPerTextByte;
}

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
