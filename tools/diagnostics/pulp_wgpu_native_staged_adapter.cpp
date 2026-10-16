#include "detail/shared_io_arena.hpp"
#include "detail/shared_io_program_session.hpp"

#include <pulp/gpu_audio/gpu_audio_program.hpp>

#include <CommonCrypto/CommonDigest.h>
#include <webgpu/webgpu.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// This file is a diagnostic-only provider/program spike.  It intentionally
// implements just the staged-buffer rechannel fragment used by the historical
// WaveNet probe.  The realtime scheduler, slot ledger, terminal inbox, and
// release ordering are exercised through SharedIoArena unchanged.  It is not
// compiled into pulp-gpu-audio and does not advertise a production provider.

namespace {

using pulp::gpu_audio::detail::SharedIoArenaProvider;
using pulp::gpu_audio::detail::SharedIoPreparedProgram;
using pulp::gpu_audio::detail::SharedIoProviderCapabilities;
using pulp::gpu_audio::detail::SharedIoProviderIdentity;
using pulp::gpu_audio::detail::SharedIoTerminalInbox;
using pulp::gpu_audio::detail::SharedIoTerminalStatus;

constexpr const char* kShader = R"wgsl(
struct Params { in_ch:u32, out_ch:u32, B:u32, woff:u32 };
@group(0) @binding(0) var<storage, read> weights:array<f32>;
@group(0) @binding(1) var<storage, read> input:array<f32>;
@group(0) @binding(2) var<storage, read_write> output:array<f32>;
@group(0) @binding(3) var<uniform> params:Params;
@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id:vec3u) {
  let t=id.x;
  if (t >= params.B) { return; }
  for (var oc=0u; oc<params.out_ch; oc++) {
    var value=0.0;
    let base=params.woff + oc*params.in_ch;
    for (var ic=0u; ic<params.in_ch; ic++) {
      value += weights[base+ic] * input[t*params.in_ch+ic];
    }
    output[t*params.out_ch+oc]=value;
  }
}
)wgsl";

constexpr int kProviderUnavailableExit = 77;
constexpr int kDiagnosticFailureExit = 16;

struct AdapterWait {
    WGPUAdapter adapter = nullptr;
    WGPURequestAdapterStatus status = WGPURequestAdapterStatus_Unknown;
    bool done = false;
};

struct DeviceWait {
    WGPUDevice device = nullptr;
    WGPURequestDeviceStatus status = WGPURequestDeviceStatus_Unknown;
    bool done = false;
};

struct MapWait {
    WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Unknown;
    bool done = false;
};

struct DeviceLossWait {
    bool observed = false;
    const void* callback_userdata = nullptr;
    WGPUDeviceLostReason reason = WGPUDeviceLostReason_Unknown;
    std::string message;
};

const char* string_view(WGPUStringView value) {
    static thread_local char buffer[512];
    const auto size = std::min<std::size_t>(value.length, sizeof(buffer) - 1);
    if (value.data != nullptr && size != 0)
        std::memcpy(buffer, value.data, size);
    buffer[size] = '\0';
    return buffer;
}

std::string sha256_hex(const void* data, std::size_t size) {
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256(data, static_cast<CC_LONG>(size), digest.data());
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

void adapter_callback(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message,
                      void* userdata, void*) {
    auto* result = static_cast<AdapterWait*>(userdata);
    result->status = status;
    result->adapter = adapter;
    result->done = true;
    if (status != WGPURequestAdapterStatus_Success)
        std::fprintf(stderr, "wgpu adapter: %s\n", string_view(message));
}

void device_callback(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message,
                     void* userdata, void*) {
    auto* result = static_cast<DeviceWait*>(userdata);
    result->status = status;
    result->device = device;
    result->done = true;
    if (status != WGPURequestDeviceStatus_Success)
        std::fprintf(stderr, "wgpu device: %s\n", string_view(message));
}

void uncaptured_error(const WGPUDevice*, WGPUErrorType type, WGPUStringView message, void*, void*) {
    std::fprintf(stderr, "wgpu error %d: %s\n", static_cast<int>(type), string_view(message));
}

void device_lost_callback(const WGPUDevice*, WGPUDeviceLostReason reason, WGPUStringView message,
                          void* userdata, void*) {
    auto* loss = static_cast<DeviceLossWait*>(userdata);
    loss->callback_userdata = userdata;
    loss->reason = reason;
    loss->message = string_view(message);
    loss->observed = true;
}

bool process_until(WGPUInstance instance, bool& done) {
    for (int attempt = 0; attempt < 10'000 && !done; ++attempt) {
        wgpuInstanceProcessEvents(instance);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done;
}

class WgpuNativeStagedProvider;

class WgpuNativeStagedProgram final : public SharedIoPreparedProgram {
  public:
    explicit WgpuNativeStagedProgram(WgpuNativeStagedProvider& provider);
    ~WgpuNativeStagedProgram() override;

    bool prepare(SharedIoArenaProvider& provider,
                 std::span<const SlotBufferHandle> slots) noexcept override;
    bool submit(SharedIoArenaProvider& provider, const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept override;
    bool release() noexcept override;

  private:
    WgpuNativeStagedProvider* provider_ = nullptr;
    WGPUShaderModule shader_ = nullptr;
    WGPUComputePipeline pipeline_ = nullptr;
    WGPUBindGroupLayout bind_group_layout_ = nullptr;
    bool prepared_ = false;
};

class WgpuNativeStagedProvider final : public SharedIoArenaProvider {
    struct Slot {
        std::uint32_t index = 0;
        std::vector<std::byte> input;
        std::vector<std::byte> output;
        WGPUBuffer input_gpu = nullptr;
        WGPUBuffer output_gpu = nullptr;
        WGPUBuffer readback_gpu = nullptr;
        WGPUBuffer weights_gpu = nullptr;
        WGPUBuffer params_gpu = nullptr;
        bool retired = false;
    };

    struct Pending {
        WgpuNativeStagedProvider* owner = nullptr;
        Slot* slot = nullptr;
        SlotToken token;
        std::shared_ptr<SharedIoTerminalInbox> inbox;
        bool queue_done = false;
        WGPUQueueWorkDoneStatus queue_status = WGPUQueueWorkDoneStatus_Unknown;
        bool map_started = false;
        MapWait map_wait;
        bool map_done = false;
        bool map_unmapped = false;
        bool terminal_ready = false;
        SharedIoTerminalStatus terminal_status = SharedIoTerminalStatus::RetiredFailed;
        bool terminal_published = false;
        bool planted_failure = false;
        bool queue_failed = false;
    };

  public:
    static std::unique_ptr<WgpuNativeStagedProvider> create() noexcept {
        try {
            auto provider = std::unique_ptr<WgpuNativeStagedProvider>(new WgpuNativeStagedProvider);
            if (!provider->initialize())
                return nullptr;
            return provider;
        } catch (...) {
            return nullptr;
        }
    }

    ~WgpuNativeStagedProvider() override {
        if (!drain()) {
            // A callback that has not reached its terminal state still owns
            // the provider, slot buffers, and userdata. Do not tear any of
            // those down underneath it; this diagnostic fails closed instead.
            std::fputs("wgpu staged provider: undrained callback; aborting\n", stderr);
            std::abort();
        }
        for (auto& slot : slots_)
            release_slot_buffers(*slot);
        if (queue_ != nullptr)
            wgpuQueueRelease(queue_);
        if (device_ != nullptr)
            wgpuDeviceRelease(device_);
        if (adapter_ != nullptr)
            wgpuAdapterRelease(adapter_);
        if (instance_ != nullptr)
            wgpuInstanceRelease(instance_);
    }

    WgpuNativeStagedProvider(const WgpuNativeStagedProvider&) = delete;
    WgpuNativeStagedProvider& operator=(const WgpuNativeStagedProvider&) = delete;

    std::unique_ptr<SharedIoPreparedProgram> make_program() {
        return std::unique_ptr<SharedIoPreparedProgram>(new WgpuNativeStagedProgram(*this));
    }

    bool create_slot(std::uint32_t index, std::size_t input_bytes, std::size_t output_bytes,
                     SlotResources& resources) noexcept override {
        std::unique_ptr<Slot> slot;
        try {
            // Reserve the fixed pending table before creating native resources.
            // A later submission can then retain its terminal userdata without
            // allocating after Queue::Submit has started.
            pending_.reserve(slots_.size() + 1);
            slots_.reserve(slots_.size() + 1);
            slot = std::make_unique<Slot>();
            slot->index = index;
            slot->input.resize(input_bytes);
            slot->output.resize(output_bytes);
            slot->input_gpu =
                make_buffer(input_bytes, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
            slot->output_gpu =
                make_buffer(output_bytes, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
            slot->readback_gpu =
                make_buffer(output_bytes, WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead);
            constexpr std::array<float, 3> weights{{-0.6180188f, 1.0314025f, -1.0111561f}};
            slot->weights_gpu =
                make_buffer(sizeof(weights), WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
            struct Params {
                std::uint32_t in_ch, out_ch, block, weight_offset;
            } params{1, 3, 64, 0};
            slot->params_gpu =
                make_buffer(sizeof(params), WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
            if (slot->input_gpu == nullptr || slot->output_gpu == nullptr ||
                slot->readback_gpu == nullptr || slot->weights_gpu == nullptr ||
                slot->params_gpu == nullptr) {
                release_slot_buffers(*slot);
                return false;
            }
            wgpuQueueWriteBuffer(queue_, slot->weights_gpu, 0, weights.data(), sizeof(weights));
            wgpuQueueWriteBuffer(queue_, slot->params_gpu, 0, &params, sizeof(params));
            resources.input = slot->input.data();
            resources.input_size = input_bytes;
            resources.output = slot->output.data();
            resources.output_size = output_bytes;
            resources.opaque = slot.get();
            resources.input_lifecycle.allocated = true;
            resources.output_lifecycle.allocated = true;
            resources.storage_kind = StorageKind::Staged;
            slots_.push_back(std::move(slot));
            return true;
        } catch (...) {
            if (slot != nullptr)
                release_slot_buffers(*slot);
            return false;
        }
    }

    void retire_slot(SlotResources& resources) noexcept override {
        if (auto* slot = static_cast<Slot*>(resources.opaque))
            slot->retired = true;
        resources.input_lifecycle.dispose_observed = true;
        resources.output_lifecycle.dispose_observed = true;
    }

    void destroy_slot(SlotResources& resources) noexcept override {
        auto* slot = static_cast<Slot*>(resources.opaque);
        if (slot == nullptr)
            return;
        release_slot_buffers(*slot);
        std::erase_if(slots_, [slot](const auto& candidate) { return candidate.get() == slot; });
        resources.input_lifecycle.host_freed = true;
        resources.output_lifecycle.host_freed = true;
        resources.opaque = nullptr;
    }

    bool acquire_slot_buffers(const SlotResources& resources,
                              SlotBufferHandle& handle) const noexcept override {
        const auto* slot = static_cast<const Slot*>(resources.opaque);
        if (slot == nullptr || slot->retired)
            return false;
        handle = {.provider = this,
                  .device = device_,
                  .input_buffer = slot->input_gpu,
                  .output_buffer = slot->output_gpu,
                  .slot = slot->index,
                  .generation = 1,
                  .lifetime = lifetime_};
        return true;
    }

    bool validate_slot_buffers(const SlotBufferHandle& handle) const noexcept override {
        return handle.provider == this && handle.device == device_ && !handle.lifetime.expired() &&
               std::any_of(slots_.begin(), slots_.end(), [&](const auto& slot) {
                   return !slot->retired && handle.slot == slot->index &&
                          handle.input_buffer == slot->input_gpu &&
                          handle.output_buffer == slot->output_gpu;
               });
    }

    bool submit(const SlotResources&, SlotToken,
                std::shared_ptr<SharedIoTerminalInbox>) noexcept override {
        return false;
    }

    void poll() noexcept override {
        if (instance_ != nullptr)
            wgpuInstanceProcessEvents(instance_);
        for (auto& pending : pending_) {
            if (pending->queue_done && pending->queue_status == WGPUQueueWorkDoneStatus_Success &&
                !pending->map_started)
                begin_map(*pending);
            else if (pending->queue_done &&
                     pending->queue_status != WGPUQueueWorkDoneStatus_Success) {
                pending->queue_failed = true;
            }
            if (physical_drain_ && pending->queue_failed && !pending->terminal_ready) {
                // Queue status alone is not a retirement proof. Publish only
                // after the provider-specific drain barrier has crossed the
                // device-loss boundary; ordinary queue errors stay quarantined.
                pending->terminal_status = SharedIoTerminalStatus::RetiredFailed;
                pending->terminal_ready = true;
            }
            if (pending->map_started && pending->map_done && !pending->terminal_ready)
                finish_map(*pending);
            if (pending->terminal_ready && !pending->terminal_published)
                publish_terminal(*pending);
        }
        std::erase_if(pending_, [](const auto& pending) {
            return pending->terminal_published && (!pending->map_started || pending->map_done);
        });
    }

    bool drain() noexcept override {
        if (device_loss_.observed)
            physical_drain_ = true;
        for (int attempt = 0; attempt < 10'000 && !pending_.empty(); ++attempt) {
            poll();
            if (!pending_.empty())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return pending_.empty();
    }

    SharedIoProviderIdentity provider_identity() const noexcept override {
        // The out-of-process verifier binds the loaded image to the pinned
        // manifest and executable. AdapterInfo alone cannot authenticate it.
        return {.authenticated = false,
                .provider_revision = {},
                .adapter_name = adapter_name_,
                .adapter_backend = "Metal-requested",
                .adapter_vendor_id = vendor_id_,
                .adapter_device_id = device_id_,
                .native_runtime_authenticated = false,
                .native_runtime_name = "wgpu-native",
                .native_runtime_backend = {}};
    }

    SharedIoProviderCapabilities provider_capabilities() const noexcept override {
        return {.imported_host_pointer = false,
                .ordered_causal_state = false,
                .completion_service = true,
                .device_loss_recovery = false,
                .gpu_timestamps = false};
    }

    bool device_lost() const noexcept override {
        return device_loss_.observed;
    }

    bool device_loss_callback_registered() const noexcept {
        // Registration is established by the native callback invoking this
        // exact state object, never by re-reading the descriptor we assigned.
        return device_loss_.observed && device_loss_.callback_userdata == &device_loss_;
    }

    // wgpu-native v24 exposes device destruction as the only portable,
    // controlled loss trigger. This is a diagnostic hook; production provider
    // selection and the public ABI never call it.
    bool force_device_loss_for_diagnostic() noexcept {
        if (device_ == nullptr || device_loss_.observed || loss_forced_)
            return false;
        loss_forced_ = true;
        wgpuDeviceDestroy(device_);
        return true;
    }

    bool device_loss_observed() const noexcept {
        return device_loss_.observed;
    }

    bool wait_for_device_loss_callback() noexcept {
        for (int attempt = 0; attempt < 10'000 && !device_loss_.observed; ++attempt) {
            if (instance_ != nullptr)
                wgpuInstanceProcessEvents(instance_);
            if (!device_loss_.observed)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return device_loss_.observed;
    }

    WGPUDeviceLostReason device_loss_reason() const noexcept {
        return device_loss_.reason;
    }

    bool submit_program(Slot& slot, SlotToken token, std::shared_ptr<SharedIoTerminalInbox> inbox,
                        WGPUComputePipeline pipeline, WGPUBindGroupLayout layout) noexcept {
        try {
            auto pending = std::make_unique<Pending>();
            pending->owner = this;
            pending->slot = &slot;
            pending->token = token;
            pending->inbox = std::move(inbox);
            pending->planted_failure = token.stream_sequence == 4;
            wgpuQueueWriteBuffer(queue_, slot.input_gpu, 0, slot.input.data(), slot.input.size());
            WGPUBuffer weights = slot.weights_gpu;
            WGPUBuffer input = slot.input_gpu;
            WGPUBuffer output = slot.output_gpu;
            WGPUBuffer params = slot.params_gpu;
            WGPUBindGroupEntry entries[4]{};
            entries[0] = {.binding = 0, .buffer = weights, .offset = 0, .size = 3 * sizeof(float)};
            entries[1] = {.binding = 1, .buffer = input, .offset = 0, .size = slot.input.size()};
            entries[2] = {.binding = 2, .buffer = output, .offset = 0, .size = slot.output.size()};
            entries[3] = {.binding = 3, .buffer = params, .offset = 0, .size = 16};
            WGPUBindGroupDescriptor bind_desc{};
            bind_desc.layout = layout;
            bind_desc.entryCount = 4;
            bind_desc.entries = entries;
            WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(device_, &bind_desc);
            if (bind_group == nullptr)
                return false;
            WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, nullptr);
            if (encoder == nullptr) {
                wgpuBindGroupRelease(bind_group);
                return false;
            }
            WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder, nullptr);
            if (pass == nullptr) {
                wgpuCommandEncoderRelease(encoder);
                wgpuBindGroupRelease(bind_group);
                return false;
            }
            wgpuComputePassEncoderSetPipeline(pass, pipeline);
            wgpuComputePassEncoderSetBindGroup(pass, 0, bind_group, 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(pass, 1, 1, 1);
            wgpuComputePassEncoderEnd(pass);
            wgpuComputePassEncoderRelease(pass);
            wgpuCommandEncoderCopyBufferToBuffer(encoder, output, 0, slot.readback_gpu, 0,
                                                 slot.output.size());
            WGPUCommandBuffer command = wgpuCommandEncoderFinish(encoder, nullptr);
            wgpuCommandEncoderRelease(encoder);
            wgpuBindGroupRelease(bind_group);
            if (command == nullptr)
                return false;
            // The pending table was reserved while slots were prepared, so
            // terminal userdata is retained before the queue submission.
            pending_.push_back(std::move(pending));
            auto* submitted = pending_.back().get();
            wgpuQueueSubmit(queue_, 1, &command);
            wgpuCommandBufferRelease(command);
            WGPUQueueWorkDoneCallbackInfo callback{};
            callback.mode = WGPUCallbackMode_AllowProcessEvents;
            callback.callback = [](WGPUQueueWorkDoneStatus status, void* userdata, void*) {
                auto* pending = static_cast<Pending*>(userdata);
                pending->queue_status = status;
                pending->queue_done = true;
            };
            callback.userdata1 = submitted;
            (void)wgpuQueueOnSubmittedWorkDone(queue_, callback);
            return true;
        } catch (...) {
            return false;
        }
    }

    WGPUDevice device() const noexcept {
        return device_;
    }

  private:
    WgpuNativeStagedProvider() = default;

    bool initialize() noexcept {
        WGPUInstanceDescriptor descriptor{};
        instance_ = wgpuCreateInstance(&descriptor);
        if (instance_ == nullptr) {
            std::fprintf(stderr, "wgpu instance creation failed\n");
            return false;
        }
        WGPURequestAdapterOptions options{};
        options.featureLevel = WGPUFeatureLevel_Core;
        options.powerPreference = WGPUPowerPreference_HighPerformance;
        options.backendType = WGPUBackendType_Metal;
        AdapterWait adapter_wait;
        WGPURequestAdapterCallbackInfo adapter_callback_info{};
        adapter_callback_info.mode = WGPUCallbackMode_AllowProcessEvents;
        adapter_callback_info.callback = adapter_callback;
        adapter_callback_info.userdata1 = &adapter_wait;
        (void)wgpuInstanceRequestAdapter(instance_, &options, adapter_callback_info);
        if (!process_until(instance_, adapter_wait.done) ||
            adapter_wait.status != WGPURequestAdapterStatus_Success) {
            std::fprintf(stderr, "wgpu adapter request failed done=%d status=%d\n",
                         adapter_wait.done, static_cast<int>(adapter_wait.status));
            return false;
        }
        adapter_ = adapter_wait.adapter;
        WGPUAdapterInfo info{};
        (void)wgpuAdapterGetInfo(adapter_, &info);
        adapter_name_ = string_view(info.device);
        vendor_id_ = info.vendorID;
        device_id_ = info.deviceID;
        WGPUDeviceDescriptor device_descriptor{};
        device_loss_.message.clear();
        WGPUDeviceLostCallbackInfo device_loss_callback_info{};
        device_loss_callback_info.mode = WGPUCallbackMode_AllowProcessEvents;
        device_loss_callback_info.callback = device_lost_callback;
        device_loss_callback_info.userdata1 = &device_loss_;
        device_descriptor.deviceLostCallbackInfo = device_loss_callback_info;
        WGPUUncapturedErrorCallbackInfo error_callback{};
        error_callback.callback = uncaptured_error;
        device_descriptor.uncapturedErrorCallbackInfo = error_callback;
        DeviceWait device_wait;
        WGPURequestDeviceCallbackInfo device_callback_info{};
        device_callback_info.mode = WGPUCallbackMode_AllowProcessEvents;
        device_callback_info.callback = device_callback;
        device_callback_info.userdata1 = &device_wait;
        (void)wgpuAdapterRequestDevice(adapter_, &device_descriptor, device_callback_info);
        if (!process_until(instance_, device_wait.done) ||
            device_wait.status != WGPURequestDeviceStatus_Success) {
            std::fprintf(stderr, "wgpu device request failed done=%d status=%d\n", device_wait.done,
                         static_cast<int>(device_wait.status));
            return false;
        }
        device_ = device_wait.device;
        queue_ = wgpuDeviceGetQueue(device_);
        return queue_ != nullptr;
    }

    WGPUBuffer make_buffer(std::size_t size, WGPUBufferUsage usage) const noexcept {
        WGPUBufferDescriptor descriptor{};
        descriptor.size = size;
        descriptor.usage = usage;
        return wgpuDeviceCreateBuffer(device_, &descriptor);
    }

    void release_slot_buffers(Slot& slot) noexcept {
        if (slot.params_gpu != nullptr)
            wgpuBufferRelease(slot.params_gpu);
        if (slot.weights_gpu != nullptr)
            wgpuBufferRelease(slot.weights_gpu);
        if (slot.readback_gpu != nullptr)
            wgpuBufferRelease(slot.readback_gpu);
        if (slot.output_gpu != nullptr)
            wgpuBufferRelease(slot.output_gpu);
        if (slot.input_gpu != nullptr)
            wgpuBufferRelease(slot.input_gpu);
        slot.params_gpu = nullptr;
        slot.weights_gpu = nullptr;
        slot.readback_gpu = nullptr;
        slot.output_gpu = nullptr;
        slot.input_gpu = nullptr;
    }

    void begin_map(Pending& pending) noexcept {
        pending.map_started = true;
        WGPUBufferMapCallbackInfo callback{};
        callback.mode = WGPUCallbackMode_AllowProcessEvents;
        callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* userdata, void*) {
            auto* pending = static_cast<Pending*>(userdata);
            pending->map_wait.status = status;
            pending->map_wait.done = true;
            pending->map_done = true;
        };
        pending.map_wait = {};
        callback.userdata1 = &pending;
        (void)wgpuBufferMapAsync(pending.slot->readback_gpu, WGPUMapMode_Read, 0,
                                 pending.slot->output.size(), callback);
    }

    void finish_map(Pending& pending) noexcept {
        bool valid_readback = false;
        if (pending.map_wait.status == WGPUMapAsyncStatus_Success) {
            const auto* mapped = static_cast<const std::byte*>(wgpuBufferGetConstMappedRange(
                pending.slot->readback_gpu, 0, pending.slot->output.size()));
            valid_readback = mapped != nullptr;
            if (valid_readback && !pending.planted_failure)
                std::memcpy(pending.slot->output.data(), mapped, pending.slot->output.size());
            wgpuBufferUnmap(pending.slot->readback_gpu);
            pending.map_unmapped = true;
        }
        pending.terminal_status = valid_readback && !pending.planted_failure
                                      ? SharedIoTerminalStatus::RetiredSuccess
                                      : SharedIoTerminalStatus::RetiredFailed;
        pending.terminal_ready = true;
    }

    void publish_terminal(Pending& pending) noexcept {
        const auto result = pending.inbox->push(pending.token, pending.terminal_status);
        if (result == SharedIoTerminalInbox::PushResult::Accepted ||
            result == SharedIoTerminalInbox::PushResult::Rejected)
            pending.terminal_published = true;
    }

    // Callback state must outlive every native handle: raw handles are
    // declared after it and therefore destroyed first during teardown.
    DeviceLossWait device_loss_;
    bool loss_forced_ = false;
    bool physical_drain_ = false;
    WGPUInstance instance_ = nullptr;
    WGPUAdapter adapter_ = nullptr;
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    std::string adapter_name_;
    std::uint32_t vendor_id_ = 0;
    std::uint32_t device_id_ = 0;
    std::shared_ptr<const void> lifetime_ = std::make_shared<int>(0);
    std::vector<std::unique_ptr<Slot>> slots_;
    std::vector<std::unique_ptr<Pending>> pending_;

    friend class WgpuNativeStagedProgram;
};

WgpuNativeStagedProgram::WgpuNativeStagedProgram(WgpuNativeStagedProvider& provider)
    : provider_(&provider) {
    WGPUChainedStruct chain{};
    chain.sType = WGPUSType_ShaderSourceWGSL;
    WGPUShaderSourceWGSL source{};
    source.chain = chain;
    source.code = {kShader, std::strlen(kShader)};
    WGPUShaderModuleDescriptor shader_descriptor{};
    shader_descriptor.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&source);
    shader_ = wgpuDeviceCreateShaderModule(provider.device(), &shader_descriptor);
    if (shader_ == nullptr)
        return;
    WGPUProgrammableStageDescriptor stage{};
    stage.module = shader_;
    stage.entryPoint = {"main", 4};
    WGPUComputePipelineDescriptor pipeline_descriptor{};
    pipeline_descriptor.compute = stage;
    pipeline_ = wgpuDeviceCreateComputePipeline(provider.device(), &pipeline_descriptor);
    if (pipeline_ != nullptr)
        bind_group_layout_ = wgpuComputePipelineGetBindGroupLayout(pipeline_, 0);
}

WgpuNativeStagedProgram::~WgpuNativeStagedProgram() {
    (void)release();
}

bool WgpuNativeStagedProgram::prepare(SharedIoArenaProvider& provider,
                                      std::span<const SlotBufferHandle> slots) noexcept {
    if (prepared_ || provider_ != &provider || slots.empty() || pipeline_ == nullptr ||
        bind_group_layout_ == nullptr)
        return false;
    for (const auto& slot : slots)
        if (!provider.validate_slot_buffers(slot))
            return false;
    prepared_ = true;
    return true;
}

bool WgpuNativeStagedProgram::submit(
    SharedIoArenaProvider& provider, const SlotResources& resources, SlotToken token,
    std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept {
    if (!prepared_ || provider_ != &provider || terminal_inbox == nullptr)
        return false;
    auto& native = static_cast<WgpuNativeStagedProvider&>(provider);
    auto* slot = static_cast<WgpuNativeStagedProvider::Slot*>(resources.opaque);
    if (slot == nullptr || slot->retired)
        return false;
    return native.submit_program(*slot, token, std::move(terminal_inbox), pipeline_,
                                 bind_group_layout_);
}

bool WgpuNativeStagedProgram::release() noexcept {
    prepared_ = false;
    if (bind_group_layout_ != nullptr)
        wgpuBindGroupLayoutRelease(bind_group_layout_);
    if (pipeline_ != nullptr)
        wgpuComputePipelineRelease(pipeline_);
    if (shader_ != nullptr)
        wgpuShaderModuleRelease(shader_);
    bind_group_layout_ = nullptr;
    pipeline_ = nullptr;
    shader_ = nullptr;
    provider_ = nullptr;
    return true;
}

} // namespace

namespace {

bool validate_contract_admission() noexcept {
    using namespace pulp::gpu_audio;
    GpuAudioProgramDescriptor program{
        .kind = GpuAudioProgramKind::Neural,
        .path = GpuAudioExecutionPath::Staged,
        .provider = GpuAudioProvider::Unknown,
        .miss_policy = MissPolicy::Silence,
        .channels = 1,
        .block_size = 64,
        .sample_rate = 48'000,
        .algorithmic_lead_blocks = 1,
        .pipeline_depth = 3,
        .provider_slots = 2,
        .provider_owned_resources = false,
        .cpu_fallback_prepared = false,
    };
    if (!validate_gpu_audio_program(program).accepted())
        return false;

    auto expect = [](GpuAudioProgramDescriptor candidate, GpuAudioProgramError error) {
        return validate_gpu_audio_program(candidate).error == error;
    };
    auto missing_lead = program;
    missing_lead.algorithmic_lead_blocks = 0;
    if (!expect(missing_lead, GpuAudioProgramError::MissingAlgorithmicLead))
        return false;
    auto shallow_pipeline = program;
    shallow_pipeline.pipeline_depth = 1;
    if (!expect(shallow_pipeline, GpuAudioProgramError::InsufficientPipelineDepth))
        return false;
    auto resources_on_staged = program;
    resources_on_staged.provider_owned_resources = true;
    if (!expect(resources_on_staged, GpuAudioProgramError::ProviderResourcesOnStaged))
        return false;
    auto missing_fallback = program;
    missing_fallback.miss_policy = MissPolicy::CpuFallback;
    if (!expect(missing_fallback, GpuAudioProgramError::CpuFallbackNotPrepared))
        return false;

    GpuAudioCapabilityReport capability{
        .path = GpuAudioExecutionPath::Staged,
        .provider = GpuAudioProvider::Unknown,
        .eligibility = GpuAudioEligibility::Eligible,
        .fallback_policy = MissPolicy::Silence,
        .prepared_lead_blocks = 1,
        .prepared = true,
        .fallback_available = false,
        .diagnostics_available = true,
        .prepared_latency_blocks = 1,
    };
    if (!validate_gpu_audio_program(program, capability).accepted())
        return false;
    capability.path = GpuAudioExecutionPath::SharedMemory;
    return validate_gpu_audio_program(program, capability).error ==
           GpuAudioProgramError::CapabilityPathMismatch;
}

} // namespace

int main() {
    using pulp::gpu_audio::detail::SharedIoProgramSession;
    if (!validate_contract_admission()) {
        std::puts("wgpu_native_contract_admission=failed");
        return 12;
    }
    Dl_info loaded{};
    if (dladdr(reinterpret_cast<const void*>(&wgpuCreateInstance), &loaded) == 0 ||
        loaded.dli_fname == nullptr)
        return 78;
    auto provider = WgpuNativeStagedProvider::create();
    if (provider == nullptr) {
        std::puts("wgpu_native_provider=unavailable");
        return 77;
    }
    auto program = provider->make_program();
    SharedIoProgramSession session;
    if (program == nullptr ||
        !session.prepare({std::move(provider), std::move(program)},
                         {.slots = 2,
                          .input_bytes_per_slot = 64 * sizeof(float),
                          .output_bytes_per_slot = 64 * 3 * sizeof(float),
                          .storage_kind = SharedIoArenaProvider::StorageKind::Staged})) {
        std::puts("wgpu_native_session=failed");
        return 2;
    }

    // Exercise the fixed scheduler with two admitted blocks before waiting for
    // either terminal result. This remains a private staged diagnostic; it is
    // not realtime or product/provider evidence.
    std::array<std::optional<SharedIoProgramSession::InputLease>, 2> inflight;
    for (std::size_t i = 0; i < inflight.size(); ++i) {
        inflight[i] = session.acquire_input(100 + i, 0);
        if (!inflight[i] || !session.submit({inflight[i]->token, 0}))
            return 13;
    }
    std::array<std::optional<SharedIoProgramSession::Completion>, 2> inflight_completions;
    for (int attempt = 0;
         attempt < 10'000 && (!inflight_completions[0] || !inflight_completions[1]); ++attempt) {
        (void)session.service(0);
        for (auto& completion : inflight_completions)
            if (!completion)
                completion = session.pop_completion();
        if (!inflight_completions[0] || !inflight_completions[1])
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (const auto& completion : inflight_completions) {
        if (!completion || completion->status != SharedIoTerminalStatus::RetiredSuccess)
            return 14;
        auto output = session.acquire_output(*completion);
        if (!output || !session.release_output({output->token}))
            return 15;
    }
    constexpr std::array<float, 3> weights{{-0.6180188f, 1.0314025f, -1.0111561f}};
    std::uint32_t successes = 0;
    std::uint32_t failures = 0;
    float max_error = 0.0f;
    std::array<float, 6 * 64> submitted_inputs{};
    for (std::uint64_t sequence = 0; sequence < 6; ++sequence) {
        auto lease = session.acquire_input(sequence, 0);
        if (!lease || lease->bytes.size() != 64 * sizeof(float))
            return 3;
        auto* input = reinterpret_cast<float*>(lease->bytes.data());
        for (std::size_t t = 0; t < 64; ++t)
            input[t] = static_cast<float>(0.17 * std::sin(0.11 * (sequence * 64 + t)) +
                                          0.03 * std::cos(0.037 * (sequence * 64 + t)));
        std::memcpy(submitted_inputs.data() + sequence * 64, input, 64 * sizeof(float));
        if (!session.submit({lease->token, 0}))
            return 4;
        std::optional<SharedIoProgramSession::Completion> completion;
        for (int attempt = 0; attempt < 10'000 && !completion; ++attempt) {
            (void)session.service(0);
            completion = session.pop_completion();
            if (!completion)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!completion || completion->token.slot.stream_sequence != sequence)
            return 5;
        if (sequence == 4) {
            if (completion->status != SharedIoTerminalStatus::RetiredFailed ||
                session.acquire_output(*completion) || !session.discard_completion(*completion))
                return 6;
            ++failures;
            continue;
        }
        if (completion->status != SharedIoTerminalStatus::RetiredSuccess)
            return 7;
        auto output = session.acquire_output(*completion);
        if (!output || output->bytes.size() != 64 * 3 * sizeof(float))
            return 8;
        const auto* values = reinterpret_cast<const float*>(output->bytes.data());
        for (std::size_t t = 0; t < 64; ++t)
            for (std::size_t channel = 0; channel < weights.size(); ++channel) {
                const float expected = input[t] * weights[channel];
                const float actual = values[t * weights.size() + channel];
                if (!std::isfinite(actual))
                    return 9;
                max_error = std::max(max_error, std::abs(actual - expected));
            }
        if (!session.release_output({output->token}))
            return 10;
        ++successes;
    }
    if (!session.release() || successes != 5 || failures != 1 || max_error > 1.0e-5f)
        return 11;

    // Exercise the pinned runtime's controlled loss path in a fresh, bounded
    // session. The device is destroyed while one submission is outstanding;
    // the queue callback must become a terminal failure and no output lease
    // may be acquired from that record. A lost device cannot prove that the
    // existing prepared state can resume, so re-prime remains typed unavailable.
    bool loss_callback_registered = false;
    bool loss_scenario_triggered = false;
    bool loss_no_output_leases = false;
    std::uint32_t loss_terminal_records = 0;
    WGPUDeviceLostReason loss_reason = WGPUDeviceLostReason_Unknown;
    {
        // Probe the controlled loss trigger before preparing an outstanding
        // submission. Some pinned wgpu-native images accept the callback
        // descriptor but never deliver a loss notification for destroy();
        // classify that explicit capability gap as provider-unavailable so
        // the test skips without leaving callback userdata in flight.
        auto loss_probe = WgpuNativeStagedProvider::create();
        if (loss_probe == nullptr)
            return kProviderUnavailableExit;
        if (!loss_probe->force_device_loss_for_diagnostic())
            return kDiagnosticFailureExit;
        if (!loss_probe->wait_for_device_loss_callback()) {
            std::fputs("wgpu staged device-loss observation unavailable\n", stderr);
            // Some pinned wgpu-native images leave their internal Metal fence
            // wait blocked after destroy() when no loss callback is delivered.
            // Releasing that provider here would hang the verifier during
            // teardown, turning the typed unavailable result into a timeout.
            // The isolated diagnostic process is exiting with 77, so leak the
            // provider and let the OS reclaim its native handles.
            loss_probe.release();
            return kProviderUnavailableExit;
        }
        if (!loss_probe->device_loss_callback_registered())
            return kDiagnosticFailureExit;
        if (loss_probe->device_loss_reason() != WGPUDeviceLostReason_Destroyed)
            return kDiagnosticFailureExit;
        // The probe is capability-only. Even after a successful callback, the
        // pinned Metal runtime can block while releasing its idle device
        // fence. Keep the validated result and abandon this process-scoped
        // probe before entering the outstanding-submission session.
        loss_probe.release();

        // The probe above is a capability gate for the session path: only a
        // runtime that delivered the exact callback userdata and Destroyed
        // reason is allowed to enter the outstanding-submission scenario.
        // Its normal callback path therefore keeps the regular drain/release
        // cleanup below; only the probe's typed-unavailable branch abandons
        // ownership to avoid the pinned runtime's blocked fence teardown.
        auto loss_provider = WgpuNativeStagedProvider::create();
        if (loss_provider == nullptr)
            return kProviderUnavailableExit;
        auto loss_program = loss_provider->make_program();
        auto loss_session = std::make_unique<SharedIoProgramSession>();
        // Once the second loss session has submitted work, a provider failure
        // can leave a pinned Metal fence blocked during normal C++ teardown.
        // Keep this diagnostic fail-closed: abandon the whole session on that
        // diagnostic-failure path so its native handles die with the process.
        const auto abandon_loss_session = [&]() noexcept {
            loss_session.release();
            return kDiagnosticFailureExit;
        };
        if (loss_program == nullptr ||
            !loss_session->prepare({std::move(loss_provider), std::move(loss_program)},
                                   {.slots = 2,
                                    .input_bytes_per_slot = 64 * sizeof(float),
                                    .output_bytes_per_slot = 64 * 3 * sizeof(float),
                                    .storage_kind = SharedIoArenaProvider::StorageKind::Staged}))
            return abandon_loss_session();
        auto* loss_native = static_cast<WgpuNativeStagedProvider*>(loss_session->owned_provider());
        auto loss_input = loss_session->acquire_input(902, 0);
        if (!loss_input || !loss_session->submit({loss_input->token, 0}) ||
            !loss_native->force_device_loss_for_diagnostic())
            return abandon_loss_session();
        loss_scenario_triggered = true;

        std::optional<SharedIoProgramSession::Completion> loss_completion;
        loss_callback_registered = loss_native->wait_for_device_loss_callback() &&
                                   loss_native->device_loss_callback_registered();
        (void)loss_session->service(0);
        if (!loss_callback_registered || !loss_native->drain())
            return abandon_loss_session();
        for (int attempt = 0; attempt < 10'000 && !loss_completion; ++attempt) {
            (void)loss_session->service(0);
            loss_completion = loss_session->pop_completion();
            if (!loss_completion)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!loss_completion || loss_completion->status != SharedIoTerminalStatus::RetiredFailed)
            return abandon_loss_session();
        ++loss_terminal_records;
        loss_no_output_leases = !loss_session->acquire_output(*loss_completion);
        loss_reason = loss_native->device_loss_reason();
        if (!loss_no_output_leases || !loss_session->discard_completion(*loss_completion) ||
            !loss_session->release())
            return abandon_loss_session();
        loss_session.reset();
    }
    std::printf("loaded_runtime_path=%s\n", loaded.dli_fname);
    std::printf("input_sha256=%s weights_sha256=%s shader_sha256=%s\n",
                sha256_hex(submitted_inputs.data(), sizeof(submitted_inputs)).c_str(),
                sha256_hex(weights.data(), sizeof(weights)).c_str(),
                sha256_hex(kShader, std::strlen(kShader)).c_str());
    std::printf("staged_blocks=%u planted_completion_failures=%u cpu_oracle_max_error=%.9g\n",
                successes, failures, max_error);
    std::printf("device_loss_callback=%s device_loss_scenario=%s device_loss_reason=%s "
                "device_loss_terminal_records=%u device_loss_no_output_leases=%s "
                "device_loss_reprime=unavailable\n",
                loss_callback_registered ? "registered" : "unavailable",
                loss_scenario_triggered ? "triggered" : "unavailable",
                loss_reason == WGPUDeviceLostReason_Destroyed ? "destroyed" : "unknown",
                loss_terminal_records, loss_no_output_leases ? "true" : "false");
    std::puts(
        "device_loss_proof=callback_terminal_no_output_lease callback_fallback_proof=unavailable");
    return 0;
}
