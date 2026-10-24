// A presentable GpuSurface configures its swapchain in begin_frame(), not in
// resize(). These cases run against a real CAMetalLayer so the counted
// configures are Dawn's own surface configures and the acquired texture is the
// one a frame would present.

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <QuartzCore/QuartzCore.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/render/gpu_surface.hpp>

#include "webgpu/webgpu_cpp.h"

#include <cstdint>
#include <memory>

using namespace pulp::render;

namespace {

constexpr const char* kNoPresentableSurface =
    "GpuSurface would not initialize on a CAMetalLayer (no Metal adapter)";

struct LayerSurface {
    CAMetalLayer* layer = nil;
    std::unique_ptr<GpuSurface> gpu;

    bool open(uint32_t width, uint32_t height) {
        layer = [CAMetalLayer layer];
        layer.drawableSize = CGSizeMake(width, height);
        gpu = GpuSurface::create_dawn();
        if (!gpu)
            return false;
        GpuSurface::Config config{};
        config.width = width;
        config.height = height;
        config.native_surface_handle = (__bridge void*)layer;
        return gpu->initialize(config) && gpu->has_surface();
    }

    uint64_t configures() const {
        return gpu->swapchain_stats().configure_count;
    }

    // The size of the texture this frame acquired, or 0x0 when it acquired none.
    bool frame(uint32_t* width, uint32_t* height) {
        *width = 0;
        *height = 0;
        if (!gpu->begin_frame())
            return false;
        auto* texture = static_cast<wgpu::Texture*>(gpu->current_texture_handle());
        if (texture && *texture) {
            *width = texture->GetWidth();
            *height = texture->GetHeight();
        }
        gpu->end_frame();
        return true;
    }
};

} // namespace

TEST_CASE("Presentable GpuSurface configures once for many resizes before a frame",
          "[render][gpu][swapchain][macos]") {
    @autoreleasepool {
        LayerSurface s;
        if (!s.open(400, 300))
            SKIP(kNoPresentableSurface);
        REQUIRE(s.configures() == 1); // the initial configure

        uint32_t w = 0, h = 0;
        REQUIRE(s.frame(&w, &h));
        REQUIRE(s.configures() == 1);

        // A live resize: sixty size events land before the next frame.
        for (uint32_t i = 1; i <= 60; ++i)
            s.gpu->resize(400 + 4 * i, 300 + 2 * i);
        REQUIRE(s.configures() == 1);
        REQUIRE(s.gpu->width() == 640);
        REQUIRE(s.gpu->height() == 420);

        REQUIRE(s.frame(&w, &h));
        REQUIRE(s.configures() == 2);
    }
}

TEST_CASE("Presentable GpuSurface presents a resize at the new size on the next frame",
          "[render][gpu][swapchain][macos]") {
    @autoreleasepool {
        LayerSurface s;
        if (!s.open(400, 300))
            SKIP(kNoPresentableSurface);

        uint32_t w = 0, h = 0;
        REQUIRE(s.frame(&w, &h));
        REQUIRE(w == 400);
        REQUIRE(h == 300);

        s.gpu->resize(800, 500);
        REQUIRE(s.frame(&w, &h));
        CHECK(w == 800);
        CHECK(h == 500);
        CHECK(s.layer.drawableSize.width == 800);
        CHECK(s.layer.drawableSize.height == 500);
    }
}

TEST_CASE("Presentable GpuSurface resize to the same size configures nothing",
          "[render][gpu][swapchain][macos]") {
    @autoreleasepool {
        LayerSurface s;
        if (!s.open(400, 300))
            SKIP(kNoPresentableSurface);
        uint32_t w = 0, h = 0;
        REQUIRE(s.frame(&w, &h));
        const uint64_t before = s.configures();

        s.gpu->resize(400, 300);
        REQUIRE(s.frame(&w, &h));
        REQUIRE(s.configures() == before);
        REQUIRE(w == 400);
    }
}

TEST_CASE("Presentable GpuSurface at a zero size skips the frame and configures nothing",
          "[render][gpu][swapchain][macos]") {
    @autoreleasepool {
        LayerSurface s;
        if (!s.open(400, 300))
            SKIP(kNoPresentableSurface);
        uint32_t w = 0, h = 0;
        REQUIRE(s.frame(&w, &h));
        const uint64_t before = s.configures();

        s.gpu->resize(0, 0);
        REQUIRE_FALSE(s.frame(&w, &h));
        s.gpu->resize(400, 0);
        REQUIRE_FALSE(s.frame(&w, &h));
        REQUIRE(s.configures() == before);

        s.gpu->resize(640, 480);
        REQUIRE(s.frame(&w, &h));
        REQUIRE(s.configures() == before + 1);
        CHECK(w == 640);
        CHECK(h == 480);
    }
}

TEST_CASE("Presentable GpuSurface created at a zero size configures on its first sized frame",
          "[render][gpu][swapchain][macos]") {
    @autoreleasepool {
        LayerSurface s;
        if (!s.open(0, 0))
            SKIP(kNoPresentableSurface);
        REQUIRE(s.configures() == 0);

        uint32_t w = 0, h = 0;
        REQUIRE_FALSE(s.frame(&w, &h));
        s.gpu->resize(320, 240);
        REQUIRE(s.frame(&w, &h));
        REQUIRE(s.configures() == 1);
        CHECK(w == 320);
        CHECK(h == 240);
    }
}

#endif // TARGET_OS_OSX
