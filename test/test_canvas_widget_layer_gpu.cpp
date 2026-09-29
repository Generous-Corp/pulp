// CanvasWidget's per-canvas isolation layer on a live offscreen Dawn + Skia
// Graphite surface: skipping it for a source-over stream leaves the pixels
// unchanged, and what it costs in GPU time.
//
// Two full-window canvases at 2x (the shape of a scripted plugin editor) paint
// the same source-over scene, once as the stream is (no layer) and once with
// an empty clearRect appended, which forces the layer and changes no pixel.
// The GPU time comes from Graphite's kElapsedTime stats over Dawn timestamp
// queries, so it measures the recording's GPU work, not CPU submission.
//
// The timing case is tagged [bench]: it prints its numbers and only asserts
// that timing was available and both modes produced samples. Both cases
// soft-skip without an adapter (the timing case also without timestamp
// queries).

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/canvas_widget.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <vector>

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#endif

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)

using namespace pulp;
using pulp::view::CanvasDrawCmd;
using pulp::view::CanvasWidget;

namespace {

constexpr uint32_t kLogicalW = 1320;
constexpr uint32_t kLogicalH = 860;
constexpr float kScale = 2.0f;

void add_editor_scene(CanvasWidget& cw) {
    using T = CanvasDrawCmd::Type;
    auto rect = [&](float x, float y, float w, float h, canvas::Color c) {
        CanvasDrawCmd cmd;
        cmd.type = T::fill_rect;
        cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
        cmd.color = c;
        cw.add_command(cmd);
    };
    for (int g = 0; g <= 16; ++g)
        rect(0, g * 50.0f, kLogicalW, 1, canvas::Color::rgba8(255, 255, 255, 20));
    for (int b = 0; b < 64; ++b) {
        const float h = 60.0f + static_cast<float>((b * 37) % 500);
        rect(b * 20.0f + 10, 820 - h, 14, h, canvas::Color::rgba8(58, 160, 255, 255));
    }
    CanvasDrawCmd glow_color;
    glow_color.type = T::set_fill_color;
    glow_color.color = canvas::Color::rgba8(58, 160, 255, 90);
    cw.add_command(glow_color);
    for (int k = 0; k < 64; ++k) {
        CanvasDrawCmd begin; begin.type = T::begin_path; cw.add_command(begin);
        CanvasDrawCmd arc;
        arc.type = T::path_arc;
        arc.x = k * 20.0f + 17;
        arc.y = 820 - (60.0f + static_cast<float>((k * 37) % 500));
        arc.extra = 12;
        arc.x2 = 0;
        arc.y2 = 6.2831853f;
        cw.add_command(arc);
        CanvasDrawCmd fill; fill.type = T::fill_path; cw.add_command(fill);
    }
}

struct Samples {
    std::vector<double> gpu_ms;
};

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

struct Fixture {
    std::unique_ptr<render::GpuSurface> gpu;
    std::unique_ptr<render::SkiaSurface> skia;
};

Fixture make_fixture(bool timing) {
    Fixture f;
    f.gpu = render::GpuSurface::create_dawn();
    if (!f.gpu) return f;
    render::GpuSurface::Config gpu_config{};
    gpu_config.width = static_cast<uint32_t>(kLogicalW * kScale);
    gpu_config.height = static_cast<uint32_t>(kLogicalH * kScale);
    gpu_config.native_surface_handle = nullptr;
    gpu_config.enable_gpu_timing = timing;
    if (!f.gpu->initialize(gpu_config)) { f.gpu.reset(); return f; }
    render::SkiaSurface::Config skia_config{};
    skia_config.width = kLogicalW;
    skia_config.height = kLogicalH;
    skia_config.scale_factor = kScale;
    f.skia = render::SkiaSurface::create(*f.gpu, skia_config);
    return f;
}

// Paints one frame of two stacked full-window canvases over a dark panel and
// reads it back. Returns false when the offscreen surface is unusable.
bool paint_frame(Fixture& f, CanvasWidget& a, CanvasWidget& b,
                 std::vector<uint8_t>& px, uint32_t& pw, uint32_t& ph) {
    if (!f.gpu->begin_frame()) return false;
    auto* canvas = f.skia->begin_frame();
    if (!canvas) { f.gpu->end_frame(); return false; }
    canvas->set_fill_color(canvas::Color::rgba8(16, 18, 26, 255));
    canvas->fill_rect(0, 0, kLogicalW, kLogicalH);
    a.paint(*canvas);
    b.paint(*canvas);
    f.skia->end_frame();
    f.gpu->end_frame();
    // Readback waits for the GPU, which also delivers the frame's GPU stats.
    return f.skia->read_current_rgba(px, pw, ph);
}

void make_canvases(CanvasWidget& direct_a, CanvasWidget& direct_b,
                   CanvasWidget& layered_a, CanvasWidget& layered_b) {
    for (auto* cw : {&direct_a, &direct_b, &layered_a, &layered_b}) {
        cw->set_bounds({0, 0, static_cast<float>(kLogicalW), static_cast<float>(kLogicalH)});
        add_editor_scene(*cw);
    }
    // An empty clearRect reads the backdrop in principle, so it forces the
    // layer, and in practice changes no pixel.
    for (auto* cw : {&layered_a, &layered_b}) {
        CanvasDrawCmd empty_clear;
        empty_clear.type = CanvasDrawCmd::Type::clear_rect;
        cw->add_command(empty_clear);
    }
}

} // namespace

TEST_CASE("CanvasWidget without a layer composites the same pixels on Graphite",
          "[gpu][skia][canvas_widget][backdrop-isolation]") {
    auto f = make_fixture(false);
    if (!f.gpu || !f.skia || !f.skia->is_available())
        SKIP("Dawn/Graphite unavailable on this host.");
    CanvasWidget direct_a, direct_b, layered_a, layered_b;
    make_canvases(direct_a, direct_b, layered_a, layered_b);
    REQUIRE_FALSE(direct_a.needs_backdrop_isolation());
    REQUIRE(layered_a.needs_backdrop_isolation());

    std::vector<uint8_t> direct, layered;
    uint32_t w1 = 0, h1 = 0, w2 = 0, h2 = 0;
    if (!paint_frame(f, direct_a, direct_b, direct, w1, h1))
        SKIP("GPU readback failed (no adapter).");
    REQUIRE(paint_frame(f, layered_a, layered_b, layered, w2, h2));
    REQUIRE(w1 == w2);
    REQUIRE(h1 == h2);
    REQUIRE(direct.size() == layered.size());

    int worst = 0;
    size_t lit = 0;
    for (size_t i = 0; i < direct.size(); ++i) {
        worst = std::max(worst, std::abs(int(direct[i]) - int(layered[i])));
        if (i % 4 == 2 && direct[i] > 200) ++lit;  // the blue bars
    }
    INFO("max channel delta " << worst << ", bright-blue channels " << lit);
    if (lit == 0) SKIP("Offscreen GPU composited nothing (no real adapter).");
    CHECK(worst <= 1);
}

TEST_CASE("CanvasWidget isolation layer GPU cost per frame",
          "[gpu][skia][canvas_widget][bench]") {
    auto f = make_fixture(true);
    if (!f.gpu || !f.skia || !f.skia->is_available())
        SKIP("Dawn/Graphite unavailable on this host.");
    if (!f.skia->gpu_render_timing_available())
        SKIP("GPU timestamp queries unavailable on this adapter.");

    CanvasWidget direct_a, direct_b, layered_a, layered_b;
    make_canvases(direct_a, direct_b, layered_a, layered_b);

    std::vector<uint8_t> px;
    uint32_t pw = 0, ph = 0;
    auto frame = [&](CanvasWidget& a, CanvasWidget& b) -> double {
        if (!paint_frame(f, a, b, px, pw, ph)) return -1.0;
        return f.skia->gpu_render_time_ms();
    };

    for (int i = 0; i < 10; ++i) { frame(direct_a, direct_b); frame(layered_a, layered_b); }
    Samples direct, layered;
    for (int i = 0; i < 60; ++i) {
        const double d = frame(direct_a, direct_b);
        const double l = frame(layered_a, layered_b);
        if (d > 0) direct.gpu_ms.push_back(d);
        if (l > 0) layered.gpu_ms.push_back(l);
    }
    REQUIRE(direct.gpu_ms.size() >= 30);
    REQUIRE(layered.gpu_ms.size() >= 30);
    const double d_med = median(direct.gpu_ms);
    const double l_med = median(layered.gpu_ms);
    std::printf("[canvas-layer-bench] %ux%u px, 2 canvases, median GPU ms/frame: "
                "with layers %.3f, without %.3f (n=%zu/%zu)\n",
                pw, ph, l_med, d_med, layered.gpu_ms.size(), direct.gpu_ms.size());
    CHECK(d_med > 0.0);
    CHECK(l_med > 0.0);
}

#endif // PULP_HAS_SKIA && __APPLE__
