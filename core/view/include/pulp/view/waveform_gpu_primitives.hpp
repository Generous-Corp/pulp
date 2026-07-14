#pragma once

/// @file waveform_gpu_primitives.hpp
/// Backend-neutral waveform thumbnail upload and composition planning helpers.
///
/// These primitives describe what a GPU-backed waveform renderer can cache or
/// upload, but they do not own Dawn/Skia/Metal resources. They are safe to use
/// in CPU-only/headless builds and keep WaveformView display-only.

#include <pulp/audio/waveform_overview.hpp>
#include <pulp/view/waveform_editor_primitives.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::view {

struct WaveformPeakVertex {
    float x = 0.0f;
    float y_min = 0.0f;
    float y_max = 0.0f;
    float min_value = 0.0f;
    float max_value = 0.0f;
    std::uint32_t source_peak_index = 0;
};

struct WaveformGpuUploadKey {
    std::uint64_t source_generation = 0;
    std::uint32_t channel = pulp::audio::WaveformOverview::kAllChannels;
    std::uint32_t thumbnail_level_index = 0;
    std::uint32_t samples_per_peak = 0;
    std::uint32_t first_peak = 0;
    std::uint32_t peak_count = 0;
    std::int64_t visible_start = 0;
    std::int64_t visible_length = 0;
    std::int32_t bounds_x_milli_px = 0;
    std::int32_t bounds_y_milli_px = 0;
    std::int32_t bounds_width_milli_px = 0;
    std::int32_t bounds_height_milli_px = 0;
    std::uint32_t target_width_px = 0;
    std::uint32_t target_height_px = 0;

    [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] bool operator==(const WaveformGpuUploadKey& a,
                              const WaveformGpuUploadKey& b) noexcept;
[[nodiscard]] bool operator!=(const WaveformGpuUploadKey& a,
                              const WaveformGpuUploadKey& b) noexcept;

struct WaveformGpuLayerConfig {
    // Required cache identity supplied by the source/editor owner. Increment
    // whenever thumbnail content changes; 0 produces an invalid GPU plan.
    std::uint64_t source_generation = 0;
    std::uint32_t channel = pulp::audio::WaveformOverview::kAllChannels;
    std::uint32_t max_columns = 0;
    bool prefer_gpu = true;
};

struct WaveformGpuStaticLayerPlan {
    bool valid = false;
    WaveformViewport viewport{};
    WaveformGpuUploadKey upload_key{};
    std::size_t vertex_count = 0;
    std::size_t upload_bytes = 0;
    bool prefer_gpu = true;
    bool cpu_fallback_available = true;
};

struct WaveformGpuLayerPlan {
    WaveformGpuStaticLayerPlan static_layer{};
    WaveformPlayheadOverlay playhead{};
    bool static_layer_dirty = false;
    bool playhead_only_redraw = false;
};

[[nodiscard]] WaveformGpuStaticLayerPlan build_waveform_gpu_static_layer_plan(
    const pulp::audio::WaveformOverview& thumbnail,
    const WaveformViewport& viewport,
    const WaveformGpuLayerConfig& config = {});

[[nodiscard]] WaveformGpuLayerPlan build_waveform_gpu_layer_plan(
    const pulp::audio::WaveformOverview& thumbnail,
    const WaveformViewport& viewport,
    int64_t playhead_sample,
    const WaveformGpuLayerConfig& config = {},
    const WaveformGpuUploadKey* previous_static_key = nullptr);

/// Fill caller-owned vertices for CPU fallback or GPU upload staging. Returns
/// the number of vertices written, capped by dst.size(). This function performs
/// no allocation and does not touch backend GPU resources.
std::size_t fill_waveform_peak_vertices(
    const pulp::audio::WaveformOverview& thumbnail,
    const WaveformGpuStaticLayerPlan& plan,
    std::span<WaveformPeakVertex> dst) noexcept;

struct WaveformGpuResourceRecord {
    WaveformGpuUploadKey key{};
    std::uint64_t resource_id = 0;
    std::size_t bytes = 0;
    std::uint64_t backend_generation = 0;
    std::uint64_t last_used = 0;

    [[nodiscard]] bool valid() const noexcept;
};

struct WaveformGpuResourceCacheStats {
    std::size_t entries = 0;
    std::size_t capacity = 0;
    std::size_t bytes = 0;
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;
};

struct WaveformGpuResourcePutResult {
    bool ok = false;
    bool replaced = false;
    bool evicted = false;
    WaveformGpuResourceRecord replaced_record{};
    WaveformGpuResourceRecord evicted_record{};
};

/// Metadata-only LRU for backend-owned waveform resources. The cache stores
/// stable keys and opaque resource IDs; platform renderers own the real GPU
/// buffers/textures and may use resource_id however they choose. Any operation
/// that removes a record can report it so the backend can release resources.
/// Not thread-safe; async backend completions should marshal cache updates back
/// to the owning render/control thread or use external synchronization.
class WaveformGpuResourceCache {
public:
    explicit WaveformGpuResourceCache(std::size_t capacity = 8);

    bool prepare(std::size_t capacity,
                 std::vector<WaveformGpuResourceRecord>* evicted_records = nullptr);
    [[nodiscard]] std::vector<WaveformGpuResourceRecord> clear_and_return_records();

    [[nodiscard]] WaveformGpuResourcePutResult put(const WaveformGpuUploadKey& key,
                                                   std::uint64_t resource_id,
                                                   std::size_t bytes,
                                                   std::uint64_t backend_generation = 1);
    const WaveformGpuResourceRecord* find(const WaveformGpuUploadKey& key) noexcept;
    const WaveformGpuResourceRecord* find(const WaveformGpuUploadKey& key,
                                          std::uint64_t backend_generation) noexcept;
    bool erase(const WaveformGpuUploadKey& key,
               WaveformGpuResourceRecord& removed_record) noexcept;

    [[nodiscard]] WaveformGpuResourceCacheStats stats() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

private:
    std::vector<WaveformGpuResourceRecord> records_;
    std::size_t capacity_ = 0;
    std::uint64_t clock_ = 0;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
    std::size_t evictions_ = 0;
};

} // namespace pulp::view
