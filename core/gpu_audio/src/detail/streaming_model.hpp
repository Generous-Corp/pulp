#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/gpu_audio/gpu_audio_program.hpp>

namespace pulp::gpu_audio::detail {

/// Why a prepared streaming model's mutable causal state is being replaced.
/// Reset is an explicit state boundary; it is never inferred from a late result.
enum class StreamingResetReason : std::uint8_t {
    TransportRestart,
    ModelSwap,
    DeviceLoss,
    SampleRateChange,
    BlockSizeChange,
    OfflineTransition,
};

enum class StreamingFallbackStrategy : std::uint8_t {
    ContinuouslyPrimedCpuShadow,
    BoundedReconstructable,
    NoFallback,
};

/// Immutable metadata shared by every prepared instance and backend plan.
/// Strings refer to control-side storage and are never read from the callback
/// unless the caller has explicitly made that storage immutable.
struct StreamingModelSpec {
    std::string_view model_id{};
    std::string_view architecture{};
    std::string_view model_version{};
    std::string_view weights_hash{};
    std::string_view runtime_hash{};
    std::uint32_t input_channels = 0;
    std::uint32_t output_channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t block_size = 0;
    std::uint32_t feature_rate = 0;
    std::uint32_t intrinsic_latency_samples = 0;
    std::uint32_t receptive_field_samples = 0;
    std::uint64_t state_bytes = 0;
    StreamingFallbackStrategy fallback = StreamingFallbackStrategy::NoFallback;
    bool deterministic = true;
    bool supports_cpu = true;
    bool supports_worker_backend = false;
};

enum class StreamingModelSpecError : std::uint8_t {
    None,
    MissingIdentity,
    InvalidShape,
    CpuFallbackUnavailable,
};

struct StreamingModelSpecValidation {
    StreamingModelSpecError error = StreamingModelSpecError::None;

    constexpr bool accepted() const noexcept { return error == StreamingModelSpecError::None; }
};

constexpr StreamingModelSpecValidation
validate_streaming_model_spec(const StreamingModelSpec& spec) noexcept {
    if (spec.model_id.empty() || spec.architecture.empty() || spec.model_version.empty() ||
        spec.weights_hash.empty() || spec.runtime_hash.empty())
        return {StreamingModelSpecError::MissingIdentity};
    if (spec.input_channels == 0 || spec.output_channels == 0 || spec.sample_rate == 0 ||
        spec.block_size == 0)
        return {StreamingModelSpecError::InvalidShape};
    if (spec.fallback == StreamingFallbackStrategy::ContinuouslyPrimedCpuShadow &&
        !spec.supports_cpu)
        return {StreamingModelSpecError::CpuFallbackUnavailable};
    return {};
}

struct StreamingBlockStamp {
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;

    friend constexpr bool operator==(StreamingBlockStamp, StreamingBlockStamp) noexcept = default;
};

/// One worker/backend terminal. Exactly one terminal must be published for
/// every admitted stamp; a later result cannot fill a missing sequence hole.
struct StreamingTerminal {
    StreamingBlockStamp stamp{};
    GpuAudioTerminalDisposition disposition = GpuAudioTerminalDisposition::Unknown;
};

enum class StreamingModelMethod : std::uint8_t {
    Describe,
    Prepare,
    ProcessCpu,
    Reset,
    Release,
    EnqueueBackend,
    ServiceBackend,
    ReceiveBackend,
};

[[nodiscard]] constexpr audio::RtSafetyClass streaming_method_safety(
    StreamingModelMethod method) noexcept {
    switch (method) {
        case StreamingModelMethod::Describe:
            return audio::RtSafetyClass::AudioCallbackSafeWithImmutableInputs;
        case StreamingModelMethod::ProcessCpu:
            return audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;
        case StreamingModelMethod::Prepare:
        case StreamingModelMethod::Reset:
        case StreamingModelMethod::Release:
            return audio::RtSafetyClass::ControlThreadOnly;
        case StreamingModelMethod::EnqueueBackend:
        case StreamingModelMethod::ServiceBackend:
        case StreamingModelMethod::ReceiveBackend:
            return audio::RtSafetyClass::BackgroundThreadOnly;
    }
    return audio::RtSafetyClass::ControlThreadOnly;
}

/// Internal first-slice contract. It deliberately does not inherit
/// GpuAudioNode, expose MLX/Dawn types, or prescribe a tensor representation.
/// A model instance owns exactly one mutable causal state. Backend state and a
/// CPU fallback state are separate instances and must never share activations.
class StreamingModel {
  public:
    virtual ~StreamingModel() = default;

    virtual const StreamingModelSpec& spec() const noexcept = 0;
    virtual bool prepare() noexcept = 0;
    virtual void process_cpu(const audio::BufferView<const float>& input,
                             audio::BufferView<float>& output,
                             std::uint32_t frames,
                             StreamingBlockStamp stamp) noexcept = 0;
    virtual void reset(std::uint64_t epoch, StreamingResetReason reason) noexcept = 0;
    virtual void release() noexcept = 0;
};

/// Optional worker-only seam reserved for a later MLX/Dawn adapter. Keeping it
/// separate from StreamingModel lets CPU models ship on every Pulp CI platform.
class StreamingBackend {
  public:
    virtual ~StreamingBackend() = default;
    virtual bool prepare(const StreamingModelSpec&) noexcept = 0;
    virtual bool enqueue(const StreamingBlockStamp&) noexcept = 0;
    virtual std::size_t service_until(std::uint64_t deadline_ns) noexcept = 0;
    virtual bool receive(StreamingTerminal&) noexcept = 0;
    virtual bool reprepare_after_device_loss(std::uint64_t epoch) noexcept = 0;
    virtual void release() noexcept = 0;
};

static_assert(streaming_method_safety(StreamingModelMethod::ProcessCpu) ==
              audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);
static_assert(streaming_method_safety(StreamingModelMethod::EnqueueBackend) ==
              audio::RtSafetyClass::BackgroundThreadOnly);

} // namespace pulp::gpu_audio::detail
