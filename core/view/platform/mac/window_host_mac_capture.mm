// Per-binary-unique ObjC class names (see header).
#include "pulp_mac_objc_names.h"
// window_host_mac_capture.mm — PNG / capture helpers for the macOS
// window host.
//
// Only free functions that don't touch PulpView ivars live here; PulpView's
// @implementation and ivars stay in window_host_mac.mm.

#include "window_host_mac_capture.h"

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#include <cstring>

namespace pulp::view::mac_capture {

std::vector<uint8_t> nsdata_to_bytes(NSData* data) {
    if (!data || data.length == 0) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(data.length));
    memcpy(bytes.data(), data.bytes, static_cast<size_t>(data.length));
    return bytes;
}

std::vector<uint8_t> bitmap_rep_to_png(NSBitmapImageRep* rep) {
    if (!rep) return {};
    NSData* data = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return nsdata_to_bytes(data);
}

std::vector<uint8_t> encode_rgba_to_png(const uint8_t* pixels,
                                        uint32_t pixel_w,
                                        uint32_t pixel_h,
                                        size_t row_bytes) {
    if (!pixels || pixel_w == 0 || pixel_h == 0) return {};

    CGColorSpaceRef color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!color_space) return {};

    CGBitmapInfo bitmap_info =
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
        static_cast<CGBitmapInfo>(kCGBitmapByteOrderDefault);

    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr, pixels, row_bytes * pixel_h, nullptr);
    if (!provider) {
        CGColorSpaceRelease(color_space);
        return {};
    }

    CGImageRef image = CGImageCreate(
        pixel_w, pixel_h,
        8, 32,
        row_bytes,
        color_space,
        bitmap_info,
        provider,
        nullptr,
        false,
        kCGRenderingIntentDefault);

    CGDataProviderRelease(provider);
    CGColorSpaceRelease(color_space);
    if (!image) return {};

    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:image];
    CGImageRelease(image);
    return bitmap_rep_to_png(rep);
}

std::vector<uint8_t> capture_view_cache_png(NSView* view) {
    if (!view) return {};
    NSRect bounds = [view bounds];
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return {};
    NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:bounds];
    if (!rep) return {};
    [view cacheDisplayInRect:bounds toBitmapImageRep:rep];
    return bitmap_rep_to_png(rep);
}

std::vector<uint8_t> capture_window_content_png(NSWindow* window, NSView* contentView) {
    if (!window || !contentView) return {};

    [window displayIfNeeded];
    [contentView displayIfNeeded];
    return capture_view_cache_png(contentView);
}

std::vector<uint8_t> capture_window_screencapture_png(NSWindow* window) {
    if (!window) return {};

    NSString* temp_name = [NSString stringWithFormat:@"pulp-window-capture-%@.png", NSUUID.UUID.UUIDString];
    NSString* temp_path = [NSTemporaryDirectory() stringByAppendingPathComponent:temp_name];
    NSString* window_arg = [NSString stringWithFormat:@"-l%u", static_cast<unsigned int>(window.windowNumber)];

    NSTask* task = [[NSTask alloc] init];
    task.launchPath = @"/usr/sbin/screencapture";
    task.arguments = @[ @"-x", @"-o", window_arg, temp_path ];

    @try {
        [task launch];
        [task waitUntilExit];
    } @catch (NSException*) {
        [[NSFileManager defaultManager] removeItemAtPath:temp_path error:nil];
        return {};
    }

    if (task.terminationStatus != 0) {
        [[NSFileManager defaultManager] removeItemAtPath:temp_path error:nil];
        return {};
    }

    NSData* data = [NSData dataWithContentsOfFile:temp_path];
    [[NSFileManager defaultManager] removeItemAtPath:temp_path error:nil];
    return nsdata_to_bytes(data);
}

}  // namespace pulp::view::mac_capture

#endif  // TARGET_OS_OSX
