// test_plugin_frame_renderer.cpp — the shared Windows/Linux plug-in editor
// frame pipeline, and the visible-frame success contract (WAH-2).
//
// The bug this pins: a frame could render perfectly and reach nothing. When
// SkiaSurface failed to wrap the acquired native drawable it returned the
// OFFSCREEN canvas instead; the host painted, submitted, presented, reported
// success, and CLEARED ITS DAMAGE — while the texture actually being presented
// had never been drawn into. The editor was black and every observable signal,
// including screenshot readback (which reads the offscreen target), said the
// frame was fine. Persistent-scene blit failures had the same shape: log and
// continue.
//
// Damage retention is the half that makes recovery possible. A frame that did
// not reach its output must put its damage back, or the retry repaints nothing
// and the un-presented region stays stale forever.

// The damage->clip decision and the scene paint body are Skia-free and live in
// test_plugin_frame_clip.cpp, which runs in every configuration. THIS file
// drives PluginFrameRenderer itself, which only exists in Skia builds.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/plugin_frame_renderer.hpp>

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/view/view.hpp>

#include <cmath>
#include <memory>
#include <vector>

using namespace pulp::view;
using pulp::render::FrameOutcome;

namespace {

class FakeGpuSurface : public pulp::render::GpuSurface {
public:
    bool acquire_succeeds = true;
    bool presentable = true;
    int begin_calls = 0;
    int end_calls = 0;

    bool initialize(const Config&) override { return true; }
    void resize(uint32_t, uint32_t) override {}
    bool begin_frame() override {
        ++begin_calls;
        return acquire_succeeds;
    }
    void end_frame() override { ++end_calls; }
    bool is_initialized() const override { return true; }
    bool has_surface() const override { return presentable; }
    uint32_t width() const override { return 400; }
    uint32_t height() const override { return 300; }
    void* dawn_device_handle() const override { return nullptr; }
    void* dawn_queue_handle() const override { return nullptr; }
    void* dawn_instance_handle() const override { return nullptr; }
    void* current_texture_handle() const override { return nullptr; }
    AdapterInfo adapter_info() const override { return {}; }
};

// A SkiaSurface whose per-stage outcomes are injectable. Every failure this can
// produce corresponds to a real one: `begin_outcome` is the failed
// WrapBackendTexture, `end_outcome` covers the persistent-scene blit and the
// Graphite submit.
class FakeSkiaSurface : public pulp::render::SkiaSurface {
public:
    bool begin_returns_canvas = true;
    FrameOutcome begin_outcome = FrameOutcome::failed;
    FrameOutcome end_outcome = FrameOutcome::presented;
    bool readback_succeeds = true;
    int begin_calls = 0;
    int end_calls = 0;
    /// Canvas handed to the caller; a RecordingCanvas so the paint body's draw
    /// ops (and its clip) are observable.
    pulp::canvas::RecordingCanvas recording;

    pulp::canvas::Canvas* begin_frame() override {
        ++begin_calls;
        if (!begin_returns_canvas) {
            last_outcome_ = begin_outcome;
            return nullptr;
        }
        return &recording;
    }
    pulp::render::FrameOutcome end_frame() override {
        ++end_calls;
        last_outcome_ = end_outcome;
        return end_outcome;
    }
    pulp::render::FrameOutcome last_frame_outcome() const override {
        return last_outcome_;
    }
    bool has_presentable_target() const override { return true; }
    void resize(uint32_t, uint32_t, float) override {}
    bool read_current_rgba(std::vector<uint8_t>& pixels, uint32_t& w,
                           uint32_t& h) override {
        if (!readback_succeeds) return false;
        w = 400;
        h = 300;
        pixels.assign(static_cast<size_t>(w) * h * 4u, 0xFFu);
        return true;
    }
    bool is_available() const override { return true; }
    skgpu::graphite::Context* graphite_context() const override { return nullptr; }
    double gpu_render_time_ms() const override { return 0.0; }
    bool gpu_render_timing_available() const override { return false; }

private:
    FrameOutcome last_outcome_ = FrameOutcome::presented;
};

FrameGeometry plain_geometry() {
    FrameGeometry g;
    g.width = 400.0f;
    g.height = 300.0f;
    g.scale = 1.0f;
    return g;
}

struct Fixture {
    View root;
    FakeGpuSurface gpu;
    FakeSkiaSurface skia;
    PendingDamage damage;
    PluginFrameRenderer renderer;

    PluginFrameRenderer::Request request() {
        PluginFrameRenderer::Request r;
        r.root = &root;
        r.geometry = plain_geometry();
        return r;
    }

    /// Put the damage into a known BOUNDED state, so "was it retired?" and
    /// "was it escalated to full?" are both distinguishable afterwards.
    void arm_bounded_damage() {
        damage.clear();  // past the always-full first frame
        damage.mark(Rect{10, 20, 30, 40});
        REQUIRE_FALSE(damage.is_full());
        REQUIRE(damage.has_bounds());
    }
};

}  // namespace

// ── Success retires damage ──────────────────────────────────────────────────

TEST_CASE("a presented frame retires its damage",
          "[plugin-frame-renderer][wah-2]") {
    Fixture f;
    f.arm_bounded_damage();
    f.skia.end_outcome = FrameOutcome::presented;

    const auto frame = f.renderer.render(f.gpu, f.skia, f.damage, f.request());

    REQUIRE(frame.outcome == FrameOutcome::presented);
    REQUIRE(frame.reached_output());
    REQUIRE_FALSE(frame.should_recreate_surface);
    REQUIRE_FALSE(f.damage.is_full());
    REQUIRE_FALSE(f.damage.has_bounds());
}

TEST_CASE("an offscreen frame is a success, not a fallback",
          "[plugin-frame-renderer][wah-2]") {
    // Headless capture and render_to_png have no presentable surface BY DESIGN.
    // Treating that as failure would make every headless frame retry forever.
    Fixture f;
    f.arm_bounded_damage();
    f.skia.end_outcome = FrameOutcome::offscreen;

    const auto frame = f.renderer.render(f.gpu, f.skia, f.damage, f.request());

    REQUIRE(frame.reached_output());
    REQUIRE_FALSE(f.damage.has_bounds());
}

// ── Failure keeps damage, and cannot be counted as a visible frame ──────────

TEST_CASE("a failed native-drawable wrap is not a rendered frame",
          "[plugin-frame-renderer][wah-2]") {
    Fixture f;
    f.arm_bounded_damage();
    f.skia.begin_returns_canvas = false;
    f.skia.begin_outcome = FrameOutcome::recreate;

    const auto frame = f.renderer.render(f.gpu, f.skia, f.damage, f.request());

    REQUIRE_FALSE(frame.reached_output());
    REQUIRE(frame.outcome == FrameOutcome::recreate);
    REQUIRE(frame.should_recreate_surface);
    // The acquired swapchain texture is still released even though nothing was
    // drawn — otherwise the swapchain starves after the first failure.
    REQUIRE(f.gpu.end_calls == 1);
    // Damage survives. Restored as bounded (not escalated): nothing was
    // presented, so the drawable's contents were never disturbed.
    REQUIRE(f.damage.has_bounds());
    REQUIRE(f.damage.bounds().x == 10.0f);
}

TEST_CASE("a failed persistent-scene blit keeps the damage and escalates",
          "[plugin-frame-renderer][wah-2]") {
    Fixture f;
    f.arm_bounded_damage();
    f.skia.end_outcome = FrameOutcome::recreate;

    const auto frame = f.renderer.render(f.gpu, f.skia, f.damage, f.request());

    REQUIRE_FALSE(frame.reached_output());
    // After a failed PRESENT the drawable's contents are undefined, so a
    // bounded retry could composite onto garbage. Full is the only safe answer.
    REQUIRE(f.damage.is_full());
}

TEST_CASE("a failed submit keeps the damage and escalates",
          "[plugin-frame-renderer][wah-2]") {
    Fixture f;
    f.arm_bounded_damage();
    f.skia.end_outcome = FrameOutcome::failed;

    const auto frame = f.renderer.render(f.gpu, f.skia, f.damage, f.request());

    REQUIRE_FALSE(frame.reached_output());
    REQUIRE_FALSE(frame.should_recreate_surface);  // retry, don't rebuild
    REQUIRE(f.damage.is_full());
}

TEST_CASE("a failed swapchain acquire never consumes the damage",
          "[plugin-frame-renderer][wah-2]") {
    Fixture f;
    f.arm_bounded_damage();
    f.gpu.acquire_succeeds = false;

    const auto frame = f.renderer.render(f.gpu, f.skia, f.damage, f.request());

    REQUIRE_FALSE(frame.reached_output());
    REQUIRE(f.skia.begin_calls == 0);  // never started a frame
    // Bounded damage survives EXACTLY as it was: a busy/resizing swapchain is
    // transient and must not cost a full repaint.
    REQUIRE_FALSE(f.damage.is_full());
    REQUIRE(f.damage.has_bounds());
    REQUIRE(f.damage.bounds().width == 30.0f);
}

TEST_CASE("damage marked while a failing frame was in flight is preserved too",
          "[plugin-frame-renderer][wah-2]") {
    Fixture f;
    f.arm_bounded_damage();
    f.skia.end_outcome = FrameOutcome::failed;
    // A repaint request arriving mid-frame is the common case under a drag.
    f.damage.mark(Rect{200, 200, 10, 10});

    f.renderer.render(f.gpu, f.skia, f.damage, f.request());

    // Restoring must UNION, not overwrite; the escalation covers both.
    REQUIRE(f.damage.is_full());
}

TEST_CASE("a retried frame after a failure repaints and then retires",
          "[plugin-frame-renderer][wah-2]") {
    Fixture f;
    f.arm_bounded_damage();
    f.skia.end_outcome = FrameOutcome::failed;
    f.renderer.render(f.gpu, f.skia, f.damage, f.request());
    REQUIRE(f.damage.is_full());

    f.skia.end_outcome = FrameOutcome::presented;
    const auto retry = f.renderer.render(f.gpu, f.skia, f.damage, f.request());

    REQUIRE(retry.reached_output());
    REQUIRE_FALSE(f.damage.is_full());
    REQUIRE_FALSE(f.damage.has_bounds());
}

// ── Recreate budget ─────────────────────────────────────────────────────────

TEST_CASE("repeated recreate requests are bounded, then the GPU path is retired",
          "[plugin-frame-renderer][wah-2]") {
    Fixture f;
    f.skia.begin_returns_canvas = false;
    f.skia.begin_outcome = FrameOutcome::recreate;

    for (int i = 1; i <= PluginFrameRenderer::kMaxConsecutiveRecreates; ++i) {
        const auto frame = f.renderer.render(f.gpu, f.skia, f.damage, f.request());
        REQUIRE(frame.should_recreate_surface);
        REQUIRE_FALSE(frame.gpu_path_exhausted);
        REQUIRE(f.renderer.consecutive_recreates() == i);
    }

    // A permanently broken drawable must degrade to the host's CPU raster path
    // rather than spinning on surface creation forever.
    const auto exhausted = f.renderer.render(f.gpu, f.skia, f.damage, f.request());
    REQUIRE_FALSE(exhausted.should_recreate_surface);
    REQUIRE(exhausted.gpu_path_exhausted);
}

TEST_CASE("a successful frame resets the recreate budget",
          "[plugin-frame-renderer][wah-2]") {
    Fixture f;
    f.skia.begin_returns_canvas = false;
    f.skia.begin_outcome = FrameOutcome::recreate;
    f.renderer.render(f.gpu, f.skia, f.damage, f.request());
    f.renderer.render(f.gpu, f.skia, f.damage, f.request());
    REQUIRE(f.renderer.consecutive_recreates() == 2);

    f.skia.begin_returns_canvas = true;
    f.skia.end_outcome = FrameOutcome::presented;
    f.renderer.render(f.gpu, f.skia, f.damage, f.request());

    REQUIRE(f.renderer.consecutive_recreates() == 0);
}

TEST_CASE("note_surfaces_created resets the budget after a host rebuild",
          "[plugin-frame-renderer][wah-2]") {
    Fixture f;
    f.skia.begin_returns_canvas = false;
    f.skia.begin_outcome = FrameOutcome::recreate;
    f.renderer.render(f.gpu, f.skia, f.damage, f.request());
    REQUIRE(f.renderer.consecutive_recreates() == 1);

    f.renderer.note_surfaces_created();

    REQUIRE(f.renderer.consecutive_recreates() == 0);
}

// ── Readback is reported independently of presentation ──────────────────────

TEST_CASE("a failed readback does not turn a presented frame into a failure",
          "[plugin-frame-renderer][wah-2]") {
    Fixture f;
    std::vector<uint8_t> pixels;
    uint32_t w = 0, h = 0;
    auto request = f.request();
    request.capture = &pixels;
    request.capture_width = &w;
    request.capture_height = &h;
    f.skia.readback_succeeds = false;

    const auto frame = f.renderer.render(f.gpu, f.skia, f.damage, request);

    REQUIRE(frame.reached_output());  // the screen got its pixels
    REQUIRE_FALSE(frame.readback_ok);  // the capture did not
}

TEST_CASE("a successful capture reports its dimensions",
          "[plugin-frame-renderer][wah-2]") {
    Fixture f;
    std::vector<uint8_t> pixels;
    uint32_t w = 0, h = 0;
    auto request = f.request();
    request.capture = &pixels;
    request.capture_width = &w;
    request.capture_height = &h;

    const auto frame = f.renderer.render(f.gpu, f.skia, f.damage, request);

    REQUIRE(frame.readback_ok);
    REQUIRE(w == 400);
    REQUIRE(h == 300);
    REQUIRE(pixels.size() == 400u * 300u * 4u);
}

// ── The idle pump runs exactly once, before acquire ─────────────────────────

TEST_CASE("the idle pump runs once per frame, before the swapchain acquire",
          "[plugin-frame-renderer][wah-2]") {
    Fixture f;
    int idle_calls = 0;
    int acquire_at_idle = -1;
    auto request = f.request();
    request.idle = [&] {
        ++idle_calls;
        acquire_at_idle = f.gpu.begin_calls;
    };

    f.renderer.render(f.gpu, f.skia, f.damage, request);

    REQUIRE(idle_calls == 1);
    // Before acquire: the pump can request a repaint, and blocking on the
    // swapchain first would run it a frame late.
    REQUIRE(acquire_at_idle == 0);
}

#else

TEST_CASE("PluginFrameRenderer requires a Skia build",
          "[plugin-frame-renderer][wah-2]") {
    // The pure damage->clip and paint contracts still run everywhere; see
    // test_plugin_frame_clip.cpp.
    SUCCEED("Not a Skia build — the GPU frame drive is not compiled.");
}

#endif  // PULP_HAS_SKIA
