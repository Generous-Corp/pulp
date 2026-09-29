// SkiaSurface's count of frames submitted to the GPU but not finished, on a
// live offscreen Dawn + Graphite surface. Soft-skips without an adapter.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>

using namespace pulp;

TEST_CASE("SkiaSurface frames in flight return to zero once the GPU finishes",
          "[gpu][skia][gpu-frames-in-flight]") {
    auto gpu = render::GpuSurface::create_dawn();
    if (!gpu) SKIP("Dawn unavailable on this host.");
    render::GpuSurface::Config gpu_config{};
    gpu_config.width = 256;
    gpu_config.height = 128;
    if (!gpu->initialize(gpu_config)) SKIP("Dawn device initialization failed.");
    render::SkiaSurface::Config skia_config{};
    skia_config.width = 256;
    skia_config.height = 128;
    auto skia = render::SkiaSurface::create(*gpu, skia_config);
    if (!skia || !skia->is_available()) SKIP("Skia Graphite unavailable.");

    REQUIRE(skia->gpu_frames_in_flight() == 0);
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(gpu->begin_frame());
        auto* canvas = skia->begin_frame();
        REQUIRE(canvas != nullptr);
        canvas->set_fill_color(canvas::Color::rgba8(200, 40, 40));
        canvas->fill_rect(0, 0, 256, 128);
        skia->end_frame();
        gpu->end_frame();
        const int in_flight = skia->gpu_frames_in_flight();
        INFO("frame " << frame << " in flight " << in_flight);
        CHECK(in_flight >= 0);
        CHECK(in_flight <= frame + 1);
    }
    // A readback waits for the GPU and pumps completion, which delivers every
    // finished callback.
    if (!skia->read_current_rgba(px, w, h)) SKIP("GPU readback failed (no adapter).");
    CHECK(skia->gpu_frames_in_flight() == 0);
}
#endif
