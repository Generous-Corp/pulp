// FU-2 / WI-17 — persistent-scene mode LIVE GPU proof.
//
// The raster equivalence tests (test_partial_repaint_equivalence.cpp) cover the
// damage model + clip logic on the CPU, but they cannot exercise the one thing
// the GPU host actually relies on: SkiaSurface::set_persistent_scene keeps the
// previous frame's pixels in a retained Graphite scene surface, so a CLIPPED
// repaint of frame N updates only the damaged rect and everything else is frame
// N-1. This test drives an offscreen Dawn+Skia (Graphite) surface in
// persistent-scene mode across two frames — a full frame, then a frame clipped
// to a small region — and asserts the untouched content survived while the
// clipped region changed. That is the cross-frame retention proof.
//
// Gated PULP_HAS_SKIA && APPLE && PULP_ENABLE_GPU at compile time (CMake);
// soft-skips at run time when no Dawn adapter is available. Mirrors
// test_subtree_cache_gpu.cpp.
//
// Tag: [gpu][skia][partial-render][issue-6262]

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#endif

using namespace pulp;

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)

namespace {

constexpr uint32_t kW = 128;
constexpr uint32_t kH = 128;

struct GpuFixture {
    std::unique_ptr<render::GpuSurface> gpu;
    std::unique_ptr<render::SkiaSurface> skia;
    bool ready() const { return gpu && skia && skia->is_available(); }
};

GpuFixture make_offscreen_fixture(uint32_t w, uint32_t h) {
    GpuFixture f;
    f.gpu = render::GpuSurface::create_dawn();
    if (!f.gpu) return f;
    render::GpuSurface::Config gpu_config{};
    gpu_config.width = w;
    gpu_config.height = h;
    gpu_config.native_surface_handle = nullptr;  // headless / offscreen
    if (!f.gpu->initialize(gpu_config)) {
        f.gpu.reset();
        return f;
    }
    render::SkiaSurface::Config skia_config{};
    skia_config.width = w;
    skia_config.height = h;
    skia_config.scale_factor = 1.0f;
    f.skia = render::SkiaSurface::create(*f.gpu, skia_config);
    return f;
}

// Count pixels matching a colour within a tolerance (readback is RGBA8).
uint32_t count_color(const std::vector<uint8_t>& px, uint32_t w, uint32_t h,
                     uint8_t r, uint8_t g, uint8_t b) {
    const size_t need = static_cast<size_t>(w) * h * 4u;
    if (px.size() < need) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < w * h; ++i) {
        const int dr = int(px[i * 4 + 0]) - int(r);
        const int dg = int(px[i * 4 + 1]) - int(g);
        const int db = int(px[i * 4 + 2]) - int(b);
        if (dr * dr + dg * dg + db * db < 40 * 40) ++n;
    }
    return n;
}

// Render one frame through the persistent scene and read it back. `paint` draws
// into the retained scene canvas (with its own clip, if any). Returns false if
// begin/readback fails (no adapter).
bool render_frame(GpuFixture& f,
                  const std::function<void(canvas::Canvas&)>& paint,
                  std::vector<uint8_t>& px, uint32_t& pw, uint32_t& ph) {
    if (!f.gpu->begin_frame()) return false;
    auto* canvas = f.skia->begin_frame();
    if (!canvas) { f.gpu->end_frame(); return false; }
    paint(*canvas);
    f.skia->end_frame();
    f.gpu->end_frame();
    return f.skia->read_current_rgba(px, pw, ph);
}

}  // namespace

TEST_CASE("Persistent-scene mode retains content across a clipped repaint (offscreen)",
          "[gpu][skia][partial-render][issue-6262]") {
    auto f = make_offscreen_fixture(kW, kH);
    if (!f.ready()) {
        SUCCEED("Dawn/Graphite unavailable on this host — persistent-scene proof skipped.");
        return;
    }

    REQUIRE(f.skia->set_persistent_scene(true));
    REQUIRE(f.skia->persistent_scene());

    // Frame 1 — FULL: blue background with a RED square in the top-left corner
    // and a GREEN-free bottom-right region we will overwrite next frame.
    std::vector<uint8_t> px1;
    uint32_t pw = 0, ph = 0;
    const bool ok1 = render_frame(f, [](canvas::Canvas& c) {
        c.set_fill_color(canvas::Color::rgba8(20, 40, 120));
        c.fill_rect(0, 0, static_cast<float>(kW), static_cast<float>(kH));
        c.set_fill_color(canvas::Color::rgba8(220, 40, 40));
        c.fill_rect(10, 10, 40, 40);  // red marker, top-left
    }, px1, pw, ph);
    if (!ok1) {
        SUCCEED("GPU readback failed (no adapter) — persistent-scene proof skipped.");
        return;
    }

    const uint32_t red1 = count_color(px1, pw, ph, 220, 40, 40);
    INFO("frame 1 red pixels: " << red1);
    // The record frame DEFINITELY draws the red square, so a (near-)blank
    // readback means Dawn initialized but the offscreen surface composites
    // nothing — a headless runner with no real adapter. Skip rather than assert,
    // matching test_subtree_cache_gpu's guard.
    if (red1 < 100u) {
        SUCCEED("Offscreen GPU composites nothing (no real adapter) — "
                "persistent-scene proof skipped.");
        return;
    }
    REQUIRE(red1 > 1000u);  // the red square painted on the full frame

    // Frame 2 — CLIPPED to the bottom-right quadrant only: paint it GREEN. In
    // persistent-scene mode the retained scene keeps everything outside the
    // clip, so the red square (top-left, outside the clip) must survive.
    std::vector<uint8_t> px2;
    uint32_t pw2 = 0, ph2 = 0;
    REQUIRE(render_frame(f, [](canvas::Canvas& c) {
        c.save();
        c.clip_rect(80, 80, 40, 40);
        c.set_fill_color(canvas::Color::rgba8(30, 200, 60));
        c.fill_rect(0, 0, static_cast<float>(kW), static_cast<float>(kH));
        c.restore();
    }, px2, pw2, ph2));

    const uint32_t red2 = count_color(px2, pw2, ph2, 220, 40, 40);
    const uint32_t green2 = count_color(px2, pw2, ph2, 30, 200, 60);
    INFO("frame 2 red pixels (retained): " << red2 << ", green pixels (clipped): " << green2);

    // The clipped region is now green…
    REQUIRE(green2 > 500u);
    // …and the red square OUTSIDE the clip survived — the retained scene was not
    // cleared. Within readback noise it matches frame 1's red count.
    REQUIRE(red2 > 1000u);
    const uint32_t hi = red1 > red2 ? red1 : red2;
    const uint32_t lo = red1 > red2 ? red2 : red1;
    REQUIRE((hi - lo) < hi / 20u + 16u);  // within ~5% — the marker is unchanged
}

TEST_CASE("Persistent scene is recreated at the new physical size on a scale change",
          "[gpu][skia][partial-render][issue-6262]") {
    // The DPI-desync fix: a backing-scale (DPI) change resizes the SkiaSurface,
    // which must recreate the retained scene at the new PHYSICAL size — otherwise
    // the fixed-size scene blits with an undefined region where the drawable
    // grew. The host wiring (viewDidChangeBackingProperties -> handle_backing_change)
    // needs a live window + display move and cannot be driven headless, so this
    // proves the SkiaSurface half directly: resize() in persistent-scene mode
    // yields a scene at the new physical dimensions that still composites.
    auto f = make_offscreen_fixture(kW, kH);
    if (!f.ready()) {
        SUCCEED("Dawn/Graphite unavailable — scale-change proof skipped.");
        return;
    }
    REQUIRE(f.skia->set_persistent_scene(true));

    // Frame 1 at scale 1.0 → readback is kW x kH physical.
    std::vector<uint8_t> px1;
    uint32_t pw = 0, ph = 0;
    const bool ok1 = render_frame(f, [](canvas::Canvas& c) {
        c.set_fill_color(canvas::Color::rgba8(20, 40, 120));
        c.fill_rect(0, 0, static_cast<float>(kW), static_cast<float>(kH));
        c.set_fill_color(canvas::Color::rgba8(220, 40, 40));
        c.fill_rect(10, 10, 40, 40);
    }, px1, pw, ph);
    if (!ok1 || count_color(px1, pw, ph, 220, 40, 40) < 100u) {
        SUCCEED("No real adapter — scale-change proof skipped.");
        return;
    }
    REQUIRE(pw == kW);
    REQUIRE(ph == kH);

    // Simulate a DPI increase to 2x at the SAME logical size (what
    // handle_backing_change drives). The scene must be recreated at 2*kW x 2*kH.
    f.skia->resize(kW, kH, 2.0f);
    REQUIRE(f.skia->persistent_scene());  // still in persistent-scene mode

    std::vector<uint8_t> px2;
    uint32_t pw2 = 0, ph2 = 0;
    REQUIRE(render_frame(f, [](canvas::Canvas& c) {
        c.set_fill_color(canvas::Color::rgba8(20, 40, 120));
        c.fill_rect(0, 0, static_cast<float>(kW), static_cast<float>(kH));
        c.set_fill_color(canvas::Color::rgba8(220, 40, 40));
        c.fill_rect(10, 10, 40, 40);
    }, px2, pw2, ph2));

    // The retained scene now matches the new physical size (no stale small
    // buffer that would blit an undefined region onto a larger drawable)…
    REQUIRE(pw2 == kW * 2u);
    REQUIRE(ph2 == kH * 2u);
    // …and it still composites the content.
    REQUIRE(count_color(px2, pw2, ph2, 220, 40, 40) > 1000u);
}

#endif  // PULP_HAS_SKIA && __APPLE__
