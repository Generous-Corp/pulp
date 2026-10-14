// drag_drop_mac_internal.h -- pasteboard extraction and per-view drag-session
// state shared by the standalone window's drag category (drag_drop_mac.mm) and
// the hosted plug-in editor's drag categories (plugin_view_host_mac_drag.mm).
//
// The helpers have internal linkage, so neither an editor binary nor a
// hot-reload host exports them. Built as separate objects, each file keeps its
// own session table; that is enough because a session is keyed by the view
// pointer and each file serves its own view classes. In the per-binary unity
// translation unit the header is included once and the table is shared.

#pragma once

#import <Cocoa/Cocoa.h>

#include <pulp/view/drag_drop.hpp>

#include <string>
#include <unordered_map>

namespace pulp::view {

// Translate an NSDraggingInfo pasteboard into a DropData (files take priority
// over text, matching the SDL producer's file/text split).
static DropData extract_drop_data(id<NSDraggingInfo> info) {
    DropData data;
    NSPasteboard* pb = [info draggingPasteboard];

    NSArray<NSURL*>* urls = [pb readObjectsForClasses:@[ [NSURL class] ]
                                              options:@{
                                                  NSPasteboardURLReadingFileURLsOnlyKey : @YES
                                              }];
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

} // namespace pulp::view
