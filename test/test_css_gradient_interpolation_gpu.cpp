// CSS gradient interpolation methods on the live GPU backend (Dawn + Skia
// Graphite).
//
// The raster proof lives in test_css_gradient_interpolation.cpp. Graphite
// compiles its own gradient pipelines, including the colour-space conversion,
// so a space the raster backend honours is not evidence that Graphite does.
// This paints the same Views on an offscreen Graphite surface, reads the frame
// back, and holds the midpoint to Chrome's. Soft-skips without an adapter.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/css_gradient.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>

using namespace pulp;

namespace {

constexpr uint32_t kW = 101;
constexpr uint32_t kH = 16;
constexpr double kChromeTolerance = 3.0; // as in the raster suite

struct ChromeMidpoint {
    const char* css;
    std::array<double, 3> rgb;
};

// chrome_gradient_oracle.py --colors, Chrome 156, column 50.
const ChromeMidpoint kCases[] = {
    {"linear-gradient(to right, #ff0000, #0000ff)", {127.5, 0.0, 127.5}},
    {"linear-gradient(to right in oklab, #ff0000, #0000ff)", {140.25, 83.0, 162.25}},
    {"linear-gradient(to right in oklch, #ff0000, #0000ff)", {186.0, 0.0, 193.75}},
    {"linear-gradient(to right in oklch longer hue, #ff0000, #0000ff)", {0.0, 146.75, 0.0}},
    {"linear-gradient(to right in oklch increasing hue, #ff0000, #0000ff)", {0.0, 146.75, 0.0}},
    {"linear-gradient(to right in oklch decreasing hue, #ff0000, #0000ff)", {186.0, 0.0, 193.75}},
    {"linear-gradient(to right in hsl, #ffffff, #0000ff)", {159.25, 159.25, 223.0}},
};

} // namespace

TEST_CASE("Graphite gradient midpoints match Chrome for each interpolation method",
          "[gpu][skia][gradient][interpolation]") {
    auto gpu = render::GpuSurface::create_dawn();
    if (!gpu)
        SKIP("Dawn unavailable on this host.");
    render::GpuSurface::Config gpu_config{};
    gpu_config.width = kW;
    gpu_config.height = kH;
    gpu_config.native_surface_handle = nullptr;
    if (!gpu->initialize(gpu_config))
        SKIP("Dawn device could not be initialized.");
    render::SkiaSurface::Config skia_config{};
    skia_config.width = kW;
    skia_config.height = kH;
    skia_config.scale_factor = 1.0f;
    auto skia = render::SkiaSurface::create(*gpu, skia_config);
    if (!skia || !skia->is_available())
        SKIP("Graphite surface unavailable.");

    bool bgra = false;
    for (const auto& c : kCases) {
        view::View v;
        v.set_bounds({0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)});
        REQUIRE(view::apply_css_background_gradient(v, c.css));

        REQUIRE(gpu->begin_frame());
        auto* canvas = skia->begin_frame();
        REQUIRE(canvas != nullptr);
        canvas->set_fill_color(canvas::Color::rgba8(0, 255, 0, 255));
        canvas->fill_rect(0, 0, static_cast<float>(kW), static_cast<float>(kH));
        v.paint_all(*canvas);
        skia->end_frame();
        gpu->end_frame();

        std::vector<uint8_t> px;
        uint32_t pw = 0, ph = 0;
        REQUIRE(skia->read_current_rgba(px, pw, ph));
        REQUIRE(pw == kW);
        REQUIRE(ph == kH);
        REQUIRE(px.size() == static_cast<size_t>(kW) * kH * 4);

        // The first case's left edge is its red stop, which fixes the channel
        // order whatever the surface's format; later cases may start on white.
        if (&c == &kCases[0])
            bgra = px[2] > px[0];
        std::array<double, 3> got{};
        for (uint32_t y = 0; y < kH; ++y) {
            const uint8_t* p = &px[(static_cast<size_t>(y) * kW + kW / 2) * 4];
            got[0] += bgra ? p[2] : p[0];
            got[1] += p[1];
            got[2] += bgra ? p[0] : p[2];
        }
        double worst = 0.0;
        for (int i = 0; i < 3; ++i) {
            got[i] /= kH;
            worst = std::max(worst, std::fabs(got[i] - c.rgb[i]));
        }
        INFO(c.css << "\n  Graphite (" << got[0] << ", " << got[1] << ", " << got[2] << ") Chrome ("
                   << c.rgb[0] << ", " << c.rgb[1] << ", " << c.rgb[2] << ")");
        CHECK(worst <= kChromeTolerance);
    }
}

#endif // PULP_HAS_SKIA && __APPLE__
