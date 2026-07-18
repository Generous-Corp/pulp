// macOS host-owned editor container. See hosted_editor_container.hpp for why
// the container exists and why it is inserted into the parent window at
// creation rather than at attach time.
//
// core/host is compiled WITHOUT ARC, so every view here is retained and
// released by hand: -[NSView alloc] yields +1, -addSubview: takes its own
// reference, and destroy_editor_container() drops both.

#include <pulp/host/hosted_editor_container.hpp>
#include <pulp/runtime/log.hpp>

#import <AppKit/AppKit.h>

namespace pulp::host {

namespace {

/// Resolve the content view of whatever the WindowHost handed us. A standalone
/// WindowHost reports an NSWindow*; a caller that already has a content NSView
/// may pass that instead, so accept both rather than silently failing on one.
NSView* resolve_parent_view(void* parent_window) {
    if (!parent_window) return nil;
    id obj = (__bridge id) parent_window;
    if ([obj isKindOfClass:[NSWindow class]]) {
        return [(NSWindow*) obj contentView];
    }
    if ([obj isKindOfClass:[NSView class]]) {
        return (NSView*) obj;
    }
    return nil;
}

} // namespace

void* create_editor_container(void* parent_window, uint32_t width, uint32_t height) {
    if (![NSThread isMainThread]) {
        runtime::log_error("hosted editor: create_editor_container off the main thread");
        return nullptr;
    }
    NSView* parent = resolve_parent_view(parent_window);
    if (!parent) return nullptr;

    NSRect frame = NSMakeRect(0.0, 0.0, static_cast<CGFloat>(width), static_cast<CGFloat>(height));
    NSView* container = [[NSView alloc] initWithFrame:frame];
    if (!container) return nullptr;

    // A plugin editor sizes itself against the container, so the container must
    // not resize its own subviews.
    [container setAutoresizesSubviews:NO];
    [parent addSubview:container];

    return (__bridge void*) container;
}

void resize_editor_container(void* container, uint32_t width, uint32_t height) {
    if (!container) return;
    if (![NSThread isMainThread]) {
        runtime::log_error("hosted editor: resize_editor_container off the main thread");
        return;
    }
    NSView* view = (__bridge NSView*) container;
    NSRect frame = [view frame];
    frame.size = NSMakeSize(static_cast<CGFloat>(width), static_cast<CGFloat>(height));
    [view setFrame:frame];
}

void destroy_editor_container(void* container) {
    if (!container) return;
    if (![NSThread isMainThread]) {
        runtime::log_error("hosted editor: destroy_editor_container off the main thread");
        return;
    }
    NSView* view = (__bridge NSView*) container;
    [view removeFromSuperview];
    [view release];
}

double editor_container_scale(void* parent_window) {
    if (!parent_window) return 1.0;
    id obj = (__bridge id) parent_window;
    NSWindow* window = nil;
    if ([obj isKindOfClass:[NSWindow class]]) {
        window = (NSWindow*) obj;
    } else if ([obj isKindOfClass:[NSView class]]) {
        window = [(NSView*) obj window];
    }
    if (!window) return 1.0;
    const CGFloat scale = [window backingScaleFactor];
    return scale > 0.0 ? static_cast<double>(scale) : 1.0;
}

} // namespace pulp::host
