#include <pulp/view/drag_drop.hpp>

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>

// Per-binary-unique ObjC class names (renames the PulpView the drag category
// attaches to when a shipped binary defines PULP_VIEW_OBJC_SUFFIX). Must
// precede the first reference to those classes.
#include "pulp_mac_objc_names.h"
// The category below extends the standalone host NSView; its interface is
// shared with the file that implements it. The hosted plug-in editor views get
// the same behavior from plugin_view_host_mac_drag.mm.
#include "drag_drop_mac_internal.h"
#include "window_host_mac_view.h"

#include <pulp/platform/file_drag.hpp>
#include <pulp/platform/main_thread.hpp>

#include <unordered_map>

// macOS native drag-drop delivery.
//
// Two layers live here, both for the macOS NSView host:
//   1. The legacy register_drop_target / DropTarget registration API (kept for
//      source compatibility; see the note in drag_drop.hpp).
//   2. The real delivery path: an NSDraggingDestination category on PulpView
//      (the host's content NSView) that extracts the pasteboard payload and
//      routes it into the cross-platform view-tree dispatch core
//      (dispatch_drag_*/dispatch_drop in drag_drop.cpp) — the same core the SDL
//      standalone host uses. PulpView registers for dragged types in its init
//      (window_host_mac.mm); this file implements what happens when a drag
//      arrives.

// Store active legacy drop targets (DropTarget registration path).
static std::unordered_map<void*, pulp::view::DropTarget*> g_drop_targets;

@interface PulpView (PulpDragDrop) <NSDraggingDestination>
@end

@implementation PulpView (PulpDragDrop)

// Register the content view for file + text drags once it joins a window. Done
// here (a category override of NSView's no-op) rather than in PulpView's init so
// the host file (window_host_mac.mm) needs no change. PulpView does not define
// this method, so this is the class's sole implementation; PulpMetalView inherits
// it. Safe to call repeatedly — AppKit treats re-registration idempotently.
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) {
        [self registerForDraggedTypes:@[
            NSPasteboardTypeFileURL,
            NSPasteboardTypeString
        ]];
    }
}

// Convert a drag's window-space location into root-view coordinates, mirroring
// PulpView's -localPoint: (NSView is not flipped → flip Y; then apply the
// inverse design-viewport transform when one is set).
- (pulp::view::Point)pulpDropPoint:(id<NSDraggingInfo>)sender {
    NSPoint p = [self convertPoint:[sender draggingLocation] fromView:nil];
    float viewHeight = static_cast<float>(self.bounds.size.height);
    pulp::view::Point pt{static_cast<float>(p.x), viewHeight - static_cast<float>(p.y)};
    if (self.pointTransform) pt = self.pointTransform(pt);
    return pt;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    return [self draggingUpdated:sender];
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    if (!self.rootView) return NSDragOperationNone;
    @autoreleasepool {
        auto& session = pulp::view::mac_drag_session((__bridge void*)self);
        auto data = pulp::view::extract_drop_data(sender);
        bool accepted = pulp::view::dispatch_drag_enter(
            *self.rootView, session, data, [self pulpDropPoint:sender]);
        return accepted ? NSDragOperationCopy : NSDragOperationNone;
    }
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
    (void)sender;
    if (!self.rootView) return;
    pulp::view::dispatch_drag_exit(
        *self.rootView, pulp::view::mac_drag_session((__bridge void*)self));
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
    (void)sender;
    return YES;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    if (!self.rootView) return NO;
    @autoreleasepool {
        auto& session = pulp::view::mac_drag_session((__bridge void*)self);
        auto data = pulp::view::extract_drop_data(sender);
        return pulp::view::dispatch_drop(*self.rootView, session, data,
                                         [self pulpDropPoint:sender])
                   ? YES
                   : NO;
    }
}

@end

namespace pulp::view {

bool begin_file_drag(void* native_view, const FileDragRequest& request) {
    if (!native_view || request.file_paths.empty())
        return false;
    // The platform layer owns the native drag: it anchors to the current mouse
    // event, drags only the files that exist, and honours a recorder installed
    // with pulp::platform::set_file_drag_recorder(), so tests and automation can
    // start this drag without a real pointer event. Nothing is reported back
    // when the drag ends, so a local scope is enough.
    pulp::platform::FileDragRequest platform_request;
    platform_request.paths = request.file_paths;
    platform_request.allowed = pulp::platform::drag_copy;
    platform_request.origin_x = request.root_position.x;
    platform_request.origin_y = request.root_position.y;
    pulp::platform::MainThreadScope scope;
    return pulp::platform::begin_file_drag(scope, native_view, platform_request, {}) ==
           pulp::platform::FileDragError::none;
}

bool register_drop_target(void* native_view, DropTarget& target) {
    @autoreleasepool {
        NSView* view = (__bridge NSView*)native_view;
        if (!view) return false;
        [view registerForDraggedTypes:@[
            NSPasteboardTypeFileURL,
            NSPasteboardTypeString
        ]];
        g_drop_targets[native_view] = &target;
        return true;
    }
}

void unregister_drop_target(void* native_view) {
    @autoreleasepool {
        NSView* view = (__bridge NSView*)native_view;
        if (view) [view unregisterDraggedTypes];
        g_drop_targets.erase(native_view);
    }
}

} // namespace pulp::view

#else // !__APPLE__

namespace pulp::view {
bool register_drop_target(void*, DropTarget&) { return false; }
void unregister_drop_target(void*) {}
bool begin_file_drag(void*, const FileDragRequest&) { return false; }
}

#endif
