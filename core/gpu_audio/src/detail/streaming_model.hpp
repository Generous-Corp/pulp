#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
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
    std::string_view state_schema{};
    std::uint32_t state_schema_version = 0;
    bool deterministic = true;
};

enum class StreamingModelSpecError : std::uint8_t {
    None,
    MissingIdentity,
    InvalidShape,
};

struct StreamingModelSpecValidation {
    StreamingModelSpecError error = StreamingModelSpecError::None;

    constexpr bool accepted() const noexcept {
        return error == StreamingModelSpecError::None;
    }
};

constexpr StreamingModelSpecValidation
validate_streaming_model_spec(const StreamingModelSpec& spec) noexcept {
    if (spec.model_id.empty() || spec.architecture.empty() || spec.model_version.empty() ||
        spec.weights_hash.empty() || spec.runtime_hash.empty())
        return {StreamingModelSpecError::MissingIdentity};
    if (spec.input_channels == 0 || spec.output_channels == 0 || spec.sample_rate == 0 ||
        spec.block_size == 0)
        return {StreamingModelSpecError::InvalidShape};
    if ((spec.state_bytes != 0) != (!spec.state_schema.empty()) ||
        (spec.state_bytes != 0 && spec.state_schema_version == 0))
        return {StreamingModelSpecError::InvalidShape};
    return {};
}

/// Preparation is control-thread-only. The artifact and all string views must
/// outlive the prepared model/backend and remain immutable until release.
struct StreamingPrepareContext {
    const StreamingModelSpec* spec = nullptr;
    std::string_view artifact_id{};
    std::string_view artifact_hash{};
    StreamingFallbackStrategy fallback = StreamingFallbackStrategy::NoFallback;
    std::uint32_t max_frames = 0;
    std::uint32_t lead_blocks = 0;
    bool worker_backend_requested = false;
};

constexpr bool valid_streaming_prepare_context(const StreamingPrepareContext& context) noexcept {
    return context.spec != nullptr && validate_streaming_model_spec(*context.spec).accepted() &&
           !context.artifact_id.empty() && !context.artifact_hash.empty() &&
           context.max_frames >= context.spec->block_size;
}

struct StreamingBlockStamp {
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;

    friend constexpr bool operator==(StreamingBlockStamp, StreamingBlockStamp) noexcept = default;
};

struct StreamingBlock {
    StreamingBlockStamp stamp{};
    std::span<const float> input{};
    std::span<float> output{};
    std::uint32_t input_channels = 0;
    std::uint32_t output_channels = 0;
    std::uint32_t frames = 0;
    // Explicit sample strides make the worker contract compatible with
    // planar BufferView leases as well as interleaved shared-I/O buffers.
    std::uint32_t input_channel_stride = 0;
    std::uint32_t output_channel_stride = 0;
    std::uint32_t input_frame_stride = 1;
    std::uint32_t output_frame_stride = 1;
};

constexpr bool valid_streaming_block_layout(const StreamingBlock& block) noexcept {
    if (block.input_channels == 0 || block.output_channels == 0 || block.frames == 0 ||
        block.input.empty() || block.output.empty() || block.input_channel_stride == 0 ||
        block.output_channel_stride == 0 || block.input_frame_stride == 0 ||
        block.output_frame_stride == 0)
        return false;
    const auto covers = [](std::size_t size, std::uint32_t channels, std::uint32_t frames,
                           std::uint32_t channel_stride,
                           std::uint32_t frame_stride) constexpr noexcept {
        const auto last = static_cast<std::size_t>(channels - 1) * channel_stride +
                          static_cast<std::size_t>(frames - 1) * frame_stride;
        return last < size;
    };
    // Bounds do not prove that the affine channel/frame address map is
    // injective. For positive strides, the smallest duplicate displacement is
    // (frame_stride/gcd, channel_stride/gcd); reject it whenever both
    // displacements fit inside the declared channel/frame extents.
    const auto injective = [](std::uint32_t channels, std::uint32_t frames,
                              std::uint32_t channel_stride,
                              std::uint32_t frame_stride) constexpr noexcept {
        const auto divisor = std::gcd(channel_stride, frame_stride);
        return frame_stride / divisor >= channels || channel_stride / divisor >= frames;
    };
    return injective(block.input_channels, block.frames, block.input_channel_stride,
                     block.input_frame_stride) &&
           injective(block.output_channels, block.frames, block.output_channel_stride,
                     block.output_frame_stride) &&
           covers(block.input.size(), block.input_channels, block.frames,
                  block.input_channel_stride, block.input_frame_stride) &&
           covers(block.output.size(), block.output_channels, block.frames,
                  block.output_channel_stride, block.output_frame_stride);
}

enum class StreamingAdmission : std::uint8_t {
    Accepted,
    Rejected,
};

enum class StreamingBackendTerminalDisposition : std::uint8_t {
    Completed,
    ProviderFailed,
    DeviceLost,
    Cancelled,
    Stale,
};

/// One worker/backend terminal. Exactly one terminal must be published for
/// every admitted stamp; a later result cannot fill a missing sequence hole.
struct StreamingTerminal {
    StreamingBlockStamp stamp{};
    StreamingBackendTerminalDisposition disposition =
        StreamingBackendTerminalDisposition::Cancelled;
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

[[nodiscard]] constexpr audio::RtSafetyClass
streaming_method_safety(StreamingModelMethod method) noexcept {
    switch (method) {
    case StreamingModelMethod::Describe:
        return audio::RtSafetyClass::ControlThreadOnly;
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
    virtual bool prepare(const StreamingPrepareContext&) noexcept = 0;
    virtual void process_cpu(const audio::BufferView<const float>& input,
                             audio::BufferView<float>& output, std::uint32_t frames,
                             StreamingBlockStamp stamp) noexcept = 0;
    // The owner must stop admission and fence callback/worker users before
    // quiesce, reset, or release. These methods never race process_cpu().
    // release() is idempotent and retryable after a failed prepare/release;
    // callers retain ownership until it returns true.
    virtual bool quiesce() noexcept = 0;
    virtual void reset(std::uint64_t epoch, StreamingResetReason reason) noexcept = 0;
    virtual bool release() noexcept = 0;
};

/// Optional worker-only seam reserved for a later MLX/Dawn adapter. Keeping it
/// separate from StreamingModel lets CPU models ship on every Pulp CI platform.
class StreamingBackend {
  public:
    virtual ~StreamingBackend() = default;
    virtual bool prepare(const StreamingPrepareContext&) noexcept = 0;
    // On Accepted, the transport retains the block's input/output leases until
    // receive() publishes exactly one terminal for the same stamp. Rejected
    // blocks carry no terminal obligation.
    // Admission owns the layout safety check so every backend rejects
    // out-of-bounds or aliasing leases before retaining them. Implementations
    // only receive a validated block through enqueue_validated().
    StreamingAdmission enqueue(const StreamingBlock& block) noexcept {
        if (!valid_streaming_block_layout(block))
            return StreamingAdmission::Rejected;
        return enqueue_validated(block);
    }
    virtual std::size_t service_until(std::uint64_t deadline_ns) noexcept = 0;
    virtual bool receive(StreamingTerminal&) noexcept = 0;
    // Host/quiescent only: stop admission, fence/drain or cancel queued work,
    // then advance the backend epoch so old terminals are stale.
    virtual bool begin_epoch(std::uint64_t epoch, StreamingResetReason reason) noexcept = 0;
    virtual bool reprepare_after_device_loss(std::uint64_t epoch) noexcept = 0;
    virtual bool quiesce() noexcept = 0;
    virtual bool release() noexcept = 0;

  protected:
    virtual StreamingAdmission enqueue_validated(const StreamingBlock&) noexcept = 0;
};

/// Small, portable causal-convolution model used to prove that the streaming
/// contract is not coupled to WaveNet.  The topology is intentionally fixed:
/// one depthwise dilated-convolution layer followed by ReLU.  Weights and the
/// ring buffer are owned by the instance, so process_cpu() performs no
/// allocation and carries state across host blocks.
template <std::size_t Channels, std::size_t KernelSize> struct MicroTcnWeights {
    static_assert(Channels > 0 && KernelSize > 0);
    std::array<std::array<float, KernelSize>, Channels> taps{};
    std::array<float, Channels> bias{};
};

template <std::size_t Channels, std::size_t KernelSize>
class MicroTcnModel final : public StreamingModel {
  public:
    using Weights = MicroTcnWeights<Channels, KernelSize>;

    explicit MicroTcnModel(Weights weights = {})
        : weights_(weights),
          spec_{.model_id = "pulp.micro-tcn",
                .architecture = "tcn.depthwise-relu",
                .model_version = "1",
                .weights_hash = "embedded-test-weights",
                .runtime_hash = "pulp.micro-tcn.v1",
                .input_channels = static_cast<std::uint32_t>(Channels),
                .output_channels = static_cast<std::uint32_t>(Channels),
                .sample_rate = 48000,
                .block_size = 64,
                .feature_rate = 48000,
                .intrinsic_latency_samples = 0,
                .receptive_field_samples = static_cast<std::uint32_t>(KernelSize),
                .state_bytes = Channels * KernelSize * sizeof(float),
                .state_schema = "causal-ring-v1",
                .state_schema_version = 1,
                .deterministic = true} {
        reset_state();
    }

    const StreamingModelSpec& spec() const noexcept override {
        return spec_;
    }

    bool prepare(const StreamingPrepareContext& context) noexcept override {
        prepared_ = valid_streaming_prepare_context(context) && context.spec == &spec_ &&
                    context.spec->input_channels == Channels &&
                    context.spec->output_channels == Channels &&
                    context.spec->block_size <= MaxSupportedFrames &&
                    context.max_frames <= MaxSupportedFrames;
        if (prepared_)
            reset_state();
        return prepared_;
    }

    void process_cpu(const audio::BufferView<const float>& input, audio::BufferView<float>& output,
                     std::uint32_t frames, StreamingBlockStamp stamp) noexcept override {
        last_stamp_ = stamp;
        if (!prepared_ || frames > MaxSupportedFrames || input.num_channels() < Channels ||
            output.num_channels() < Channels || input.num_samples() < frames ||
            output.num_samples() < frames) {
            output.clear();
            return;
        }
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            for (std::size_t channel = 0; channel < Channels; ++channel) {
                float value = weights_.bias[channel];
                const auto input_sample = input.channel_ptr(channel)[frame];
                value += weights_.taps[channel][0] * input_sample;
                for (std::size_t tap = 1; tap < KernelSize; ++tap) {
                    const auto index = (ring_cursor_ + KernelSize - tap) % KernelSize;
                    value += weights_.taps[channel][tap] * state_[channel][index];
                }
                output.channel_ptr(channel)[frame] = value > 0.0f ? value : 0.0f;
                state_[channel][ring_cursor_] = input_sample;
            }
            ring_cursor_ = (ring_cursor_ + 1) % KernelSize;
        }
    }

    bool quiesce() noexcept override {
        return true;
    }

    void reset(std::uint64_t epoch, StreamingResetReason) noexcept override {
        reset_state();
        last_stamp_ = {.epoch = epoch, .sequence = 0};
    }

    bool release() noexcept override {
        prepared_ = false;
        reset_state();
        return true;
    }

    StreamingBlockStamp last_stamp() const noexcept {
        return last_stamp_;
    }

  private:
    static constexpr std::size_t MaxSupportedFrames = 4096;

    void reset_state() noexcept {
        for (auto& channel : state_)
            channel.fill(0.0f);
        ring_cursor_ = 0;
    }

    Weights weights_;
    StreamingModelSpec spec_;
    std::array<std::array<float, KernelSize>, Channels> state_{};
    std::size_t ring_cursor_ = 0;
    StreamingBlockStamp last_stamp_{};
    bool prepared_ = false;
};

/// Allocation-free causal max pooling for model fixtures and learned-filter
/// adapters. The current sample is included in the window, so the primitive
/// has zero lookahead and zero intrinsic latency; WindowSize describes the
/// number of current/previous samples retained in its persistent state.
template <std::size_t Channels, std::size_t WindowSize> class CausalMaxPool {
  public:
    static_assert(Channels > 0 && WindowSize > 0);
    static constexpr std::size_t lookahead_samples = 0;
    static constexpr std::size_t intrinsic_latency_samples = 0;
    static constexpr std::size_t receptive_field_samples = WindowSize;
    static constexpr std::size_t state_bytes = Channels * WindowSize * sizeof(float);

    CausalMaxPool() noexcept {
        reset();
    }

    void process(const audio::BufferView<const float>& input, audio::BufferView<float>& output,
                 std::uint32_t frames) noexcept {
        if (frames > MaxSupportedFrames || input.num_channels() < Channels ||
            output.num_channels() < Channels || input.num_samples() < frames ||
            output.num_samples() < frames) {
            output.clear();
            return;
        }
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            for (std::size_t channel = 0; channel < Channels; ++channel) {
                const auto sample = input.channel_ptr(channel)[frame];
                auto maximum = sample;
                for (std::size_t offset = 1; offset < WindowSize; ++offset) {
                    const auto index = (cursor_ + WindowSize - offset) % WindowSize;
                    maximum = maximum > state_[channel][index] ? maximum : state_[channel][index];
                }
                output.channel_ptr(channel)[frame] = maximum;
                state_[channel][cursor_] = sample;
            }
            cursor_ = (cursor_ + 1) % WindowSize;
        }
    }

    void reset() noexcept {
        for (auto& channel : state_)
            channel.fill(std::numeric_limits<float>::lowest());
        cursor_ = 0;
    }

  private:
    static constexpr std::size_t MaxSupportedFrames = 4096;
    std::array<std::array<float, WindowSize>, Channels> state_{};
    std::size_t cursor_ = 0;
};

static_assert(streaming_method_safety(StreamingModelMethod::ProcessCpu) ==
              audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);
static_assert(streaming_method_safety(StreamingModelMethod::EnqueueBackend) ==
              audio::RtSafetyClass::BackgroundThreadOnly);

} // namespace pulp::gpu_audio::detail
