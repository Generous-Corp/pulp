#include <pulp/gpu_audio/gpu_wavenet.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace pulp::gpu_audio;

TEST_CASE("public WaveNet descriptor accepts the supported mono shape",
          "[gpu_audio][wavenet]") {
    const GpuWaveNetLayerDescriptor layer{1, 1, 1, 1, 1, 1, false, false, true};
    const GpuWaveNetDescriptor descriptor{32, 48000, 1, 1.0f, {&layer, 1}, 8};
    CHECK(validate_gpu_wavenet_descriptor(descriptor).accepted());
}

TEST_CASE("public WaveNet descriptor rejects unsupported topology", "[gpu_audio][wavenet]") {
    const GpuWaveNetLayerDescriptor layer{1, 1, 2, 3, 1, 1, true, false, true};
    const GpuWaveNetDescriptor descriptor{32, 48000, 1, 1.0f, {&layer, 1}, 31};
    CHECK(validate_gpu_wavenet_descriptor(descriptor).error ==
          GpuWaveNetError::UnsupportedTopology);
}

TEST_CASE("public WaveNet descriptor rejects a mismatched weight blob",
          "[gpu_audio][wavenet]") {
    const GpuWaveNetLayerDescriptor layer{1, 1, 1, 1, 1, 1, false, false, true};
    const GpuWaveNetDescriptor descriptor{32, 48000, 1, 1.0f, {&layer, 1}, 7};
    CHECK(validate_gpu_wavenet_descriptor(descriptor).error ==
          GpuWaveNetError::WeightBlobMismatch);
}
