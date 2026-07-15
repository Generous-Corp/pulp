#pragma once

/// @file waveform_headless_render_backend.hpp
/// Concrete CPU/headless consumer of waveform GPU render decisions.
///
/// This backend is a render/control-thread helper for tests, headless tools,
/// and CPU fallback paths. It consumes WaveformGpuRenderController decisions,
/// owns copied vertex payloads as opaque backend resources, and exercises the
/// same upload/cache/release lifecycle that a Dawn/Skia/Metal adapter must
/// implement. It does not create GPU objects, perform file I/O, or wait on live
/// audio threads.

#include <pulp/audio/waveform_overview.hpp>
#include <pulp/view/waveform_gpu_render_controller.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::view {

struct WaveformHeadlessRenderConfig {
    std::size_t cache_capacity = 8;
    std::size_t max_staging_vertices = 4096;
};

enum class WaveformHeadlessRenderStatus : std::uint8_t {
    NoOp,
    CpuFallbackReady,
    UploadedStaticLayer,
    DrawCachedStaticLayer,
    PlayheadOnlyRedraw,
    MissingCachedResource,
    ResourceAllocationFailed,
    ResourceCommitRejected,
};

struct WaveformHeadlessRenderFrame {
    WaveformHeadlessRenderStatus status = WaveformHeadlessRenderStatus::NoOp;
    WaveformGpuRenderDecision decision{};
    // Non-owning view into backend-owned staging or cached static-layer
    // vertices. Valid until the next non-const backend call, clear(), prepare(),
    // resource eviction/replacement, or backend destruction.
    std::span<const WaveformPeakVertex> static_vertices{};
    WaveformPlayheadOverlay playhead{};
    std::uint64_t resource_id = 0;
    bool used_cached_resource = false;

    [[nodiscard]] bool has_static_vertices() const noexcept {
        return !static_vertices.empty();
    }
};

/// Headless backend adapter over WaveformGpuRenderController.
///
/// A real GPU renderer should mirror this lifecycle with backend buffers or
/// textures instead of copied vectors: plan, create/upload the static layer,
/// commit the resource on the owning render/control thread, release returned
/// replacement/eviction records, then compose cached static layers with cheap
/// playhead-only redraws. GPU-assisted offline analysis may use the same
/// generation-keyed publication shape, but live audio callbacks must never wait
/// for GPU completion.
class WaveformHeadlessRenderBackend {
public:
    explicit WaveformHeadlessRenderBackend(WaveformHeadlessRenderConfig config = {});
    ~WaveformHeadlessRenderBackend();

    bool prepare(WaveformHeadlessRenderConfig config);
    [[nodiscard]] WaveformHeadlessRenderFrame render(
        const pulp::audio::WaveformOverview& thumbnail,
        const WaveformGpuLayerPlan& plan,
        const WaveformGpuRenderContext& context = {});

    void clear();

    [[nodiscard]] WaveformGpuResourceCacheStats stats() const noexcept;
    [[nodiscard]] std::size_t resource_count() const noexcept { return resources_.size(); }
    [[nodiscard]] std::size_t released_resource_count() const noexcept {
        return released_resource_count_;
    }
    [[nodiscard]] std::uint64_t last_released_resource_id() const noexcept {
        return last_released_resource_id_;
    }
    [[nodiscard]] std::size_t staging_vertex_capacity() const noexcept {
        return staging_vertices_.size();
    }

private:
    struct ResourcePayload {
        WaveformGpuUploadKey key{};
        std::uint64_t resource_id = 0;
        std::uint64_t backend_generation = 0;
        std::vector<WaveformPeakVertex> vertices;
    };

    [[nodiscard]] WaveformHeadlessRenderFrame make_noop_frame(
        const WaveformGpuRenderDecision& decision) const noexcept;
    [[nodiscard]] WaveformHeadlessRenderFrame make_cached_frame(
        const WaveformGpuRenderDecision& decision,
        WaveformHeadlessRenderStatus status) const noexcept;
    [[nodiscard]] WaveformHeadlessRenderFrame upload_static_layer(
        const WaveformGpuRenderDecision& decision);

    const ResourcePayload* find_resource(const WaveformGpuResourceRecord& record) const noexcept;
    ResourcePayload* find_resource_by_id(std::uint64_t resource_id) noexcept;
    bool release_record(const WaveformGpuResourceRecord& record) noexcept;
    bool release_resource_by_id(std::uint64_t resource_id) noexcept;
    void release_all_resources() noexcept;
    void release_commit_records(const WaveformGpuResourcePutResult& result) noexcept;

    WaveformGpuRenderController controller_;
    std::vector<WaveformPeakVertex> staging_vertices_;
    std::vector<ResourcePayload> resources_;
    std::uint64_t next_resource_id_ = 1;
    std::size_t released_resource_count_ = 0;
    std::uint64_t last_released_resource_id_ = 0;
};

} // namespace pulp::view
