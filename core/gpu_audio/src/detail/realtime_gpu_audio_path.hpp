#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/gpu_audio/gpu_audio_capability.hpp>
#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/gpu_audio/gpu_audio_status.hpp>

#include "shared_io_provider_identity.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace pulp::gpu_audio::detail {

inline constexpr std::uint8_t kRealtimeGpuInactive = 0;
inline constexpr std::uint8_t kRealtimeGpuPriming = 1;
inline constexpr std::uint8_t kRealtimeGpuReady = 2;
inline constexpr std::uint8_t kRealtimeGpuMissed = 3;
inline constexpr std::uint32_t kRealtimeGpuServiceInactive =
    std::numeric_limits<std::uint32_t>::max();

using RealtimeGpuProcessFn = std::uint8_t (*)(void*, const audio::BufferView<const float>&,
                                              audio::BufferView<float>&, std::uint32_t,
                                              std::uint64_t, bool, std::uint64_t) noexcept;
using RealtimeGpuServiceFn = std::uint32_t (*)(void*, std::uint64_t) noexcept;
using RealtimeGpuDeliveryFn = void (*)(void*, std::uint64_t, std::uint8_t, std::uint64_t,
                                       std::uint64_t) noexcept;
using RealtimeGpuSequenceFn = std::uint64_t (*)(void*) noexcept;
using RealtimeGpuFenceFn = bool (*)(void*) noexcept;

// One allocation-free health sample, filled by the node's `health` hook.
// Hook contract: serialized non-RT only. GpuAudioTransport calls it from its
// worker after each service step and after the offline fence, or from a
// quiescent host inside prepare(). It is never called from a UI or realtime
// thread, so a node may read worker-owned state there. The UI-safe read is
// GpuAudioTransport::status_snapshot(), which serves the transport's coherent
// copy of the last sample and never calls into the node.
struct RealtimeGpuHealth {
    bool fenced = false;
    GpuAudioRecoveryReason recovery_reason = GpuAudioRecoveryReason::None;
    std::uint64_t recovery_sequence = UINT64_MAX;
    std::uint64_t epoch = 0;
    bool delivery_epoch_active = false;
    bool device_lost = false;
    // Indexed by GpuAudioTerminalDisposition.
    std::array<std::uint64_t, GpuAudioStatus::kTerminalDispositionCount> terminal_counts{};
    // The node's CPU shadow, with the meaning of GpuAudioStatus's shadow_*
    // fields. Nodes without a shadow leave them at their defaults.
    GpuAudioShadowMode shadow_mode = GpuAudioShadowMode::None;
    float shadow_inline_cost_pct = 0.0f;
    std::uint64_t shadow_delivered_blocks = 0;
    std::uint64_t shadow_late_blocks = 0;
    std::uint64_t shadow_double_miss_blocks = 0;
    bool shadow_model_differs = false;
    // Provider facts with the meaning of the GpuAudioStatus fields of the same
    // names. Nodes that do not report them leave the defaults.
    bool gpu_kernel_available = false;
    GpuAudioCompletionMode completion_mode = GpuAudioCompletionMode::Unknown;
    bool timestamps_available = false;
    std::uint64_t last_delivery_ns = 0;
};
// Returns false when no coherent read could be taken.
using RealtimeGpuHealthFn = bool (*)(const void*, RealtimeGpuHealth&) noexcept;
// Non-RT: release and re-prepare the provider, opening a new epoch.
using RealtimeGpuReprepareFn = bool (*)(void*) noexcept;
// Brackets each offline run. `true` comes on the offline render thread before
// the run's first block, with the callback idle; `false` comes on the audio
// thread at the first realtime callback after the run, before that callback
// touches the node, so it must not allocate, lock or wait.
using RealtimeGpuOfflineRenderFn = void (*)(void*, bool offline) noexcept;

struct RealtimeGpuNodePath {
    void* context = nullptr;
    RealtimeGpuProcessFn process = nullptr;
    RealtimeGpuServiceFn service = nullptr;
    RealtimeGpuFenceFn fence = nullptr;
    RealtimeGpuDeliveryFn delivered = nullptr;
    // Host/quiescent only: the prepared node retains the callback timeline
    // across transport release, reconstruction and reprepare.
    RealtimeGpuSequenceFn next_sequence = nullptr;
    // Optional; not part of active(). `health` is populated for the WaveNet
    // realtime node and the shared GpuConvolver path; `reprepare` is unused;
    // `offline_render` is populated for the neural realtime node.
    RealtimeGpuHealthFn health = nullptr;
    RealtimeGpuReprepareFn reprepare = nullptr;
    RealtimeGpuOfflineRenderFn offline_render = nullptr;

    bool active() const noexcept {
        return context != nullptr && process != nullptr && service != nullptr && fence != nullptr &&
               delivered != nullptr && next_sequence != nullptr;
    }
};

using RealtimeGpuIdentityFn = SharedIoProviderIdentity (*)(const void*) noexcept;
using RealtimeGpuPipelineHostingFn = bool (*)(void*, bool hosted) noexcept;

// What a node tells the transport about its realtime path. Filled by
// GpuAudioNode::bind_realtime_path(), so the transport never names node types.
struct RealtimeGpuNodeBinding {
    // Active only once the node is ready to run on the realtime path.
    RealtimeGpuNodePath path;
    // The node cannot fall back to the untyped staged worker path.
    bool requires_realtime_path = false;
    // Non-RT: the identity reported by the provider the node prepared.
    const void* identity_context = nullptr;
    RealtimeGpuIdentityFn identity = nullptr;
    // Control thread, no worker running: the transport's worker-pipelining
    // opt-in (see worker_pipeline_hosting.hpp). Null for a node without one.
    void* pipeline_context = nullptr;
    RealtimeGpuPipelineHostingFn pipeline_hosting = nullptr;
};

RealtimeGpuNodeBinding realtime_gpu_node_binding(GpuAudioNode* node) noexcept;
RealtimeGpuNodePath realtime_gpu_node_path(GpuAudioNode* node) noexcept;
// Concrete SDK nodes that cannot fall back to the untyped legacy FIFO.
bool requires_realtime_gpu_path(GpuAudioNode* node) noexcept;

// Returns a provider only when the concrete private path can establish its
// identity. Generic/test hooks deliberately return Unknown.
GpuAudioProvider realtime_gpu_provider(GpuAudioNode* node) noexcept;
SharedIoProviderIdentity realtime_gpu_provider_identity(GpuAudioNode* node) noexcept;

} // namespace pulp::gpu_audio::detail
