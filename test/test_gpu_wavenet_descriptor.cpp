#include <pulp/gpu_audio/gpu_wavenet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace pulp::gpu_audio;

TEST_CASE("public WaveNet descriptor accepts the supported mono shape", "[gpu_audio][wavenet]") {
    const GpuWaveNetLayerDescriptor layer{1, 1, 1, 1, 1, 1, false, false, true, {}};
    const GpuWaveNetDescriptor descriptor{32, 48000, 1, 1.0f, {&layer, 1}, 8};
    CHECK(validate_gpu_wavenet_descriptor(descriptor).accepted());
}

TEST_CASE("public WaveNet descriptor rejects unsupported topology", "[gpu_audio][wavenet]") {
    const GpuWaveNetLayerDescriptor layer{1, 1, 2, 3, 1, 1, false, false, false, {}};
    const GpuWaveNetDescriptor descriptor{32, 48000, 1, 1.0f, {&layer, 1}, 27};
    CHECK(validate_gpu_wavenet_descriptor(descriptor).error ==
          GpuWaveNetError::UnsupportedTopology);
}

TEST_CASE("public WaveNet descriptor counts all convolution input channels",
          "[gpu_audio][wavenet]") {
    const GpuWaveNetLayerDescriptor layer{1, 1, 2, 3, 1, 1, false, false, true, {}};
    GpuWaveNetDescriptor descriptor{32, 48000, 1, 1.0f, {&layer, 1}, 27};
    CHECK(validate_gpu_wavenet_descriptor(descriptor).accepted());
    descriptor.weight_count = 21;
    CHECK(validate_gpu_wavenet_descriptor(descriptor).error == GpuWaveNetError::WeightBlobMismatch);
}

TEST_CASE("public WaveNet descriptor rejects a mismatched weight blob", "[gpu_audio][wavenet]") {
    const GpuWaveNetLayerDescriptor layer{1, 1, 1, 1, 1, 1, false, false, true, {}};
    const GpuWaveNetDescriptor descriptor{32, 48000, 1, 1.0f, {&layer, 1}, 7};
    CHECK(validate_gpu_wavenet_descriptor(descriptor).error == GpuWaveNetError::WeightBlobMismatch);
}

TEST_CASE("public WaveNet descriptor rejects non-mono output and resource overflow",
          "[gpu_audio][wavenet]") {
    GpuWaveNetLayerDescriptor layer{1, 1, 2, 1, 2, 1, false, false, true, {}};
    GpuWaveNetDescriptor descriptor{32, 48000, 1, 1.0f, {&layer, 1}, 1};
    CHECK(validate_gpu_wavenet_descriptor(descriptor).error ==
          GpuWaveNetError::UnsupportedTopology);

    layer.head_size = std::numeric_limits<std::uint32_t>::max();
    CHECK(validate_gpu_wavenet_descriptor(descriptor).error == GpuWaveNetError::ResourceOverflow);
}

TEST_CASE("public WaveNet descriptor accepts GPU NAM bundled model shapes",
          "[gpu_audio][wavenet]") {
    const std::uint32_t example_d0[] = {1, 2};
    const std::uint32_t example_d1[] = {8};
    const GpuWaveNetLayerArrayDescriptor example[] = {
        {.input_size = 1,
         .condition_size = 1,
         .channels = 3,
         .kernel = 3,
         .head_size = 2,
         .gated = false,
         .head_bias = false,
         .tanh_activation = true,
         .dilations = example_d0},
        {.input_size = 3,
         .condition_size = 1,
         .channels = 2,
         .kernel = 3,
         .head_size = 1,
         .gated = false,
         .head_bias = true,
         .tanh_activation = true,
         .dilations = example_d1},
    };
    CHECK(validate_gpu_wavenet_descriptor({32, 48'000, 1, 0.02f, example, 131}).accepted());

    const std::uint32_t standard_d[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
    const GpuWaveNetLayerArrayDescriptor standard[] = {
        {.input_size = 1,
         .condition_size = 1,
         .channels = 16,
         .kernel = 3,
         .head_size = 8,
         .gated = false,
         .head_bias = false,
         .tanh_activation = true,
         .dilations = standard_d},
        {.input_size = 16,
         .condition_size = 1,
         .channels = 8,
         .kernel = 3,
         .head_size = 1,
         .gated = false,
         .head_bias = true,
         .tanh_activation = true,
         .dilations = standard_d},
    };
    CHECK(validate_gpu_wavenet_descriptor({32, 48'000, 1, 0.02f, standard, 13'802}).accepted());
    CHECK(validate_gpu_wavenet_descriptor({32, 48'000, 1, 0.02f, standard, 13'801}).error ==
          GpuWaveNetError::WeightBlobMismatch);
}
