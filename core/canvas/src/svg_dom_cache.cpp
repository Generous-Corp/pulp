// svg_dom_cache.cpp — process-wide LRU cache of parsed SVG documents
// (SkSVGDOM), keyed on the exact document bytes. See svg_dom_cache.hpp.

#ifdef PULP_HAS_SKIA

#include "include/core/SkStream.h"
#include "include/core/SkSize.h"
#include "modules/svg/include/SkSVGDOM.h"

#include <pulp/canvas/svg_dom_cache.hpp>

#include <atomic>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace pulp::canvas {

namespace {
// Identity of a cached document: a 64-bit FNV-1a hash of the bytes plus the
// length. The full source string is stored alongside the DOM so a hash
// collision is resolved by an exact compare before a hit is honored — a wrong
// DOM for a faithful frame would be a visible corruption, so we never trust the
// hash alone.
struct SvgKey {
    std::uint64_t hash = 0;
    std::size_t length = 0;
    bool operator==(const SvgKey& o) const noexcept {
        return hash == o.hash && length == o.length;
    }
};
struct SvgKeyHash {
    std::size_t operator()(const SvgKey& k) const noexcept {
        return static_cast<std::size_t>(k.hash ^ (k.length * 1099511628211ull));
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

struct SvgDomCache::Impl {
    std::mutex mutex;
    std::list<SvgKey> lru;  // front = most recently used
    // value = (source bytes for collision check, parsed DOM, lru position)
    struct Entry {
        std::string source;
        sk_sp<SkSVGDOM> dom;
        std::list<SvgKey>::iterator lru_it;
    };
    std::unordered_map<SvgKey, Entry, SvgKeyHash> map;
    std::size_t capacity = 16;  // a handful of frames' worth of distinct docs
    bool enabled = true;
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> builds{0};
};

SvgDomCache::Impl& SvgDomCache::impl() {
    static Impl s_impl;
    return s_impl;
}

SvgDomCache& SvgDomCache::instance() {
    static SvgDomCache s_cache;
    return s_cache;
}

sk_sp<SkSVGDOM> SvgDomCache::get_or_build(const std::string& svg_document) {
    return get_or_build(svg_document, [&] {
        // copyData=false: the stream borrows the caller's string only for the
        // duration of make(), which fully materializes the DOM.
        SkMemoryStream stream(svg_document.data(), svg_document.size(),
                              /*copyData=*/false);
        return SkSVGDOM::Builder().make(stream);
    });
}

sk_sp<SkSVGDOM> SvgDomCache::get_or_build(
    const std::string& svg_document,
    const std::function<sk_sp<SkSVGDOM>()>& build) {
    Impl& d = impl();
    const SvgKey key{fnv1a(svg_document), svg_document.size()};
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        if (d.enabled) {
            auto it = d.map.find(key);
            // Honor a hit only when the stored bytes match exactly (defends
            // against a hash collision producing the wrong DOM).
            if (it != d.map.end() && it->second.source == svg_document) {
                d.lru.splice(d.lru.begin(), d.lru, it->second.lru_it);
                d.hits.fetch_add(1, std::memory_order_relaxed);
                return it->second.dom;
            }
        }
    }

    // Build outside the lock — the parse is the expensive part and another
    // painter thread should not block on it.
    sk_sp<SkSVGDOM> dom = build();
    d.builds.fetch_add(1, std::memory_order_relaxed);

    if (d.enabled && dom) {
        std::lock_guard<std::mutex> lock(d.mutex);
        auto existing = d.map.find(key);
        if (existing != d.map.end() && existing->second.source == svg_document) {
            existing->second.dom = dom;
            d.lru.splice(d.lru.begin(), d.lru, existing->second.lru_it);
        } else {
            // Either a fresh key or a colliding key from a different document;
            // in the collision case the prior entry's iterator is replaced, so
            // erase any stale mapping first to avoid a dangling lru iterator.
            if (existing != d.map.end()) {
                d.lru.erase(existing->second.lru_it);
                d.map.erase(existing);
            }
            d.lru.push_front(key);
            d.map.emplace(key, Impl::Entry{svg_document, dom, d.lru.begin()});
            while (d.map.size() > d.capacity && !d.lru.empty()) {
                const SvgKey victim = d.lru.back();
                d.map.erase(victim);
                d.lru.pop_back();
            }
        }
    }
    return dom;
}

SvgDomCache::Stats SvgDomCache::stats() const {
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    return Stats{d.hits.load(std::memory_order_relaxed),
                 d.builds.load(std::memory_order_relaxed), d.map.size()};
}

void SvgDomCache::reset_stats() {
    Impl& d = impl();
    d.hits.store(0, std::memory_order_relaxed);
    d.builds.store(0, std::memory_order_relaxed);
}

void SvgDomCache::clear() {
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.map.clear();
    d.lru.clear();
}

void SvgDomCache::set_enabled(bool enabled) {
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.enabled = enabled;
}

bool SvgDomCache::enabled() const {
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    return d.enabled;
}

void SvgDomCache::set_capacity(std::size_t capacity) {
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.capacity = capacity == 0 ? 1 : capacity;
}

}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
