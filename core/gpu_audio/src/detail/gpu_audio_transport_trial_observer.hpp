#pragma once

#include <pulp/gpu_audio/gpu_audio_transport.hpp>

#include <cstdint>

namespace pulp::gpu_audio {

// Host-only P4 benchmark hook. It is deliberately kept outside the public
// transport configuration surface; callers must install it before invoking
// the first callback and the observer must be realtime-safe.
using GpuAudioTransportTrialDeliveryFn = void (*)(void*, std::uint64_t, std::uint8_t,
                                                   std::uint64_t, std::uint64_t) noexcept;

bool configure_gpu_audio_transport_trial_observer(
    GpuAudioTransport&, void*, GpuAudioTransportTrialDeliveryFn) noexcept;

} // namespace pulp::gpu_audio
