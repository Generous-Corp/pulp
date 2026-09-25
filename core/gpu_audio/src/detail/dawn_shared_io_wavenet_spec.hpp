#pragma once

// Shape-only contract for the future provider-owned WaveNet program.  This
// header deliberately contains no Dawn types or handles.  It is useful to
// validate model metadata before a provider allocates any pipeline, history,
// or slot resources.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace pulp::gpu_audio::detail {

struct DawnSharedIoWavenetLayerSpec {
    std::uint32_t input_size = 0;
    std::uint32_t condition_size = 0;
    std::uint32_t channels = 0;
    std::uint32_t kernel = 0;
    std::uint32_t head_size = 0;
    std::uint32_t gated = 0;
    std::uint32_t head_bias = 0;
    std::span<const std::uint32_t> dilations;
};

struct DawnSharedIoWavenetProgramSpec {
    std::uint32_t block_size = 0;
    float head_scale = 1.0f;
    std::uint32_t stream_instances = 0;
    std::span<const DawnSharedIoWavenetLayerSpec> arrays;
    std::span<const float> weights;
};

enum class DawnSharedIoWavenetSpecError : std::uint8_t {
    None,
    InvalidShape,
    MissingArrays,
    MissingWeights,
    InvalidLayer,
    InvalidCondition,
    InvalidChain,
    InvalidDilation,
    HistoryOverflow,
    WeightBlobMismatch,
};

struct DawnSharedIoWavenetSpecValidation {
    DawnSharedIoWavenetSpecError error = DawnSharedIoWavenetSpecError::None;

    constexpr bool accepted() const noexcept {
        return error == DawnSharedIoWavenetSpecError::None;
    }
};

// Mirrors the flat weight order consumed by render::GpuCompute's WaveNet
// primitive.  No resource is allocated and no provider is touched here.
constexpr DawnSharedIoWavenetSpecValidation
validate_dawn_shared_io_wavenet_spec(const DawnSharedIoWavenetProgramSpec& spec) noexcept {
    if (spec.block_size == 0 || spec.stream_instances == 0)
        return {DawnSharedIoWavenetSpecError::InvalidShape};
    if (spec.arrays.empty())
        return {DawnSharedIoWavenetSpecError::MissingArrays};
    if (spec.weights.empty())
        return {DawnSharedIoWavenetSpecError::MissingWeights};

    std::uint64_t required = 0;
    for (std::size_t a = 0; a < spec.arrays.size(); ++a) {
        const auto& layer = spec.arrays[a];
        if (layer.channels == 0 || layer.channels > 64 || layer.kernel == 0 ||
            layer.head_size == 0 || layer.dilations.empty())
            return {DawnSharedIoWavenetSpecError::InvalidLayer};
        if (layer.condition_size != 1)
            return {DawnSharedIoWavenetSpecError::InvalidCondition};
        const auto expected_input = a == 0 ? 1u : spec.arrays[a - 1].channels;
        if (layer.input_size != expected_input ||
            (a > 0 && spec.arrays[a - 1].head_size != layer.channels))
            return {DawnSharedIoWavenetSpecError::InvalidChain};

        const auto z = layer.gated != 0 ? 2ull * layer.channels : layer.channels;
        required += static_cast<std::uint64_t>(layer.channels) * layer.input_size;
        for (const auto dilation : layer.dilations) {
            if (dilation == 0)
                return {DawnSharedIoWavenetSpecError::InvalidDilation};
            if (dilation > (std::numeric_limits<std::uint32_t>::max() /
                            (layer.kernel - 1u == 0 ? 1u : layer.kernel - 1u)))
                return {DawnSharedIoWavenetSpecError::HistoryOverflow};
            required += z * layer.channels * layer.kernel + z + z * layer.condition_size +
                        static_cast<std::uint64_t>(layer.channels) * layer.channels +
                        layer.channels;
        }
        required += static_cast<std::uint64_t>(layer.head_size) * layer.channels;
        if (layer.head_bias != 0)
            required += layer.head_size;
    }
    ++required; // trailing head_scale
    if (required != spec.weights.size())
        return {DawnSharedIoWavenetSpecError::WeightBlobMismatch};
    return {};
}

} // namespace pulp::gpu_audio::detail
