#include "detail/shared_io_arena.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("shared IO arena production source links through pulp gpu audio",
          "[gpu_audio][shared_io][link]") {
    pulp::gpu_audio::detail::SharedIoArena arena;
    CHECK_FALSE(arena.prepared());
}
