#pragma once

// Format-level checks for an editor that draws into a native Metal surface
// (integrations/pulp-native), shared by the tests that open the metal-probe
// editor through one format adapter each.
//
// A test supplies a SurfaceFormatDriver: how its format opens the editor into a
// host view, resizes it and closes it. `check_native_surface_lifecycle` then
// asserts, the same way for every format, what a host sees: the editor attaches
// at the size it reports, its first frame carries the gain colour at the
// drawable's size, a host resize round trip moves the drawable and returns,
// and a close removes the surface so a reopen starts clean and shows the
// current gain. Objective-C++ with ARC; included by one translation unit per
// test executable.

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/editor_host.hpp>
#include <pulp/native/frame_pump.hpp>
#include <pulp/native/metal_surface.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/// A window that reports a 2x backing scale on any host, so the drawable size
/// is checked at 2x even on a display-less build machine.
@interface PulpNativeSurfaceTestWindow : NSWindow
@end
@implementation PulpNativeSurfaceTestWindow
- (CGFloat)backingScaleFactor {
    return 2.0;
}
@end

namespace pulp::test_support {

/// How one plug-in format opens, resizes and closes the editor.
struct SurfaceFormatDriver {
    /// Set the gain to `gain` (0 to 1) the way the host would, then open the
    /// editor into a new view inside `host_window` and return the view the
    /// surface lives under (the driver keeps it alive until `close`), or nil
    /// when the format refused. The gain is set first so the editor's first
    /// frame is the one under test.
    std::function<NSView*(NSWindow* host_window, float gain)> open;
    /// The editor the format holds open, or null.
    std::function<format::Editor*()> editor;
    /// The host sizes its view to width x height, then calls the format's own
    /// resize entry. Returns the format's answer.
    std::function<bool(std::uint32_t width, std::uint32_t height)> host_resize;
    /// The size the format reports for the open editor.
    std::function<format::EditorSize()> reported_size;
    /// The host closes the editor.
    std::function<void()> close;
    /// Run the main run loop for about `seconds`.
    std::function<void(double seconds)> run_loop;
};

inline bool metal_device_available() {
    return MTLCreateSystemDefaultDevice() != nil;
}

inline int native_surfaces_under(NSView* root) {
    int n = [NSStringFromClass([root class]) hasPrefix:@"PulpNativeSurfaceView_"] ? 1 : 0;
    for (NSView* child in root.subviews)
        n += native_surfaces_under(child);
    return n;
}

inline NSWindow* make_native_surface_window() {
    [NSApplication sharedApplication];
    NSWindow* window = [[PulpNativeSurfaceTestWindow alloc]
        initWithContentRect:NSMakeRect(-20000, -20000, 1100, 800)
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.releasedWhenClosed = NO;
    return window;
}

/// One captured frame: its size and the RGBA of one pixel.
struct CapturedFrame {
    std::size_t width = 0;
    std::size_t height = 0;
    std::uint8_t rgba[4] = {};
};

/// Capture `editor`'s frame and read the pixel at (px, py). False when the
/// editor offers no capture, the capture fails or the PNG cannot be decoded.
inline bool capture_frame(format::Editor& editor, std::size_t px, std::size_t py,
                          CapturedFrame& out) {
    auto* automation = editor.automation();
    std::vector<std::uint8_t> png;
    if (automation == nullptr || !automation->capture_png(png))
        return false;
    NSData* data = [NSData dataWithBytes:png.data() length:png.size()];
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr);
    if (!src)
        return false;
    CGImageRef image = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!image)
        return false;
    out.width = CGImageGetWidth(image);
    out.height = CGImageGetHeight(image);
    std::vector<std::uint8_t> pixels(out.width * out.height * 4);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(pixels.data(), out.width, out.height, 8, out.width * 4,
                                             space, kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(ctx, CGRectMake(0, 0, out.width, out.height), image);
    const std::size_t index = (py * out.width + px) * 4;
    for (int i = 0; i < 4; ++i)
        out.rgba[i] = pixels[index + i];
    CGContextRelease(ctx);
    CGColorSpaceRelease(space);
    CGImageRelease(image);
    return true;
}

/// The editor's frame holds the colour `gain` sets: red at 1, blue at 0, read
/// away from the triangle.
inline void require_gain_colour(format::Editor& editor, float gain, std::size_t width,
                                std::size_t height) {
    CapturedFrame frame;
    REQUIRE(capture_frame(editor, 2, 2, frame));
    INFO("captured " << frame.width << "x" << frame.height << " pixel " << int(frame.rgba[0]) << ","
                     << int(frame.rgba[1]) << "," << int(frame.rgba[2]));
    CHECK(frame.width == width);
    CHECK(frame.height == height);
    if (gain >= 1.0f) {
        CHECK(frame.rgba[0] > 235);
        CHECK(frame.rgba[2] < 20);
    } else {
        CHECK(frame.rgba[0] < 20);
        CHECK(frame.rgba[2] > 235);
    }
}

/// The lifecycle every format must give an editor with a native surface, twice
/// (the second round shows a reopen starts clean and reads the current gain).
/// The editor is 360x240.
inline void check_native_surface_lifecycle(const SurfaceFormatDriver& driver) {
    constexpr std::uint32_t kWidth = 360, kHeight = 240;
    const auto live_pumps = pulp::native::FramePump::live_tick_sources();
    const auto live_surfaces = pulp::native::MetalSurface::live_surfaces();
    NSWindow* window = make_native_surface_window();

    for (int round = 0; round < 2; ++round) {
        const float gain = round == 0 ? 1.0f : 0.0f;
        INFO("round " << round);

        @autoreleasepool {
            // Not retained here: the driver owns the view, and closing it may
            // release it.
            NSView* __unsafe_unretained parent = driver.open(window, gain);
            REQUIRE(parent != nil);
            REQUIRE(driver.editor() != nullptr);
            driver.run_loop(0.15);

            // Attached: one surface under the host's view, at the editor's size.
            REQUIRE(native_surfaces_under(parent) == 1);
            const auto size = driver.reported_size();
            CHECK(size.width == kWidth);
            CHECK(size.height == kHeight);
            REQUIRE(pulp::native::MetalSurface::live_surfaces() == live_surfaces + 1);
            REQUIRE(pulp::native::FramePump::live_tick_sources() == live_pumps + 1);

            // First frame: the drawable has the surface's size at the window's
            // scale and shows the gain.
            const double scale = window.backingScaleFactor;
            require_gain_colour(*driver.editor(), gain, static_cast<std::size_t>(kWidth * scale),
                                static_cast<std::size_t>(kHeight * scale));

            // A host resize round trip.
            REQUIRE(driver.host_resize(480, 320));
            driver.run_loop(0.15);
            CHECK(driver.reported_size().width == 480);
            CHECK(driver.reported_size().height == 320);
            require_gain_colour(*driver.editor(), gain, static_cast<std::size_t>(480 * scale),
                                static_cast<std::size_t>(320 * scale));
            REQUIRE(driver.host_resize(kWidth, kHeight));
            driver.run_loop(0.15);
            CHECK(driver.reported_size().width == kWidth);
            CHECK(driver.reported_size().height == kHeight);
            require_gain_colour(*driver.editor(), gain, static_cast<std::size_t>(kWidth * scale),
                                static_cast<std::size_t>(kHeight * scale));

            driver.close();
            // Closed: no surface left in the window, no editor.
            CHECK(native_surfaces_under(window.contentView) == 0);
            CHECK(driver.editor() == nullptr);
        }
        driver.run_loop(0.05);
        REQUIRE(pulp::native::MetalSurface::live_surfaces() == live_surfaces);
        REQUIRE(pulp::native::FramePump::live_tick_sources() == live_pumps);
    }
    [window close];
}

} // namespace pulp::test_support
