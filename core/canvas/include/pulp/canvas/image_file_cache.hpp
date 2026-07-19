#pragma once

/// @file image_file_cache.hpp
/// Process-wide cache for decoded (and GPU-uploaded) file images (SkImage).
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

    struct Stats {
        std::uint64_t hits = 0;    ///< Cache hits (decode + upload skipped).
        std::uint64_t builds = 0;  ///< Cache misses (image rebuilt).
        std::size_t size = 0;      ///< Live entries.
    };
    Stats stats() const;
    void reset_stats();

    /// Drop all cached images. The invalidation hook for on-disk file mutation
    /// (see the semantic-change note above) and for GPU-context teardown.
    void clear();

    /// Toggle caching globally. When false, get_or_build still works but always
    /// rebuilds and stores nothing (used to A/B against the uncached path and to
    /// keep golden / micro-benchmark harnesses deterministic).
    void set_enabled(bool enabled);
    bool enabled() const;

    void set_capacity(std::size_t capacity);

private:
    ImageFileCache() = default;
    struct Impl;
    // Defined in image_file_cache.cpp; stateful members live there to keep this
    // header free of <mutex>/<list>/<unordered_map> and the full SkImage type.
    static Impl& impl();
};

}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
