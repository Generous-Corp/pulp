#pragma once

// Offscreen raster helpers shared by the component tests.

#include <vellum/view/screenshot.hpp>
#include <vellum/view/view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace pulp::vellum_components::testing {

struct Pixel {
    int r, g, b, a;
};

struct Frame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    Pixel at(std::uint32_t x, std::uint32_t y) const {
        REQUIRE(!pixels.empty());
        REQUIRE(x < width);
        REQUIRE(y < height);
        const auto o = (static_cast<std::size_t>(y) * width + x) * 4;
        return {pixels[o], pixels[o + 1], pixels[o + 2], pixels[o + 3]};
    }
};

inline Frame render(::vellum::view::View& view, std::uint32_t w, std::uint32_t h) {
    Frame frame;
    frame.pixels = ::vellum::view::render_to_rgba(view, w, h, 1.0f, &frame.width, &frame.height);
    return frame;
}

// PULP_VELLUM_COMPONENTS_PNG_DIR names a directory to receive rendered frames
// for review; unset (the default) writes nothing.
inline void maybe_write_png(::vellum::view::View& view, std::uint32_t w, std::uint32_t h,
                            const std::string& name) {
    if (const char* dir = std::getenv("PULP_VELLUM_COMPONENTS_PNG_DIR"))
        CHECK(::vellum::view::render_to_file(view, w, h, std::string(dir) + "/" + name, 2.0f));
}

inline bool near(const Pixel& p, int r, int g, int b, int tol = 3) {
    return std::abs(p.r - r) <= tol && std::abs(p.g - g) <= tol && std::abs(p.b - b) <= tol;
}

} // namespace pulp::vellum_components::testing
