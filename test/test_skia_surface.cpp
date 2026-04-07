#include <catch2/catch_test_macros.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/render/gpu_surface.hpp>

using namespace pulp::render;

TEST_CASE("SkiaSurface requires initialized GpuSurface", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) return;

    // Don't initialize GpuSurface — SkiaSurface should fail gracefully
    SkiaSurface::Config config{};
    config.width = 400;
    config.height = 300;

    auto skia = SkiaSurface::create(*gpu, config);
    // Should be null because GpuSurface has no Dawn handles
    REQUIRE(skia == nullptr);
#else
    REQUIRE(true);  // Skia not compiled in
#endif
}

TEST_CASE("SkiaSurface uses shared GpuSurface device", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) return;

    GpuSurface::Config gpu_config{};
    gpu_config.width = 400;
    gpu_config.height = 300;

    if (!gpu->initialize(gpu_config)) return;  // no GPU adapter

    SkiaSurface::Config config{};
    config.width = 400;
    config.height = 300;
    config.scale_factor = 1.0f;

    auto skia = SkiaSurface::create(*gpu, config);
    if (!skia) return;  // Graphite context creation failed

    REQUIRE(skia->is_available());
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface offscreen frame cycle", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) return;

    GpuSurface::Config gpu_config{};
    gpu_config.width = 200;
    gpu_config.height = 150;

    if (!gpu->initialize(gpu_config)) return;

    SkiaSurface::Config config{};
    config.width = 200;
    config.height = 150;

    auto skia = SkiaSurface::create(*gpu, config);
    if (!skia || !skia->is_available()) return;

    // Frame cycle: GpuSurface brackets the frame, SkiaSurface draws
    REQUIRE(gpu->begin_frame());

    auto* canvas = skia->begin_frame();
    REQUIRE(canvas != nullptr);

    // Draw something
    canvas->set_fill_color(pulp::canvas::Color::rgba8(255, 0, 0));
    canvas->fill_rect(0, 0, 200, 150);

    skia->end_frame();  // submit Graphite recording
    gpu->end_frame();   // present (no-op in offscreen mode)
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface multiple frame cycles", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) return;

    GpuSurface::Config gpu_config{};
    gpu_config.width = 100;
    gpu_config.height = 100;

    if (!gpu->initialize(gpu_config)) return;

    auto skia = SkiaSurface::create(*gpu, {.width = 100, .height = 100});
    if (!skia || !skia->is_available()) return;

    // Multiple frames — verify no state leaks
    for (int i = 0; i < 5; ++i) {
        REQUIRE(gpu->begin_frame());
        auto* canvas = skia->begin_frame();
        REQUIRE(canvas != nullptr);
        canvas->fill_rect(0, 0, 100, 100);
        skia->end_frame();
        gpu->end_frame();
    }
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface resize", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) return;

    GpuSurface::Config gpu_config{};
    gpu_config.width = 200;
    gpu_config.height = 200;

    if (!gpu->initialize(gpu_config)) return;

    auto skia = SkiaSurface::create(*gpu, {.width = 200, .height = 200});
    if (!skia || !skia->is_available()) return;

    // Resize both GpuSurface and SkiaSurface
    gpu->resize(400, 300);
    skia->resize(400, 300);

    // Should still work after resize
    REQUIRE(gpu->begin_frame());
    auto* canvas = skia->begin_frame();
    REQUIRE(canvas != nullptr);
    canvas->fill_rect(0, 0, 400, 300);
    skia->end_frame();
    gpu->end_frame();
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface null without Skia", "[render][skia]") {
#ifndef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (gpu) {
        auto skia = SkiaSurface::create(*gpu, {});
        REQUIRE(skia == nullptr);
    }
#else
    REQUIRE(true);  // Skia is available, tested elsewhere
#endif
}
