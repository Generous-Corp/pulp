#include "pulp/render/gpu_timestamps.hpp"

#include <pulp/runtime/log.hpp>

// Phase 6.5 — Dawn GPU timestamp queries.
//
// The header (`gpu_timestamps.hpp`) carries the pure tick→ms conversion
// math, which is unit-tested without a device. This file carries the
// Dawn-specific QuerySet / resolve-buffer plumbing.
//
// The C++ WebGPU API (`webgpu/webgpu_cpp.h`, the `wgpu::` types) ships
// inside Skia's bundled Dawn, so the real implementation gates on
// `PULP_HAS_SKIA` — the same guard `gpu_surface_dawn.cpp` and
// `gpu_compute.cpp` use for their `wgpu::`-typed code. (The bare
// `PULP_HAS_WEBGPU` path only carries the C-only `webgpu.h`, which has
// no QuerySet C++ wrappers.) When Skia/Dawn is absent — the sanitizer
// matrix, a CPU-only configure — every method degrades to a safe no-op
// and `support()` stays `unsupported`, so callers never need their own
// `#ifdef`s.

#if defined(PULP_HAS_SKIA)
#include "webgpu/webgpu_cpp.h"
#endif

namespace pulp::render {

#if defined(PULP_HAS_SKIA)

// ── Dawn-backed implementation ──────────────────────────────────────────────

struct GpuTimestamps::Impl {
    wgpu::Device   device;
    wgpu::QuerySet query_set;       ///< 2*N timestamp slots.
    wgpu::Buffer   resolve_buffer;  ///< ResolveQuerySet target (QueryResolve|CopySrc).
    wgpu::Buffer   readback_buffer; ///< Host-mappable copy (MapRead|CopyDst).
    std::uint32_t  slot_capacity = 0;  ///< Slots the QuerySet currently holds.

    /// Last successfully map-read frame of ticks.
    std::vector<std::uint64_t> last_resolved;
};

GpuTimestamps::GpuTimestamps() = default;

GpuTimestamps::~GpuTimestamps() {
    delete impl_;
}

bool GpuTimestamps::initialize(void* dawn_device_handle) {
    support_ = GpuTimestampSupport::unsupported;
    delete impl_;
    impl_ = nullptr;

    if (dawn_device_handle == nullptr) {
        runtime::log_info("GpuTimestamps: no device — GPU timestamps unavailable");
        return false;
    }

    auto* device = static_cast<wgpu::Device*>(dawn_device_handle);
    if (device == nullptr || *device == nullptr) {
        return false;
    }

    // `timestamp-query` is an optional feature. It must have been
    // requested at device creation; here we only verify the live device
    // actually carries it. `HasFeature` is the graceful-degradation
    // gate — older mobile GPUs and software adapters return false.
    if (!device->HasFeature(wgpu::FeatureName::TimestampQuery)) {
        runtime::log_info(
            "GpuTimestamps: adapter lacks timestamp-query — using CPU time");
        return false;
    }

    impl_ = new Impl{};
    impl_->device = *device;
    support_ = GpuTimestampSupport::supported;
    runtime::log_info("GpuTimestamps: timestamp-query active");
    return true;
}

void GpuTimestamps::begin_frame(std::size_t pass_count) {
    if (support_ != GpuTimestampSupport::supported || impl_ == nullptr) {
        return;
    }
    if (pass_count == pass_count_ && impl_->query_set != nullptr) {
        return;  // QuerySet already sized for this frame shape.
    }

    pass_count_ = pass_count;
    const std::uint32_t slots =
        static_cast<std::uint32_t>(pass_count) * kTimestampsPerPass;
    if (slots == 0) {
        impl_->query_set = nullptr;
        impl_->resolve_buffer = nullptr;
        impl_->readback_buffer = nullptr;
        impl_->slot_capacity = 0;
        return;
    }

    wgpu::QuerySetDescriptor qs_desc{};
    qs_desc.label = "Pulp Pass Timestamps";
    qs_desc.type = wgpu::QueryType::Timestamp;
    qs_desc.count = slots;
    impl_->query_set = impl_->device.CreateQuerySet(&qs_desc);

    // Each timestamp resolves to a uint64 (8 bytes).
    const std::uint64_t bytes = static_cast<std::uint64_t>(slots) * sizeof(std::uint64_t);

    wgpu::BufferDescriptor resolve_desc{};
    resolve_desc.label = "Pulp Timestamp Resolve";
    resolve_desc.size = bytes;
    resolve_desc.usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
    impl_->resolve_buffer = impl_->device.CreateBuffer(&resolve_desc);

    wgpu::BufferDescriptor readback_desc{};
    readback_desc.label = "Pulp Timestamp Readback";
    readback_desc.size = bytes;
    readback_desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    impl_->readback_buffer = impl_->device.CreateBuffer(&readback_desc);

    impl_->slot_capacity = slots;
}

std::vector<std::uint64_t> GpuTimestamps::read_back() const {
    if (impl_ == nullptr) {
        return {};
    }
    return impl_->last_resolved;
}

#else  // !PULP_HAS_SKIA

// ── CPU-only stub ───────────────────────────────────────────────────────────
//
// No Dawn in this build. Every entry point is a no-op and `support()`
// stays `unsupported`; the inspector reports "GPU timestamps unavailable".

struct GpuTimestamps::Impl {};

GpuTimestamps::GpuTimestamps() = default;
GpuTimestamps::~GpuTimestamps() { delete impl_; }

bool GpuTimestamps::initialize(void* /*dawn_device_handle*/) {
    support_ = GpuTimestampSupport::unsupported;
    return false;
}

void GpuTimestamps::begin_frame(std::size_t /*pass_count*/) {}

std::vector<std::uint64_t> GpuTimestamps::read_back() const { return {}; }

#endif // PULP_HAS_WEBGPU

} // namespace pulp::render
