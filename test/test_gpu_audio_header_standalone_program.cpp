// The public header comes first so this translation unit proves it compiles on
// its own, with nothing included ahead of it.
#include <pulp/gpu_audio/gpu_audio_program.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("gpu_audio_program.hpp compiles as the first include", "[gpu_audio][headers]") {
    STATIC_REQUIRE(static_cast<unsigned>(pulp::gpu_audio::GpuAudioTerminalDisposition::kCount) ==
                   pulp::gpu_audio::GpuAudioStatus::kTerminalDispositionCount);
}
