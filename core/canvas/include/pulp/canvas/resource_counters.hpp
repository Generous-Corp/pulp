#pragma once

// Process-wide counts of the resource work painting does beneath the Canvas
// API: decoding encoded images, uploading raster images to the GPU, building
// SkParagraphs, and recording or replaying cached subtree scenes. None of it
// is visible in a paint-command stream, and all of it is what a steady-state
// frame should not repeat, so a benchmark reads these counters before and
// after each frame and gates on the deltas.
//
// The counters only ever increase and use relaxed atomics: an increment is a
// single uncontended add on the painting thread, and readers want totals, not
// ordering. ImageFileCache keeps its own hit/build statistics; these count the
// work itself, whichever path asked for it.

#include <atomic>
#include <cstdint>

namespace pulp::canvas {

struct ResourceCounters {
    /// Encoded image bytes handed to Skia for decoding. Each count is one
    /// decode: the image it creates is new, so nothing upstream can reuse an
    /// earlier decode of the same bytes.
    std::uint64_t image_decodes = 0;
    /// Raster images uploaded to a GPU texture.
    std::uint64_t gpu_uploads = 0;
    /// SkParagraph builds (shaping plus line layout).
    std::uint64_t paragraph_builds = 0;
    /// Subtree scenes recorded for set_subtree_cached() views.
    std::uint64_t scene_records = 0;
    /// Recorded subtree scenes replayed instead of repainting the subtree.
    std::uint64_t scene_replays = 0;
};

namespace detail {
inline std::atomic<std::uint64_t> image_decodes{0};
inline std::atomic<std::uint64_t> gpu_uploads{0};
inline std::atomic<std::uint64_t> paragraph_builds{0};
inline std::atomic<std::uint64_t> scene_records{0};
inline std::atomic<std::uint64_t> scene_replays{0};
} // namespace detail

inline ResourceCounters resource_counters() noexcept {
    constexpr auto relaxed = std::memory_order_relaxed;
    return {detail::image_decodes.load(relaxed), detail::gpu_uploads.load(relaxed),
            detail::paragraph_builds.load(relaxed), detail::scene_records.load(relaxed),
            detail::scene_replays.load(relaxed)};
}

inline void note_image_decode() noexcept {
    detail::image_decodes.fetch_add(1, std::memory_order_relaxed);
}
inline void note_gpu_upload() noexcept {
    detail::gpu_uploads.fetch_add(1, std::memory_order_relaxed);
}
inline void note_paragraph_build() noexcept {
    detail::paragraph_builds.fetch_add(1, std::memory_order_relaxed);
}
inline void note_scene_record() noexcept {
    detail::scene_records.fetch_add(1, std::memory_order_relaxed);
}
inline void note_scene_replay() noexcept {
    detail::scene_replays.fetch_add(1, std::memory_order_relaxed);
}

} // namespace pulp::canvas
