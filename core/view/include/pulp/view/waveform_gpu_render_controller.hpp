#pragma once

/// @file waveform_gpu_render_controller.hpp
/// Backend-neutral render orchestration for waveform GPU plans.
///
/// The controller consumes WaveformGpuLayerPlan values and turns them into
/// deterministic upload/draw/fallback decisions for a concrete renderer. It
/// owns only metadata cache state; Dawn, Skia, Metal, WebGPU, fences, and
/// release queues remain backend responsibilities. Use it from render/control
/// code, not from live audio callbacks. It is not thread-safe; async GPU upload
/// completions should marshal commits back to the owning render/control thread.

#include <pulp/audio/waveform_overview.hpp>
#include <pulp/view/waveform_gpu_primitives.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::view {

struct WaveformGpuRenderContext {
    bool backend_available = true;
    // Opaque renderer-owned namespace for backend resources. Increment when a
    // GPU device/surface/resource arena is recreated; 0 disables GPU cache use.
    std::uint64_t backend_generation = 1;
};

enum class WaveformGpuRenderActionKind : std::uint8_t {
    NoOpInvalidPlan,
    NoOpBackendUnavailable,
    NoOpCpuFallbackUnavailable,
    NoOpStagingTooSmall,
    NoOpVertexFillFailed,
    CpuFallbackReady,
    UploadStaticLayerReady,
    DrawCachedStaticLayer,
    PlayheadOnlyRedraw,
};

struct WaveformGpuRenderDecision {
    WaveformGpuRenderActionKind action = WaveformGpuRenderActionKind::NoOpInvalidPlan;
    WaveformGpuUploadKey upload_key{};
    WaveformGpuResourceRecord cached_resource{};
    WaveformPlayheadOverlay playhead{};
    std::uint64_t backend_generation = 0;
    // Caller-owned staging vertices are valid only for CpuFallbackReady and
    // UploadStaticLayerReady, and only when vertices_written equals this count.
    std::size_t required_vertices = 0;
    std::size_t vertices_written = 0;
    std::size_t upload_bytes = 0;
    bool used_gpu_path = false;
    bool cache_hit = false;
    bool static_layer_dirty = false;
};

/// Stateful policy helper for backend-owned waveform resources.
///
/// plan_render() never inserts placeholder cache records. Backends should call
/// commit_uploaded_resource() only after a real resource has been created.
/// Replaced or evicted records are then returned so the backend can release its
/// own GPU resources. GPU-assisted offline analysis should publish CPU-readable
/// generation-keyed summaries and must never require live audio threads to wait
/// on GPU completion.
class WaveformGpuRenderController {
public:
    explicit WaveformGpuRenderController(std::size_t cache_capacity = 8);

    bool prepare(std::size_t cache_capacity,
                 std::vector<WaveformGpuResourceRecord>* evicted_records = nullptr);

    [[nodiscard]] WaveformGpuRenderDecision plan_render(
        const pulp::audio::WaveformOverview& thumbnail,
        const WaveformGpuLayerPlan& plan,
        const WaveformGpuRenderContext& context,
        std::span<WaveformPeakVertex> staging_vertices) noexcept;

    [[nodiscard]] WaveformGpuResourcePutResult commit_uploaded_resource(
        const WaveformGpuUploadKey& key,
        std::uint64_t resource_id,
        std::size_t bytes,
        std::uint64_t backend_generation = 1);

    [[nodiscard]] WaveformGpuResourcePutResult commit_uploaded_resource(
        const WaveformGpuRenderDecision& decision,
        std::uint64_t resource_id,
        std::uint64_t current_backend_generation);

    bool erase(const WaveformGpuUploadKey& key,
               WaveformGpuResourceRecord& removed_record) noexcept;

    [[nodiscard]] std::vector<WaveformGpuResourceRecord> clear_and_return_records();

    [[nodiscard]] WaveformGpuResourceCacheStats stats() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return cache_.capacity(); }
    [[nodiscard]] std::size_t size() const noexcept { return cache_.size(); }

private:
    WaveformGpuResourceCache cache_;
};

} // namespace pulp::view
