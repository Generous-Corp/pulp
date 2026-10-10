#include <pulp/view/waveform_headless_render_backend.hpp>

#include <algorithm>
#include <utility>

namespace pulp::view {

WaveformHeadlessRenderBackend::WaveformHeadlessRenderBackend(
    WaveformHeadlessRenderConfig config)
    : controller_(config.cache_capacity) {
    prepare(config);
}

WaveformHeadlessRenderBackend::~WaveformHeadlessRenderBackend() {
    clear();
}

bool WaveformHeadlessRenderBackend::prepare(WaveformHeadlessRenderConfig config) {
    std::vector<WaveformPeakVertex> prepared_staging;
    try {
        prepared_staging.resize(config.max_staging_vertices);
        resources_.reserve(config.cache_capacity);
    } catch (...) {
        return false;
    }

    std::vector<WaveformGpuResourceRecord> evicted_records;
    if (!controller_.prepare(config.cache_capacity, &evicted_records)) return false;

    staging_vertices_.swap(prepared_staging);
    for (const auto& record : evicted_records) release_record(record);
    return true;
}

WaveformHeadlessRenderFrame WaveformHeadlessRenderBackend::render(
    const pulp::audio::WaveformOverview& thumbnail,
    const WaveformGpuLayerPlan& plan,
    const WaveformGpuRenderContext& context) {
    const auto decision = controller_.plan_render(
        thumbnail,
        plan,
        context,
        std::span<WaveformPeakVertex>(staging_vertices_.data(), staging_vertices_.size()));

    switch (decision.action) {
        case WaveformGpuRenderActionKind::CpuFallbackReady: {
            WaveformHeadlessRenderFrame frame;
            frame.status = WaveformHeadlessRenderStatus::CpuFallbackReady;
            frame.decision = decision;
            frame.playhead = decision.playhead;
            frame.static_vertices = std::span<const WaveformPeakVertex>(
                staging_vertices_.data(),
                decision.vertices_written);
            return frame;
        }
        case WaveformGpuRenderActionKind::UploadStaticLayerReady:
            return upload_static_layer(decision);
        case WaveformGpuRenderActionKind::DrawCachedStaticLayer:
            return make_cached_frame(decision, WaveformHeadlessRenderStatus::DrawCachedStaticLayer);
        case WaveformGpuRenderActionKind::PlayheadOnlyRedraw:
            return make_cached_frame(decision, WaveformHeadlessRenderStatus::PlayheadOnlyRedraw);
        default:
            return make_noop_frame(decision);
    }
}

void WaveformHeadlessRenderBackend::clear() {
    const auto records = controller_.clear_and_return_records();
    for (const auto& record : records) release_record(record);
    release_all_resources();
}

WaveformGpuResourceCacheStats WaveformHeadlessRenderBackend::stats() const noexcept {
    return controller_.stats();
}

WaveformHeadlessRenderFrame WaveformHeadlessRenderBackend::make_noop_frame(
    const WaveformGpuRenderDecision& decision) const noexcept {
    WaveformHeadlessRenderFrame frame;
    frame.status = WaveformHeadlessRenderStatus::NoOp;
    frame.decision = decision;
    frame.playhead = decision.playhead;
    return frame;
}

WaveformHeadlessRenderFrame WaveformHeadlessRenderBackend::make_cached_frame(
    const WaveformGpuRenderDecision& decision,
    WaveformHeadlessRenderStatus status) const noexcept {
    WaveformHeadlessRenderFrame frame;
    frame.status = status;
    frame.decision = decision;
    frame.playhead = decision.playhead;
    frame.resource_id = decision.cached_resource.resource_id;
    frame.used_cached_resource = true;

    const auto* resource = find_resource(decision.cached_resource);
    if (!resource) {
        frame.status = WaveformHeadlessRenderStatus::MissingCachedResource;
        frame.resource_id = 0;
        frame.used_cached_resource = false;
        return frame;
    }

    frame.static_vertices = std::span<const WaveformPeakVertex>(
        resource->vertices.data(),
        resource->vertices.size());
    return frame;
}

WaveformHeadlessRenderFrame WaveformHeadlessRenderBackend::upload_static_layer(
    const WaveformGpuRenderDecision& decision) {
    WaveformHeadlessRenderFrame frame;
    frame.status = WaveformHeadlessRenderStatus::UploadedStaticLayer;
    frame.decision = decision;
    frame.playhead = decision.playhead;

    ResourcePayload payload;
    payload.key = decision.upload_key;
    payload.resource_id = next_resource_id_++;
    payload.backend_generation = decision.backend_generation;

    try {
        payload.vertices.assign(
            staging_vertices_.begin(),
            staging_vertices_.begin() + static_cast<std::ptrdiff_t>(decision.vertices_written));
        resources_.push_back(std::move(payload));
    } catch (...) {
        frame.status = WaveformHeadlessRenderStatus::ResourceAllocationFailed;
        return frame;
    }

    const auto resource_id = resources_.back().resource_id;
    const auto commit = controller_.commit_uploaded_resource(
        decision,
        resource_id,
        decision.backend_generation);
    if (!commit.ok) {
        release_resource_by_id(resource_id);
        frame.status = WaveformHeadlessRenderStatus::ResourceCommitRejected;
        frame.resource_id = 0;
        return frame;
    }

    release_commit_records(commit);

    const auto* resource = find_resource_by_id(resource_id);
    if (!resource) {
        frame.status = WaveformHeadlessRenderStatus::MissingCachedResource;
        frame.resource_id = 0;
        return frame;
    }

    frame.resource_id = resource_id;
    frame.static_vertices = std::span<const WaveformPeakVertex>(
        resource->vertices.data(),
        resource->vertices.size());
    return frame;
}

const WaveformHeadlessRenderBackend::ResourcePayload*
WaveformHeadlessRenderBackend::find_resource(
    const WaveformGpuResourceRecord& record) const noexcept {
    const auto it = std::find_if(resources_.begin(), resources_.end(), [&](const auto& resource) {
        return resource.resource_id == record.resource_id &&
               resource.backend_generation == record.backend_generation &&
               resource.key == record.key;
    });
    return it == resources_.end() ? nullptr : &*it;
}

WaveformHeadlessRenderBackend::ResourcePayload*
WaveformHeadlessRenderBackend::find_resource_by_id(std::uint64_t resource_id) noexcept {
    const auto it = std::find_if(resources_.begin(), resources_.end(), [&](const auto& resource) {
        return resource.resource_id == resource_id;
    });
    return it == resources_.end() ? nullptr : &*it;
}

bool WaveformHeadlessRenderBackend::release_record(
    const WaveformGpuResourceRecord& record) noexcept {
    return release_resource_by_id(record.resource_id);
}

bool WaveformHeadlessRenderBackend::release_resource_by_id(std::uint64_t resource_id) noexcept {
    if (resource_id == 0) return false;
    const auto it = std::find_if(resources_.begin(), resources_.end(), [&](const auto& resource) {
        return resource.resource_id == resource_id;
    });
    if (it == resources_.end()) return false;
    last_released_resource_id_ = it->resource_id;
    ++released_resource_count_;
    resources_.erase(it);
    return true;
}

void WaveformHeadlessRenderBackend::release_all_resources() noexcept {
    while (!resources_.empty()) {
        last_released_resource_id_ = resources_.back().resource_id;
        ++released_resource_count_;
        resources_.pop_back();
    }
}

void WaveformHeadlessRenderBackend::release_commit_records(
    const WaveformGpuResourcePutResult& result) noexcept {
    if (result.replaced) release_record(result.replaced_record);
    if (result.evicted) release_record(result.evicted_record);
}

} // namespace pulp::view
