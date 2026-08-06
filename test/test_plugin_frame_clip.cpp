// test_plugin_frame_clip.cpp — the PURE half of the shared plug-in editor
// frame pipeline: the damage → clip decision and the scene paint body.
//
// Deliberately Skia-free so it compiles and RUNS in every configuration,
// including the no-GPU one. That is the whole reason plugin_frame_renderer.hpp
// splits this out: the Windows and Linux hosts are the only consumers, and
// logic reachable only under `_WIN32` is logic no CI gate ever executes (the
// same reasoning as win_pointer_input.hpp). The GPU drive and failure policy
// that sit on top of these functions are covered by
// test_plugin_frame_renderer.cpp, which needs a real SkiaSurface.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/view/plugin_frame_renderer.hpp>
#include <pulp/view/view.hpp>

#include <cmath>

using namespace pulp::view;

namespace {

FrameGeometry plain_geometry() {
    FrameGeometry g;
    g.width = 400.0f;
    g.height = 300.0f;
    g.scale = 1.0f;
    return g;
}

}  // namespace

// ── The damage → clip decision (shared, and pure) ───────────────────────────

TEST_CASE("full damage never produces a clip", "[plugin-frame-renderer][wah-6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    PendingDamage damage;  // starts full
    Rect clip{};
    REQUIRE_FALSE(compute_frame_clip(root, damage.take(), plain_geometry(), clip));
}

TEST_CASE("absent damage never produces a clip", "[plugin-frame-renderer][wah-6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    PendingDamage damage;
    damage.clear();  // no full, no bounds
    Rect clip{};
    REQUIRE_FALSE(compute_frame_clip(root, damage.take(), plain_geometry(), clip));
}

TEST_CASE("bounded damage clips to the damaged rect without a design viewport",
          "[plugin-frame-renderer][wah-6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    PendingDamage damage;
    damage.clear();
    damage.mark(Rect{100, 50, 40, 30});

    Rect clip{};
    REQUIRE(compute_frame_clip(root, damage.take(), plain_geometry(), clip));
    // The hazard model may only GROW the rect (never shrink it), so containment
    // is the invariant worth asserting rather than exact equality.
    REQUIRE(clip.x <= 100.0f);
    REQUIRE(clip.y <= 50.0f);
    REQUIRE(clip.x + clip.width >= 140.0f);
    REQUIRE(clip.y + clip.height >= 80.0f);
}

TEST_CASE("a design viewport maps the clip into surface space",
          "[plugin-frame-renderer][wah-6]") {
    // Plug-in editors ALWAYS set a design viewport, so this is the only mapping
    // that matters in a DAW. Paint applies translate+scale; the clip is
    // installed before that transform, so an unmapped root rect lands in the
    // wrong place — which is why plug-in editors could not use partial repaint
    // at all before this mapping existed.
    View root;
    root.set_bounds({0, 0, 200, 150});
    PendingDamage damage;
    damage.clear();
    damage.mark(Rect{100, 75, 20, 20});

    FrameGeometry g = plain_geometry();  // 400x300 host
    g.design_width = 200.0f;             // 2x uniform scale, no letterbox
    g.design_height = 150.0f;

    Rect clip{};
    REQUIRE(compute_frame_clip(root, damage.take(), g, clip));
    // Root (100,75) maps to surface (200,150) at 2x.
    REQUIRE(clip.x <= 200.0f);
    REQUIRE(clip.y <= 150.0f);
    REQUIRE(clip.x + clip.width >= 240.0f);
    REQUIRE(clip.y + clip.height >= 190.0f);
}

TEST_CASE("a mapped clip is snapped out to whole surface pixels",
          "[plugin-frame-renderer][wah-6]") {
    View root;
    root.set_bounds({0, 0, 300, 300});
    PendingDamage damage;
    damage.clear();
    damage.mark(Rect{10, 10, 5, 5});

    FrameGeometry g = plain_geometry();
    g.width = 400.0f;
    g.height = 400.0f;
    g.design_width = 300.0f;  // 4/3 scale — guarantees fractional edges
    g.design_height = 300.0f;

    Rect clip{};
    REQUIRE(compute_frame_clip(root, damage.take(), g, clip));
    // A fractional edge would clip a partially covered pixel.
    REQUIRE(clip.x == std::floor(clip.x));
    REQUIRE(clip.y == std::floor(clip.y));
    REQUIRE(clip.width == std::floor(clip.width));
    REQUIRE(clip.height == std::floor(clip.height));
}

// ── The shared paint body ───────────────────────────────────────────────────

TEST_CASE("the scene paint fills the host background at host size",
          "[plugin-frame-renderer][wah-6]") {
    View root;
    pulp::canvas::RecordingCanvas canvas;
    paint_plugin_scene(canvas, root, plain_geometry(), nullptr);

    // The background fill is what makes the editor opaque; a host that skipped
    // it showed the DAW's own window content through the editor.
    REQUIRE_FALSE(canvas.commands().empty());
}

TEST_CASE("a design viewport lays the root out at design size, not host size",
          "[plugin-frame-renderer][wah-6]") {
    View root;
    FrameGeometry g = plain_geometry();
    g.design_width = 200.0f;
    g.design_height = 150.0f;

    pulp::canvas::RecordingCanvas canvas;
    paint_plugin_scene(canvas, root, g, nullptr);

    // Pinning the root to the DESIGN size (and letterbox-scaling paint) is the
    // whole contract: the tree lays out once, at the size it was authored for.
    REQUIRE(root.bounds().width == 200.0f);
    REQUIRE(root.bounds().height == 150.0f);
}

TEST_CASE("without a design viewport the root is laid out at host size",
          "[plugin-frame-renderer][wah-6]") {
    View root;
    pulp::canvas::RecordingCanvas canvas;
    paint_plugin_scene(canvas, root, plain_geometry(), nullptr);

    REQUIRE(root.bounds().width == 400.0f);
    REQUIRE(root.bounds().height == 300.0f);
}

TEST_CASE("the paint body balances its save/restore with and without a clip",
          "[plugin-frame-renderer][wah-6]") {
    // An unbalanced clip would leak into the next frame's draws on the retained
    // scene surface, which is exactly the kind of corruption that only shows up
    // under partial repaint.
    View root;
    pulp::canvas::RecordingCanvas canvas;
    const int before = canvas.save_count();

    paint_plugin_scene(canvas, root, plain_geometry(), nullptr);
    REQUIRE(canvas.save_count() == before);

    const Rect clip{10, 10, 50, 50};
    paint_plugin_scene(canvas, root, plain_geometry(), &clip);
    REQUIRE(canvas.save_count() == before);
}

// ── FrameGeometry's pixel arithmetic (WAH-6) ────────────────────────────────
//
// This rule used to be a private `pixel_w()`/`pixel_h()` pair copied into each
// platform host. Three consumers depend on the SAME answer — the GPU
// swapchain's size, the CPU raster fallback's buffer, and the headless capture
// the embed smoke asserts on — so a host that computed it differently from its
// own surface produced a capture that did not match what it presented.

TEST_CASE("pixel dimensions are logical size times scale",
          "[plugin-frame][wah-6]") {
    FrameGeometry g;
    g.width = 400.0f;
    g.height = 300.0f;
    g.scale = 2.0f;

    REQUIRE(g.pixel_width() == 800);
    REQUIRE(g.pixel_height() == 600);
}

TEST_CASE("fractional pixel dimensions truncate rather than round up",
          "[plugin-frame][wah-6]") {
    // The surface must never be asked for MORE pixels than the window has, or
    // the readback reads past the drawable.
    FrameGeometry g;
    g.width = 100.0f;
    g.height = 100.0f;
    g.scale = 1.5f;

    REQUIRE(g.pixel_width() == 150);
    REQUIRE(g.pixel_height() == 150);

    g.scale = 1.255f;  // 125.5 -> 125
    REQUIRE(g.pixel_width() == 125);
}

TEST_CASE("a collapsed editor still asks for at least one pixel",
          "[plugin-frame][wah-6]") {
    // A DAW can size an editor to zero mid-teardown. A 0-sized surface is a
    // backend error on every platform, so the clamp is what keeps a collapse
    // from becoming a crash.
    FrameGeometry g;
    g.width = 0.0f;
    g.height = 0.0f;
    g.scale = 1.0f;

    REQUIRE(g.pixel_width() == 1);
    REQUIRE(g.pixel_height() == 1);

    g.width = 10.0f;
    g.height = 10.0f;
    g.scale = 0.01f;  // 0.1 logical pixels
    REQUIRE(g.pixel_width() == 1);
    REQUIRE(g.pixel_height() == 1);
}
