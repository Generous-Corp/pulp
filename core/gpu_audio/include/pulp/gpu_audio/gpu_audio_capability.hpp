#pragma once

#include <cstdint>

#include <pulp/gpu_audio/gpu_audio_node.hpp>

namespace pulp::gpu_audio {

/// The transport path selected for a prepared audio node.
enum class GpuAudioExecutionPath : std::uint8_t {
    Unavailable = 0,
    Staged = 1,
    SharedMemory = 2,
    /// A prepared CPU implementation with no GPU provider resources.
    /// Appended to preserve the established numeric values above.
    Cpu = 3,
};

/// Provider identity when the selected path can establish it without exposing
/// backend handles. Unknown is honest for a generic staged node.
enum class GpuAudioProvider : std::uint8_t {
    Unknown = 0,
    Dawn = 1,
    Metal = 2,
};

/// Eligibility of the configured transport path. This is not a hard
/// real-time scheduling guarantee.
enum class GpuAudioEligibility : std::uint8_t {
    Unavailable = 0,
    Eligible = 1,
};

/// Backend-neutral, allocation-free capability snapshot for installed SDK
/// consumers. It intentionally omits queues, rings, callbacks, and handles.
struct GpuAudioCapabilityReport {
    GpuAudioExecutionPath path = GpuAudioExecutionPath::Unavailable;
    GpuAudioProvider provider = GpuAudioProvider::Unknown;
    GpuAudioEligibility eligibility = GpuAudioEligibility::Unavailable;
    MissPolicy fallback_policy = MissPolicy::Silence;
    std::uint32_t prepared_lead_blocks = 0;
    bool prepared = false;
    bool fallback_available = false;
    bool diagnostics_available = false;
};

} // namespace pulp::gpu_audio
