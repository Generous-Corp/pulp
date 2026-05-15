#include <catch2/catch_test_macros.hpp>
#include <pulp/render/sdl3_surface.hpp>

using namespace pulp;

TEST_CASE("SDL3 surface info validity follows backend state - issue-646",
          "[render][sdl3][issue-646]") {
    render::Sdl3SurfaceInfo info;
    REQUIRE_FALSE(info.is_valid());

    info.x11_display = reinterpret_cast<void*>(0x1);
    info.x11_window = 42;
    REQUIRE_FALSE(info.is_valid());

    info.backend = render::Sdl3SurfaceInfo::Backend::vulkan_x11;
    REQUIRE(info.is_valid());

    info = {};
    info.hwnd = reinterpret_cast<void*>(0x2);
    info.hinstance = reinterpret_cast<void*>(0x3);
    info.backend = render::Sdl3SurfaceInfo::Backend::d3d12;
    REQUIRE(info.is_valid());
}

TEST_CASE("SDL3 surface extraction rejects null windows - issue-646",
          "[render][sdl3][issue-646]") {
    auto info = render::extract_sdl3_surface(nullptr);
    REQUIRE_FALSE(info.is_valid());
    REQUIRE(info.backend == render::Sdl3SurfaceInfo::Backend::none);
    REQUIRE(info.metal_layer == nullptr);
    REQUIRE(info.hwnd == nullptr);
    REQUIRE(info.hinstance == nullptr);
    REQUIRE(info.x11_display == nullptr);
    REQUIRE(info.x11_window == 0);
    REQUIRE(info.wayland_display == nullptr);
    REQUIRE(info.wayland_surface == nullptr);
}
