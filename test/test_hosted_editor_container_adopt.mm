// editor_container_adopt_view: inserting an editor view that a plug-in RETURNS
// (AU Cocoa UI) into the shared hosted-editor container, as opposed to the
// parent-consuming CLAP / VST3 path. Apple-only + real NSViews: the container
// is a real NSView and adoption is an AppKit subview insertion.
//
// The AU Cocoa-UI negotiation itself (bundle load + uiViewForAudioUnit:) needs a
// real Audio Unit with a view and is proven by the #12 real-DAW smoke; this test
// covers the container-adoption logic that negotiation feeds into.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/hosted_editor_container.hpp>

#import <AppKit/AppKit.h>

using pulp::host::create_editor_container;
using pulp::host::destroy_editor_container;
using pulp::host::editor_container_adopt_view;
using pulp::host::resize_editor_container;

TEST_CASE("editor_container_adopt_view fills the container with the returned view",
          "[host][hosted-editor][au][issue-10]") {
    @autoreleasepool {
        NSView* parent =
            [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1024, 768)] autorelease];
        void* container = create_editor_container((__bridge void*) parent, 400, 300);
        REQUIRE(container != nullptr);
        NSView* c = (__bridge NSView*) container;

        NSView* child =
            [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)] autorelease];
        REQUIRE(editor_container_adopt_view(container, (__bridge void*) child, 400, 300));

        // Adopted as a subview, sized to fill, set to track the container.
        REQUIRE([[c subviews] containsObject:child]);
        REQUIRE(child.frame.size.width == 400.0);
        REQUIRE(child.frame.size.height == 300.0);
        REQUIRE((child.autoresizingMask & NSViewWidthSizable) != 0);
        REQUIRE((child.autoresizingMask & NSViewHeightSizable) != 0);
        REQUIRE([c autoresizesSubviews] == YES);

        // Resizing the container carries the adopted view with it (autoresize).
        resize_editor_container(container, 800, 600);
        REQUIRE(c.frame.size.width == 800.0);
        REQUIRE(child.frame.size.width == 800.0);
        REQUIRE(child.frame.size.height == 600.0);

        destroy_editor_container(container);
        REQUIRE_FALSE([[parent subviews] containsObject:c]);
    }
}

TEST_CASE("editor_container_adopt_view rejects null arguments",
          "[host][hosted-editor][au][issue-10]") {
    @autoreleasepool {
        NSView* parent =
            [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)] autorelease];
        void* container = create_editor_container((__bridge void*) parent, 50, 50);
        REQUIRE(container != nullptr);
        NSView* child =
            [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)] autorelease];

        REQUIRE_FALSE(editor_container_adopt_view(nullptr, (__bridge void*) child, 50, 50));
        REQUIRE_FALSE(editor_container_adopt_view(container, nullptr, 50, 50));
        // A rejected adoption must not have inserted anything.
        REQUIRE([[(__bridge NSView*) container subviews] count] == 0u);

        destroy_editor_container(container);
    }
}
