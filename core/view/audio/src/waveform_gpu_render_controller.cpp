#include <pulp/view/waveform_gpu_render_controller.hpp>

namespace pulp::view {
namespace {

WaveformGpuRenderDecision make_base_decision(const WaveformGpuLayerPlan& plan,
                                             std::uint64_t backend_generation) noexcept {
    WaveformGpuRenderDecision decision;
    decision.upload_key = plan.static_layer.upload_key;
    decision.playhead = plan.playhead;
    decision.backend_generation = backend_generation;
    decision.required_vertices = plan.static_layer.vertex_count;
    decision.upload_bytes = plan.static_layer.upload_bytes;
    decision.static_layer_dirty = plan.static_layer_dirty;
    return decision;
}

WaveformGpuRenderDecision fill_vertices_for_action(
    const pulp::audio::WaveformOverview& thumbnail,
    const WaveformGpuLayerPlan& plan,
    std::span<WaveformPeakVertex> staging_vertices,
    std::uint64_t backend_generation,
    WaveformGpuRenderActionKind ready_action) noexcept {
    auto decision = make_base_decision(plan, backend_generation);
    if (!plan.static_layer.cpu_fallback_available &&
        ready_action == WaveformGpuRenderActionKind::CpuFallbackReady) {
        decision.action = WaveformGpuRenderActionKind::NoOpCpuFallbackUnavailable;
        return decision;
    }
    if (staging_vertices.size() < decision.required_vertices) {
        decision.action = WaveformGpuRenderActionKind::NoOpStagingTooSmall;
        return decision;
    }

    decision.vertices_written = fill_waveform_peak_vertices(
        thumbnail,
        plan.static_layer,
        staging_vertices.first(decision.required_vertices));
    if (decision.vertices_written != decision.required_vertices) {
        decision.action = WaveformGpuRenderActionKind::NoOpVertexFillFailed;
        return decision;
    }

    decision.action = ready_action;
    decision.used_gpu_path = ready_action == WaveformGpuRenderActionKind::UploadStaticLayerReady;
    return decision;
}

} // namespace

WaveformGpuRenderController::WaveformGpuRenderController(std::size_t cache_capacity)
    : cache_(cache_capacity) {}

bool WaveformGpuRenderController::prepare(
    std::size_t cache_capacity,
    std::vector<WaveformGpuResourceRecord>* evicted_records) {
    return cache_.prepare(cache_capacity, evicted_records);
}

WaveformGpuRenderDecision WaveformGpuRenderController::plan_render(
    const pulp::audio::WaveformOverview& thumbnail,
    const WaveformGpuLayerPlan& plan,
    const WaveformGpuRenderContext& context,
    std::span<WaveformPeakVertex> staging_vertices) noexcept {
    if (!plan.static_layer.valid || !plan.static_layer.upload_key.valid()) {
        return {};
    }

    if (!plan.static_layer.prefer_gpu) {
        return fill_vertices_for_action(
            thumbnail,
            plan,
            staging_vertices,
            context.backend_generation,
            WaveformGpuRenderActionKind::CpuFallbackReady);
    }

    if (!context.backend_available || context.backend_generation == 0) {
        if (!plan.static_layer.cpu_fallback_available) {
            auto decision = make_base_decision(plan, context.backend_generation);
            decision.action = WaveformGpuRenderActionKind::NoOpBackendUnavailable;
            return decision;
        }
        return fill_vertices_for_action(
            thumbnail,
            plan,
            staging_vertices,
            context.backend_generation,
            WaveformGpuRenderActionKind::CpuFallbackReady);
    }

    auto decision = make_base_decision(plan, context.backend_generation);
    decision.used_gpu_path = true;
    if (const auto* cached = cache_.find(plan.static_layer.upload_key, context.backend_generation)) {
        decision.cache_hit = true;
        decision.cached_resource = *cached;
        decision.action = plan.playhead_only_redraw && plan.playhead.visible
                              ? WaveformGpuRenderActionKind::PlayheadOnlyRedraw
                              : WaveformGpuRenderActionKind::DrawCachedStaticLayer;
        return decision;
    }

    return fill_vertices_for_action(
        thumbnail,
        plan,
        staging_vertices,
        context.backend_generation,
        WaveformGpuRenderActionKind::UploadStaticLayerReady);
}

WaveformGpuResourcePutResult WaveformGpuRenderController::commit_uploaded_resource(
    const WaveformGpuUploadKey& key,
    std::uint64_t resource_id,
    std::size_t bytes,
    std::uint64_t backend_generation) {
    return cache_.put(key, resource_id, bytes, backend_generation);
}

WaveformGpuResourcePutResult WaveformGpuRenderController::commit_uploaded_resource(
    const WaveformGpuRenderDecision& decision,
    std::uint64_t resource_id,
    std::uint64_t current_backend_generation) {
    if (decision.action != WaveformGpuRenderActionKind::UploadStaticLayerReady ||
        !decision.upload_key.valid() || decision.upload_bytes == 0 ||
        decision.backend_generation == 0 ||
        decision.backend_generation != current_backend_generation) {
        return {};
    }
    return cache_.put(
        decision.upload_key,
        resource_id,
        decision.upload_bytes,
        decision.backend_generation);
}

bool WaveformGpuRenderController::erase(
    const WaveformGpuUploadKey& key,
    WaveformGpuResourceRecord& removed_record) noexcept {
    return cache_.erase(key, removed_record);
}

std::vector<WaveformGpuResourceRecord> WaveformGpuRenderController::clear_and_return_records() {
    return cache_.clear_and_return_records();
}

WaveformGpuResourceCacheStats WaveformGpuRenderController::stats() const noexcept {
    return cache_.stats();
}

} // namespace pulp::view
