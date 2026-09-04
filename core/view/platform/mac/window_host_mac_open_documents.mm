// window_host_mac_open_documents.mm — the NSApplication delegate that receives
// files the OS asks the app to open.
//
// Extracted from window_host_mac.mm: the delegate is process-global rather than
// per-window, so it is not window-host state. Declarations, and the ordering
// contract callers must honour: window_host_mac_open_documents.h.

#include "window_host_mac_open_documents.h"

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#import <Cocoa/Cocoa.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

// The installed handler, and the paths that arrived before there was one.
//
// A launch-time open is delivered around applicationDidFinishLaunching, which
// is inside [NSApp run] — so it can land before the app has installed anything.
// Dropping it would lose exactly the file that caused the launch, which is the
// common case. Held paths are flushed the moment a handler appears.
std::function<void(const std::vector<std::string>&)>& mac_open_files_handler() {
    static std::function<void(const std::vector<std::string>&)> handler;
    return handler;
}

std::vector<std::string>& mac_pending_open_files() {
    static std::vector<std::string> pending;
    return pending;
}

void mac_deliver_open_files(std::vector<std::string> paths) {
    if (paths.empty()) return;
    auto& handler = mac_open_files_handler();
    if (!handler) {
        auto& pending = mac_pending_open_files();
        pending.insert(pending.end(), paths.begin(), paths.end());
        return;
    }
    handler(paths);
}

}  // namespace

@interface PulpAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation PulpAppDelegate

- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
    (void)application;
    std::vector<std::string> paths;
    paths.reserve(static_cast<size_t>(urls.count));
    for (NSURL* url in urls) {
        // Only file URLs. Pulp declares no CFBundleURLTypes, so a non-file URL
        // cannot be routed here today; passing one through as a path would hand
        // the app something that is not a path.
        if (!url.isFileURL || url.path == nil) continue;
        paths.emplace_back([url.path UTF8String]);
    }
    mac_deliver_open_files(std::move(paths));
}

@end

namespace pulp::view::mac_open_documents {

void install_app_delegate() {
    NSApplication* app = [NSApplication sharedApplication];
    if (app.delegate != nil) return;
    static PulpAppDelegate* delegate = nil;
    if (delegate == nil) delegate = [[PulpAppDelegate alloc] init];
    app.delegate = delegate;
}

void set_open_files_handler(
    std::function<void(const std::vector<std::string>&)> handler) {
    mac_open_files_handler() = std::move(handler);
    install_app_delegate();
    auto& pending = mac_pending_open_files();
    if (pending.empty() || !mac_open_files_handler()) return;
    // Move before calling: a handler that opens a window can re-enter, and the
    // queue must already be empty by then or the same file opens twice.
    std::vector<std::string> flushed;
    flushed.swap(pending);
    mac_open_files_handler()(flushed);
}

}  // namespace pulp::view::mac_open_documents

#endif  // TARGET_OS_OSX
