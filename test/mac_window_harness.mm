// Mac-only Catch2 test harness — implementation. See header for the contract.
//
// issue #2001 — provides a hidden NSWindow + CAMetalLayer fixture that
// reuses the production WindowHost path. The "trick" is twofold:
//
//   1. WindowOptions{ initially_hidden=true, use_gpu=true } gets the host
//      to construct a real NSWindow + CAMetalLayer + render::GpuSurface
//      without ever calling makeKeyAndOrderFront / activateIgnoringOtherApps
//      (those only fire from `run_event_loop()`, which the harness never
//      invokes — construction alone is sufficient).
//
//   2. PulpView::mouseUp: defers the click handler via dispatch_async to
//      the main queue, so the harness MUST drain the main run loop after
//      every synthetic event or click-driven state changes will appear
//      "missing" relative to a subsequent capture / assertion.

#import "mac_window_harness.hpp"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <pulp/view/view.hpp>
#import <pulp/view/window_host.hpp>

namespace {

// Drain the main run loop so any pending dispatch_async work (notably
// PulpView's deferred click handler) settles before the caller asserts.
// Spins the run loop briefly with a near-zero deadline rather than
// sleeping; this is the lightest-weight reliable settle on AppKit.
void drain_main_queue_once() {
    @autoreleasepool {
        [[NSRunLoop currentRunLoop]
            runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.0]];
    }
}

// Build an NSEvent for the given simulated mouse event. Coordinates are
// converted from Pulp top-left logical coords to Cocoa bottom-left
// window coords using the host's content size.
NSEvent* build_event(NSWindow* window,
                     pulp::view::WindowHost::ContentSize content,
                     const pulp::test::mac::SimulatedMouse& ev) {
    if (!window || content.width == 0 || content.height == 0) return nil;

    NSPoint location = NSMakePoint(
        static_cast<CGFloat>(ev.x),
        // top-left → bottom-left flip
        static_cast<CGFloat>(static_cast<float>(content.height) - ev.y));

    NSEventType type;
    switch (ev.phase) {
        case pulp::test::mac::SimulatedMouse::Phase::down:    type = NSEventTypeLeftMouseDown;    break;
        case pulp::test::mac::SimulatedMouse::Phase::up:      type = NSEventTypeLeftMouseUp;      break;
        case pulp::test::mac::SimulatedMouse::Phase::move:    type = NSEventTypeMouseMoved;       break;
        case pulp::test::mac::SimulatedMouse::Phase::drag:    type = NSEventTypeLeftMouseDragged; break;
        case pulp::test::mac::SimulatedMouse::Phase::scroll:  type = NSEventTypeScrollWheel;      break;
    }

    if (ev.button == pulp::view::MouseButton::right) {
        if (type == NSEventTypeLeftMouseDown)    type = NSEventTypeRightMouseDown;
        if (type == NSEventTypeLeftMouseUp)      type = NSEventTypeRightMouseUp;
        if (type == NSEventTypeLeftMouseDragged) type = NSEventTypeRightMouseDragged;
    } else if (ev.button == pulp::view::MouseButton::middle) {
        if (type == NSEventTypeLeftMouseDown)    type = NSEventTypeOtherMouseDown;
        if (type == NSEventTypeLeftMouseUp)      type = NSEventTypeOtherMouseUp;
        if (type == NSEventTypeLeftMouseDragged) type = NSEventTypeOtherMouseDragged;
    }

    if (type == NSEventTypeScrollWheel) {
        // Codex review (PR #2009): `+[NSEvent mouseEventWithType:]` does
        // NOT carry scrolling deltas — the produced event reports
        // `scrollingDeltaX/Y == 0`, so the synthetic scroll falls
        // through `PulpView::scrollWheel:` as a no-op and tests pass
        // while exercising nothing. Build the event via a CGEvent
        // source so `event.scrollingDeltaX/Y` reflect the requested
        // deltas. Axis order: CGEventCreateScrollWheelEvent2's
        // variadic wheel args are (wheel1, wheel2) ≡ (Y, X).
        CGEventRef cge = CGEventCreateScrollWheelEvent2(
            /* source */ NULL,
            kCGScrollEventUnitPixel,
            /* wheelCount */ 2,
            static_cast<int32_t>(ev.scroll_delta_y),
            static_cast<int32_t>(ev.scroll_delta_x),
            0);
        if (!cge) return nil;
        NSEvent* event = [NSEvent eventWithCGEvent:cge];
        CFRelease(cge);
        return event;
    }

    return [NSEvent
        mouseEventWithType:type
                  location:location
             modifierFlags:0
                 timestamp:[[NSProcessInfo processInfo] systemUptime]
              windowNumber:[window windowNumber]
                   context:nil
               eventNumber:0
                clickCount:ev.click_count
                  pressure:(ev.phase == pulp::test::mac::SimulatedMouse::Phase::down ? 1.0f : 0.0f)];
}

} // namespace

namespace pulp::test::mac {

std::unique_ptr<pulp::view::WindowHost>
make_test_window(pulp::view::View& root, pulp::view::WindowOptions options) {
    @autoreleasepool {
        // Ensure NSApplication exists so NSWindow construction has a
        // running app to attach to. Safe to call repeatedly.
        (void)[NSApplication sharedApplication];

        // Force the harness contract: GPU host, never visible.
        options.use_gpu = true;
        options.initially_hidden = true;
        if (options.width <= 0)  options.width  = 320;
        if (options.height <= 0) options.height = 240;

        auto host = pulp::view::WindowHost::create(root, options);
        if (!host)                                return nullptr;
        if (!host->native_window_handle())        return nullptr;
        if (!host->native_content_view_handle())  return nullptr;
        if (!host->gpu_surface())                 return nullptr;

        // Make the content view first responder so synthetic input
        // routes correctly without depending on the window having been
        // shown / made key.
        NSWindow* window = (__bridge NSWindow*)host->native_window_handle();
        NSView*   view   = (__bridge NSView*)host->native_content_view_handle();
        if (window && view) {
            [window setInitialFirstResponder:view];
            (void)[window makeFirstResponder:view];
        }

        return host;
    }
}

bool simulate_mouse(pulp::view::WindowHost& host, const SimulatedMouse& event) {
    @autoreleasepool {
        NSWindow* window = (__bridge NSWindow*)host.native_window_handle();
        NSView*   view   = (__bridge NSView*)host.native_content_view_handle();
        if (!window || !view) return false;

        const auto content = host.get_content_size();
        if (content.width == 0 || content.height == 0) return false;

        NSEvent* nsevent = build_event(window, content, event);
        if (!nsevent) return false;

        // Codex review (PR #2009): `build_event` correctly stamped
        // NSEventType{Right,Other}Mouse* based on `event.button`, but the
        // dispatch below previously routed every phase through
        // `mouseDown:`/`mouseUp:`/`mouseDragged:`. That meant a right-
        // click test exercised `mouseDown:` (single-click path) instead
        // of `rightMouseDown:` (context-menu path), and middle-clicks
        // never reached `otherMouseDown:`. Route by button so synthetic
        // right/middle clicks hit the matching selectors on PulpView.
        switch (event.phase) {
            case SimulatedMouse::Phase::down:
                if (event.button == pulp::view::MouseButton::right)
                    [view rightMouseDown:nsevent];
                else if (event.button == pulp::view::MouseButton::middle)
                    [view otherMouseDown:nsevent];
                else
                    [view mouseDown:nsevent];
                break;
            case SimulatedMouse::Phase::up:
                if (event.button == pulp::view::MouseButton::right)
                    [view rightMouseUp:nsevent];
                else if (event.button == pulp::view::MouseButton::middle)
                    [view otherMouseUp:nsevent];
                else
                    [view mouseUp:nsevent];
                break;
            case SimulatedMouse::Phase::move:
                [view mouseMoved:nsevent];
                break;
            case SimulatedMouse::Phase::drag:
                if (event.button == pulp::view::MouseButton::right)
                    [view rightMouseDragged:nsevent];
                else if (event.button == pulp::view::MouseButton::middle)
                    [view otherMouseDragged:nsevent];
                else
                    [view mouseDragged:nsevent];
                break;
            case SimulatedMouse::Phase::scroll:
                [view scrollWheel:nsevent];
                break;
            default:
                return false;
        }

        // Settle the deferred click handler from PulpView::mouseUp:.
        drain_main_queue_once();
        return true;
    }
}

std::vector<uint8_t> capture_back_buffer_png(pulp::view::WindowHost& host) {
    drain_main_queue_once();
    return host.capture_back_buffer_png();
}

} // namespace pulp::test::mac
