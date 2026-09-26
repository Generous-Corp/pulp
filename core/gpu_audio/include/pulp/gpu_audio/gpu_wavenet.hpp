#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
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
