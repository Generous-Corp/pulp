#pragma once

#include <cstdint>
#include <type_traits>

#include <pulp/gpu_audio/gpu_audio_capability.hpp>

namespace pulp::gpu_audio {

/// The typed workload family selected during host-side preparation.
///
/// A program kind is deliberately descriptive. It does not expose a shader,
/// queue, device, or other backend object to an SDK consumer. Providers may
/// add an implementation behind the same Pulp-owned kind in a later release.
enum class GpuAudioProgramKind : std::uint8_t {
    Unknown = 0,
    Convolution = 1,
    Spectral = 2,
    Neural = 3,
    Custom = 255,
};

/// One terminal audio disposition for an admitted block. A GPU rejection and
/// the audio delivered for that block are intentionally represented by one
/// stable, host-visible value so a trace cannot leave an admitted block
/// orphaned between provider completion and callback delivery.
enum class GpuAudioTerminalDisposition : std::uint8_t {
    Unknown = 0,
    GpuDelivered = 1,
    CpuFallback = 2,
    Silence = 3,
    StaleRejected = 4,
    LateRejected = 5,
    DeviceLost = 6,
    Cancelled = 7,
};

/// Why a prepared program descriptor was rejected. Validation is constexpr so
/// providers and SDK consumers can use the same fail-closed rules without
/// allocating or calling into Dawn/Metal.
enum class GpuAudioProgramError : std::uint8_t {
    None = 0,
    MissingKind = 1,
    InvalidShape = 2,
    PathUnavailable = 3,
    MissingAlgorithmicLead = 4,
    MissingPipelineDepth = 5,
    InsufficientPipelineDepth = 6,
    MissingProviderSlots = 7,
    MissingProviderIdentity = 8,
    ProviderResourcesNotOwned = 9,
    ProviderResourcesOnCpu = 10,
    ProviderResourcesOnStaged = 11,
    ProviderOnCpu = 12,
    CpuPipelineOnCpu = 13,
    CpuFallbackNotPrepared = 14,
    // The descriptor is valid in isolation, but the prepared transport did
    // not authenticate the same path/provider contract.  These values are
    // appended so the existing wire/debug values remain stable.
    CapabilityNotPrepared = 15,
    CapabilityIneligible = 16,
    CapabilityPathMismatch = 17,
    CapabilityProviderMismatch = 18,
    CapabilityLeadMismatch = 19,
    CapabilityFallbackPolicyMismatch = 20,
    CapabilityFallbackUnavailable = 21,
};

/// Backend-neutral metadata for a typed program prepared for a fixed audio
/// stream. This is a declaration/validation surface, not an execution API:
/// there are no Dawn, Metal, queue, ring, callback, or opaque native handles.
///
/// The descriptor is copied during host-side preparation and remains stable
/// until the prepared program is released. `pipeline_depth` is the bounded
/// completion-table capacity and must be greater than the declared algorithmic
/// lead for GPU paths. `provider_slots` counts persistent provider-owned slots,
/// rather than exposing their addresses or resource objects.
struct GpuAudioProgramDescriptor {
    GpuAudioProgramKind kind = GpuAudioProgramKind::Unknown;
    GpuAudioExecutionPath path = GpuAudioExecutionPath::Unavailable;
    GpuAudioProvider provider = GpuAudioProvider::Unknown;
    MissPolicy miss_policy = MissPolicy::Silence;
    std::uint32_t channels = 0;
    std::uint32_t block_size = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t algorithmic_lead_blocks = 0;
    std::uint32_t pipeline_depth = 0;
    std::uint32_t provider_slots = 0;
    bool provider_owned_resources = false;
    bool cpu_fallback_prepared = false;
};

struct GpuAudioProgramValidation {
    GpuAudioProgramError error = GpuAudioProgramError::None;

    constexpr bool accepted() const noexcept {
        return error == GpuAudioProgramError::None;
    }
};

/// Validate the backend-neutral prepared-program contract.
constexpr GpuAudioProgramValidation
validate_gpu_audio_program(const GpuAudioProgramDescriptor& program) noexcept {
    if (program.kind == GpuAudioProgramKind::Unknown)
        return {GpuAudioProgramError::MissingKind};
    if (program.channels == 0 || program.block_size == 0 || program.sample_rate == 0)
        return {GpuAudioProgramError::InvalidShape};
    if (program.miss_policy == MissPolicy::CpuFallback && !program.cpu_fallback_prepared)
        return {GpuAudioProgramError::CpuFallbackNotPrepared};

    if (program.path == GpuAudioExecutionPath::Unavailable)
        return {GpuAudioProgramError::PathUnavailable};

    if (program.path == GpuAudioExecutionPath::Staged ||
        program.path == GpuAudioExecutionPath::SharedMemory) {
        if (program.algorithmic_lead_blocks == 0)
            return {GpuAudioProgramError::MissingAlgorithmicLead};
        if (program.pipeline_depth == 0)
            return {GpuAudioProgramError::MissingPipelineDepth};
        if (program.pipeline_depth <= program.algorithmic_lead_blocks)
            return {GpuAudioProgramError::InsufficientPipelineDepth};
        if (program.provider_slots == 0)
            return {GpuAudioProgramError::MissingProviderSlots};
    }

    if (program.path == GpuAudioExecutionPath::SharedMemory) {
        if (program.provider == GpuAudioProvider::Unknown)
            return {GpuAudioProgramError::MissingProviderIdentity};
        if (!program.provider_owned_resources)
            return {GpuAudioProgramError::ProviderResourcesNotOwned};
    } else if (program.path == GpuAudioExecutionPath::Staged) {
        if (program.provider_owned_resources)
            return {GpuAudioProgramError::ProviderResourcesOnStaged};
    } else {
        if (program.provider != GpuAudioProvider::Unknown)
            return {GpuAudioProgramError::ProviderOnCpu};
        if (program.provider_owned_resources)
            return {GpuAudioProgramError::ProviderResourcesOnCpu};
        if (program.algorithmic_lead_blocks != 0 || program.pipeline_depth != 0 ||
            program.provider_slots != 0)
            return {GpuAudioProgramError::CpuPipelineOnCpu};
    }

    return {};
}

/// Validate a prepared program against the capability snapshot returned by
/// the host transport.  The descriptor alone is a declaration and can be
/// authored by an SDK consumer; this overload is the fail-closed preparation
/// boundary that proves the selected transport actually accepted the same
/// path, provider identity, lead, and fallback policy.  It still carries no
/// backend handle and performs no provider or callback operation.
constexpr GpuAudioProgramValidation
validate_gpu_audio_program(const GpuAudioProgramDescriptor& program,
                           const GpuAudioCapabilityReport& capability) noexcept {
    const auto descriptor = validate_gpu_audio_program(program);
    if (!descriptor.accepted())
        return descriptor;
    if (!capability.prepared)
        return {GpuAudioProgramError::CapabilityNotPrepared};
    if (capability.eligibility != GpuAudioEligibility::Eligible)
        return {GpuAudioProgramError::CapabilityIneligible};
    if (capability.path != program.path)
        return {GpuAudioProgramError::CapabilityPathMismatch};
    if (capability.fallback_policy != program.miss_policy)
        return {GpuAudioProgramError::CapabilityFallbackPolicyMismatch};
    if (program.algorithmic_lead_blocks != capability.prepared_lead_blocks)
        return {GpuAudioProgramError::CapabilityLeadMismatch};
    if (program.path == GpuAudioExecutionPath::SharedMemory &&
        capability.provider != program.provider)
        return {GpuAudioProgramError::CapabilityProviderMismatch};
    // A known descriptor provider must still be witnessed by the prepared
    // capability for staged paths.  Unknown is the honest generic staged
    // value and remains valid when both sides leave identity unspecified.
    if (program.path == GpuAudioExecutionPath::Staged &&
        program.provider != GpuAudioProvider::Unknown && capability.provider != program.provider)
        return {GpuAudioProgramError::CapabilityProviderMismatch};
    if (program.miss_policy == MissPolicy::CpuFallback && !capability.fallback_available)
        return {GpuAudioProgramError::CapabilityFallbackUnavailable};
    return {};
}

static_assert(std::is_trivially_copyable_v<GpuAudioProgramDescriptor>);
static_assert(std::is_trivially_copyable_v<GpuAudioProgramValidation>);

} // namespace pulp::gpu_audio
