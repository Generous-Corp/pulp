#pragma once

#include <cstdint>

#include <pulp/gpu_audio/gpu_audio_capability.hpp>

namespace pulp::gpu_audio {

/// The engine a host asked the audio provider to use.  Unknown is used when
/// the host did not make an explicit selection; it is deliberately distinct
/// from the engine selected by a prepared transport.
enum class GpuAudioEngine : std::uint8_t {
    Unknown = 0,
    Cpu = 1,
    Gpu = 2,
};

/// Lifecycle state suitable for UI, trace, and host diagnostics. This is a
/// provider state, not a real-time guarantee. Ready means an authenticated
/// execution provider is active. Degraded means the transport is prepared but
/// its provider or execution engine cannot be established. Scheduling and
/// deadline behavior are reported by the counters and timings below.
enum class GpuAudioProviderState : std::uint8_t {
    Uninitialized = 0,
    Ready = 1,
    Degraded = 2,
    Unavailable = 3,
};

/// Versioned, allocation-free status shared by native hosts and UI adapters.
/// A web or plugin-specific status object may project this into JSON, but must
/// preserve the distinction between GPU production and CPU fallback.  In
/// particular, `missed_blocks` is not a synonym for `fallback_blocks`.
struct GpuAudioStatus {
    static constexpr std::uint32_t kSchemaVersion = 1;

    std::uint32_t schema_version = kSchemaVersion;
    GpuAudioEngine requested_engine = GpuAudioEngine::Unknown;
    GpuAudioEngine selected_engine = GpuAudioEngine::Unknown;
    GpuAudioProviderState provider_state = GpuAudioProviderState::Uninitialized;
    GpuAudioProvider provider = GpuAudioProvider::Unknown;
    MissPolicy fallback_policy = MissPolicy::Silence;

    std::uint32_t sample_rate = 0;
    std::uint32_t block_size = 0;
    std::uint32_t latency_samples = 0;
    std::uint32_t prepared_lead_blocks = 0;
    bool fallback_available = false;
    bool diagnostics_available = false;

    std::uint64_t produced_blocks = 0;
    std::uint64_t missed_blocks = 0;
    std::uint64_t fallback_blocks = 0;
    std::uint64_t resynced_blocks = 0;
    std::uint64_t input_dropped_frames = 0;
    double last_block_us = 0.0;
    double avg_block_us = 0.0;
};

} // namespace pulp::gpu_audio
