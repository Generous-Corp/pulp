// svg_dom_cache.cpp — process-wide LRU cache of parsed SVG documents
// (SkSVGDOM), keyed on the exact document bytes. See svg_dom_cache.hpp.

#ifdef PULP_HAS_SKIA

#include "include/core/SkStream.h"
#include "include/core/SkSize.h"
#include "modules/svg/include/SkSVGDOM.h"

#ifdef PULP_HAS_SKRESOURCES
#include "include/codec/SkCodec.h"
#include "include/codec/SkGifDecoder.h"
#include "include/codec/SkJpegDecoder.h"
#include "include/codec/SkPngDecoder.h"
#include "include/codec/SkWebpDecoder.h"
// SkResources.h only forward-declares SkFontMgr, but
// DataURIResourceProviderProxy::Make defaults its `sk_sp<const SkFontMgr>`
// argument to nullptr — instantiating that temporary needs the complete type.
#include "include/core/SkFontMgr.h"
#include "modules/skresources/include/SkResources.h"
#endif

#include <pulp/canvas/svg_dom_cache.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
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

// Skia's SVG parser only recognizes the XLink form of the href attribute: the
// literal name it matches is "xlink:href" (SkSVGImage / SkSVGUse /
// SkSVGFeImage). SVG 2 deprecated that in favor of a bare `href`, and that is
// what every modern design-tool export emits — Figma's, for one, writes
// `<image href="data:image/png;base64,...">` with no xlink namespace at all.
// Skia silently drops the attribute, the IRI stays empty, and the element
// renders blank no matter how good the resource provider is.
//
// Rewrite the bare attribute NAME to the XLink spelling before parsing. Only
// inside a tag, never inside an attribute value or text content, and never on a
// tag that already carries an `xlink:href` (that would duplicate the
// attribute). The document bytes the cache is keyed on stay untouched — this
// runs inside the build step.
//
// Skia's XML reader does not do namespace resolution (it matches the prefixed
// name as a literal string), so the rewritten document does not need an
// xmlns:xlink declaration to parse.
std::string normalize_svg_hrefs(const std::string& doc) {
    static constexpr char kBare[] = "href";
    static constexpr std::size_t kBareLen = 4;

    // Is `pos` the start of a bare `href` attribute NAME? It must be preceded
    // by a name delimiter (not ':' — that is the xlink: form — and not another
    // name character), and followed by '=' (allowing whitespace).
    auto is_bare_href_name = [&](std::size_t pos) {
        if (doc.compare(pos, kBareLen, kBare) != 0) return false;
        if (pos == 0) return false;
        const char prev = doc[pos - 1];
        const bool name_char = std::isalnum(static_cast<unsigned char>(prev)) ||
                               prev == ':' || prev == '-' || prev == '_';
        if (name_char) return false;
        std::size_t after = pos + kBareLen;
        while (after < doc.size() &&
               std::isspace(static_cast<unsigned char>(doc[after])))
            ++after;
        return after < doc.size() && doc[after] == '=';
    };

    std::string out;
    out.reserve(doc.size() + 64);

    std::size_t i = 0;
    while (i < doc.size()) {
        if (doc[i] != '<') {
            out.push_back(doc[i++]);
            continue;
        }
        // Span the whole tag, honoring quoted attribute values (a '>' inside a
        // value does not end the tag).
        std::size_t end = i + 1;
        char quote = '\0';
        while (end < doc.size()) {
            const char c = doc[end];
            if (quote != '\0') {
                if (c == quote) quote = '\0';
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                break;
            }
            ++end;
        }
        const std::size_t tag_end = std::min(end, doc.size() - 1);
        const std::string_view tag(doc.data() + i, tag_end - i + 1);

        // A tag that already spells it xlink:href needs no rewrite (and must
        // not get a duplicate attribute).
        if (tag.find("xlink:href") == std::string_view::npos) {
            std::size_t j = i;
            char q = '\0';
            while (j <= tag_end) {
                const char c = doc[j];
                if (q != '\0') {
                    if (c == q) q = '\0';
                    out.push_back(c);
                    ++j;
                    continue;
                }
                if (c == '"' || c == '\'') {
                    q = c;
                    out.push_back(c);
                    ++j;
                    continue;
                }
                if (c == 'h' && is_bare_href_name(j)) {
                    out.append("xlink:href");
                    j += kBareLen;
                    continue;
                }
                out.push_back(c);
                ++j;
            }
        } else {
            out.append(tag);
        }
        i = tag_end + 1;
    }
    return out;
}

#ifdef PULP_HAS_SKRESOURCES
// SkSVGDOM resolves <image href="..."> through a skresources::ResourceProvider.
// Skia's builder defaults it to NULL, and SkSVGImage::LoadImage bails out on a
// null provider — so every <image> element, including the base64 data URIs a
// Figma / design-tool SVG export uses for its bitmap assets, rendered BLANK.
// DataURIResourceProviderProxy base64-decodes `data:` hrefs into an SkImage;
// it forwards anything else to its inner provider, which is null here (Pulp
// deliberately does not let an SVG document pull external files off disk or the
// network — only self-contained documents resolve).
//
// The decode itself goes through SkCodec, whose codec registry is EMPTY until
// something calls SkCodecs::Register (Skia m119+ dropped the built-in list), so
// register the formats the bundled libskia.a actually defines before the first
// document is parsed.
sk_sp<skresources::ResourceProvider> svg_resource_provider() {
    static const sk_sp<skresources::ResourceProvider> provider = [] {
        SkCodecs::Register(SkPngDecoder::Decoder());
        SkCodecs::Register(SkJpegDecoder::Decoder());
        SkCodecs::Register(SkWebpDecoder::Decoder());
        SkCodecs::Register(SkGifDecoder::Decoder());
        // kPreDecode: the DOM is cached and replayed every frame, so decoding
        // once at parse time keeps the paint path off the codec.
        return sk_sp<skresources::ResourceProvider>(
            skresources::DataURIResourceProviderProxy::Make(
                /*rp=*/nullptr, skresources::ImageDecodeStrategy::kPreDecode));
    }();
    return provider;
}
#endif  // PULP_HAS_SKRESOURCES
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
        // Rewrite SVG-2 `href` to the `xlink:href` Skia's parser matches; see
        // normalize_svg_hrefs(). Keyed on the ORIGINAL bytes — the rewrite is
        // an implementation detail of the build step.
        const std::string parsed = normalize_svg_hrefs(svg_document);
        // copyData=false: the stream borrows `parsed` only for the duration of
        // make(), which fully materializes the DOM.
        SkMemoryStream stream(parsed.data(), parsed.size(),
                              /*copyData=*/false);
        SkSVGDOM::Builder builder;
#ifdef PULP_HAS_SKRESOURCES
        // Without this, <image href="data:image/png;base64,..."> never resolves
        // and renders blank. See svg_resource_provider().
        builder.setResourceProvider(svg_resource_provider());
#endif
        return builder.make(stream);
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
