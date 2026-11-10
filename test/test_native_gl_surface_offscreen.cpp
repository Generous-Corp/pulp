// GlSurface: request selection, the client-thread context rule, drawing and
// reading back on that thread, independent surfaces, and two-phase teardown,
// on the offscreen backend (CGL on macOS).

#define GL_SILENCE_DEPRECATION 1

#include <pulp/native/gl_surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace pulp::native;

namespace {

// Clear the surface to `rgb` on the calling thread and read one pixel back.
std::array<std::uint8_t, 4> clear_and_read(GlSurface& surface, float r, float g, float b) {
    std::array<std::uint8_t, 4> pixel{0, 0, 0, 0};
    GlContext& gl = surface.context();
    if (!gl.make_current())
        return pixel;
    glBindFramebuffer(GL_FRAMEBUFFER, gl.framebuffer());
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    gl.swap_buffers();
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    gl.release_current();
    return pixel;
}

bool near(std::uint8_t value, int expected) {
    return value >= expected - 2 && value <= expected + 2;
}

} // namespace

TEST_CASE("the first request a context honours is chosen", "[native][gl-surface]") {
    const std::vector<GlContextRequest> list{
        {4, 6, GlProfile::core, 0},
        {4, 1, GlProfile::core, 0},
        {3, 0, GlProfile::compatibility, 0},
        {2, 1, GlProfile::any, 0},
    };
    REQUIRE(first_honoured_request(list, 4, 6, GlProfile::core) == 0u);
    REQUIRE(first_honoured_request(list, 4, 5, GlProfile::core) == 1u);  // never "4.6"
    REQUIRE(first_honoured_request(list, 4, 1, GlProfile::core) == 1u);
    REQUIRE(first_honoured_request(list, 4, 5, GlProfile::compatibility) == 2u);
    REQUIRE(first_honoured_request(list, 2, 1, GlProfile::any) == 3u);
    REQUIRE_FALSE(first_honoured_request(list, 1, 5, GlProfile::any).has_value());
    REQUIRE(is_software_renderer("llvmpipe (LLVM 17.0.6, 256 bits)"));
    REQUIRE(is_software_renderer("Apple Software Renderer"));
    REQUIRE(is_software_renderer("GDI Generic"));
    REQUIRE_FALSE(is_software_renderer("Apple M1 Pro"));
}

TEST_CASE("a windowed GL surface is not offered on macOS", "[native][gl-surface]") {
    REQUIRE(GlSurface::create() == nullptr);
    REQUIRE(GlSurface::create_offscreen(0, 16) == nullptr);
}

TEST_CASE("a request above the driver falls through to the next entry",
          "[native][gl-surface][offscreen]") {
    GlSurfaceOptions options;
    options.requests = {{4, 6, GlProfile::core, 0},  // macOS stops at 4.1
                        {4, 1, GlProfile::core, 0},
                        {2, 1, GlProfile::any, 0}};
    auto surface = GlSurface::create_offscreen(64, 32, options);
    REQUIRE(surface != nullptr);
    REQUIRE(surface->offscreen());
    REQUIRE_FALSE(surface->attach(reinterpret_cast<void*>(1)));

    GlContextReport report;
    bool made = false;
    std::thread render([&] {
        made = surface->context().make_current();
        report = surface->context().report();
        surface->context().release_current();
    });
    render.join();
    REQUIRE(made);
    INFO("GL_VERSION " << report.version << ", GL_RENDERER " << report.renderer);
    REQUIRE(report.request_index == 1);
    REQUIRE(report.major == 4);
    REQUIRE(report.minor == 1);
    REQUIRE(report.profile == GlProfile::core);
    REQUIRE_FALSE(report.renderer.empty());
    REQUIRE(report.max_texture_size >= 2048);

    const auto g = surface->geometry();
    REQUIRE(g.drawable_width == 64);
    REQUIRE(g.drawable_height == 32);
    REQUIRE_FALSE(g.in_window);
}

TEST_CASE("the context is created and bound only on the client's thread",
          "[native][gl-surface][offscreen]") {
    REQUIRE(CGLGetCurrentContext() == nullptr);
    auto surface = GlSurface::create_offscreen(16, 16);
    REQUIRE(surface != nullptr);
    CGLContextObj on_render_thread = nullptr;
    CGLContextObj before_on_render_thread = reinterpret_cast<CGLContextObj>(1);
    std::thread render([&] {
        before_on_render_thread = CGLGetCurrentContext();
        if (surface->context().make_current())
            on_render_thread = CGLGetCurrentContext();
        surface->context().release_current();
    });
    render.join();
    REQUIRE(before_on_render_thread == nullptr);
    REQUIRE(on_render_thread != nullptr);
    // Creating, binding and releasing on the render thread left this thread
    // untouched.
    REQUIRE(CGLGetCurrentContext() == nullptr);
}

TEST_CASE("a client thread draws and reads back its own frame",
          "[native][gl-surface][offscreen]") {
    auto surface = GlSurface::create_offscreen(8, 8);
    REQUIRE(surface != nullptr);
    std::array<std::uint8_t, 4> pixel{};
    std::thread render([&] { pixel = clear_and_read(*surface, 0.25f, 0.5f, 0.75f); });
    render.join();
    REQUIRE(near(pixel[0], 64));
    REQUIRE(near(pixel[1], 128));
    REQUIRE(near(pixel[2], 191));
    REQUIRE(pixel[3] == 255);
}

TEST_CASE("two surfaces on two threads keep their own frames",
          "[native][gl-surface][offscreen]") {
    auto a = GlSurface::create_offscreen(8, 8);
    auto b = GlSurface::create_offscreen(8, 8);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    std::array<std::uint8_t, 4> pa{}, pb{};
    std::thread ta([&] { pa = clear_and_read(*a, 1.0f, 0.0f, 0.0f); });
    std::thread tb([&] { pb = clear_and_read(*b, 0.0f, 0.0f, 1.0f); });
    ta.join();
    tb.join();
    REQUIRE(near(pa[0], 255));
    REQUIRE(near(pa[2], 0));
    REQUIRE(near(pb[0], 0));
    REQUIRE(near(pb[2], 255));
}

TEST_CASE("destruction runs the release handler once, with the context refused",
          "[native][gl-surface][offscreen]") {
    auto surface = GlSurface::create_offscreen(8, 8);
    REQUIRE(surface != nullptr);
    GlSurface* s = surface.get();  // `surface` is null while it is destroyed
    std::atomic<bool> stop{false};
    std::atomic<int> frames{0};
    std::thread render([s, &stop, &frames] {
        GlContext& gl = s->context();
        while (!stop.load()) {
            if (gl.make_current() && gl.swap_buffers())
                ++frames;
            gl.release_current();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    while (frames.load() < 3)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    int releases = 0;
    bool refused_during_release = false;
    s->set_release_handler([&] {
        ++releases;
        stop = true;
        render.join();
        // Teardown has started: the context is refused from here on.
        refused_during_release = !s->context().make_current();
    });
    surface.reset();  // destruction is a teardown
    REQUIRE(releases == 1);
    REQUIRE(refused_during_release);
}

TEST_CASE("a surface refuses the context once teardown has started",
          "[native][gl-surface][offscreen]") {
    auto surface = GlSurface::create_offscreen(8, 8);
    REQUIRE(surface != nullptr);
    GlSurface* s = surface.get();
    bool made_during_release = true;
    bool swapped_during_release = true;
    s->set_release_handler([&] {
        std::thread probe([&] {
            made_during_release = s->context().make_current();
            swapped_during_release = s->context().swap_buffers();
        });
        probe.join();
    });
    surface.reset();
    REQUIRE_FALSE(made_during_release);
    REQUIRE_FALSE(swapped_during_release);
}
