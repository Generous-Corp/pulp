#pragma once

/// @file image_file_cache.hpp
/// Process-wide cache for decoded (and GPU-uploaded) file and immutable
/// embedded images (SkImage). Embedded images use a NUL-prefixed synthetic
/// key derived from their static address and size, so they cannot collide
/// with a filesystem path.
///
/// Inline encoded bytes (a `data:image/*` source, a retained scene image blob)
/// live in a separate pool keyed by the caller's SHA-256 of the bytes, never
/// their address, because a freed buffer's address is reused for different
/// bytes. That pool is bounded by bytes (the retained encoded copy plus the
/// decoded or uploaded pixels), not by entry count, so a panel with many
/// distinct inline images cannot evict file images or pin unbounded memory;
/// an image larger than the whole budget is built and drawn uncached.
///
/// `SkiaCanvas::draw_image_from_file` used to re-read the PNG/JPEG bytes off
/// disk (`SkData::MakeFromFileName`), re-decode them
/// (`SkImages::DeferredFromEncodedData`), AND re-upload a fresh GPU texture
/// (`ensure_gpu_image` → `SkImages::TextureFromImage`) on *every* draw. For a
/// file-image drawn every frame — a dragged sprite-strip knob, a static
/// background bitmap repainted at the display refresh rate — that is a disk
/// read plus a full raster decode plus a texture upload per frame, all to
/// produce a pixel-identical result.
///
/// This cache stores the fully-prepared `SkImage` keyed on the *file path*
/// plus a backend identity token (the Graphite recorder / Ganesh context the
/// canvas draws into, or null for CPU raster). The backend token is part of
/// the key because a GPU-uploaded texture is bound to the context that created
/// it: an image uploaded for recorder A must never be handed to a canvas
/// drawing through recorder B. A CPU raster canvas (null token) caches the
/// decoded raster image; a GPU canvas caches the texture-backed image.
///
/// ─── Deliberate semantic change ───────────────────────────────────────────
/// The key is the file PATH, not the file CONTENTS. Unlike `SvgDomCache`
/// (which keys on the exact document bytes, so a render-patched string re-parses
/// and a dragged knob stays live), this cache CANNOT cheaply notice that the
/// bytes on disk changed — reading + hashing the file every frame is exactly
/// the cost being eliminated. Consequence: mutating an image file in place
/// while the app holds it will NOT live-update the on-screen image. Callers
/// that need a hot-reload story must call `clear()` (or a future per-path
/// invalidation hook) when they know a file changed. This is an acceptable
/// trade for interactive UIs, where image assets are effectively immutable for
/// the session; the previous per-frame re-read only "worked" for live editing
/// by paying the full cost on every static repaint too.
///
/// Painting is on the UI thread (not the realtime audio thread), so a small
/// mutex is acceptable; the cache is process-global with an LRU cap. Values are
/// shared `sk_sp<SkImage>` — immutable once built and safe to share across the
/// per-frame `SkiaCanvas` instances that reuse the same recorder.

#ifdef PULP_HAS_SKIA

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "include/core/SkRefCnt.h"

class SkImage;

namespace pulp::canvas {

class ImageFileCache {
public:
    static ImageFileCache& instance();

    /// Return the prepared image for (@p path, @p backend_key), building it on a
    /// miss via @p build (which decodes the file and performs any GPU upload).
    /// The backend token distinguishes GPU-context-bound textures from each
    /// other and from the CPU raster image, so the same path drawn through two
    /// contexts caches two entries. A null build result is returned but NOT
    /// stored (a transient read/decode failure does not poison the cache).
    /// When disabled, always rebuilds and stores nothing.
    sk_sp<SkImage> get_or_build(
        const std::string& path,
        const void* backend_key,
        const std::function<sk_sp<SkImage>()>& build);

    /// Default byte budget of the inline pool: room for a few retina-sized
    /// panel backdrops (encoded copy plus 4 bytes per pixel each).
    static constexpr std::size_t kDefaultInlineByteBudget = 96u * 1024u * 1024u;

    /// Inline-bytes counterpart of `get_or_build`, keyed by @p content_key (a
    /// digest of the bytes) and @p backend_key in a pool separate from the
    /// path pool. @p encoded_size is the size of the encoded copy @p build
    /// retains; the entry costs that plus the built image's pixel bytes. The
    /// pool evicts least-recently-used entries to stay within its byte budget.
    /// An image whose cost alone exceeds the budget is returned but not
    /// stored, so it is rebuilt on every call instead of flushing the pool.
    sk_sp<SkImage> get_or_build_inline(const std::string& content_key, const void* backend_key,
                                       std::size_t encoded_size,
                                       const std::function<sk_sp<SkImage>()>& build);

    struct Stats {
        std::uint64_t hits = 0;    ///< Cache hits (decode + upload skipped).
        std::uint64_t builds = 0;  ///< Cache misses (image rebuilt).
        std::size_t size = 0;      ///< Live entries.
    };
    /// Counters for the path pool.
    Stats stats() const;

    struct InlineStats {
        std::uint64_t hits = 0;        ///< Cache hits (decode + upload skipped).
        std::uint64_t builds = 0;      ///< Builds, cached or not.
        std::uint64_t over_budget = 0; ///< Builds not retained: larger than the budget.
        std::size_t size = 0;          ///< Live entries.
        std::size_t bytes = 0;         ///< Retained cost of live entries.
    };
    /// Counters for the inline pool.
    InlineStats inline_stats() const;
    /// Reset the counters of both pools.
    void reset_stats();

    /// Drop all cached images from both pools. The invalidation hook for on-disk file mutation
    /// (see the semantic-change note above) and for GPU-context teardown.
    /// Stop painting before calling this; it waits for every in-flight build.
    void clear();

    /// Drop entries tied to one GPU backend before that backend is destroyed.
    /// The caller must stop painting through the backend before calling this;
    /// this method waits for already-started builds, then removes their entries.
    /// A null backend key is ignored because it denotes the shared CPU-raster entry.
    void clear_backend(const void* backend_key);

    /// Toggle caching globally. When false, get_or_build still works but always
    /// rebuilds and stores nothing (used to A/B against the uncached path and to
    /// keep golden / micro-benchmark harnesses deterministic).
    void set_enabled(bool enabled);
    bool enabled() const;

    /// Entry cap of the path pool.
    void set_capacity(std::size_t capacity);

    /// Byte budget of the inline pool; evicts down to it immediately.
    void set_inline_byte_budget(std::size_t bytes);
    std::size_t inline_byte_budget() const;

  private:
    ImageFileCache() = default;
    struct Impl;
    // Defined in image_file_cache.cpp; stateful members live there to keep this
    // header free of <mutex>/<list>/<unordered_map> and the full SkImage type.
    static Impl& impl();
};

}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
