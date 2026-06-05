// In-process snapshot of a native WKWebView (macOS) → PNG bytes.
//
// A WebView is a native NSView composited by the window server, NOT painted into
// the Pulp Skia canvas — so render_to_png / HeadlessSurface can't see it. WebKit's
// own `takeSnapshotWithConfiguration:completionHandler:` renders the page content
// in-process (no screen-recording permission needed), which is exactly what we want
// so that even WebView UIs are headlessly capturable. The async completion fires on
// the main run loop, so we pump it (bounded) until the snapshot lands.
//
// Compiled only when PULP_BUILD_WEBVIEW is on (the CMake gate also links WebKit).

#import <WebKit/WebKit.h>
#import <AppKit/AppKit.h>

#include <cstdint>
#include <vector>

namespace pulp::view::detail {

static WKWebView* find_wkwebview(NSView* view) {
    if (!view) return nil;
    if ([view isKindOfClass:[WKWebView class]]) return (WKWebView*)view;
    for (NSView* sub in view.subviews) {
        if (WKWebView* found = find_wkwebview(sub)) return found;
    }
    return nil;
}

// `ns_view_ptr` is the WebViewPanel's native_handle() (a WKWebView or its container).
std::vector<uint8_t> web_view_snapshot_png(void* ns_view_ptr) {
    if (!ns_view_ptr) return {};
    @autoreleasepool {
        NSView* view = (__bridge NSView*)ns_view_ptr;
        WKWebView* webview = find_wkwebview(view);
        if (!webview) return {};

        __block NSImage* snapshot = nil;
        __block bool done = false;
        WKSnapshotConfiguration* cfg = [[WKSnapshotConfiguration alloc] init];
        cfg.afterScreenUpdates = YES;
        [webview takeSnapshotWithConfiguration:cfg
                            completionHandler:^(NSImage* image, NSError* error) {
                              (void)error;
                              snapshot = image;
                              done = true;
                            }];

        // Pump the main run loop until the snapshot completes (bounded ~3s). The
        // caller must be on the main thread (WKWebView is main-thread-only).
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:3.0];
        while (!done && [deadline timeIntervalSinceNow] > 0) {
            [[NSRunLoop currentRunLoop]
                runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
        if (!snapshot) return {};

        CGImageRef cg = [snapshot CGImageForProposedRect:NULL context:nil hints:nil];
        if (!cg) return {};
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG
                                        properties:@{}];
        if (!png || png.length == 0) return {};
        const uint8_t* bytes = static_cast<const uint8_t*>(png.bytes);
        return std::vector<uint8_t>(bytes, bytes + png.length);
    }
}

}  // namespace pulp::view::detail
