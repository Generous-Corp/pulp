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
    /// A CPU implementation with no GPU provider. Appended to preserve the
    /// established numeric values above.
    Cpu = 3,
};

/// Eligibility of the configured transport path. This is not a hard
/// real-time scheduling guarantee.
enum class GpuAudioEligibility : std::uint8_t {
    Unavailable = 0,
    Eligible = 1,
};

/// Whether a node may re-acquire a lost provider on its own. Manual leaves a
/// fenced node on its fallback until the host prepares it again.
enum class GpuAudioRecoveryPolicy : std::uint8_t {
    Manual = 0,
    AutoReacquire = 1,
};

/// Backend-neutral, allocation-free capability snapshot for installed SDK
/// consumers. It intentionally omits queues, rings, callbacks, and handles.
struct GpuAudioCapabilityReport {
    GpuAudioExecutionPath path = GpuAudioExecutionPath::Unavailable;
    GpuAudioProvider provider = GpuAudioProvider::Unknown;
    GpuAudioEligibility eligibility = GpuAudioEligibility::Unavailable;
    MissPolicy fallback_policy = MissPolicy::Silence;
    // Algorithmic lead is reported only when an authenticated concrete
    // provider establishes it. Generic staged nodes leave this at zero.
    std::uint32_t prepared_lead_blocks = 0;
    bool prepared = false;
    bool fallback_available = false;
    bool diagnostics_available = false;
    // The fixed output/PDC delay established by the transport. This is
    // appended to preserve the existing positional field order for SDK
    // consumers and must not be read as algorithmic pipeline lead.
    std::uint32_t prepared_latency_blocks = 0;
    // Appended: whether this node family has a GPU kernel at all. False for
    // CPU-only families, which report the Cpu path rather than a GPU miss.
    bool gpu_kernel_available = false;
    GpuAudioRecoveryPolicy recovery_policy = GpuAudioRecoveryPolicy::Manual;
};

// The status validator lives here rather than in gpu_audio_status.hpp: it needs
// MissPolicy and GpuAudioProvider complete, which the status header only
// forward-declares. Every include order reaches this point with both complete,
// and gpu_audio_status.hpp includes this header, so its users see it.

/// Why a host-visible status snapshot is inconsistent. Backend-neutral: it
/// checks relationships between the projected engine, provider, lifecycle,
/// recovery and counters, never provider handles, and it does not imply that a
/// ready provider met a real-time deadline.
enum class GpuAudioStatusError : std::uint8_t {
    None = 0,
    SchemaVersionMismatch = 1,
    InvalidEnumValue = 2,
    RecoverySequenceMismatch = 3,
    UninitializedStateHasPreparation = 4,
    UninitializedStateHasRecovery = 5,
    PreparedShapeMissing = 6,
    ReadyWithoutEngine = 7,
    GpuEngineWithoutProvider = 8,
    DegradedOrUnavailableOnGpu = 9,
    LostProviderNotUnavailable = 10,
    SustainedMissesNotDegraded = 11,
    CpuProviderWithGpuState = 12,
    ShadowCountsWithoutShadow = 13,
    TerminalCountsInconsistent = 14,
    FallbackUnavailable = 15,
};

struct GpuAudioStatusValidation {
    GpuAudioStatusError error = GpuAudioStatusError::None;

    constexpr bool accepted() const noexcept {
        return error == GpuAudioStatusError::None;
    }
};

/// Validate a status snapshot taken at a quiescent point. Counters sampled
/// while a node runs may be momentarily inconsistent with one another, so a
/// live caller should treat TerminalCountsInconsistent as advisory.
constexpr GpuAudioStatusValidation
validate_gpu_audio_status(const GpuAudioStatus& status) noexcept {
    using Error = GpuAudioStatusError;
    using Reason = GpuAudioRecoveryReason;
    using State = GpuAudioProviderState;
    if (status.schema_version != GpuAudioStatus::kSchemaVersion)
        return {Error::SchemaVersionMismatch};

    const auto engine_ok = [](GpuAudioEngine value) constexpr {
        return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(GpuAudioEngine::Gpu);
    };
    if (!engine_ok(status.requested_engine) || !engine_ok(status.selected_engine) ||
        static_cast<std::uint8_t>(status.provider_state) >
            static_cast<std::uint8_t>(State::Unavailable) ||
        static_cast<std::uint8_t>(status.provider) >
            static_cast<std::uint8_t>(GpuAudioProvider::Cpu) ||
        static_cast<int>(status.fallback_policy) < 0 ||
        static_cast<int>(status.fallback_policy) > static_cast<int>(MissPolicy::CpuFallback) ||
        static_cast<std::uint8_t>(status.recovery_reason) >
            static_cast<std::uint8_t>(Reason::SustainedDeadlineMisses) ||
        static_cast<std::uint8_t>(status.completion_mode) >
            static_cast<std::uint8_t>(GpuAudioCompletionMode::TimedWaitAny) ||
        static_cast<std::uint8_t>(status.shadow_mode) >
            static_cast<std::uint8_t>(GpuAudioShadowMode::Worker))
        return {Error::InvalidEnumValue};

    // A latched recovery always carries the sequence it latched at.
    if ((status.recovery_reason == Reason::None) != (status.recovery_sequence == UINT64_MAX))
        return {Error::RecoverySequenceMismatch};

    if (status.shadow_mode == GpuAudioShadowMode::None &&
        (status.shadow_delivered_blocks != 0 || status.shadow_late_blocks != 0 ||
         status.shadow_double_miss_blocks != 0 || status.shadow_inline_cost_pct != 0.0f))
        return {Error::ShadowCountsWithoutShadow};

    // Terminal counts are per admitted block, so the GPU and CPU-fallback
    // dispositions cannot exceed the delivery counters that count the same
    // blocks, and the total must be representable.
    std::uint64_t terminal_total = 0;
    for (const auto count : status.terminal_counts) {
        if (count > UINT64_MAX - terminal_total)
            return {Error::TerminalCountsInconsistent};
        terminal_total += count;
    }
    // GpuAudioTerminalDisposition::GpuDelivered and ::CpuFallback; the enum is
    // declared later, in gpu_audio_program.hpp, which asserts these indices.
    const auto gpu_delivered = status.terminal_counts[1];
    const auto cpu_fallback = status.terminal_counts[2];
    if (gpu_delivered > status.produced_blocks || cpu_fallback > status.fallback_blocks)
        return {Error::TerminalCountsInconsistent};

    // A CPU provider has no GPU kernel and never opens a GPU delivery epoch.
    if (status.provider == GpuAudioProvider::Cpu &&
        (status.gpu_kernel_available || status.epoch != 0))
        return {Error::CpuProviderWithGpuState};

    if (status.provider_state == State::Uninitialized) {
        if (status.recovery_reason != Reason::None)
            return {Error::UninitializedStateHasRecovery};
        if (status.selected_engine != GpuAudioEngine::Unknown ||
            status.provider != GpuAudioProvider::Unknown || status.sample_rate != 0 ||
            status.block_size != 0 || status.latency_samples != 0 ||
            status.prepared_lead_blocks != 0 || status.fallback_available ||
            status.diagnostics_available)
            return {Error::UninitializedStateHasPreparation};
        return {};
    }

    if (status.sample_rate == 0 || status.block_size == 0)
        return {Error::PreparedShapeMissing};
    if (status.provider_state == State::Ready && status.selected_engine == GpuAudioEngine::Unknown)
        return {Error::ReadyWithoutEngine};
    if (status.selected_engine == GpuAudioEngine::Gpu &&
        (status.provider == GpuAudioProvider::Unknown || status.provider == GpuAudioProvider::Cpu))
        return {Error::GpuEngineWithoutProvider};
    // A GPU provider may stay identified while the node runs on the CPU (for
    // example after sustained deadline misses, with periodic GPU probes), so a
    // CPU engine does not by itself rule out a GPU provider.
    if ((status.provider_state == State::Degraded || status.provider_state == State::Unavailable) &&
        status.selected_engine == GpuAudioEngine::Gpu)
        return {Error::DegradedOrUnavailableOnGpu};
    if ((status.recovery_reason == Reason::ProviderLost ||
         status.recovery_reason == Reason::ProviderUnresponsive) &&
        status.provider_state != State::Unavailable)
        return {Error::LostProviderNotUnavailable};
    if (status.recovery_reason == Reason::SustainedDeadlineMisses &&
        status.provider_state != State::Degraded)
        return {Error::SustainedMissesNotDegraded};
    if (status.fallback_policy == MissPolicy::CpuFallback && !status.fallback_available)
        return {Error::FallbackUnavailable};
    return {};
}

} // namespace pulp::gpu_audio
