#pragma once

#include <cstdint>

namespace pulp::gpu_audio {

// Defined in gpu_audio_node.hpp and gpu_audio_capability.hpp, which include
// this header: GpuAudioNode::status_snapshot() returns GpuAudioStatus by value,
// so the status definition cannot depend on the node header being complete.
enum class MissPolicy;
enum class GpuAudioProvider : std::uint8_t;

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

/// First latched reason that a stamped shared-I/O node entered recovery.
/// `None` means that no recovery has been requested. This is diagnostic state;
/// it does not itself promise that the next callback will use the GPU.
enum class GpuAudioRecoveryReason : std::uint8_t {
    None = 0,
    SequenceGap = 1,
    InputSaturated = 2,
    ProviderFailure = 3,
    ProviderLost = 4,
    OfflineFence = 5,
    InvalidCallback = 6,
    /// The provider stopped completing work within its watchdog budget.
    ProviderUnresponsive = 7,
    /// Informational: a lost provider was re-acquired under an automatic
    /// recovery policy and delivery resumed on a new epoch.
    ProviderReacquired = 8,
    /// The GPU is alive but repeatedly late; the node switched to CPU with
    /// periodic GPU probes. Maps to Degraded, not Unavailable.
    SustainedDeadlineMisses = 9,
};

/// How the non-realtime dispatcher services provider completions. Unknown
/// means the node does not report its completion servicing.
enum class GpuAudioCompletionMode : std::uint8_t {
    Unknown = 0,
    ProcessEvents = 1,
    WaitAny = 2,
    TimedWaitAny = 3,
};

/// Versioned, allocation-free status shared by native hosts and UI adapters.
/// A web or plugin-specific status object may project this into JSON, but must
/// preserve the distinction between GPU production and CPU fallback.  In
/// particular, `missed_blocks` is not a synonym for `fallback_blocks`.
struct GpuAudioStatus {
    static constexpr std::uint32_t kSchemaVersion = 3;
    /// Number of `GpuAudioTerminalDisposition` values counted by
    /// `terminal_counts`.
    static constexpr std::uint32_t kTerminalDispositionCount = 8;

    std::uint32_t schema_version = kSchemaVersion;
    GpuAudioEngine requested_engine = GpuAudioEngine::Unknown;
    GpuAudioEngine selected_engine = GpuAudioEngine::Unknown;
    GpuAudioProviderState provider_state = GpuAudioProviderState::Uninitialized;
    GpuAudioProvider provider = GpuAudioProvider{}; // GpuAudioProvider::Unknown
    MissPolicy fallback_policy = MissPolicy{};      // MissPolicy::Silence

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
    // Appended after the v1 fields so positional aggregate initialization of
    // the installed status prefix remains source-compatible.
    GpuAudioRecoveryReason recovery_reason = GpuAudioRecoveryReason::None;
    std::uint64_t recovery_sequence = UINT64_MAX;
    // Schema v3, appended after the v2 fields for the same reason.
    std::uint64_t epoch = 0;
    bool delivery_epoch_active = false;
    /// Admitted blocks by terminal disposition, indexed by the numeric value
    /// of `GpuAudioTerminalDisposition`.
    std::uint64_t terminal_counts[kTerminalDispositionCount] = {};
    GpuAudioCompletionMode completion_mode = GpuAudioCompletionMode::Unknown;
    bool timestamps_available = false;
    std::uint64_t last_delivery_ns = 0;
    bool gpu_kernel_available = false;
};

} // namespace pulp::gpu_audio

// Kept for consumers that reach the capability and node types through this
// header; included last because those headers include this one.
#include <pulp/gpu_audio/gpu_audio_capability.hpp>
