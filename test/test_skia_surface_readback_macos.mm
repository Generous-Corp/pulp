// SkiaSurface reads a frame back at the size of the surface it drew into. A
// host may size the swapchain from the platform's physical client rect, which
// at a fractional scale is a pixel more than logical × scale (1001 px is 667
// logical at 1.5x, and 667 × 1.5 is 1000); the readback must not crop it.

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <QuartzCore/QuartzCore.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>

#include <cstdint>
#include <vector>

using namespace pulp::render;

TEST_CASE("SkiaSurface reads back a presentable frame at the swapchain's own size",
          "[render][skia][hidpi][macos]") {
    @autoreleasepool {
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.drawableSize = CGSizeMake(1001, 751);
        auto gpu = GpuSurface::create_dawn();
        if (!gpu)
            SKIP("no Dawn backend");
        GpuSurface::Config config{};
        config.width = 1001;
        config.height = 751;
        config.native_surface_handle = (__bridge void*)layer;
        if (!gpu->initialize(config) || !gpu->has_surface())
            SKIP("GpuSurface would not initialize on a CAMetalLayer");
        auto skia = SkiaSurface::create(*gpu, {.width = 667, .height = 501, .scale_factor = 1.5f});
        if (!skia || !skia->is_available())
            SKIP("no Graphite context");

        REQUIRE(gpu->begin_frame());
        auto* canvas = skia->begin_frame();
        REQUIRE(canvas != nullptr);
        canvas->fill_rect(0, 0, 1001.0f / 1.5f, 751.0f / 1.5f);
        std::vector<uint8_t> pixels;
        uint32_t w = 0, h = 0;
        REQUIRE(skia->read_current_rgba(pixels, w, h));
        CHECK(w == 1001);
        CHECK(h == 751);
        CHECK(pixels.size() == static_cast<size_t>(w) * h * 4);
        skia->end_frame();
        gpu->end_frame();
    }
}

#endif // TARGET_OS_OSX
