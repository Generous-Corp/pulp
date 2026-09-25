#include "detail/dawn_shared_io_wavenet_spec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

using namespace pulp::gpu_audio::detail;

namespace {
struct Fixture {
    std::array<std::uint32_t, 2> dilations{1, 2};
    DawnSharedIoWavenetLayerSpec layer{
        .input_size = 1,
        .condition_size = 1,
        .channels = 2,
        .kernel = 2,
        .head_size = 1,
        .gated = 1,
        .head_bias = 1,
        .dilations = dilations,
    };
    // rechannel 2 + 2 layers * (4*2*2 + 4 + 4 + 4 + 2) + head (2 + 1) + scale
    std::vector<float> weights = std::vector<float>(static_cast<std::size_t>(2 + 2 * 30 + 4), 0.0f);
};
} // namespace

TEST_CASE("WaveNet shared spec accepts the authenticated flat-weight shape",
          "[gpu_audio][shared_io][wavenet]") {
    Fixture fixture;
    const DawnSharedIoWavenetProgramSpec spec{
        .block_size = 32,
        .head_scale = 1.0f,
        .stream_instances = 2,
        .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
        .weights = fixture.weights,
    };
    CHECK(validate_dawn_shared_io_wavenet_spec(spec).accepted());
}

TEST_CASE("WaveNet shared spec rejects mismatched flat weights before allocation",
          "[gpu_audio][shared_io][wavenet]") {
    Fixture fixture;
    fixture.weights.pop_back();
    const DawnSharedIoWavenetProgramSpec spec{
        .block_size = 32,
        .head_scale = 1.0f,
        .stream_instances = 2,
        .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
        .weights = fixture.weights,
    };
    CHECK(validate_dawn_shared_io_wavenet_spec(spec).error ==
          DawnSharedIoWavenetSpecError::WeightBlobMismatch);
}

TEST_CASE("WaveNet shared spec rejects non-mono conditioning and broken layer chains",
          "[gpu_audio][shared_io][wavenet]") {
    Fixture fixture;
    fixture.layer.condition_size = 2;
    const DawnSharedIoWavenetProgramSpec condition_spec{
        .block_size = 32,
        .head_scale = 1.0f,
        .stream_instances = 2,
        .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
        .weights = fixture.weights,
    };
    auto result = validate_dawn_shared_io_wavenet_spec(condition_spec);
    CHECK(result.error == DawnSharedIoWavenetSpecError::InvalidCondition);

    fixture.layer.condition_size = 1;
    fixture.layer.input_size = 2;
    const DawnSharedIoWavenetProgramSpec chain_spec{
        .block_size = 32,
        .head_scale = 1.0f,
        .stream_instances = 2,
        .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
        .weights = fixture.weights,
    };
    result = validate_dawn_shared_io_wavenet_spec(chain_spec);
    CHECK(result.error == DawnSharedIoWavenetSpecError::InvalidChain);
}

TEST_CASE("WaveNet shared spec rejects zero stream instances and dilation",
          "[gpu_audio][shared_io][wavenet]") {
    Fixture fixture;
    fixture.dilations[0] = 0;
    const DawnSharedIoWavenetProgramSpec dilation_spec{
        .block_size = 32,
        .head_scale = 1.0f,
        .stream_instances = 2,
        .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
        .weights = fixture.weights,
    };
    auto result = validate_dawn_shared_io_wavenet_spec(dilation_spec);
    CHECK(result.error == DawnSharedIoWavenetSpecError::InvalidDilation);

    fixture.dilations[0] = 1;
    const DawnSharedIoWavenetProgramSpec instance_spec{
        .block_size = 32,
        .head_scale = 1.0f,
        .stream_instances = 0,
        .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
        .weights = fixture.weights,
    };
    result = validate_dawn_shared_io_wavenet_spec(instance_spec);
    CHECK(result.error == DawnSharedIoWavenetSpecError::InvalidShape);
}
