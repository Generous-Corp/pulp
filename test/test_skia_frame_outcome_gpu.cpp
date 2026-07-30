// test_skia_frame_outcome_gpu.cpp — SkiaSurface::end_frame()'s FrameOutcome on
// a LIVE Dawn/Graphite surface (WAH-2).
//
// The unit tests in test_plugin_frame_renderer.cpp drive a fake SkiaSurface, so
// they pin the HOST's reaction to each outcome but say nothing about which
// outcome the real backend actually produces. That gap matters: hosts now gate
// damage retirement on `frame_reached_output()`, so a real surface that
// spuriously reported failure would make every editor repaint in full, forever
// — a silent performance regression no assertion would catch.
//
// The specific hazard is the CAPTURE path. read_current_rgba() flushes the open
// frame's recording mid-frame, so end_frame()'s own snap() legitimately finds
// nothing left to record. Judging success on "did we submit something just now"
// would fail every captured frame. These tests run the real thing and assert it
// does not.
//
// Offscreen surfaces report `offscreen`, not `presented` — there is no
// presentable drawable in a headless fixture, and that is a first-class success
// rather than a fallback. Distinguishing `offscreen` from `failed` is exactly
// the property under test.
//
// Gated PULP_HAS_SKIA && APPLE at compile time (CMake); soft-skips at run time
// when no Dawn adapter is available. Mirrors test_partial_repaint_gpu.cpp.
//
// Tag: [gpu][skia][wah-2]

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#endif

using namespace pulp;

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)

namespace {

constexpr uint32_t kW = 64;
constexpr uint32_t kH = 64;

struct GpuFixture {
    std::unique_ptr<render::GpuSurface> gpu;
    std::unique_ptr<render::SkiaSurface> skia;
    bool ready() const { return gpu && skia && skia->is_available(); }
};

GpuFixture make_offscreen_fixture() {
    GpuFixture f;
    f.gpu = render::GpuSurface::create_dawn();
    if (!f.gpu) return f;
    render::GpuSurface::Config gpu_config{};
    gpu_config.width = kW;
    gpu_config.height = kH;
    gpu_config.native_surface_handle = nullptr;  // headless / offscreen
    if (!f.gpu->initialize(gpu_config)) {
        f.gpu.reset();
        return f;
    }
    render::SkiaSurface::Config skia_config{};
    skia_config.width = kW;
    skia_config.height = kH;
    skia_config.scale_factor = 1.0f;
    f.skia = render::SkiaSurface::create(*f.gpu, skia_config);
    return f;
}

void paint_something(canvas::Canvas& c) {
    c.set_fill_color(canvas::Color::rgba8(200, 40, 40));
    c.fill_rect(0, 0, static_cast<float>(kW), static_cast<float>(kH));
}

}  // namespace

TEST_CASE("a plain offscreen frame reports offscreen, not failed",
          "[gpu][skia][wah-2]") {
    auto f = make_offscreen_fixture();
    if (!f.ready()) {
        SUCCEED("Dawn/Graphite unavailable on this host — frame-outcome proof skipped.");
        return;
    }

    REQUIRE(f.gpu->begin_frame());
    auto* canvas = f.skia->begin_frame();
    REQUIRE(canvas != nullptr);
    paint_something(*canvas);
    const auto outcome = f.skia->end_frame();
    f.gpu->end_frame();

    REQUIRE(outcome == render::FrameOutcome::offscreen);
    REQUIRE(render::frame_reached_output(outcome));
    REQUIRE(f.skia->last_frame_outcome() == outcome);
    // No native drawable exists here by design, so the offscreen target IS the
    // intended output.
    REQUIRE_FALSE(f.skia->has_presentable_target());
}

TEST_CASE("a captured frame still reaches its output",
          "[gpu][skia][wah-2]") {
    // read_current_rgba() flushes the recording mid-frame; end_frame()'s snap
    // then has nothing left to record. That must not read as a failed frame, or
    // every screenshot-producing frame would keep its damage and force a full
    // repaint on the next one.
    auto f = make_offscreen_fixture();
    if (!f.ready()) {
        SUCCEED("Dawn/Graphite unavailable on this host — frame-outcome proof skipped.");
        return;
    }

    REQUIRE(f.gpu->begin_frame());
    auto* canvas = f.skia->begin_frame();
    REQUIRE(canvas != nullptr);
    paint_something(*canvas);

    std::vector<uint8_t> pixels;
    uint32_t pw = 0, ph = 0;
    REQUIRE(f.skia->read_current_rgba(pixels, pw, ph));
    REQUIRE(pw == kW);
    REQUIRE(ph == kH);

    const auto outcome = f.skia->end_frame();
    f.gpu->end_frame();

    REQUIRE(render::frame_reached_output(outcome));
}

TEST_CASE("a persistent-scene frame reaches its output offscreen",
          "[gpu][skia][wah-2]") {
    auto f = make_offscreen_fixture();
    if (!f.ready()) {
        SUCCEED("Dawn/Graphite unavailable on this host — frame-outcome proof skipped.");
        return;
    }
    if (!f.skia->set_persistent_scene(true)) {
        SUCCEED("Backend cannot retain a scene — persistent-scene outcome skipped.");
        return;
    }

    REQUIRE(f.gpu->begin_frame());
    auto* canvas = f.skia->begin_frame();
    REQUIRE(canvas != nullptr);
    paint_something(*canvas);
    const auto outcome = f.skia->end_frame();
    f.gpu->end_frame();

    // Offscreen persistent-scene needs no blit — the scene IS the readback
    // target — so the absent blit must not be reported as a failure.
    REQUIRE(render::frame_reached_output(outcome));
}

TEST_CASE("consecutive frames all reach their output",
          "[gpu][skia][wah-2]") {
    // A per-frame flag that is not reset would make frame 2 (or 3) report
    // differently from frame 1. Drive several and require them identical.
    auto f = make_offscreen_fixture();
    if (!f.ready()) {
        SUCCEED("Dawn/Graphite unavailable on this host — frame-outcome proof skipped.");
        return;
    }

    for (int i = 0; i < 4; ++i) {
        REQUIRE(f.gpu->begin_frame());
        auto* canvas = f.skia->begin_frame();
        REQUIRE(canvas != nullptr);
        paint_something(*canvas);
        const auto outcome = f.skia->end_frame();
        f.gpu->end_frame();
        REQUIRE(render::frame_reached_output(outcome));
    }
}

TEST_CASE("a frame abandoned without end_frame does not taint the next one",
          "[gpu][skia][wah-2]") {
    auto f = make_offscreen_fixture();
    if (!f.ready()) {
        SUCCEED("Dawn/Graphite unavailable on this host — frame-outcome proof skipped.");
        return;
    }

    // Begin, capture (which flushes), then abandon without end_frame().
    REQUIRE(f.gpu->begin_frame());
    auto* canvas = f.skia->begin_frame();
    REQUIRE(canvas != nullptr);
    paint_something(*canvas);
    std::vector<uint8_t> pixels;
    uint32_t pw = 0, ph = 0;
    REQUIRE(f.skia->read_current_rgba(pixels, pw, ph));
    f.gpu->end_frame();

    // The next frame's outcome must be decided by its OWN work.
    REQUIRE(f.gpu->begin_frame());
    auto* canvas2 = f.skia->begin_frame();
    REQUIRE(canvas2 != nullptr);
    paint_something(*canvas2);
    const auto outcome = f.skia->end_frame();
    f.gpu->end_frame();

    REQUIRE(render::frame_reached_output(outcome));
}

#else

TEST_CASE("SkiaSurface frame-outcome GPU proof requires Skia on Apple",
          "[gpu][skia][wah-2]") {
    SUCCEED("Not a Skia/Apple build — live frame-outcome proof not applicable.");
}

#endif
