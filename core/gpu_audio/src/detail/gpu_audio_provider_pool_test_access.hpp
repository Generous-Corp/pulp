#pragma once

#include <pulp/gpu_audio/gpu_audio_provider_pool.hpp>
#include <pulp/gpu_audio/gpu_wavenet.hpp>

#include "wavenet_realtime_channel.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace pulp::gpu_audio::detail {

// Test-only seams for the provider pool and the sessions it holds.
struct GpuAudioProviderPoolTestAccess {
    // Replaces the program hash, for example to force a collision; null
    // restores the real one.
    static void set_key_hash(GpuAudioProviderPool&,
                             std::uint64_t (*hash)(const GpuWaveNetSession::Config&) noexcept);
    static std::unique_ptr<WaveNetRealtimeChannel> acquire(GpuAudioProviderPool&,
                                                           const GpuWaveNetSession::Config&,
                                                           const std::string& provider_key,
                                                           std::uint64_t release_budget_ns);
    static bool give(GpuAudioProviderPool&, const GpuWaveNetSession::Config&,
                     const std::string& provider_key, std::unique_ptr<WaveNetRealtimeChannel>&);
};

struct GpuWaveNetSessionTestAccess {
    // Makes device_lost() report true, as after a GPU reset.
    static void mark_device_lost(GpuWaveNetSession&) noexcept;
};

} // namespace pulp::gpu_audio::detail
