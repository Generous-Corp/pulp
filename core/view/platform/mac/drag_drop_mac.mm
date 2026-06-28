#include <pulp/view/drag_drop.hpp>

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>

// Per-binary-unique ObjC class names (renames PulpFileDragSource and the PulpView
// the drag category attaches to when a shipped binary defines
// PULP_VIEW_OBJC_SUFFIX). Must precede the first reference to those classes.
#include "pulp_mac_objc_names.h"

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

namespace pulp::view {

// Translate an NSDraggingInfo pasteboard into a DropData (files take priority
// over text, matching the SDL producer's file/text split).
static DropData extract_drop_data(id<NSDraggingInfo> info) {
    DropData data;
    NSPasteboard* pb = [info draggingPasteboard];

    NSArray<NSURL*>* urls = [pb readObjectsForClasses:@[[NSURL class]]
                             options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    if (urls.count > 0) {
        data.type = DropData::Type::files;
        for (NSURL* url in urls) {
            data.file_paths.push_back(std::string([[url path] UTF8String]));
        }
        return data;
    }

    NSString* text = [pb stringForType:NSPasteboardTypeString];
    if (text) {
        data.type = DropData::Type::text;
        data.text = std::string([text UTF8String]);
        return data;
    }

    return data;
}

// Per-view hover state for an in-flight drag (one active pointer per window).
// Keyed by the PulpView pointer; entries are tiny and bounded by window count.
static DragSession& mac_drag_session(const void* view) {
    static std::unordered_map<const void*, DragSession> sessions;
    return sessions[view];
}

}  // namespace pulp::view

// PulpView's full @interface is private to window_host_mac.mm; redeclare the
// slice this category needs. These property declarations must match that file
// exactly (rootView + the design-viewport pointTransform used for input).
@interface PulpView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@property (nonatomic, copy) pulp::view::Point (^pointTransform)(pulp::view::Point);
@end

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

// A self-retaining NSDraggingSource for outbound file drags (see
// begin_file_drag below). AppKit does NOT keep the source alive for the
// session's lifetime, so the object holds a strong reference to itself and
// clears it when the session ends — scoping its lifetime exactly to the drag
// with no leak and without having to make every host NSView a dragging source.
@interface PulpFileDragSource : NSObject <NSDraggingSource>
@property (nonatomic, strong) PulpFileDragSource* keepAlive;
@end

@implementation PulpFileDragSource
- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session;
    (void)context;
    // A dropped file is copied into the destination (DAW import, Finder copy);
    // the source file (a temp export) stays put.
    return NSDragOperationCopy;
}
- (void)draggingSession:(NSDraggingSession*)session
           endedAtPoint:(NSPoint)screenPoint
              operation:(NSDragOperation)operation {
    (void)session;
    (void)screenPoint;
    (void)operation;
    self.keepAlive = nil;  // release the session-scoped self-reference
}
@end

namespace pulp::view {

bool begin_file_drag(void* native_view, const FileDragRequest& request) {
    @autoreleasepool {
        NSView* view = (__bridge NSView*)native_view;
        if (!view || request.file_paths.empty()) return false;

        // NSDraggingSession can only be anchored to the mouse event that
        // initiated it. start_file_drag() is documented as call-from-a-pointer-
        // handler, so the initiating event is NSApp.currentEvent.
        NSEvent* event = [NSApp currentEvent];
        if (!event) return false;
        switch (event.type) {
            case NSEventTypeLeftMouseDown:
            case NSEventTypeLeftMouseDragged:
            case NSEventTypeRightMouseDown:
            case NSEventTypeRightMouseDragged:
            case NSEventTypeOtherMouseDown:
            case NSEventTypeOtherMouseDragged:
                break;
            default:
                return false;  // no mouse context → can't start a drag
        }

        // Anchor the drag image at the current mouse location in the view.
        NSPoint where = [view convertPoint:event.locationInWindow fromView:nil];

        NSMutableArray<NSDraggingItem*>* items = [NSMutableArray array];
        NSWorkspace* ws = [NSWorkspace sharedWorkspace];
        NSFileManager* fm = [NSFileManager defaultManager];
        for (const auto& path : request.file_paths) {
            if (path.empty()) continue;
            NSString* p = [NSString stringWithUTF8String:path.c_str()];
            if (p.length == 0 || ![fm fileExistsAtPath:p]) continue;
            NSURL* url = [NSURL fileURLWithPath:p];
            NSDraggingItem* item =
                [[NSDraggingItem alloc] initWithPasteboardWriter:url];
            NSImage* icon = [ws iconForFile:p];
            NSSize sz = (icon && icon.size.width > 0) ? icon.size
                                                      : NSMakeSize(32, 32);
            NSRect frame = NSMakeRect(where.x - sz.width / 2.0,
                                      where.y - sz.height / 2.0,
                                      sz.width, sz.height);
            [item setDraggingFrame:frame contents:icon];
            [items addObject:item];
        }
        if (items.count == 0) return false;  // nothing existed on disk

        PulpFileDragSource* source = [PulpFileDragSource new];
        source.keepAlive = source;  // retained until the session ends
        [view beginDraggingSessionWithItems:items event:event source:source];
        return true;
    }
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
