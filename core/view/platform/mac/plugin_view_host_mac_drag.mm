// plugin_view_host_mac_drag.mm -- file and text drops onto hosted plug-in
// editor views.
//
// PulpPluginView (CPU) and PulpGpuPluginView (GPU) embed the same View tree as
// the standalone PulpView and expose the same rootView + pointTransform slice,
// but they live in plugin_view_host_mac.mm, outside the standalone PulpView's
// drag-destination category in drag_drop_mac.mm. This file gives them the
// identical NSDraggingDestination behavior, routing into
// the same view-tree dispatch core. They register for dragged types from their
// own -viewDidMoveToWindow (plugin_view_host_mac.mm) through
// register_hosted_drag_types() below; this is the arrival path.
// PulpGpuPluginView (and its drag category below) only exist when the GPU host
// is compiled in — its @implementation in plugin_view_host_mac.mm sits inside
// that file's PULP_HAS_SKIA guard. A CoreGraphics-only build has no such class,
// so a category here referencing it would emit an unresolved
// _OBJC_CLASS_$_PulpGpuPluginView at link. Mirror the same guard below.

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>

// Per-binary-unique ObjC class names (renames PulpPluginView and
// PulpGpuPluginView when a shipped binary defines PULP_VIEW_OBJC_SUFFIX). Must
// precede the first reference to those classes.
#include "pulp_mac_objc_names.h"

#include "drag_drop_mac_internal.h"
#include "plugin_view_host_mac_view.h"

namespace pulp::view::mac_host {
void register_hosted_drag_types(NSView* view) {
    [view registerForDraggedTypes:@[ NSPasteboardTypeFileURL, NSPasteboardTypeString ]];
}
} // namespace pulp::view::mac_host

namespace pulp::view {
// Shared bodies so the CPU + GPU categories don't duplicate the dispatch.
static Point hosted_drop_point(NSView* v, Point (^xf)(Point), id<NSDraggingInfo> sender) {
    NSPoint p = [v convertPoint:[sender draggingLocation] fromView:nil];
    const float h = static_cast<float>(v.bounds.size.height);
    Point pt{static_cast<float>(p.x), h - static_cast<float>(p.y)}; // NSView unflipped → flip Y
    if (xf)
        pt = xf(pt); // inverse design-viewport transform (matches input hit-test)
    return pt;
}
static NSDragOperation hosted_drag_update(NSView* v, View* root, Point (^xf)(Point),
                                          id<NSDraggingInfo> sender) {
    if (!root)
        return NSDragOperationNone;
    @autoreleasepool {
        auto& session = mac_drag_session((__bridge void*)v);
        auto data = extract_drop_data(sender);
        return dispatch_drag_enter(*root, session, data, hosted_drop_point(v, xf, sender))
                   ? NSDragOperationCopy
                   : NSDragOperationNone;
    }
}
static void hosted_drag_exit(NSView* v, View* root) {
    if (root)
        dispatch_drag_exit(*root, mac_drag_session((__bridge void*)v));
}
static BOOL hosted_perform_drop(NSView* v, View* root, Point (^xf)(Point),
                                id<NSDraggingInfo> sender) {
    if (!root)
        return NO;
    @autoreleasepool {
        auto& session = mac_drag_session((__bridge void*)v);
        auto data = extract_drop_data(sender);
        return dispatch_drop(*root, session, data, hosted_drop_point(v, xf, sender)) ? YES : NO;
    }
}
} // namespace pulp::view

@interface PulpPluginView (PulpDragDrop) <NSDraggingDestination>
@end
@implementation PulpPluginView (PulpDragDrop)
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)s {
    return pulp::view::hosted_drag_update(self, self.rootView, self.pointTransform, s);
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)s {
    return pulp::view::hosted_drag_update(self, self.rootView, self.pointTransform, s);
}
- (void)draggingExited:(id<NSDraggingInfo>)s {
    (void)s;
    pulp::view::hosted_drag_exit(self, self.rootView);
}
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)s {
    (void)s;
    return YES;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)s {
    return pulp::view::hosted_perform_drop(self, self.rootView, self.pointTransform, s);
}
@end

// GPU hosted-view drag category — only when the GPU host (and thus the
// PulpGpuPluginView class) is compiled in; see the PULP_HAS_SKIA note at the
// top of this file. The shared pulp::view::hosted_* helpers stay outside the
// guard so the CPU category keeps using them in a no-Skia build.
#ifdef PULP_HAS_SKIA
@interface PulpGpuPluginView (PulpDragDrop) <NSDraggingDestination>
@end
@implementation PulpGpuPluginView (PulpDragDrop)
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)s {
    return pulp::view::hosted_drag_update(self, self.rootView, self.pointTransform, s);
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)s {
    return pulp::view::hosted_drag_update(self, self.rootView, self.pointTransform, s);
}
- (void)draggingExited:(id<NSDraggingInfo>)s {
    (void)s;
    pulp::view::hosted_drag_exit(self, self.rootView);
}
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)s {
    (void)s;
    return YES;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)s {
    return pulp::view::hosted_perform_drop(self, self.rootView, self.pointTransform, s);
}
@end
#endif // PULP_HAS_SKIA

#endif // __APPLE__
