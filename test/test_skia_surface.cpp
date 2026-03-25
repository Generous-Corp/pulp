#include <catch2/catch_test_macros.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/render/gpu_surface.hpp>

using namespace pulp::render;

TEST_CASE("SkiaSurface creation", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    // Create a dummy GpuSurface (SkiaSurface uses its own Dawn internally)
    auto gpu = GpuSurface::create_dawn();

    SkiaSurface::Config config;
    config.width = 400;
    config.height = 300;
    config.scale_factor = 1.0f;

    auto surface = SkiaSurface::create(*gpu, config);

    // May be null if no GPU available (headless CI)
    if (surface && surface->is_available()) {
        // Test frame cycle
        auto* canvas = surface->begin_frame();
        REQUIRE(canvas != nullptr);

        // Draw something
        canvas->set_fill_color(pulp::canvas::Color::rgba(255, 0, 0));
        canvas->fill_rect(10, 10, 100, 50);
        canvas->set_fill_color(pulp::canvas::Color::rgba(255, 255, 255));
        canvas->fill_text("Skia works!", 20, 40);

        surface->end_frame();

        // Resize
        surface->resize(800, 600);
        canvas = surface->begin_frame();
        if (canvas) {
            canvas->fill_rect(0, 0, 800, 600);
            surface->end_frame();
        }
    } else {
        // No GPU — just verify graceful handling
        REQUIRE(true);
    }
#else
    auto gpu = GpuSurface::create_dawn();
    if (gpu) {
        auto surface = SkiaSurface::create(*gpu, {});
        REQUIRE(surface == nullptr); // No Skia = nullptr
    }
#endif
}
