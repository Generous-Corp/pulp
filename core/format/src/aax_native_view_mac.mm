#include <pulp/format/aax_native_view.hpp>

#import <AppKit/AppKit.h>

namespace pulp::format::aax {

namespace {

AaxNativeRect to_rect(NSRect r) {
    return {static_cast<float>(r.origin.x), static_cast<float>(r.origin.y),
            static_cast<float>(r.size.width), static_cast<float>(r.size.height)};
}

} // namespace

float aax_display_scale(void* native_parent) noexcept {
    @autoreleasepool {
        NSView* view = (__bridge NSView*)native_parent;
        NSWindow* window = view.window;
        if (window)
            return static_cast<float>(window.backingScaleFactor);
        NSScreen* screen = NSScreen.mainScreen;
        return screen ? static_cast<float>(screen.backingScaleFactor) : 1.0f;
    }
}

AaxNativeGeometry aax_native_geometry(void* native_parent, void* native_child) noexcept {
    AaxNativeGeometry g;
    @autoreleasepool {
        NSView* parent = (__bridge NSView*)native_parent;
        NSView* child = (__bridge NSView*)native_child;
        if (!parent || !child)
            return g;
        g.valid = true;
        g.parent_flipped = parent.isFlipped;
        g.parent_frame = to_rect(parent.frame);
        g.parent_bounds = to_rect(parent.bounds);
        g.child_frame = to_rect(child.frame);
        if (NSWindow* window = parent.window) {
            g.has_window = true;
            g.window_frame = to_rect(window.frame);
            g.backing_scale = static_cast<float>(window.backingScaleFactor);
        } else {
            NSScreen* screen = NSScreen.mainScreen;
            g.backing_scale = screen ? static_cast<float>(screen.backingScaleFactor) : 1.0f;
        }
    }
    return g;
}

void aax_place_child_top_left(void* native_parent, void* native_child) noexcept {
    @autoreleasepool {
        NSView* parent = (__bridge NSView*)native_parent;
        NSView* child = (__bridge NSView*)native_child;
        if (!parent || !child)
            return;
        const NSRect bounds = parent.bounds;
        const NSSize size = child.frame.size;
        const CGFloat y = aax_child_top_left_y(static_cast<float>(bounds.size.height),
                                               static_cast<float>(size.height), parent.isFlipped);
        const NSRect target =
            NSMakeRect(bounds.origin.x, bounds.origin.y + y, size.width, size.height);
        if (!NSEqualRects(child.frame, target)) {
            [child setFrameOrigin:target.origin];
        }
    }
}

void aax_fill_parent(void* native_parent, void* native_child) noexcept {
    @autoreleasepool {
        NSView* parent = (__bridge NSView*)native_parent;
        NSView* child = (__bridge NSView*)native_child;
        if (!parent || !child)
            return;
        const NSRect bounds = parent.bounds;
        if (!NSEqualRects(child.frame, bounds)) {
            [child setFrame:bounds];
        }
    }
}

void aax_set_child_tracks_parent(void* native_child, bool tracks) noexcept {
    @autoreleasepool {
        NSView* child = (__bridge NSView*)native_child;
        if (!child)
            return;
        child.autoresizingMask =
            tracks ? (NSViewWidthSizable | NSViewHeightSizable) : NSViewNotSizable;
    }
}

} // namespace pulp::format::aax
