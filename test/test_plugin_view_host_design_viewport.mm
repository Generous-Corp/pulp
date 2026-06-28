// test_plugin_view_host_design_viewport.mm — port of WindowHost's
// set_design_viewport + aspect-lock contract onto PluginViewHost.
//
// pulp #59/#63/#64/#65 originated this on the standalone WindowHost; the
// math is already pinned by test_view_design_viewport.cpp. This file
// covers the PluginViewHost wiring: the new virtuals (set_design_viewport,
// set_fixed_aspect_ratio, window_to_root_point) and that the mac CPU and
// GPU hosts apply the inverse transform to host-space input points before
// hit-test. Without these, CLAP gui_get_resize_hints / VST3
// checkSizeConstraint can request a proportional resize and the editor
// still mis-routes clicks at the new size — the kind of silent failure
// that only surfaces inside a DAW.
//
// Tag [plugin-view-host][design-viewport]. Mac-only (the implementations
// being tested live in plugin_view_host_mac.mm). GPU section soft-skips
// when Dawn / a window server is unavailable.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#endif

using namespace pulp::view;
using Catch::Approx;

#if defined(__APPLE__)

namespace {

// Pumps the Cocoa run loop a few ticks so -viewDidMoveToWindow fires +
// the GPU host's display link drives a first frame. Mirrors the helper
// in test_plugin_editor_host_smoke_mac.mm.
void pump_run_loop(int frames) {
    for (int i = 0; i < frames; ++i) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop]
                runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
    }
}

bool looks_like_png(const std::vector<uint8_t>& d) {
    return d.size() > 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
}

}  // namespace

TEST_CASE("PluginViewHost (mac CPU) — set_design_viewport + inverse hit-test",
          "[plugin-view-host][design-viewport][mac][cpu]") {
    @autoreleasepool {
        View root;
        PluginViewHost::Options opts;
        opts.size = {1800u, 1040u};
        opts.use_gpu = false;
        auto host = PluginViewHost::create(root, opts);
        REQUIRE(host != nullptr);

        SECTION("default — window_to_root_point is identity") {
            pulp::view::Point in{500.0f, 300.0f};
            pulp::view::Point out = host->window_to_root_point(in);
            REQUIRE(out.x == Approx(in.x));
            REQUIRE(out.y == Approx(in.y));
        }

        SECTION("matching aspect (1800x1040 host, 900x520 design) — scale 2") {
            host->set_design_viewport(900.0f, 520.0f);

            // Window-space origin → design-space origin.
            pulp::view::Point at_origin = host->window_to_root_point({0.0f, 0.0f});
            REQUIRE(at_origin.x == Approx(0.0f).margin(0.01f));
            REQUIRE(at_origin.y == Approx(0.0f).margin(0.01f));

            // Window-space center → design-space center.
            pulp::view::Point at_center = host->window_to_root_point({900.0f, 520.0f});
            REQUIRE(at_center.x == Approx(450.0f).margin(0.01f));
            REQUIRE(at_center.y == Approx(260.0f).margin(0.01f));

            // Arbitrary: window-space (200, 200) at scale=2 → design (100, 100).
            pulp::view::Point arbitrary = host->window_to_root_point({200.0f, 200.0f});
            REQUIRE(arbitrary.x == Approx(100.0f).margin(0.01f));
            REQUIRE(arbitrary.y == Approx(100.0f).margin(0.01f));
        }

        SECTION("host wider than design — letterbox tx > 0") {
            host->set_size(1800, 900);
            host->set_design_viewport(900.0f, 520.0f);
            const float s = 900.0f / 520.0f;             // height-limited
            const float tx = (1800.0f - 900.0f * s) * 0.5f;

            // The left edge of the design surface lives at window-x = tx, so
            // window (tx, 0) → design (0, 0). A point inside the letterbox
            // bar maps to a NEGATIVE design-x.
            pulp::view::Point edge = host->window_to_root_point({tx, 0.0f});
            REQUIRE(edge.x == Approx(0.0f).margin(0.05f));
            REQUIRE(edge.y == Approx(0.0f).margin(0.05f));

            pulp::view::Point bar = host->window_to_root_point({tx * 0.5f, 0.0f});
            REQUIRE(bar.x < 0.0f);
        }

        SECTION("reset (0, 0) restores identity") {
            host->set_design_viewport(900.0f, 520.0f);
            host->set_design_viewport(0.0f, 0.0f);
            pulp::view::Point identity = host->window_to_root_point({500.0f, 300.0f});
            REQUIRE(identity.x == Approx(500.0f));
            REQUIRE(identity.y == Approx(300.0f));
        }

        SECTION("set_fixed_aspect_ratio is API-parity no-op (no enforcement)") {
            // The plugin host doesn't own the OS window; aspect enforcement
            // is the per-format resize-hint path's job. Make sure the call
            // doesn't crash and doesn't affect window_to_root_point.
            host->set_fixed_aspect_ratio(900.0f / 520.0f);
            host->set_design_viewport(900.0f, 520.0f);
            pulp::view::Point at_center = host->window_to_root_point({900.0f, 520.0f});
            REQUIRE(at_center.x == Approx(450.0f).margin(0.01f));
            REQUIRE(at_center.y == Approx(260.0f).margin(0.01f));
        }
    }
}

TEST_CASE("PluginViewHost (mac CPU) — dtor clears pointTransform (UAF guard)",
          "[plugin-view-host][design-viewport][mac][cpu][uaf]") {
    // Regression for the pointTransform-UAF: the block stored on the NSView
    // captures `this` by raw pointer; if the DAW retains the NSView after
    // host disposal, a later mouseDown: would call the block on freed memory.
    // The host dtor MUST clear pointTransform before returning. Capture a
    // strong NSView reference, destroy the host, then assert pointTransform
    // is nil — proves the dtor ran the cleanup.
    @autoreleasepool {
        View root;
        PluginViewHost::Options opts;
        opts.size = {1800u, 1040u};
        opts.use_gpu = false;
        auto host = PluginViewHost::create(root, opts);
        REQUIRE(host != nullptr);

        host->set_design_viewport(900.0f, 520.0f);
        NSView* view = (__bridge NSView*)host->native_handle();
        REQUIRE(view != nil);
        // Set: pointTransform is non-nil after set_design_viewport.
        // (Use KVC so we can introspect without exposing the ObjC interface.)
        REQUIRE([view valueForKey:@"pointTransform"] != nil);

        // Strong-ref the view past the host so we can probe after teardown.
        NSView* live_view = view;
        host.reset();

        // After dtor: pointTransform must be nil so any subsequent mouseDown:
        // is a no-op (identity transform) rather than a UAF call into freed
        // memory. live_view is still alive because the test holds a ref.
        REQUIRE([live_view valueForKey:@"pointTransform"] == nil);
        live_view = nil;
    }
}

#if defined(PULP_HAS_SKIA)

TEST_CASE("PluginViewHost (mac GPU) — set_design_viewport renders + inverse-maps",
          "[plugin-view-host][design-viewport][mac][gpu][skia]") {
    @autoreleasepool {
        // Hidden NSWindow so the GPU host's CAMetalLayer can acquire a
        // drawable for capture_back_buffer_png. Soft-skip if no window
        // server is available (e.g. CI lane without a desktop session).
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1800, 1040)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SKIP("Cocoa window unavailable — GPU design-viewport smoke cannot run");
            return;
        }

        View root;
        root.set_requires_gpu_host(true);
        root.set_background_color(pulp::canvas::Color::rgba8(200, 50, 50));

        PluginViewHost::Options opts;
        opts.size = {1800u, 1040u};
        opts.use_gpu = true;
        auto host = PluginViewHost::create(root, opts);
        REQUIRE(host != nullptr);

        if (!host->is_gpu_backed()) {
            // Dawn/Metal adapter not available — host fell back to CPU.
            SKIP("Dawn/Metal adapter unavailable — GPU design-viewport smoke cannot run");
            [window close];
            return;
        }

        // Set the design viewport BEFORE attach so the first paint already
        // uses the transform — this is what CLAP gui_set_size will need to
        // do before -viewDidMoveToWindow fires for first paint.
        host->set_design_viewport(900.0f, 520.0f);
        host->attach_to_parent((__bridge void*)window.contentView);
        pump_run_loop(5);

        auto png = host->capture_back_buffer_png();
        INFO("captured PNG bytes: " << png.size());
        REQUIRE_FALSE(png.empty());
        REQUIRE(looks_like_png(png));

        // Inverse mapping consistent with the math (and identical to the
        // CPU host — proves both hosts share the formula).
        pulp::view::Point at_center = host->window_to_root_point({900.0f, 520.0f});
        REQUIRE(at_center.x == Approx(450.0f).margin(0.01f));
        REQUIRE(at_center.y == Approx(260.0f).margin(0.01f));

        // Resize the host — window_to_root_point must use the NEW size when
        // re-computing the inverse transform (CLAP gui_set_size path).
        host->set_size(900, 520);
        pump_run_loop(3);
        pulp::view::Point identity_after_resize = host->window_to_root_point({450.0f, 260.0f});
        REQUIRE(identity_after_resize.x == Approx(450.0f).margin(0.01f));
        REQUIRE(identity_after_resize.y == Approx(260.0f).margin(0.01f));

        host->detach();
        host.reset();
        [window close];
    }
}

#endif  // PULP_HAS_SKIA

#endif  // __APPLE__
