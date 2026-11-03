#include "layer_view_mac.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

namespace pulp::slice {

void* layer_view_create(void* parent, std::uint32_t width, std::uint32_t height) {
    NSView* host = (__bridge NSView*)parent;
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    view.wantsLayer = YES;
    [host addSubview:view];
    return (__bridge_retained void*)view;
}

void layer_view_resize(void* handle, std::uint32_t width, std::uint32_t height) {
    NSView* view = (__bridge NSView*)handle;
    [view setFrameSize:NSMakeSize(width, height)];
}

void layer_view_update(void* handle, const std::uint8_t* rgba, std::uint32_t width,
                       std::uint32_t height) {
    if (!rgba || width == 0 || height == 0)
        return;
    NSView* view = (__bridge NSView*)handle;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFDataRef data = CFDataCreate(nullptr, rgba, static_cast<CFIndex>(width) * height * 4);
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, space,
                                     kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast,
                                     provider, nullptr, false, kCGRenderingIntentDefault);
    view.layer.contents = (__bridge id)image;
    CGImageRelease(image);
    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(space);
}

void layer_view_destroy(void* handle) {
    NSView* view = (__bridge_transfer NSView*)handle;
    [view removeFromSuperview];
}

void layer_view_abandon(void* handle) {
    // The parent is gone; drop our reference without messaging it.
    NSView* view = (__bridge_transfer NSView*)handle;
    (void)view;
}

int layer_view_subview_count(void* parent) {
    NSView* host = (__bridge NSView*)parent;
    return static_cast<int>(host.subviews.count);
}

} // namespace pulp::slice
