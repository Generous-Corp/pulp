// ParagraphCache on Skia Graphite (Dawn). A cached paragraph is drawn into a
// different SkiaCanvas each frame, through a recorder rather than a raster
// SkCanvas; each frame must read back the same bytes as the same frame drawn
// with the cache disabled. Soft-skips when no Dawn adapter is available.

#include <catch2/catch_test_macros.hpp>

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)

#include <pulp/canvas/paragraph_cache.hpp>
#include <pulp/canvas/resource_counters.hpp>
#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::canvas;

namespace {

constexpr std::uint32_t kWidth = 240;
constexpr std::uint32_t kHeight = 48;

struct Gpu {
    std::unique_ptr<pulp::render::GpuSurface> gpu;
    std::unique_ptr<pulp::render::SkiaSurface> skia;
    bool ready() const {
        return gpu && skia && skia->is_available();
    }
};

Gpu make_gpu() {
    Gpu g;
    g.gpu = pulp::render::GpuSurface::create_dawn();
    if (!g.gpu)
        return g;
    pulp::render::GpuSurface::Config config{};
    config.width = kWidth;
    config.height = kHeight;
    config.native_surface_handle = nullptr;
    if (!g.gpu->initialize(config)) {
        g.gpu.reset();
        return g;
    }
    pulp::render::SkiaSurface::Config skia_config{};
    skia_config.width = kWidth;
    skia_config.height = kHeight;
    skia_config.scale_factor = 1.0f;
    g.skia = pulp::render::SkiaSurface::create(*g.gpu, skia_config);
    return g;
}

using Draw = std::function<void(Canvas&)>;

std::vector<std::uint8_t> frame(Gpu& g, const Draw& draw) {
    std::vector<std::uint8_t> pixels;
    if (!g.gpu->begin_frame())
        return pixels;
    auto* canvas = g.skia->begin_frame();
    if (!canvas) {
        g.gpu->end_frame();
        return pixels;
    }
    canvas->set_fill_color(Color::rgba8(255, 255, 255));
    canvas->fill_rect(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight));
    canvas->set_fill_color(Color::rgba8(0, 0, 0));
    draw(*canvas);
    g.skia->end_frame();
    std::uint32_t w = 0, h = 0;
    if (!g.skia->read_current_rgba(pixels, w, h) || w != kWidth || h != kHeight)
        pixels.clear();
    g.gpu->end_frame();
    return pixels;
}

Draw label(std::string text, float size, float spacing, Color color, float max_width = 0.0f) {
    return [=](Canvas& c) {
        c.set_font_full("Inter", size, 400, 0, spacing);
        c.set_fill_color(color);
        if (max_width > 0.0f)
            c.fill_text_with_max_width(text, 8.0f, 32.0f, max_width);
        else
            c.fill_text(text, 8.0f, 32.0f);
    };
}

} // namespace

TEST_CASE("Paragraph cache on Graphite reads back the uncached frames",
          "[canvas][paragraph-cache][gpu]") {
    auto g = make_gpu();
    if (!g.ready())
        SKIP("Dawn/Graphite unavailable on this host");

    const Color ink = Color::rgba8(20, 20, 20);
    const std::vector<Draw> frames = {
        label("Output 0.0 dB", 16.0f, 0.5f, ink),
        label("Output 0.0 dB", 16.0f, 0.5f, ink),                        // repaint
        label("Output -3.0 dB", 16.0f, 0.5f, ink),                       // text change
        label("Output -3.0 dB", 16.0f, 0.5f, Color::rgba8(200, 40, 40)), // paint change
        label("Output -3.0 dB", 16.0f, 0.5f, ink, 70.0f),                // width change
        label("Output -3.0 dB", 18.0f, 0.5f, ink),                       // size change
    };

    auto& cache = ParagraphCache::instance();
    cache.set_enabled(false);
    std::vector<std::vector<std::uint8_t>> want;
    for (const auto& draw : frames)
        want.push_back(frame(g, draw));
    cache.set_enabled(true);
    cache.clear();
    for (const auto& w : want)
        REQUIRE(w.size() == static_cast<std::size_t>(kWidth) * kHeight * 4u);
    REQUIRE(want[1] == want[0]);
    REQUIRE(want[2] != want[1]);
    REQUIRE(want[3] != want[2]);
    REQUIRE(want[4] != want[3]);
    REQUIRE(want[5] != want[4]);

    const auto builds_before = resource_counters().paragraph_builds;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        INFO("frame " << i);
        CHECK(frame(g, frames[i]) == want[i]);
    }
    // Shaped once per distinct run: frames 0, 2 and 5. The width squeeze
    // measures the same run it then draws.
    CHECK(resource_counters().paragraph_builds - builds_before == 3);
}

#endif
