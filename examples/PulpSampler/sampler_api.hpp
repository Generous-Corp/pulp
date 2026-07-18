#pragma once

/// @file sampler_api.hpp
/// Stable, behavior-neutral public records for configuring and inspecting the
/// PulpSampler example. The processor/runtime integration owns publication and
/// threading; these records deliberately contain no locks, owners, or dynamic
/// storage so they can also be used in coherent diagnostic snapshots.

#include <pulp/audio/sample_heritage_runtime_state.hpp>
#include <pulp/audio/sample_heritage_schema.hpp>
#include <pulp/audio/sample_interpolation.hpp>
#include <pulp/audio/sample_starvation_envelope.hpp>
#include <pulp/format/prepare_resources.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace pulp::examples {

/// Configuration accepted before PulpSampler is prepared. Zero keeps the
/// runtime's certified, derived default. This cap is specifically for streamed
/// preload/page/decode storage; resident sample/mip and DSP-table ownership are
/// separate and must not be reported as covered by it.
struct PulpSamplerConfig {
    std::uint64_t streaming_memory_budget_bytes = 0;
};

enum class PulpSamplerPrepareStatus : std::uint8_t {
    NotPrepared,
    Ok,
    InvalidHostConfiguration,
    ResourceLimitExceeded,
    StreamingMemoryBudgetTooSmall,
    StreamingServiceUnavailable,
    HeritagePrepareFailed,
    AllocationFailure,
};

constexpr std::string_view pulp_sampler_prepare_status_name(
    PulpSamplerPrepareStatus status) noexcept {
    switch (status) {
        case PulpSamplerPrepareStatus::NotPrepared: return "not-prepared";
        case PulpSamplerPrepareStatus::Ok: return "ok";
        case PulpSamplerPrepareStatus::InvalidHostConfiguration:
            return "invalid-host-configuration";
        case PulpSamplerPrepareStatus::ResourceLimitExceeded:
            return "resource-limit-exceeded";
        case PulpSamplerPrepareStatus::StreamingMemoryBudgetTooSmall:
            return "streaming-memory-budget-too-small";
        case PulpSamplerPrepareStatus::StreamingServiceUnavailable:
            return "streaming-service-unavailable";
        case PulpSamplerPrepareStatus::HeritagePrepareFailed:
            return "heritage-prepare-failed";
        case PulpSamplerPrepareStatus::AllocationFailure:
            return "allocation-failure";
    }
    return "unknown";
}

struct PulpSamplerPrepareResult {
    PulpSamplerPrepareStatus status = PulpSamplerPrepareStatus::NotPrepared;
    format::PrepareResourceLimit exceeded_limit =
        format::PrepareResourceLimit::None;
    std::uint64_t required_streaming_memory_bytes = 0;
    std::uint64_t configured_streaming_memory_bytes = 0;

    constexpr bool prepared() const noexcept {
        return status == PulpSamplerPrepareStatus::Ok;
    }
};

/// How the selected codec can supply sample frames. PulpSampler's strict
/// streaming path admits Ranged only; DecodeOnceFallback is still reported so
/// callers can distinguish a PulpSampler policy rejection from a codec that is
/// unavailable to Pulp as a whole.
enum class PulpSamplerCodecCapability : std::uint8_t {
    Unknown,
    Ranged,
    DecodeOnceFallback,
};

constexpr std::string_view pulp_sampler_codec_capability_name(
    PulpSamplerCodecCapability capability) noexcept {
    switch (capability) {
        case PulpSamplerCodecCapability::Unknown: return "unknown";
        case PulpSamplerCodecCapability::Ranged: return "ranged";
        case PulpSamplerCodecCapability::DecodeOnceFallback:
            return "decode-once-fallback";
    }
    return "unknown";
}

enum class PulpSamplerSidecarStatus : std::uint8_t {
    NotPresent,
    Loaded,
    IgnoredInvalid,
};

constexpr std::string_view pulp_sampler_sidecar_status_name(
    PulpSamplerSidecarStatus status) noexcept {
    switch (status) {
        case PulpSamplerSidecarStatus::NotPresent: return "not-present";
        case PulpSamplerSidecarStatus::Loaded: return "loaded";
        case PulpSamplerSidecarStatus::IgnoredInvalid:
            return "ignored-invalid";
    }
    return "unknown";
}

enum class PulpSamplerLoadStatus : std::uint8_t {
    NotAttempted,
    Ok,
    EmptyPath,
    NotPrepared,
    ShuttingDown,
    Busy,
    BundleCapacityExceeded,
    OpenFailed,
    UnsupportedCodec,
    DecodeOnceFallbackRejected,
    UnsupportedChannelCount,
    UnsupportedSampleRate,
    InvalidPreloadContract,
    StreamingMemoryBudgetExceeded,
    PreloadReadFailed,
    SourceRegistrationFailed,
    ReversePrewarmTimedOut,
    GenerationExhausted,
    PublishFailed,
    AllocationFailure,
    InternalFailure,
};

constexpr std::string_view pulp_sampler_load_status_name(
    PulpSamplerLoadStatus status) noexcept {
    switch (status) {
        case PulpSamplerLoadStatus::NotAttempted: return "not-attempted";
        case PulpSamplerLoadStatus::Ok: return "ok";
        case PulpSamplerLoadStatus::EmptyPath: return "empty-path";
        case PulpSamplerLoadStatus::NotPrepared: return "not-prepared";
        case PulpSamplerLoadStatus::ShuttingDown: return "shutting-down";
        case PulpSamplerLoadStatus::Busy: return "busy";
        case PulpSamplerLoadStatus::BundleCapacityExceeded:
            return "bundle-capacity-exceeded";
        case PulpSamplerLoadStatus::OpenFailed: return "open-failed";
        case PulpSamplerLoadStatus::UnsupportedCodec:
            return "unsupported-codec";
        case PulpSamplerLoadStatus::DecodeOnceFallbackRejected:
            return "decode-once-fallback-rejected";
        case PulpSamplerLoadStatus::UnsupportedChannelCount:
            return "unsupported-channel-count";
        case PulpSamplerLoadStatus::UnsupportedSampleRate:
            return "unsupported-sample-rate";
        case PulpSamplerLoadStatus::InvalidPreloadContract:
            return "invalid-preload-contract";
        case PulpSamplerLoadStatus::StreamingMemoryBudgetExceeded:
            return "streaming-memory-budget-exceeded";
        case PulpSamplerLoadStatus::PreloadReadFailed:
            return "preload-read-failed";
        case PulpSamplerLoadStatus::SourceRegistrationFailed:
            return "source-registration-failed";
        case PulpSamplerLoadStatus::ReversePrewarmTimedOut:
            return "reverse-prewarm-timed-out";
        case PulpSamplerLoadStatus::GenerationExhausted:
            return "generation-exhausted";
        case PulpSamplerLoadStatus::PublishFailed: return "publish-failed";
        case PulpSamplerLoadStatus::AllocationFailure:
            return "allocation-failure";
        case PulpSamplerLoadStatus::InternalFailure: return "internal-failure";
    }
    return "unknown";
}

inline constexpr std::size_t kPulpSamplerCodecNameBytes = 31;

struct PulpSamplerLoadResult {
    PulpSamplerLoadStatus status = PulpSamplerLoadStatus::NotAttempted;
    PulpSamplerCodecCapability codec_capability =
        PulpSamplerCodecCapability::Unknown;
    PulpSamplerSidecarStatus sidecar_status =
        PulpSamplerSidecarStatus::NotPresent;
    std::array<char, kPulpSamplerCodecNameBytes + 1> codec_name{};
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint64_t total_frames = 0;
    std::uint64_t required_preload_frames = 0;
    std::uint64_t configured_preload_frames = 0;
    std::uint64_t requested_streaming_memory_bytes = 0;
    std::uint32_t sidecar_level_count = 0;
    std::uint64_t selection_generation = 0;

    constexpr bool loaded() const noexcept {
        return status == PulpSamplerLoadStatus::Ok;
    }

    std::string_view codec() const noexcept {
        std::size_t size = 0;
        while (size < codec_name.size() && codec_name[size] != '\0') ++size;
        return {codec_name.data(), size};
    }
};

/// The exact certified assumptions used to derive one streamed asset's preload
/// and page geometry. This is an inspection record, not permission to mutate
/// latency claims at runtime.
struct PulpSamplerPreloadPolicy {
    std::uint64_t selection_generation = 0;
    double source_sample_rate = 0.0;
    double host_sample_rate = 0.0;
    double maximum_playback_ratio = 4.0;
    double certified_io_latency_seconds = 0.020;
    double scheduler_margin_seconds = 0.005;
    double decoder_latency_seconds = 0.005;
    std::uint64_t maximum_host_block_frames = 0;
    std::uint64_t interpolation_guard_frames =
        audio::kHighQualitySampleSincHalfWidth;
    std::uint64_t loop_prefetch_guard_frames = 0;
    std::uint64_t required_preload_frames = 0;
    std::uint64_t configured_preload_frames = 0;
    std::uint64_t page_frames = 0;

    constexpr bool sufficient() const noexcept {
        return required_preload_frames > 0 &&
               configured_preload_frames >= required_preload_frames;
    }
};

/// Coherent callback-boundary view of the streamed voices' starvation
/// envelopes plus lifetime transition counters harvested before voice reuse.
struct PulpSamplerEnvelopeDiagnostics {
    std::uint32_t active_streamed_voices = 0;
    std::uint32_t ready_voices = 0;
    std::uint32_t fading_out_voices = 0;
    std::uint32_t silent_voices = 0;
    std::uint32_t recovering_voices = 0;
    audio::SampleStarvationEnvelopeStats lifetime{};
};

enum class PulpSamplerHeritageStatus : std::uint8_t {
    Disabled,
    PendingPrepare,
    Ready,
    ReadyRuntimeResetForHostRate,
    InvalidJson,
    InvalidProfile,
    HostRateMismatch,
    PrepareFailed,
    RuntimeStateRejected,
    RenderPlanFailed,
    RenderFailed,
    StreamDomainRebindRequired,
};

constexpr std::string_view pulp_sampler_heritage_status_name(
    PulpSamplerHeritageStatus status) noexcept {
    switch (status) {
        case PulpSamplerHeritageStatus::Disabled: return "disabled";
        case PulpSamplerHeritageStatus::PendingPrepare: return "pending-prepare";
        case PulpSamplerHeritageStatus::Ready: return "ready";
        case PulpSamplerHeritageStatus::ReadyRuntimeResetForHostRate:
            return "ready-runtime-reset-for-host-rate";
        case PulpSamplerHeritageStatus::InvalidJson: return "invalid-json";
        case PulpSamplerHeritageStatus::InvalidProfile: return "invalid-profile";
        case PulpSamplerHeritageStatus::HostRateMismatch:
            return "host-rate-mismatch";
        case PulpSamplerHeritageStatus::PrepareFailed: return "prepare-failed";
        case PulpSamplerHeritageStatus::RuntimeStateRejected:
            return "runtime-state-rejected";
        case PulpSamplerHeritageStatus::StreamDomainRebindRequired:
            return "stream-domain-rebind-required";
        case PulpSamplerHeritageStatus::RenderPlanFailed:
            return "render-plan-failed";
        case PulpSamplerHeritageStatus::RenderFailed: return "render-failed";
    }
    return "unknown";
}

struct PulpSamplerHeritageDiagnostics {
    PulpSamplerHeritageStatus status = PulpSamplerHeritageStatus::Disabled;
    audio::SampleHeritageProfileStatus profile_status =
        audio::SampleHeritageProfileStatus::Ok;
    audio::SampleHeritageRuntimeStateStatus runtime_state_status =
        audio::SampleHeritageRuntimeStateStatus::NotPrepared;
    std::array<char, audio::kSampleHeritageMaximumProfileIdBytes + 1>
        profile_id{};
    std::array<std::uint8_t, 32> profile_digest{};
    double machine_sample_rate = 0.0;
    double clock_ratio = 1.0;
    double nominal_latency_frames = 0.0;
    std::uint32_t reported_latency_frames = 0;
    std::uint64_t render_plan_failures = 0;
    std::uint64_t render_failures = 0;
    std::uint64_t rate_admission_rejections = 0;
    std::uint64_t rate_automation_rejections = 0;
    bool all_stages_bypassed = true;

    std::string_view profile() const noexcept {
        std::size_t size = 0;
        while (size < profile_id.size() && profile_id[size] != '\0') ++size;
        return {profile_id.data(), size};
    }
};

/// Top-level inspection record. Existing detailed stream counters remain on
/// PulpSamplerStreamStats; integration can return both without duplicating or
/// renaming that established surface.
struct PulpSamplerDiagnostics {
    std::uint64_t snapshot_epoch = 0;
    PulpSamplerPrepareResult prepare{};
    PulpSamplerLoadResult last_load{};
    PulpSamplerPreloadPolicy preload{};
    PulpSamplerEnvelopeDiagnostics envelope{};
    PulpSamplerHeritageDiagnostics heritage{};
    std::uint64_t streaming_memory_capacity_bytes = 0;
    std::uint64_t current_streaming_memory_bytes = 0;
    std::uint64_t peak_streaming_memory_bytes = 0;
};

static_assert(std::is_trivially_copyable_v<PulpSamplerConfig>);
static_assert(std::is_trivially_copyable_v<PulpSamplerPrepareResult>);
static_assert(std::is_trivially_copyable_v<PulpSamplerLoadResult>);
static_assert(std::is_trivially_copyable_v<PulpSamplerPreloadPolicy>);
static_assert(std::is_trivially_copyable_v<PulpSamplerEnvelopeDiagnostics>);
static_assert(std::is_trivially_copyable_v<PulpSamplerHeritageDiagnostics>);
static_assert(std::is_trivially_copyable_v<PulpSamplerDiagnostics>);

}  // namespace pulp::examples
