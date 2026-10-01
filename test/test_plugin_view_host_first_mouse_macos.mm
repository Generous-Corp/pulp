// A press into a hosted plug-in editor whose window is not key must reach the
// control under it.
//
// AppKit gives a mouse-down in a non-key window to the view only when that view
// answers YES to -acceptsFirstMouse:; otherwise the press just makes the window
// key. A DAW keeps its own windows key most of the time, so without the
// override the first click after any interaction in the host was swallowed and
// the user's retry, usually elsewhere on the control, worked. That reads as a
// control that responds on part of its surface and not the rest.
//
// This drives the press through -[NSWindow sendEvent:], the path that applies
// the first-mouse rule. Calling -mouseDown: on the view directly would skip it.

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <AppKit/AppKit.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>

#include <memory>

using namespace pulp::view;

namespace {

class ClickTarget final : public View {
public:
    bool wants_mouse_input() const override { return true; }
};

void pump(double seconds) {
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([until timeIntervalSinceNow] > 0) {
        @autoreleasepool {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event) [NSApp sendEvent:event];
        }
    }
}

NSWindow* make_window(NSRect frame) {
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:NSWindowStyleMaskTitled
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.alphaValue = 0.0;  // routed by AppKit, painted nowhere
    window.releasedWhenClosed = NO;
    return window;
}

void send_click(NSWindow* window, NSView* view, NSPoint local) {
    const NSPoint at = [view convertPoint:local toView:nil];
    for (NSEventType type : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp}) {
        NSEvent* event = [NSEvent mouseEventWithType:type
                                            location:at
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:window.windowNumber
                                             context:nil
                                         eventNumber:0
                                          clickCount:1
                                            pressure:type == NSEventTypeLeftMouseUp ? 0.0 : 1.0];
        [window sendEvent:event];
        pump(0.02);
    }
}

void check_first_press_lands(bool use_gpu) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp activateIgnoringOtherApps:YES];

    View root;
    root.set_bounds({0, 0, 320, 200});
    auto child = std::make_unique<ClickTarget>();
    // Size through flex: the host lays the tree out, and a manual bounds on a
    // child without a flex size collapses to 0x0 on the first layout pass.
    child->set_bounds({0, 0, 320, 200});
    child->flex().preferred_width = 320.0f;
    child->flex().preferred_height = 200.0f;
    int clicks = 0;
    child->on_click = [&] { ++clicks; };
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    PluginViewHost::Options options;
    options.size = {320, 200};
    options.use_gpu = use_gpu;
    auto host = PluginViewHost::create(root, options);
    REQUIRE(host != nullptr);

    NSWindow* editor_window = make_window(NSMakeRect(40, 40, 320, 200));
    host->attach_to_parent((__bridge void*) editor_window.contentView);
    NSView* native_view = (__bridge NSView*) host->native_handle();
    REQUIRE(native_view != nil);
    [editor_window orderFront:nil];

    CHECK([native_view acceptsFirstMouse:nil]);
    // Control: the tree itself resolves a press here to the target, so a
    // miss below is AppKit's routing, not the view tree.
    REQUIRE(root.hit_test({160.0f, 100.0f}) == child_ptr);
    // The host's own window takes key, as a DAW's arrange window does.
    NSWindow* host_window = make_window(NSMakeRect(420, 40, 120, 80));
    for (int press = 0; press < 3; ++press) {
        [host_window makeKeyAndOrderFront:nil];
        pump(0.1);
        // Control: the editor window is really not key, so this press is the
        // one AppKit gates on -acceptsFirstMouse:.
        REQUIRE(NSApp.keyWindow != editor_window);
        const int before = clicks;
        send_click(editor_window, native_view, NSMakePoint(160, 30 + press * 60));
        CHECK(clicks == before + 1);
    }

    [host_window orderOut:nil];
    [editor_window orderOut:nil];
    host->detach();
}

}  // namespace

TEST_CASE("a press in a non-key hosted editor window reaches the control (CPU host)",
          "[view][first-mouse][macos]") {
    @autoreleasepool {
        check_first_press_lands(/*use_gpu=*/false);
    }
}

TEST_CASE("a press in a non-key hosted editor window reaches the control (GPU host)",
          "[view][first-mouse][macos]") {
    @autoreleasepool {
        check_first_press_lands(/*use_gpu=*/true);
    }
}

#else

#include <catch2/catch_test_macros.hpp>

// Sentinel so the target exists and ctest output stays stable off macOS.
TEST_CASE("plugin view host first mouse is macOS-only", "[view][first-mouse]") {
    SKIP("macOS-only suite: AppKit first-mouse routing does not exist here");
}

#endif
