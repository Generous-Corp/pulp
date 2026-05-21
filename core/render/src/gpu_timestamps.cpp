#include "pulp/render/gpu_timestamps.hpp"

#include <pulp/runtime/log.hpp>

#include <chrono>
#include <cstddef>
#include <thread>

// Phase 6.5 — Dawn GPU timestamp queries.
//
// The header (`gpu_timestamps.hpp`) carries the pure tick→ms conversion
// math and the `decode_resolved_ticks` byte-decode seam, both unit-
// tested without a device. This file carries the Dawn-specific QuerySet
// / resolve-buffer plumbing: `begin_frame` sizes the QuerySet, `resolve`
// encodes `ResolveQuerySet` + a copy into a host-mappable buffer, maps
// it, and decodes the ticks into `last_resolved` so `read_back` actually
// returns live GPU numbers.
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
    pass_count_ = 0;
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

    runtime::log_info(
        "GpuTimestamps: timestamp-query present but resolve/readback is not wired - using CPU time");
    return false;
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

bool GpuTimestamps::resolve(void* dawn_instance_handle) {
    if (support_ != GpuTimestampSupport::supported || impl_ == nullptr) {
        return false;
    }
    // begin_frame() must have created the QuerySet + buffers first. A
    // zero-pass frame leaves them null; nothing to resolve.
    if (impl_->query_set == nullptr || impl_->resolve_buffer == nullptr ||
        impl_->readback_buffer == nullptr || impl_->slot_capacity == 0) {
        return false;
    }

    // The instance is required to pump `ProcessEvents` while the async
    // buffer map completes — Dawn never advances callbacks on its own.
    if (dawn_instance_handle == nullptr) {
        return false;
    }
    auto* instance = static_cast<wgpu::Instance*>(dawn_instance_handle);
    if (instance == nullptr || *instance == nullptr) {
        return false;
    }

    const std::uint64_t bytes =
        static_cast<std::uint64_t>(impl_->slot_capacity) * sizeof(std::uint64_t);

    // Encode: ResolveQuerySet writes the QuerySet's ticks into the
    // resolve buffer; CopyBufferToBuffer stages them into the host-
    // mappable readback buffer. One encoder, one submit.
    wgpu::CommandEncoderDescriptor enc_desc{};
    enc_desc.label = "Pulp Timestamp Resolve";
    wgpu::CommandEncoder encoder = impl_->device.CreateCommandEncoder(&enc_desc);
    encoder.ResolveQuerySet(impl_->query_set, 0, impl_->slot_capacity,
                            impl_->resolve_buffer, 0);
    encoder.CopyBufferToBuffer(impl_->resolve_buffer, 0,
                               impl_->readback_buffer, 0, bytes);
    wgpu::CommandBuffer cmd = encoder.Finish();
    impl_->device.GetQueue().Submit(1, &cmd);

    // Map the readback buffer for host reads. The map is asynchronous;
    // pump `ProcessEvents` until the callback fires or a deadline trips.
    // The deadline mirrors `gpu_compute.cpp`'s readback path so a wedged
    // GPU degrades to "no sample this frame" instead of hanging.
    bool mapped = false;
    bool ok = false;
    impl_->readback_buffer.MapAsync(
        wgpu::MapMode::Read, 0, static_cast<std::size_t>(bytes),
        wgpu::CallbackMode::AllowProcessEvents,
        [&mapped, &ok](wgpu::MapAsyncStatus status, wgpu::StringView) {
            mapped = true;
            ok = (status == wgpu::MapAsyncStatus::Success);
        });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!mapped && std::chrono::steady_clock::now() < deadline) {
        instance->ProcessEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!mapped || !ok) {
        // On timeout the buffer may still be mid-map; leave it and keep
        // the previous `last_resolved` so the inspector shows stale-but-
        // honest numbers rather than flickering to empty.
        runtime::log_info("GpuTimestamps: timestamp readback did not map");
        return false;
    }

    const void* data = impl_->readback_buffer.GetConstMappedRange(
        0, static_cast<std::size_t>(bytes));
    if (data == nullptr) {
        impl_->readback_buffer.Unmap();
        return false;
    }

    // Decode the mapped bytes through the pure, Dawn-free helper — the
    // same path the unit tests exercise with synthetic bytes.
    impl_->last_resolved = decode_resolved_ticks(
        static_cast<const std::byte*>(data), static_cast<std::size_t>(bytes));
    impl_->readback_buffer.Unmap();
    return !impl_->last_resolved.empty();
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

bool GpuTimestamps::resolve(void* /*dawn_instance_handle*/) { return false; }

std::vector<std::uint64_t> GpuTimestamps::read_back() const { return {}; }

#endif // PULP_HAS_SKIA

} // namespace pulp::render
