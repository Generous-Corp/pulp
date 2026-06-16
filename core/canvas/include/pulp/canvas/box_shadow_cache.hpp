#pragma once

/// @file box_shadow_cache.hpp
/// Process-wide cache for rasterized outset box-shadow coverage.
///
/// Drawing a CSS box shadow blurs a rounded rect every paint, which is wasteful
/// when the same shadow is painted frame after frame — a static shadow on a
/// repainting view, or a card that only translates or whose shadow color/opacity
/// animates. The blurred *shape* is origin-agnostic: it depends on the box size,
/// blur, spread, corner radius, shadow offset, and device scale, but not on the
/// box's screen position or the shadow color.
///
/// This cache stores the blurred white coverage image keyed on that
/// origin-agnostic fingerprint. The shadow path then:
///   * re-blits the cached coverage translated to the box position (free move),
///   * re-tints it with the shadow color via a SrcIn color filter (free
///     color/opacity change),
///   * only re-blurs when the geometry fingerprint changes.
///
/// Painting is on the UI thread (not the realtime audio thread), so a small
/// mutex is acceptable; the cache is process-global with an LRU cap. Disabled or
/// non-axis-aligned transforms fall back to the direct (uncached) shadow path,
/// so output is unchanged when caching cannot apply.

#ifdef PULP_HAS_SKIA

#include <cstddef>
#include <cstdint>
#include <functional>

#include "include/core/SkRefCnt.h"

class SkImage;

#include <pulp/canvas/canvas.hpp>  // Color (for the test-render helper)

namespace pulp::canvas {

/// Origin-agnostic geometry fingerprint. All values are quantized to 1/4-unit
/// buckets so near-identical requests share a cache entry.
struct BoxShadowKey {
    std::int32_t w = 0;
    std::int32_t h = 0;
    std::int32_t dx = 0;
    std::int32_t dy = 0;
    std::int32_t blur = 0;
    std::int32_t spread = 0;
    std::int32_t corner_radius = 0;
    std::int32_t scale = 0;

    bool operator==(const BoxShadowKey& o) const noexcept {
        return w == o.w && h == o.h && dx == o.dx && dy == o.dy &&
               blur == o.blur && spread == o.spread &&
               corner_radius == o.corner_radius && scale == o.scale;
    }

    static std::int32_t q(float v) noexcept {
        return static_cast<std::int32_t>(v * 4.0f + (v < 0 ? -0.5f : 0.5f));
    }
};

class BoxShadowCache {
public:
    static BoxShadowCache& instance();

    /// Return the cached coverage image for @p key, rendering it via @p render
    /// on a miss (and storing the result). Counts hits and renders.
    sk_sp<SkImage> get_or_render(const BoxShadowKey& key,
                                 const std::function<sk_sp<SkImage>()>& render);

    struct Stats {
        std::uint64_t hits = 0;     ///< Cache hits (blur skipped).
        std::uint64_t renders = 0;  ///< Cache misses (blur performed).
        std::size_t size = 0;       ///< Live entries.
    };
    Stats stats() const;
    void reset_stats();
    void clear();

    /// Toggle caching globally. When false, get_or_render still works but the
    /// shadow path bypasses it entirely (used to A/B against the direct path).
    void set_enabled(bool enabled);
    bool enabled() const;

    void set_capacity(std::size_t capacity);

private:
    BoxShadowCache() = default;
    struct Impl;
    // Defined in box_shadow_cache.cpp; stateful members live there to keep this
    // header free of <mutex>/<list>/<unordered_map>.
    static Impl& impl();
};

// ── Test-only render helper ─────────────────────────────────────────────────
// Renders a single box shadow into an RGBA8888 (unpremultiplied) raster buffer
// of px_w x px_h via the real SkiaCanvas path, so tests can pixel-compare the
// cached vs. uncached output without including Skia headers themselves. Returns
// the pixel buffer (size px_w*px_h*4).
std::vector<std::uint8_t> render_box_shadow_to_rgba(
    int px_w, int px_h, float scale,
    float x, float y, float w, float h,
    float dx, float dy, float blur, float spread,
    Color color, float corner_radius, bool inset);

}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
