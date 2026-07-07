#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <Cocoa/Cocoa.h>

#include <catch2/catch_test_macros.hpp>

// The native-child clip masking used by NativeViewHost lives in
// window_host_mac_geometry.mm (and the plugin/window host overrides forward to
// it). Forward-declare the internal helper here — mirroring how
// test_view_host_bridge_mac.mm reaches key_code_from_ns — so the CALayer-mask
// contract is covered without pulling the private platform header.
namespace pulp::view::mac_geometry {
bool clip_child_view_in_host(NSView* container,
                             void* child_view_handle,
                             bool has_clip,
                             float x,
                             float y,
                             float width,
                             float height);
}

using pulp::view::mac_geometry::clip_child_view_in_host;

TEST_CASE("mac clip helper installs and removes a CALayer mask",
          "[view][native-view-host][mac]") {
    @autoreleasepool {
        NSView* container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 200, 200)];
        NSView* child = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
        void* child_handle = (__bridge void*)child;

        // Null / unattached inputs are rejected.
        REQUIRE_FALSE(clip_child_view_in_host(nullptr, child_handle, true, 0, 0, 10, 10));
        REQUIRE_FALSE(clip_child_view_in_host(container, nullptr, true, 0, 0, 10, 10));
        REQUIRE_FALSE(clip_child_view_in_host(container, child_handle, true, 0, 0, 10, 10));

        [container addSubview:child];

        // has_clip=true installs a mask layer sized to the local clip rect.
        REQUIRE(clip_child_view_in_host(container, child_handle, true, 10, 20, 40, 30));
        REQUIRE(child.wantsLayer);
        REQUIRE(child.layer != nil);
        REQUIRE(child.layer.mask != nil);
        REQUIRE(child.layer.mask.frame.size.width == 40.0);
        REQUIRE(child.layer.mask.frame.size.height == 30.0);

        // Re-clipping reuses the existing mask layer (no leak of stacked masks).
        CALayer* first_mask = child.layer.mask;
        REQUIRE(clip_child_view_in_host(container, child_handle, true, 0, 0, 50, 50));
        REQUIRE(child.layer.mask == first_mask);
        REQUIRE(child.layer.mask.frame.size.width == 50.0);

        // has_clip=false removes the mask.
        REQUIRE(clip_child_view_in_host(container, child_handle, false, 0, 0, 0, 0));
        REQUIRE(child.layer.mask == nil);
    }
}

#endif
