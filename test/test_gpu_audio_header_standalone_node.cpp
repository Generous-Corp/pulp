// The public header comes first so this translation unit proves it compiles on
// its own, with nothing included ahead of it.
#include <pulp/gpu_audio/gpu_audio_node.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("gpu_audio_node.hpp compiles as the first include", "[gpu_audio][headers]") {
    STATIC_REQUIRE(pulp::gpu_audio::GpuAudioStatus::kTerminalDispositionCount == 8);
    STATIC_REQUIRE(sizeof(pulp::gpu_audio::GpuAudioNode) > 0);
}
