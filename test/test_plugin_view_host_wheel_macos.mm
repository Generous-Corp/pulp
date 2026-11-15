// A plug-in editor's wheel event says whether its deltas are wheel lines or
// points. A classic mouse wheel reports line counts (one tick = 1) and a
// trackpad reports points; a view that sizes its scroll step from the delta
// alone scrolls a patch list by a fraction of a row per tick. The macOS
// plug-in host reads NSEvent.hasPreciseScrollingDeltas into
// MouseEvent::scroll_precise; these tests send real line and pixel scroll
// events through the host's -scrollWheel: and check the flag that arrives.

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <AppKit/AppKit.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>

#include <memory>

using namespace pulp::view;

namespace {

struct WheelProbe {
    int calls = 0;
    bool precise = false;
    float dy = 0.0f;
};

// Build the scroll as a CGEvent so scrollingDelta and
// hasPreciseScrollingDeltas are real, aim it at the view's centre, and hand it
// straight to the view's -scrollWheel:.
NSEvent* scroll_event(NSWindow* window, NSView* view, bool lines) {
    const NSRect in_window = [view convertRect:view.bounds toView:nil];
    const NSPoint centre_window = NSMakePoint(NSMidX(in_window), NSMidY(in_window));
    const NSRect on_screen =
        [window convertRectToScreen:NSMakeRect(centre_window.x, centre_window.y, 0, 0)];
    const CGFloat primary_height = NSScreen.screens.firstObject.frame.size.height;
    CGEventRef cg = CGEventCreateScrollWheelEvent2(
        nullptr, lines ? kCGScrollEventUnitLine : kCGScrollEventUnitPixel, 1, 1, 0, 0);
    if (!cg)
        return nil;
    CGEventSetLocation(cg, CGPointMake(on_screen.origin.x, primary_height - on_screen.origin.y));
    NSEvent* event = [NSEvent eventWithCGEvent:cg];
    CFRelease(cg);
    return event;
}

WheelProbe deliver(bool lines) {
    [NSApplication sharedApplication];
    View root;
    root.set_bounds({0, 0, 240, 160});
    auto child = std::make_unique<View>();
    child->set_bounds({0, 0, 240, 160});
    WheelProbe probe;
    child->on_pointer_event = [&probe](const MouseEvent& me) {
        if (!me.is_wheel)
            return;
        ++probe.calls;
        probe.precise = me.scroll_precise;
        probe.dy = me.scroll_delta_y;
    };
    root.add_child(std::move(child));

    PluginViewHost::Options options;
    options.size = {240, 160};
    options.use_gpu = false;
    auto host = PluginViewHost::create(root, options);
    REQUIRE(host);
    NSView* view = (__bridge NSView*)host->native_handle();
    REQUIRE(view != nil);

    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, 240, 160)
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.releasedWhenClosed = NO;
    [window.contentView addSubview:view];
    view.frame = window.contentView.bounds;

    NSEvent* event = scroll_event(window, view, lines);
    REQUIRE(event != nil);
    // Control: the synthetic event really is what it claims to be.
    REQUIRE(event.hasPreciseScrollingDeltas == !lines);
    REQUIRE(event.scrollingDeltaY != 0.0);
    [view scrollWheel:event];

    host->detach();
    host.reset();
    [window close];
    return probe;
}

} // namespace

TEST_CASE("plug-in host marks a classic wheel tick as line-based",
          "[plugin-view-host][wheel][macos]") {
    const auto probe = deliver(/*lines=*/true);
    REQUIRE(probe.calls == 1);
    CHECK_FALSE(probe.precise);
    CHECK(probe.dy != 0.0f);
}

TEST_CASE("plug-in host marks trackpad-style scrolling as precise",
          "[plugin-view-host][wheel][macos]") {
    const auto probe = deliver(/*lines=*/false);
    REQUIRE(probe.calls == 1);
    CHECK(probe.precise);
}

#endif
