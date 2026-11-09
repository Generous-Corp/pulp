// macOS outbound file drag: an NSDraggingSession of file URLs, with a
// per-binary drag-source class that offers the requested operations and
// reports the destination's choice.

#include <pulp/platform/file_drag.hpp>
#include <pulp/platform/objc_unique_class.hpp>

#import <Cocoa/Cocoa.h>
#include <objc/runtime.h>

#include <cstring>
#include <memory>
#include <utility>

static_assert(NSDragOperationCopy == 1u, "native_drag_mask assumes AppKit's copy bit");
static_assert(NSDragOperationMove == 16u, "native_drag_mask assumes AppKit's move bit");
static_assert(NSDragOperationDelete == 32u,
              "drag_operation_from_native assumes AppKit's delete bit");

namespace pulp::platform::detail {

namespace {

// Lives exactly as long as the drag source object.
struct SourceState {
    unsigned allowed = drag_copy;
    std::function<void(FileDragEnd)> on_end;
    bool ended = false;
};

SourceState* state_of(id source) {
    return static_cast<SourceState*>(mac::instance_context((__bridge void*)source));
}

NSDragOperation source_mask(id self, SEL, NSDraggingSession*, NSDraggingContext) {
    SourceState* state = state_of(self);
    return state != nullptr ? static_cast<NSDragOperation>(native_drag_mask(state->allowed))
                            : NSDragOperationNone;
}

void session_ended(id self, SEL, NSDraggingSession*, NSPoint screen_point,
                   NSDragOperation operation) {
    SourceState* state = state_of(self);
    if (state == nullptr || state->ended)
        return;
    state->ended = true;
    if (state->on_end) {
        const float top = static_cast<float>(NSMaxY(NSScreen.screens.firstObject.frame));
        auto on_end = std::move(state->on_end);
        state->on_end = nullptr;
        on_end(FileDragEnd{drag_operation_from_native(static_cast<unsigned>(operation)),
                           static_cast<float>(screen_point.x),
                           top - static_cast<float>(screen_point.y)});
    }
    // AppKit does not keep a drag source alive for its session, so the source
    // keeps the +1 it was created with and lets go once the session is over.
    [self release];
}

void* source_class();

void source_dealloc(id self, SEL cmd) {
    delete state_of(self);
    mac::set_instance_context((__bridge void*)self, nullptr);
    // Super of the source class itself, not of the object's class, so a
    // runtime subclass (KVO) cannot make this recurse.
    using SuperDealloc = void (*)(id, SEL);
    void* super_imp = mac::super_implementation(source_class(), "dealloc");
    if (super_imp != nullptr)
        reinterpret_cast<SuperDealloc>(super_imp)(self, cmd);
}

void add_source_methods(void* cls, void*) {
    Class c = static_cast<Class>(cls);
    class_addProtocol(c, @protocol(NSDraggingSource));
    class_addMethod(c, @selector(draggingSession:sourceOperationMaskForDraggingContext:),
                    reinterpret_cast<IMP>(&source_mask), "Q@:@q");
    class_addMethod(c, @selector(draggingSession:endedAtPoint:operation:),
                    reinterpret_cast<IMP>(&session_ended), "v@:@{CGPoint=dd}Q");
    class_addMethod(c, sel_registerName("dealloc"), reinterpret_cast<IMP>(&source_dealloc), "v@:");
}

void* source_class() {
    return mac::unique_objc_class("PulpPlatformFileDragSource", (__bridge void*)[NSObject class],
                                  &add_source_methods);
}

bool is_drag_event(NSEvent* event) {
    switch (event.type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDown:
    case NSEventTypeOtherMouseDragged:
        return true;
    default:
        return false;
    }
}

} // namespace

bool drag_event_anchors(NativeViewHandle view_handle, void* event_handle) {
    NSView* view = (__bridge NSView*)view_handle;
    NSEvent* event = (__bridge NSEvent*)event_handle;
    return view != nil && event != nil && view.window != nil && is_drag_event(event) &&
           event.window == view.window;
}

void* make_drag_image(const DragImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.scale <= 0 ||
        image.rgba.size() !=
            static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4u)
        return nullptr;
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                    pixelsWide:image.width
                                                                    pixelsHigh:image.height
                                                                 bitsPerSample:8
                                                               samplesPerPixel:4
                                                                      hasAlpha:YES
                                                                      isPlanar:NO
                                                                colorSpaceName:NSDeviceRGBColorSpace
                                                                  bitmapFormat:0
                                                                   bytesPerRow:image.width * 4
                                                                  bitsPerPixel:32];
    if (rep == nil)
        return nullptr;
    std::memcpy(rep.bitmapData, image.rgba.data(), image.rgba.size());
    const NSSize points = NSMakeSize(image.width / image.scale, image.height / image.scale);
    rep.size = points;
    NSImage* result = [[[NSImage alloc] initWithSize:points] autorelease];
    [result addRepresentation:rep];
    [rep release];
    return (__bridge void*)result;
}

void* make_file_drag_source(unsigned allowed, std::function<void(FileDragEnd)> on_end) {
    void* cls = source_class();
    if (cls == nullptr)
        return nullptr;
    id source = [[static_cast<Class>(cls) alloc] init];
    auto* state = new SourceState{allowed, std::move(on_end)};
    mac::set_instance_context((__bridge void*)source, state);
    return (__bridge void*)source;
}

FileDragError begin_native_file_drag(MainThreadScope::Handle handle, NativeViewHandle view_handle,
                                     const FileDragRequest& request,
                                     std::function<void(FileDragEnd)> on_end,
                                     void* triggering_event) {
    @autoreleasepool {
        NSView* view = (__bridge NSView*)view_handle;
        if (view == nil)
            return FileDragError::unavailable;
        if (view.window == nil)
            return FileDragError::no_window;
        NSEvent* event = triggering_event != nullptr ? (__bridge NSEvent*)triggering_event
                                                     : [NSApp currentEvent];
        if (!drag_event_anchors(view_handle, (__bridge void*)event))
            return FileDragError::not_in_drag_event;

        NSImage* custom = nil;
        if (request.image) {
            custom = (__bridge NSImage*)make_drag_image(*request.image);
            if (custom == nil)
                return FileDragError::unavailable;
        }

        const NSPoint where = [view convertPoint:event.locationInWindow fromView:nil];
        NSMutableArray<NSDraggingItem*>* items = [NSMutableArray array];
        for (const auto& path : request.paths) {
            NSString* p = [NSString stringWithUTF8String:path.c_str()];
            if (p == nil)
                continue;
            NSURL* url = [NSURL fileURLWithPath:p];
            NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:url];
            NSImage* picture =
                custom != nil ? custom : [[NSWorkspace sharedWorkspace] iconForFile:p];
            const NSSize size = picture.size.width > 0 ? picture.size : NSMakeSize(32, 32);
            // Hotspot is measured from the image's top-left; views are
            // bottom-left based unless flipped.
            const CGFloat hx = custom != nil ? request.image->hotspot_x : size.width / 2;
            const CGFloat hy = custom != nil ? request.image->hotspot_y : size.height / 2;
            const CGFloat originY = view.isFlipped ? where.y - hy : where.y - (size.height - hy);
            [item setDraggingFrame:NSMakeRect(where.x - hx, originY, size.width, size.height)
                          contents:picture];
            [items addObject:item];
            [item release];
        }
        if (items.count == 0)
            return FileDragError::missing_file;

        // Report through the scope: once, on the main thread, never after it
        // is gone.
        auto report = [handle, on_end = std::move(on_end)](FileDragEnd end) {
            handle.post([on_end, end] {
                if (on_end)
                    on_end(end);
            });
        };
        id source = (__bridge id)make_file_drag_source(request.allowed, std::move(report));
        if (source == nil)
            return FileDragError::unavailable;
        [view beginDraggingSessionWithItems:items event:event source:source];
        return FileDragError::none;
    }
}

} // namespace pulp::platform::detail
