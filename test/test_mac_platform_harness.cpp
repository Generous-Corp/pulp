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
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <vector>

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
