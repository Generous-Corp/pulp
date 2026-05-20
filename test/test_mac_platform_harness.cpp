// Mac-only Catch2 smoke for the platform-test harness — issue #2001.
//
// First customer (Phase B-1): asserts the harness can construct a
// hidden GPU-backed NSWindow + CAMetalLayer host without ever calling
// orderFront / makeKey, and that the new
// `WindowHost::capture_back_buffer_png()` production seam returns
// non-empty PNG bytes through the existing render_frame() path.
//
// Phase B-2 (next PR) will add `simulate_mouse` exercise + migrate one
// PR-#1984 invariant (e.g. set_design_viewport overlay-inside-transform
// from `test_view_design_viewport.cpp`) to the harness so the fixture
// has a real production-bug consumer.
//
// Tag [issue-2001] so the coverage harness can attribute these to the
// slice that introduced them.

#include "mac_window_harness.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <vector>

using pulp::view::MouseButton;
using pulp::view::MouseEvent;
using pulp::view::Rect;
using pulp::view::View;
using pulp::view::WindowHost;
using pulp::view::WindowOptions;

namespace pt = pulp::test::mac;

TEST_CASE("mac harness constructs a hidden GPU-backed window",
          "[mac][platform-harness][issue-2001]") {
    View root;
    auto host = pt::make_test_window(root);

    REQUIRE(host != nullptr);
    REQUIRE(host->is_visible() == false);
    REQUIRE(host->native_window_handle() != nullptr);
    REQUIRE(host->native_content_view_handle() != nullptr);
    REQUIRE(host->gpu_surface() != nullptr);
}

TEST_CASE("mac harness back-buffer capture returns non-empty PNG bytes",
          "[mac][platform-harness][issue-2001]") {
    View root;
    auto host = pt::make_test_window(root);
    REQUIRE(host != nullptr);

    auto png = pt::capture_back_buffer_png(*host);

    // The harness's contract is "deterministic host-managed pixels".
    // For the GPU host that means render_frame() succeeded and
    // encode_rgba_to_png() produced a real PNG. An empty result here
    // would indicate the back-buffer path is broken on hidden windows.
    INFO("png byte count: " << png.size());
    REQUIRE_FALSE(png.empty());

    // PNG signature sanity-check (8-byte magic). Catches the case where
    // the back-buffer readback returns raw RGBA without the encode step.
    REQUIRE(png.size() >= 8);
    REQUIRE(png[0] == 0x89);
    REQUIRE(png[1] == 0x50);  // 'P'
    REQUIRE(png[2] == 0x4E);  // 'N'
    REQUIRE(png[3] == 0x47);  // 'G'
}

TEST_CASE("mac harness honors caller-provided window options size",
          "[mac][platform-harness][issue-2001]") {
    View root;
    WindowOptions opts;
    opts.width = 640;
    opts.height = 480;

    auto host = pt::make_test_window(root, opts);
    REQUIRE(host != nullptr);

    const auto content = host->get_content_size();
    // CAMetalLayer drawableSize is the logical size scaled by
    // contentsScale on Retina, so don't assert byte-equal — just
    // assert the host rounded the request up to something plausible.
    REQUIRE(content.width  >= 600);
    REQUIRE(content.height >= 400);
}

// ── Codex review P2 fixes (PR #2009) ────────────────────────────────────
//
// Before these fixes:
//   1. `build_event` constructed scroll wheel events via
//      `+[NSEvent mouseEventWithType:]`, which does not carry
//      `scrollingDeltaX/Y`. `PulpView::scrollWheel:` reads those, so the
//      synthetic event delivered a zero-delta wheel — scroll tests
//      passed without ever exercising real scroll math.
//   2. `simulate_mouse` routed every phase through `mouseDown:` /
//      `mouseUp:` / `mouseDragged:` regardless of `event.button`, so a
//      right-click in a test reached the left-click selector instead of
//      `rightMouseDown:` (which is what triggers the context-menu path).
//
// These two tests pin both behaviors. They build a hidden GPU window,
// install a child view with a known hit-test rect, and assert the
// production selectors actually fired.

TEST_CASE("mac harness scroll event carries non-zero deltas through PulpView",
          "[mac][platform-harness][issue-2001]") {
    View root;
    root.set_bounds({0, 0, 320, 240});

    // Child fills the window. on_pointer_event is the callback
    // `PulpView::scrollWheel:` invokes once it has walked ancestors to
    // dispatch a wheel-flagged MouseEvent.
    auto child = std::make_unique<View>();
    child->flex().preferred_width = 320.0f;
    child->flex().preferred_height = 240.0f;

    int wheel_calls = 0;
    float captured_dx = 0.0f;
    float captured_dy = 0.0f;
    child->on_pointer_event = [&](const MouseEvent& me) {
        if (!me.is_wheel) return;
        ++wheel_calls;
        captured_dx = me.scroll_delta_x;
        captured_dy = me.scroll_delta_y;
    };
    root.add_child(std::move(child));
    root.layout_children();

    auto host = pt::make_test_window(root);
    REQUIRE(host != nullptr);

    pt::SimulatedMouse ev;
    ev.phase = pt::SimulatedMouse::Phase::scroll;
    ev.x = 100.0f;
    ev.y = 100.0f;
    ev.scroll_delta_y = 10.0f;
    ev.scroll_delta_x = 0.0f;
    REQUIRE(pt::simulate_mouse(*host, ev));

    REQUIRE(wheel_calls >= 1);
    // PulpView::scrollWheel: negates the Y axis (Cocoa wheel deltas are
    // bottom-up; the View MouseEvent is top-down). The harness already
    // hands the CGEvent the caller's raw scroll_delta_y, so the View
    // callback observes |delta| > 0 with the production sign.
    REQUIRE(captured_dy != 0.0f);
    REQUIRE(captured_dx == 0.0f);
}

TEST_CASE("mac harness right-click reaches PulpView::rightMouseDown: not mouseDown:",
          "[mac][platform-harness][issue-2001]") {
    View root;
    root.set_bounds({0, 0, 320, 240});

    auto child = std::make_unique<View>();
    child->flex().preferred_width = 320.0f;
    child->flex().preferred_height = 240.0f;

    // `on_click` is the left-click signal: PulpView::mouseUp: posts it
    // via dispatch_async. `on_context_menu` is the right-click signal:
    // PulpView::rightMouseDown: invokes it synchronously. Wiring both
    // here lets us prove the synthetic right-click did NOT fall into
    // the left-click path.
    int left_clicks = 0;
    int context_menus = 0;
    child->on_click = [&] { ++left_clicks; };
    child->on_context_menu = [&](pulp::view::Point) { ++context_menus; };
    root.add_child(std::move(child));
    root.layout_children();

    auto host = pt::make_test_window(root);
    REQUIRE(host != nullptr);

    pt::SimulatedMouse down;
    down.phase = pt::SimulatedMouse::Phase::down;
    down.button = MouseButton::right;
    down.x = 50.0f;
    down.y = 50.0f;
    REQUIRE(pt::simulate_mouse(*host, down));

    // Right-click only fires on rightMouseDown:; the matching up event
    // is exercised here mainly to keep the gesture symmetrical and to
    // confirm `simulate_mouse` does not crash when routing to
    // `rightMouseUp:` (which PulpView does not override).
    pt::SimulatedMouse up = down;
    up.phase = pt::SimulatedMouse::Phase::up;
    REQUIRE(pt::simulate_mouse(*host, up));

    REQUIRE(context_menus == 1);
    REQUIRE(left_clicks == 0);
}
