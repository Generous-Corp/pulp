#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>

namespace pulp::gpu_audio {

// Model-neutral shape accepted by the first provider-owned WaveNet adapter.
// The execution object remains private to the provider; consumers pass only
// immutable metadata and the flat weight count through this boundary.
struct GpuWaveNetLayerDescriptor {
    std::uint32_t input_size = 0;
    std::uint32_t condition_size = 0;
    std::uint32_t channels = 0;
    std::uint32_t kernel = 0;
    std::uint32_t head_size = 0;
    // Retained for source compatibility with the first one-layer descriptor.
    // Multi-layer arrays leave this zero and provide dilations below.
    std::uint32_t dilation = 0;
    bool gated = false;
    bool head_bias = false;
    bool tanh_activation = false;
    std::span<const std::uint32_t> dilations;
};

// Semantic alias for callers describing a full WaveNet layer array. The
// original public name remains valid and retains its aggregate field order.
using GpuWaveNetLayerArrayDescriptor = GpuWaveNetLayerDescriptor;

struct GpuWaveNetDescriptor {
    std::uint32_t block_size = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t stream_instances = 0;
    float head_scale = 1.0f;
    std::span<const GpuWaveNetLayerDescriptor> layers;
    std::size_t weight_count = 0;
};

enum class GpuWaveNetError : std::uint8_t {
    None = 0,
    InvalidShape,
    UnsupportedTopology,
    UnsupportedActivation,
    WeightBlobMismatch,
    ResourceOverflow,
};

struct GpuWaveNetValidation {
    GpuWaveNetError error = GpuWaveNetError::None;

    constexpr bool accepted() const noexcept {
        return error == GpuWaveNetError::None;
    }
};

/// Errors returned while constructing or operating a one-stream shared
/// WaveNet session. A missing provider is a normal capability result on
/// platforms that do not ship the authenticated Dawn backend.
enum class GpuWaveNetSessionError : std::uint8_t {
    None = 0,
    InvalidDescriptor,
    InvalidWeights,
    ProviderUnavailable,
    ProgramUnavailable,
    PreparationFailed,
    NotPrepared,
    InvalidBlock,
    NoSlot,
    SubmissionRejected,
    OutputTooSmall,
    ReleaseFailed,
};

enum class GpuWaveNetBlockStatus : std::uint8_t {
    GpuDelivered = 0,
    ProviderFailed,
};

struct GpuWaveNetBlockResult {
    std::uint64_t sequence = 0;
    GpuWaveNetBlockStatus status = GpuWaveNetBlockStatus::ProviderFailed;
    bool late = false;
};

/// A prepared, one-stream shared-memory WaveNet session.
///
/// The session owns the provider, resident model resources, causal history,
/// and fixed shared input/output slots. Callers provide only model metadata,
/// weights, and audio spans; Dawn, Metal, queue, and slot handles remain
/// private. `submit_block()` and `service()` are serialized non-realtime
/// operations in this first SDK seam. A caller that needs a realtime bridge
/// should connect this session to its own preallocated callback/fallback
/// machinery rather than invoking provider work from the audio callback.
///
/// The first public session is intentionally limited to one causal stream
/// (`descriptor.stream_instances == 1`). Submitted sequence numbers must be
/// contiguous because they advance the persistent WaveNet history.
class GpuWaveNetSession {
  public:
    struct Config {
        GpuWaveNetDescriptor descriptor{};
        std::span<const float> weights{};
        std::uint32_t slots = 2;
    };

    struct CreateResult {
        std::unique_ptr<GpuWaveNetSession> session;
        GpuWaveNetSessionError error = GpuWaveNetSessionError::None;

        explicit operator bool() const noexcept {
            return session != nullptr && error == GpuWaveNetSessionError::None;
        }
    };

    static CreateResult create(const Config& config) noexcept;
    ~GpuWaveNetSession();

    GpuWaveNetSession(const GpuWaveNetSession&) = delete;
    GpuWaveNetSession& operator=(const GpuWaveNetSession&) = delete;

    bool prepared() const noexcept;
    std::uint32_t block_size() const noexcept;

    /// Copies one mono block into a persistent shared slot and submits it to
    /// the authenticated provider. `deadline_ns == 0` disables late marking.
    bool submit_block(std::span<const float> input, std::uint64_t sequence,
                      std::uint64_t deadline_ns = 0) noexcept;

    /// Services already-submitted provider work without waiting for a future
    /// completion. Returns the number of newly visible terminal records.
    std::size_t service(std::uint64_t now_ns) noexcept;

    /// Copies one completed block into `output` and releases its shared slot.
    /// A failed provider completion returns a result with no output written.
    std::optional<GpuWaveNetBlockResult> receive(std::span<float> output) noexcept;

    /// Host/quiescent-only release. A false result retains the session so the
    /// caller can retry the provider's physical drain barrier.
    bool release() noexcept;

  private:
    struct Impl;
    explicit GpuWaveNetSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// The first public adapter is deliberately narrow. Unsupported NAM models
// must remain on the continuously prepared CPU fallback rather than silently
// entering a provider path with the wrong state layout.
constexpr GpuWaveNetValidation
validate_gpu_wavenet_descriptor(const GpuWaveNetDescriptor& descriptor) noexcept {
    if (descriptor.block_size == 0 || descriptor.sample_rate == 0 ||
        descriptor.stream_instances != 1 || descriptor.layers.empty() ||
        descriptor.weight_count == 0)
        return {GpuWaveNetError::InvalidShape};
    std::uint64_t required = 1u; // trailing serialized head scale
    for (std::size_t index = 0; index < descriptor.layers.size(); ++index) {
        const auto& array = descriptor.layers[index];
        const auto expected_input = index == 0 ? 1u : descriptor.layers[index - 1].channels;
        if (array.input_size != expected_input || array.condition_size != 1 ||
            array.channels == 0 || array.channels > 64 || array.kernel == 0 ||
            array.head_size == 0 || !array.tanh_activation ||
            (array.dilation == 0 && array.dilations.empty()) ||
            (array.dilation != 0 && !array.dilations.empty()))
            return {GpuWaveNetError::UnsupportedTopology};
        if (index > 0 && descriptor.layers[index - 1].head_size != array.channels)
            return {GpuWaveNetError::UnsupportedTopology};

        const auto z = array.gated ? 2ull * array.channels : array.channels;
        required += static_cast<std::uint64_t>(array.channels) * array.input_size;
        const auto dilation_count = array.dilations.empty() ? 1u : array.dilations.size();
        for (std::size_t dilation_index = 0; dilation_index < dilation_count; ++dilation_index) {
            const auto dilation =
                array.dilations.empty() ? array.dilation : array.dilations[dilation_index];
            if (dilation == 0)
                return {GpuWaveNetError::UnsupportedTopology};
            const auto reach = static_cast<std::uint64_t>(array.kernel - 1u) * dilation;
            if (reach > std::numeric_limits<std::uint32_t>::max() - descriptor.block_size)
                return {GpuWaveNetError::ResourceOverflow};
            required += z * array.channels * array.kernel + z + z * array.condition_size +
                        static_cast<std::uint64_t>(array.channels) * array.channels +
                        array.channels;
            if (required > std::numeric_limits<std::uint32_t>::max())
                return {GpuWaveNetError::ResourceOverflow};
        }
        required += static_cast<std::uint64_t>(array.head_size) * array.channels;
        if (array.head_bias)
            required += array.head_size;
        if (required >= std::numeric_limits<std::uint32_t>::max())
            return {GpuWaveNetError::ResourceOverflow};
    }
    if (descriptor.layers.back().head_size != 1)
        return {GpuWaveNetError::UnsupportedTopology};
    if (required != descriptor.weight_count)
        return {GpuWaveNetError::WeightBlobMismatch};
    return {};
}

} // namespace pulp::gpu_audio
