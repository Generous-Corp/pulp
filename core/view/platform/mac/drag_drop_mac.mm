#include <pulp/view/drag_drop.hpp>

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#include <unordered_map>

// Store active drop targets
static std::unordered_map<void*, pulp::view::DropTarget*> g_drop_targets;

// ── PulpDropView: handles NSDraggingDestination ──────────────────────────────

@interface PulpDropHelper : NSObject
+ (void)registerView:(NSView*)view;
@end

@implementation PulpDropHelper

+ (void)registerView:(NSView*)view {
    [view registerForDraggedTypes:@[
        NSPasteboardTypeFileURL,
        NSPasteboardTypeString
    ]];
}

@end

namespace pulp::view {

static DropData extract_drop_data(id<NSDraggingInfo> info) {
    DropData data;
    NSPasteboard* pb = [info draggingPasteboard];

    // Check for file URLs
    NSArray<NSURL*>* urls = [pb readObjectsForClasses:@[[NSURL class]]
                             options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    if (urls.count > 0) {
        data.type = DropData::Type::files;
        for (NSURL* url in urls) {
            data.file_paths.push_back(std::string([[url path] UTF8String]));
        }
        return data;
    }

    // Check for text
    NSString* text = [pb stringForType:NSPasteboardTypeString];
    if (text) {
        data.type = DropData::Type::text;
        data.text = std::string([text UTF8String]);
        return data;
    }

    return data;
}

bool register_drop_target(void* native_view, DropTarget& target) {
    @autoreleasepool {
        NSView* view = (__bridge NSView*)native_view;
        if (!view) return false;

        [PulpDropHelper registerView:view];
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
}

#endif
