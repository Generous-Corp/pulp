// box_shadow_cache.cpp — process-wide LRU cache of rasterized outset box-shadow
// coverage, plus a test-only render helper. See box_shadow_cache.hpp.

#ifdef PULP_HAS_SKIA

#include "include/core/SkAlphaType.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <pulp/canvas/box_shadow_cache.hpp>
#include <pulp/canvas/skia_canvas.hpp>

#include <atomic>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pulp::canvas {

namespace {
struct BoxShadowKeyHash {
    std::size_t operator()(const BoxShadowKey& k) const noexcept {
        std::uint64_t h = 1469598103934665603ull;  // FNV-1a over the 8 ints
        const std::int32_t fields[8] = {k.w, k.h, k.dx, k.dy,
                                        k.blur, k.spread, k.corner_radius, k.scale};
        for (std::int32_t f : fields) {
            h ^= static_cast<std::uint32_t>(f);
            h *= 1099511628211ull;
        }
        return static_cast<std::size_t>(h);
    }
};
}  // namespace

struct BoxShadowCache::Impl {
    std::mutex mutex;
    std::list<BoxShadowKey> lru;  // front = most recently used
    std::unordered_map<BoxShadowKey,
                       std::pair<sk_sp<SkImage>, std::list<BoxShadowKey>::iterator>,
                       BoxShadowKeyHash>
        map;
    std::size_t capacity = 128;
    bool enabled = true;
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> renders{0};
};

BoxShadowCache::Impl& BoxShadowCache::impl() {
    static Impl s_impl;
    return s_impl;
}

BoxShadowCache& BoxShadowCache::instance() {
    static BoxShadowCache s_cache;
    return s_cache;
}

sk_sp<SkImage> BoxShadowCache::get_or_render(
    const BoxShadowKey& key, const std::function<sk_sp<SkImage>()>& render) {
    Impl& d = impl();
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        if (d.enabled) {
            auto it = d.map.find(key);
            if (it != d.map.end()) {
                d.lru.splice(d.lru.begin(), d.lru, it->second.second);
                d.hits.fetch_add(1, std::memory_order_relaxed);
                return it->second.first;
            }
        }
    }

    // Render outside the lock — the blur is the expensive part and another
    // painter thread should not block on it.
    sk_sp<SkImage> image = render();
    d.renders.fetch_add(1, std::memory_order_relaxed);

    if (d.enabled && image) {
        std::lock_guard<std::mutex> lock(d.mutex);
        auto existing = d.map.find(key);
        if (existing != d.map.end()) {
            existing->second.first = image;
            d.lru.splice(d.lru.begin(), d.lru, existing->second.second);
        } else {
            d.lru.push_front(key);
            d.map.emplace(key, std::make_pair(image, d.lru.begin()));
            while (d.map.size() > d.capacity && !d.lru.empty()) {
                const BoxShadowKey& victim = d.lru.back();
                d.map.erase(victim);
                d.lru.pop_back();
            }
        }
    }
    return image;
}

BoxShadowCache::Stats BoxShadowCache::stats() const {
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    return Stats{d.hits.load(std::memory_order_relaxed),
                 d.renders.load(std::memory_order_relaxed), d.map.size()};
}

void BoxShadowCache::reset_stats() {
    Impl& d = impl();
    d.hits.store(0, std::memory_order_relaxed);
    d.renders.store(0, std::memory_order_relaxed);
}

void BoxShadowCache::clear() {
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.map.clear();
    d.lru.clear();
}

void BoxShadowCache::set_enabled(bool enabled) {
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.enabled = enabled;
}

bool BoxShadowCache::enabled() const {
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    return d.enabled;
}

void BoxShadowCache::set_capacity(std::size_t capacity) {
    Impl& d = impl();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.capacity = capacity == 0 ? 1 : capacity;
}

std::vector<std::uint8_t> render_box_shadow_to_rgba(
    int px_w, int px_h, float scale,
    float x, float y, float w, float h,
    float dx, float dy, float blur, float spread,
    Color color, float corner_radius, bool inset) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(px_w) * px_h * 4, 0);
    if (px_w <= 0 || px_h <= 0) return pixels;

    SkImageInfo info = SkImageInfo::Make(px_w, px_h, kRGBA_8888_SkColorType,
                                         kUnpremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    if (!surface) return pixels;

    SkCanvas* sk = surface->getCanvas();
    sk->clear(SK_ColorTRANSPARENT);
    if (scale != 1.0f) sk->scale(scale, scale);

    {
        SkiaCanvas canvas(sk);
        canvas.draw_box_shadow(x, y, w, h, dx, dy, blur, spread, color, inset,
                               corner_radius);
    }

    surface->readPixels(info, pixels.data(),
                        static_cast<std::size_t>(px_w) * 4, 0, 0);
    return pixels;
}

}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
