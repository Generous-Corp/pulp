// CSS gradient dithering on the live GPU backend (Dawn + Skia Graphite).
//
// The raster proof lives in test_css_gradient_dither.cpp. Graphite builds its
// own pipelines from the paint, so a dither flag the raster backend honours is
// not evidence that Graphite does; this file paints the same View on an
// offscreen Graphite surface and reads the frame back. Soft-skips without an
// adapter.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/css_gradient.hpp>
#include <pulp/view/view.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>

using namespace pulp;

namespace {

constexpr uint32_t kW = 200;
constexpr uint32_t kH = 600;

int non_uniform_rows(const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
    int rows = 0;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = &px[static_cast<size_t>(y) * w * 4];
        for (uint32_t x = 1; x < w; ++x) {
            const uint8_t* p = row + x * 4;
            if (p[0] != row[0] || p[1] != row[1] || p[2] != row[2]) {
                ++rows;
                break;
            }
        }
    }
    return rows;
}

} // namespace

TEST_CASE("CSS linear gradient background is dithered on Graphite",
          "[gpu][skia][gradient][dither]") {
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

    view::View v;
    v.set_bounds({0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)});
    REQUIRE(view::apply_css_background_gradient(v, "linear-gradient(#101418, #161c22)"));

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

    // The green backdrop must be fully covered, and the gradient's ends must be
    // its CSS stops, so the count below reads this gradient and nothing else.
    // Channel order is not assumed: the two stops differ in every channel.
    auto has_green_backdrop = [&](const uint8_t* p) { return p[1] > 200; };
    int backdrop = 0;
    for (size_t i = 0; i < px.size(); i += 4)
        if (has_green_backdrop(&px[i]))
            ++backdrop;
    INFO("backdrop pixels: " << backdrop);
    REQUIRE(backdrop == 0);
    const uint8_t* top = &px[0];
    const uint8_t* bottom = &px[(static_cast<size_t>(kH - 1) * kW) * 4];
    INFO("top " << int(top[0]) << "," << int(top[1]) << "," << int(top[2]) << " bottom "
                << int(bottom[0]) << "," << int(bottom[1]) << "," << int(bottom[2]));
    CHECK(top[1] >= 0x13);
    CHECK(top[1] <= 0x15);
    CHECK(bottom[1] >= 0x1b);
    CHECK(bottom[1] <= 0x1d);

    const int rows = non_uniform_rows(px, pw, ph);
    INFO("non-uniform rows on Graphite: " << rows << " of " << kH);
    CHECK(rows >= static_cast<int>(kH) / 4);
}

#endif // PULP_HAS_SKIA && __APPLE__
