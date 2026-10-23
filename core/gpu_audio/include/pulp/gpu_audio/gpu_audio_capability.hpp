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
    /// The recovery policy in effect, not the one configured: a transport
    /// configured for AutoReacquire reports Manual when its prepared node
    /// cannot re-acquire a provider.
    GpuAudioRecoveryPolicy recovery_policy = GpuAudioRecoveryPolicy::Manual;
};

/// Why a capability snapshot is not self-consistent. Like the status
/// validator below, this is backend-neutral: it checks enum domains and the
/// relationships a report can witness on its own, never a provider handle,
/// and it does not imply that a prepared path will meet a real-time deadline.
enum class GpuAudioCapabilityError : std::uint8_t {
    None = 0,
    InvalidEnumValue = 1,
    UnpreparedStateHasPreparation = 2,
    PreparedPathUnavailable = 3,
    PreparedPathIneligible = 4,
    CpuPathWithoutCpuProvider = 5,
    CpuProviderOnGpuPath = 6,
    CpuPathWithGpuKernel = 7,
    SharedLeadMissing = 8,
    FallbackUnavailable = 9,
};

struct GpuAudioCapabilityValidation {
    GpuAudioCapabilityError error = GpuAudioCapabilityError::None;

    constexpr bool accepted() const noexcept {
        return error == GpuAudioCapabilityError::None;
    }
};

/// Validate a capability snapshot. An unprepared report must be the default
/// snapshot. A prepared report must name an eligible path; the CPU path and
/// the CPU provider imply each other, and a CPU path has no GPU kernel. A
/// shared-memory path declares its algorithmic lead.
///
/// Known gaps, deliberately not checked because a shipped producer emits the
/// state today:
/// - A staged or shared-memory path with `gpu_kernel_available == false`.
///   A transport reports the kernel only from a node's health sample, so a
///   staged node with no health hook never reports one. A shared-memory path
///   also keeps its path after its kernel is gone: GpuConvolver hosted after
///   a device loss, offline fence or refused block, and
///   `GpuNeuralRealtimeNode::capability_report()` while its backend is
///   quiesced.
/// - A shared-memory path with provider Unknown.
///   `GpuNeuralRealtimeNode::capability_report()` reports this while its
///   backend runs without an authenticated provider.
/// - A CPU path with a non-zero lead. Both producers report the delay of a
///   latency-aligned CPU stream there.
/// - A staged path carrying a provider or a lead. No producer emits one, but
///   `validate_gpu_audio_program()` requires a staged lead and checks a named
///   staged provider against this report, so forbidding them would make every
///   staged program unbindable.
/// `recovery_policy` is checked for its domain and for the unprepared default
/// only.
constexpr GpuAudioCapabilityValidation
validate_gpu_audio_capability(const GpuAudioCapabilityReport& capability) noexcept {
    using Error = GpuAudioCapabilityError;
    using Path = GpuAudioExecutionPath;
    if (static_cast<std::uint8_t>(capability.path) > static_cast<std::uint8_t>(Path::Cpu) ||
        static_cast<std::uint8_t>(capability.provider) >
            static_cast<std::uint8_t>(GpuAudioProvider::Cpu) ||
        static_cast<std::uint8_t>(capability.eligibility) >
            static_cast<std::uint8_t>(GpuAudioEligibility::Eligible) ||
        static_cast<int>(capability.fallback_policy) < 0 ||
        static_cast<int>(capability.fallback_policy) > static_cast<int>(MissPolicy::CpuFallback) ||
        static_cast<std::uint8_t>(capability.recovery_policy) >
            static_cast<std::uint8_t>(GpuAudioRecoveryPolicy::AutoReacquire))
        return {Error::InvalidEnumValue};

    if (!capability.prepared) {
        if (capability.path != Path::Unavailable ||
            capability.provider != GpuAudioProvider::Unknown ||
            capability.eligibility != GpuAudioEligibility::Unavailable ||
            capability.fallback_policy != MissPolicy::Silence ||
            capability.prepared_lead_blocks != 0 || capability.fallback_available ||
            capability.diagnostics_available || capability.prepared_latency_blocks != 0 ||
            capability.gpu_kernel_available ||
            capability.recovery_policy != GpuAudioRecoveryPolicy::Manual)
            return {Error::UnpreparedStateHasPreparation};
        return {};
    }

    if (capability.path == Path::Unavailable)
        return {Error::PreparedPathUnavailable};
    if (capability.eligibility != GpuAudioEligibility::Eligible)
        return {Error::PreparedPathIneligible};
    if (capability.path == Path::Cpu) {
        if (capability.provider != GpuAudioProvider::Cpu)
            return {Error::CpuPathWithoutCpuProvider};
        if (capability.gpu_kernel_available)
            return {Error::CpuPathWithGpuKernel};
    } else if (capability.provider == GpuAudioProvider::Cpu) {
        return {Error::CpuProviderOnGpuPath};
    }
    if (capability.path == Path::SharedMemory && capability.prepared_lead_blocks == 0)
        return {Error::SharedLeadMissing};
    if (capability.fallback_policy == MissPolicy::CpuFallback && !capability.fallback_available)
        return {Error::FallbackUnavailable};
    return {};
}

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
    // Schema v4: automatic re-acquire accounting.
    ReacquireSuccessesExceedAttempts = 16,
    ReacquiredWithoutSuccess = 17,
    ReacquiredNotDelivering = 18,
    ReacquireTimingWithoutSuccess = 19,
    // Appended: a status naming the GPU engine must also report a prepared
    // GPU kernel. Checked after every other rule, the re-acquire rules
    // included, so a status breaking both reports the other error.
    GpuEngineWithoutKernel = 20,
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
    // A re-acquire is a counted success that left a provider able to deliver.
    if (status.reacquire_successes > status.reacquire_attempts)
        return {Error::ReacquireSuccessesExceedAttempts};
    if (status.last_reacquire_to_gpu_ns != 0 && status.reacquire_successes == 0)
        return {Error::ReacquireTimingWithoutSuccess};
    if (status.recovery_reason == Reason::ProviderReacquired) {
        if (status.reacquire_successes == 0)
            return {Error::ReacquiredWithoutSuccess};
        if (status.provider_state == State::Unavailable)
            return {Error::ReacquiredNotDelivering};
    }
    // The GPU engine delivers only through a prepared kernel; every producer
    // that selects it reads the kernel from the same health sample. Checked
    // last, so every earlier error, the re-acquire rules included, wins.
    if (status.selected_engine == GpuAudioEngine::Gpu && !status.gpu_kernel_available)
        return {Error::GpuEngineWithoutKernel};
    return {};
}

} // namespace pulp::gpu_audio
