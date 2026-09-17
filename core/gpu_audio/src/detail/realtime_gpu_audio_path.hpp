#pragma once

#include <pulp/audio/buffer.hpp>
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
                                              std::uint64_t) noexcept;
using RealtimeGpuServiceFn = std::uint32_t (*)(void*, std::uint64_t) noexcept;
using RealtimeGpuDeliveryFn = void (*)(void*, std::uint64_t, std::uint8_t) noexcept;
using RealtimeGpuFenceFn = bool (*)(void*) noexcept;

struct RealtimeGpuNodePath {
    void* context = nullptr;
    RealtimeGpuProcessFn process = nullptr;
    RealtimeGpuServiceFn service = nullptr;
    RealtimeGpuFenceFn fence = nullptr;
    RealtimeGpuDeliveryFn delivered = nullptr;

    bool active() const noexcept {
        return context != nullptr && process != nullptr && service != nullptr && fence != nullptr &&
               delivered != nullptr;
    }
};

RealtimeGpuNodePath realtime_gpu_node_path(GpuAudioNode* node) noexcept;

} // namespace pulp::gpu_audio::detail
