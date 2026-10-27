// The public header comes first so this translation unit proves it compiles on
// its own, with nothing included ahead of it.
#include <pulp/gpu_audio/gpu_audio_status.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("gpu_audio_status.hpp compiles as the first include", "[gpu_audio][headers]") {
    STATIC_REQUIRE(pulp::gpu_audio::GpuAudioStatus::kSchemaVersion == 4);
    STATIC_REQUIRE(
        pulp::gpu_audio::validate_gpu_audio_status(pulp::gpu_audio::GpuAudioStatus{}).accepted());
}
