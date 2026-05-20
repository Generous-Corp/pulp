#include <pulp/render/gpu_timestamp.hpp>

// Phase 6.5 — Dawn GPU timestamp queries.
//
// The query-set lifecycle (allocation, PassTimestampWrites, ResolveQuerySet,
// map-readback) is Dawn-coupled and cannot run without a live device, so it
// lives here behind the WebGPU build guard. The tick -> millisecond
// conversion math lives in the header (`GpuTimestampResolver`) and IS
// unit-tested without a device — see test/test_render_gpu_timestamp.cpp.

#ifdef PULP_HAS_SKIA
// Skia builds use Dawn's C++ API so the timer shares the Dawn device that
// GpuSurface created (see gpu_surface_dawn.cpp's matching guard).

#include <pulp/runtime/log.hpp>
#include "webgpu/webgpu_cpp.h"

#include <atomic>
#include <cstring>

namespace pulp::render {
namespace {

/// Two timestamp slots per pass: beginning-of-pass + end-of-pass.
constexpr std::size_t kSlotsPerPass = 2;
/// WebGPU timestamps are 8-byte (uint64) values.
constexpr std::size_t kBytesPerTimestamp = sizeof(uint64_t);

class DawnPassTimer final : public GpuPassTimer {
public:
    DawnPassTimer(wgpu::Device device, wgpu::Queue queue,
                  std::size_t pass_count)
        : device_(std::move(device)),
          queue_(std::move(queue)),
          pass_count_(pass_count) {
        // The device must have been created with the `timestamp-query`
        // feature. GpuSurface requests it best-effort; if the adapter
        // lacked it the device simply will not report it here, and we
        // degrade to an all-no-op timer.
        if (!device_ || pass_count_ == 0) {
            return;
        }
        if (!device_.HasFeature(wgpu::FeatureName::TimestampQuery)) {
            runtime::log_info(
                "GpuPassTimer: device lacks timestamp-query feature — "
                "GPU timings unavailable, falling back to CPU time");
            return;
        }

        const uint32_t slot_count =
            static_cast<uint32_t>(pass_count_ * kSlotsPerPass);

        wgpu::QuerySetDescriptor query_desc{};
        query_desc.label = "Pulp pass timestamp queries";
        query_desc.type = wgpu::QueryType::Timestamp;
        query_desc.count = slot_count;
        query_set_ = device_.CreateQuerySet(&query_desc);

        const uint64_t bytes =
            static_cast<uint64_t>(slot_count) * kBytesPerTimestamp;

        // ResolveQuerySet writes into this buffer; it must be CopySrc so
        // we can copy it into the map-readable buffer below.
        wgpu::BufferDescriptor resolve_desc{};
        resolve_desc.label = "Pulp timestamp resolve buffer";
        resolve_desc.size = bytes;
        resolve_desc.usage =
            wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
        resolve_buffer_ = device_.CreateBuffer(&resolve_desc);

        // A MapRead buffer cannot also be a QueryResolve target, so the
        // resolved ticks are copied here for host readback.
        wgpu::BufferDescriptor readback_desc{};
        readback_desc.label = "Pulp timestamp readback buffer";
        readback_desc.size = bytes;
        readback_desc.usage =
            wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        readback_buffer_ = device_.CreateBuffer(&readback_desc);

        // Pre-build the PassTimestampWrites: pass i writes its begin slot
        // at index 2*i and its end slot at 2*i+1.
        writes_.resize(pass_count_);
        for (std::size_t i = 0; i < pass_count_; ++i) {
            writes_[i].querySet = query_set_;
            writes_[i].beginningOfPassWriteIndex =
                static_cast<uint32_t>(i * kSlotsPerPass);
            writes_[i].endOfPassWriteIndex =
                static_cast<uint32_t>(i * kSlotsPerPass + 1);
        }

        supported_ = query_set_ && resolve_buffer_ && readback_buffer_;
        byte_size_ = bytes;
    }

    std::size_t pass_capacity() const override { return pass_count_; }
    bool supported() const override { return supported_; }

    void* timestamp_writes_for_pass(std::size_t pass_index) override {
        if (!supported_ || pass_index >= writes_.size()) {
            return nullptr;
        }
        return &writes_[pass_index];
    }

    void resolve(void* command_encoder) override {
        if (!supported_ || command_encoder == nullptr) {
            return;
        }
        // A map is still in flight from a previous frame — skip this
        // frame's resolve rather than racing the readback buffer.
        if (map_pending_) {
            return;
        }
        auto& encoder = *static_cast<wgpu::CommandEncoder*>(command_encoder);
        const uint32_t slot_count =
            static_cast<uint32_t>(pass_count_ * kSlotsPerPass);
        encoder.ResolveQuerySet(query_set_, 0, slot_count, resolve_buffer_,
                                0);
        encoder.CopyBufferToBuffer(resolve_buffer_, 0, readback_buffer_, 0,
                                   byte_size_);
        resolved_once_ = true;
    }

    std::vector<GpuPassTiming> read_back(
        const GpuTimestampResolver& resolver) override {
        std::vector<GpuPassTiming> out(pass_count_);
        if (!supported_ || !resolved_once_) {
            return out;  // all invalid — first frame or unsupported
        }

        // Kick a map of the readback buffer. The copy recorded by the
        // previous resolve() has had a full frame to complete, so the
        // map normally resolves within one Tick().
        if (!map_pending_) {
            map_pending_ = true;
            map_ready_.store(false, std::memory_order_relaxed);
            readback_buffer_.MapAsync(
                wgpu::MapMode::Read, 0, byte_size_,
                wgpu::CallbackMode::AllowProcessEvents,
                [this](wgpu::MapAsyncStatus status, wgpu::StringView) {
                    map_ok_ = (status == wgpu::MapAsyncStatus::Success);
                    map_ready_.store(true, std::memory_order_relaxed);
                });
        }

        // Drive Dawn's callback pump. One tick is usually enough; cap the
        // spins so a stalled GPU can never wedge the UI thread.
        for (int spin = 0; spin < 8 &&
                            !map_ready_.load(std::memory_order_relaxed);
             ++spin) {
            device_.Tick();
        }

        if (!map_ready_.load(std::memory_order_relaxed)) {
            // Map still not done — return invalid timings; the next
            // read_back() call observes the same in-flight map and waits.
            return out;
        }

        map_pending_ = false;
        if (!map_ok_) {
            return out;
        }

        const std::size_t slot_count = pass_count_ * kSlotsPerPass;
        const void* mapped =
            readback_buffer_.GetConstMappedRange(0, byte_size_);
        if (mapped != nullptr) {
            std::vector<uint64_t> ticks(slot_count);
            std::memcpy(ticks.data(), mapped,
                        slot_count * kBytesPerTimestamp);
            out = resolver.resolve_passes(ticks, pass_count_);
        }
        readback_buffer_.Unmap();
        return out;
    }

private:
    wgpu::Device device_;
    wgpu::Queue queue_;
    std::size_t pass_count_ = 0;
    wgpu::QuerySet query_set_;
    wgpu::Buffer resolve_buffer_;
    wgpu::Buffer readback_buffer_;
    std::vector<wgpu::PassTimestampWrites> writes_;
    uint64_t byte_size_ = 0;
    bool supported_ = false;
    bool resolved_once_ = false;
    bool map_pending_ = false;
    bool map_ok_ = false;
    std::atomic<bool> map_ready_{false};
};

}  // namespace

std::unique_ptr<GpuPassTimer> GpuPassTimer::create(void* dawn_device,
                                                   void* dawn_queue,
                                                   std::size_t pass_count) {
    if (dawn_device == nullptr || dawn_queue == nullptr) {
        return nullptr;
    }
    auto device = *static_cast<wgpu::Device*>(dawn_device);
    auto queue = *static_cast<wgpu::Queue*>(dawn_queue);
    return std::make_unique<DawnPassTimer>(std::move(device),
                                           std::move(queue), pass_count);
}

}  // namespace pulp::render

#else  // !PULP_HAS_SKIA
// No Dawn C++ API available (WebGPU-less build, or the wgpu-native
// fallback path which does not integrate with Skia/Graphite). GPU
// timestamps are unavailable; callers fall back to CPU timings.

namespace pulp::render {

std::unique_ptr<GpuPassTimer> GpuPassTimer::create(void* /*dawn_device*/,
                                                   void* /*dawn_queue*/,
                                                   std::size_t /*pass*/) {
    return nullptr;
}

}  // namespace pulp::render

#endif  // PULP_HAS_SKIA
