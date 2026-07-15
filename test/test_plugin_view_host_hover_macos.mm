// macOS hover delivery for an embedded/hosted PluginViewHost.
//
// A hosted editor — a DAW, or any existing plugin embedding a Pulp view —
// parents Pulp's child NSView into a window it owns. The NSView installs a hover NSTrackingArea carrying
// NSTrackingMouseMoved, but AppKit only fans -mouseMoved: out to a tracking
// area's owner when the window itself accepts mouse-moved events — and NSWindow
// defaults that flag to NO. Nothing on the host side sets it, so -mouseMoved:
// never arrives and CSS :hover / on_hover_enter / on_hover_leave stay dead even
// though the same view tree hovers correctly in the standalone window host.
//
// The plugin view host fixes this by opting its host window into mouse-moved
// delivery when the NSView moves into a window. This suite pins both halves of
// the causal chain:
//   1. the hover dispatch itself (simulate_hover -> set_hovered -> on_hover_enter)
//      fires the registered callback, and
//   2. attaching the host's NSView into a window sets acceptsMouseMovedEvents,
//      which is the AppKit precondition for -mouseMoved: (and thus hover) to be
//      delivered at all in the hosted case.

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <AppKit/AppKit.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>

#include <memory>

using namespace pulp::view;

TEST_CASE("hover dispatch fires the registered callback",
          "[view][hover][macos]") {
    // A child that arms an on_hover_enter callback the way WidgetBridge's
    // registerHover(id) does for a scripted-UI element.
    View root;
    root.set_bounds({0, 0, 200, 120});
    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 80, 40});
    bool entered = false;
    bool left = false;
    child->on_hover_enter = [&] { entered = true; };
    child->on_hover_leave = [&] { left = true; };
    View* child_ptr = child.get();
    root.add_child(std::move(child));

    // A mouse move into the child's rect must set it hovered and fire enter.
    root.simulate_hover(pulp::view::Point{30, 25});
    REQUIRE(child_ptr->is_hovered());
    REQUIRE(entered);
    REQUIRE_FALSE(left);

    // A move back out clears hover and fires leave.
    root.simulate_hover(pulp::view::Point{150, 100});
    REQUIRE_FALSE(child_ptr->is_hovered());
    REQUIRE(left);
}

TEST_CASE("hosted plugin view opts its window into mouse-moved delivery",
          "[view][hover][macos]") {
    @autoreleasepool {
        View root;
        root.set_bounds({0, 0, 320, 200});

        auto host = PluginViewHost::create(root, PluginViewHost::Size{320, 200});
        REQUIRE(host != nullptr);

        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 320, 200)
                      styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO];
        REQUIRE(window != nil);
        REQUIRE(window.contentView != nil);

        // A foreign host's freshly created window does not accept mouse-moved
        // events — this is the AppKit default the bug rode on.
        REQUIRE_FALSE(window.acceptsMouseMovedEvents);

        // Parent Pulp's child NSView into the host window — the same
        // addSubview: a DAW (or any embedding framework's NSView wrapper) does.
        host->attach_to_parent((__bridge void*) window.contentView);
        REQUIRE(host->is_attached());

        // Moving into the window must have opted it into mouse-moved delivery, so
        // the hover tracking area's -mouseMoved: (and hence simulate_hover) can
        // actually fire in the hosted editor.
        REQUIRE(window.acceptsMouseMovedEvents);

        host->detach();
    }
}

#else

#include <catch2/catch_test_macros.hpp>

// Sentinel so the target exists and ctest output stays stable off macOS.
TEST_CASE("plugin view host hover is macOS-only", "[view][hover]") {
    SUCCEED("macOS-only suite");
}

#endif
