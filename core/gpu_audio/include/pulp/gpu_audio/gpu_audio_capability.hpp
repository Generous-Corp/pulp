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
};

/// Why a host-visible capability snapshot was rejected.  The snapshot is
/// intentionally structural: it proves only the selected path and its
/// authenticated metadata, never a provider handle or realtime deadline.
enum class GpuAudioCapabilityError : std::uint8_t {
    None = 0,
    InvalidEnumValue = 1,
    UnpreparedStateHasPreparation = 2,
    PreparedPathUnavailable = 3,
    PreparedPathIneligible = 4,
    SharedProviderMissing = 5,
    SharedLeadMissing = 6,
    CpuProviderPresent = 7,
    CpuLeadPresent = 8,
    FallbackUnavailable = 9,
};

struct GpuAudioCapabilityValidation {
    GpuAudioCapabilityError error = GpuAudioCapabilityError::None;

    constexpr bool accepted() const noexcept {
        return error == GpuAudioCapabilityError::None;
    }
};

/// Validate a host-visible capability snapshot before it authenticates a
/// program descriptor.  This is allocation-free and backend-neutral: it
/// checks only versioned enum domains and relationships that the report itself
/// can witness.  A report for an unprepared transport must remain the zero
/// snapshot; a prepared shared path must identify its provider and lead, while
/// a CPU path must not claim either.
constexpr GpuAudioCapabilityValidation
validate_gpu_audio_capability(const GpuAudioCapabilityReport& capability) noexcept {
    const auto valid_path = [](GpuAudioExecutionPath value) constexpr {
        return value == GpuAudioExecutionPath::Unavailable ||
               value == GpuAudioExecutionPath::Staged ||
               value == GpuAudioExecutionPath::SharedMemory || value == GpuAudioExecutionPath::Cpu;
    };
    const auto valid_provider = [](GpuAudioProvider value) constexpr {
        return value == GpuAudioProvider::Unknown || value == GpuAudioProvider::Dawn ||
               value == GpuAudioProvider::Metal;
    };
    const auto valid_eligibility = [](GpuAudioEligibility value) constexpr {
        return value == GpuAudioEligibility::Unavailable || value == GpuAudioEligibility::Eligible;
    };
    const auto valid_miss_policy = [](MissPolicy value) constexpr {
        return value == MissPolicy::Silence || value == MissPolicy::PassthroughDry ||
               value == MissPolicy::CpuFallback;
    };
    if (!valid_path(capability.path) || !valid_provider(capability.provider) ||
        !valid_eligibility(capability.eligibility) ||
        !valid_miss_policy(capability.fallback_policy))
        return {GpuAudioCapabilityError::InvalidEnumValue};

    if (!capability.prepared) {
        if (capability.path != GpuAudioExecutionPath::Unavailable ||
            capability.provider != GpuAudioProvider::Unknown ||
            capability.eligibility != GpuAudioEligibility::Unavailable ||
            capability.fallback_policy != MissPolicy::Silence ||
            capability.prepared_lead_blocks != 0 || capability.fallback_available ||
            capability.diagnostics_available || capability.prepared_latency_blocks != 0)
            return {GpuAudioCapabilityError::UnpreparedStateHasPreparation};
        return {};
    }

    if (capability.path == GpuAudioExecutionPath::Unavailable)
        return {GpuAudioCapabilityError::PreparedPathUnavailable};
    if (capability.eligibility != GpuAudioEligibility::Eligible)
        return {GpuAudioCapabilityError::PreparedPathIneligible};

    if (capability.path == GpuAudioExecutionPath::SharedMemory) {
        if (capability.provider == GpuAudioProvider::Unknown)
            return {GpuAudioCapabilityError::SharedProviderMissing};
        if (capability.prepared_lead_blocks == 0)
            return {GpuAudioCapabilityError::SharedLeadMissing};
    } else if (capability.path == GpuAudioExecutionPath::Cpu) {
        if (capability.provider != GpuAudioProvider::Unknown)
            return {GpuAudioCapabilityError::CpuProviderPresent};
        if (capability.prepared_lead_blocks != 0)
            return {GpuAudioCapabilityError::CpuLeadPresent};
    }

    if (capability.fallback_policy == MissPolicy::CpuFallback && !capability.fallback_available)
        return {GpuAudioCapabilityError::FallbackUnavailable};
    return {};
}

} // namespace pulp::gpu_audio
