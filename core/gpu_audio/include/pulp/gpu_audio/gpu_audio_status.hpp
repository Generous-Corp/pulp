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

/// Why a host-visible status snapshot is inconsistent.  The validator is
/// intentionally backend-neutral: it checks only the relationship between
/// the projected engine, provider identity, lifecycle state, and prepared
/// fallback witness.  It does not inspect provider handles or imply that a
/// ready provider met a real-time deadline.
enum class GpuAudioStatusError : std::uint8_t {
    None = 0,
    SchemaVersionMismatch = 1,
    PreparedShapeMissing = 2,
    ReadyWithoutEngine = 3,
    GpuEngineWithoutProvider = 4,
    CpuEngineWithProvider = 5,
    DegradedWithSelectedEngine = 6,
    FallbackUnavailable = 7,
    UninitializedStateHasPreparation = 8,
};

struct GpuAudioStatusValidation {
    GpuAudioStatusError error = GpuAudioStatusError::None;

    constexpr bool accepted() const noexcept {
        return error == GpuAudioStatusError::None;
    }
};

/// Validate the backend-neutral status projection at a quiescent snapshot.
/// Status counters may be sampled concurrently and are therefore deliberately
/// excluded from this structural check.  A provider is only called GPU-ready
/// when its identity is known; an unknown provider remains a degraded,
/// prepared state rather than being promoted to a false GPU claim.
constexpr GpuAudioStatusValidation
validate_gpu_audio_status(const GpuAudioStatus& status) noexcept {
    if (status.schema_version != GpuAudioStatus::kSchemaVersion)
        return {GpuAudioStatusError::SchemaVersionMismatch};

    if (status.provider_state == GpuAudioProviderState::Uninitialized) {
        if (status.selected_engine != GpuAudioEngine::Unknown ||
            status.provider != GpuAudioProvider::Unknown || status.sample_rate != 0 ||
            status.block_size != 0 || status.latency_samples != 0 ||
            status.prepared_lead_blocks != 0 || status.fallback_available ||
            status.diagnostics_available)
            return {GpuAudioStatusError::UninitializedStateHasPreparation};
        return {};
    }

    if (status.sample_rate == 0 || status.block_size == 0)
        return {GpuAudioStatusError::PreparedShapeMissing};

    if (status.provider_state == GpuAudioProviderState::Ready &&
        status.selected_engine == GpuAudioEngine::Unknown)
        return {GpuAudioStatusError::ReadyWithoutEngine};

    if (status.selected_engine == GpuAudioEngine::Gpu &&
        status.provider == GpuAudioProvider::Unknown)
        return {GpuAudioStatusError::GpuEngineWithoutProvider};

    if (status.selected_engine == GpuAudioEngine::Cpu &&
        status.provider != GpuAudioProvider::Unknown)
        return {GpuAudioStatusError::CpuEngineWithProvider};

    if (status.provider_state == GpuAudioProviderState::Degraded &&
        status.selected_engine != GpuAudioEngine::Unknown)
        return {GpuAudioStatusError::DegradedWithSelectedEngine};

    if (status.fallback_policy == MissPolicy::CpuFallback && !status.fallback_available)
        return {GpuAudioStatusError::FallbackUnavailable};

    return {};
}

} // namespace pulp::gpu_audio
