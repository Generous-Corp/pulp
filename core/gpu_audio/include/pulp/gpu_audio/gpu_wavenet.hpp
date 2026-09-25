#pragma once

#include <cstddef>
#include <cstdint>
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
    std::uint32_t dilation = 0;
    bool gated = false;
    bool head_bias = false;
    bool tanh_activation = false;
};

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
        descriptor.stream_instances != 1 || descriptor.layers.size() != 1 ||
        descriptor.weight_count == 0)
        return {GpuWaveNetError::InvalidShape};
    const auto& layer = descriptor.layers.front();
    if (layer.input_size != 1 || layer.condition_size != 1 || layer.channels == 0 ||
        layer.kernel == 0 || layer.head_size == 0 || layer.dilation == 0 || layer.gated ||
        layer.head_bias || !layer.tanh_activation)
        return {GpuWaveNetError::UnsupportedTopology};

    const auto required = static_cast<std::uint64_t>(layer.channels) * layer.input_size +
                          static_cast<std::uint64_t>(layer.channels) * layer.kernel +
                          layer.channels + layer.channels +
                          static_cast<std::uint64_t>(layer.channels) * layer.channels +
                          layer.channels +
                          static_cast<std::uint64_t>(layer.head_size) * layer.channels + 1u;
    if (required != descriptor.weight_count)
        return {GpuWaveNetError::WeightBlobMismatch};
    return {};
}

} // namespace pulp::gpu_audio
