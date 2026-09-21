#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/gpu_audio/gpu_audio_capability.hpp>
#include <pulp/gpu_audio/gpu_audio_node.hpp>

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
                                              std::uint64_t, bool) noexcept;
using RealtimeGpuServiceFn = std::uint32_t (*)(void*, std::uint64_t) noexcept;
using RealtimeGpuDeliveryFn = void (*)(void*, std::uint64_t, std::uint8_t) noexcept;
using RealtimeGpuSequenceFn = std::uint64_t (*)(void*) noexcept;
using RealtimeGpuFenceFn = bool (*)(void*) noexcept;

struct RealtimeGpuNodePath {
    void* context = nullptr;
    RealtimeGpuProcessFn process = nullptr;
    RealtimeGpuServiceFn service = nullptr;
    RealtimeGpuFenceFn fence = nullptr;
    RealtimeGpuDeliveryFn delivered = nullptr;
    // Host/quiescent only: the prepared node retains the callback timeline
    // across transport release, reconstruction and reprepare.
    RealtimeGpuSequenceFn next_sequence = nullptr;

    bool active() const noexcept {
        return context != nullptr && process != nullptr && service != nullptr && fence != nullptr &&
               delivered != nullptr && next_sequence != nullptr;
    }
};

RealtimeGpuNodePath realtime_gpu_node_path(GpuAudioNode* node) noexcept;

// Returns a provider only when the concrete private path can establish its
// identity. Generic/test hooks deliberately return Unknown.
GpuAudioProvider realtime_gpu_provider(GpuAudioNode* node) noexcept;

} // namespace pulp::gpu_audio::detail
