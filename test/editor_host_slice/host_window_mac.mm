#include "host_window_mac.hpp"

#import <AppKit/AppKit.h>

namespace pulp::slice {

void host_app_init() {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
}

void* host_window_create(std::uint32_t width, std::uint32_t height) {
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.releasedWhenClosed = NO;
    // Never shown: the editor attaches to a real view in a real window, but
    // nothing appears on screen.
    [window orderOut:nil];
    return (__bridge_retained void*)window;
}

void* host_window_content_view(void* window) {
    return (__bridge void*)((__bridge NSWindow*)window).contentView;
}

void host_window_resize(void* window, std::uint32_t width, std::uint32_t height) {
    [(__bridge NSWindow*)window setContentSize:NSMakeSize(width, height)];
}

void host_window_destroy(void* window) {
    NSWindow* w = (__bridge_transfer NSWindow*)window;
    [w close];
}

int host_view_subview_count(void* view) {
    return static_cast<int>(((__bridge NSView*)view).subviews.count);
}

void host_pump_events() {
    @autoreleasepool {
        for (;;) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (!event)
                break;
            [NSApp sendEvent:event];
        }
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
    }
}

} // namespace pulp::slice
