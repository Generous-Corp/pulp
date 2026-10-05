// A standalone app window opens on its own colour.
//
// Wherever the window has no frame of its tree yet, it shows the window's
// backing colour, the content layer's colour (GPU host, before the first Metal
// frame and after a resize empties the drawable) and the fill under the tree.
// These used to be the platform default (white in light mode, GPU host) and
// the framework navy, so every standalone could open on a colour that was not
// its own. These tests pin WindowOptions::background_rgb on both macOS window
// hosts: it is the window's colour, the layer's colour and every pixel of a
// frame of an empty tree, and a host given no declaration paints the default.

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace pulp::view;

namespace {

constexpr std::uint32_t kDeclared = 0x05070A;

bool near_rgb(int r, int g, int b, std::uint32_t rgb) {
    return std::abs(r - int((rgb >> 16) & 0xff)) <= 1 && std::abs(g - int((rgb >> 8) & 0xff)) <= 1 &&
           std::abs(b - int(rgb & 0xff)) <= 1;
}

bool color_is(CGColorRef color, std::uint32_t rgb) {
    if (!color) return false;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGColorRef srgb = CGColorCreateCopyByMatchingToColorSpace(cs, kCGRenderingIntentDefault, color, nullptr);
    CGColorSpaceRelease(cs);
    if (!srgb) return false;
    const CGFloat* c = CGColorGetComponents(srgb);
    const bool ok = CGColorGetNumberOfComponents(srgb) >= 3 &&
                    near_rgb(int(c[0] * 255.0 + 0.5), int(c[1] * 255.0 + 0.5), int(c[2] * 255.0 + 0.5), rgb);
    CGColorRelease(srgb);
    return ok;
}

bool window_is(WindowHost& host, std::uint32_t rgb) {
    NSWindow* window = (__bridge NSWindow*)host.native_window_handle();
    return window != nil && color_is(window.backgroundColor.CGColor, rgb);
}

// Decode a PNG into sRGB RGBA and count the pixels that are not `rgb`.
// Returns -1 when there are no pixels to judge.
long count_off(const std::vector<std::uint8_t>& png, std::uint32_t rgb) {
    if (png.empty()) return -1;
    NSData* data = [NSData dataWithBytes:png.data() length:png.size()];
    NSBitmapImageRep* rep = [NSBitmapImageRep imageRepWithData:data];
    CGImageRef image = rep.CGImage;
    if (!image) return -1;
    const size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
    if (w * h == 0) return -1;
    std::vector<std::uint8_t> px(w * h * 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    long off = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4)
        if (!near_rgb(px[i], px[i + 1], px[i + 2], rgb)) ++off;
    return off;
}

WindowOptions options(bool use_gpu, std::uint32_t background) {
    WindowOptions o;
    o.title = "first-frame";
    o.width = 64;
    o.height = 40;
    o.use_gpu = use_gpu;
    o.initially_hidden = true;  // never on screen, never activates
    o.background_rgb = background;
    return o;
}

}  // namespace

TEST_CASE("window options default to the framework background", "[window-host][first-frame]") {
    CHECK(WindowOptions{}.background_rgb == kEditorHostClearRgb);
}

TEST_CASE("CPU window host: window and first frame are the declared background",
          "[window-host][first-frame][macos]") {
    [NSApplication sharedApplication];
    View root;  // nothing drawn yet: every pixel is the window's own colour
    auto host = WindowHost::create(root, options(false, kDeclared));
    REQUIRE(host);
    CHECK(window_is(*host, kDeclared));
    CHECK(count_off(host->capture_back_buffer_png(), kDeclared) == 0);

    // Control: no declaration paints the documented default, so the pass
    // above is the option's doing.
    View plain_root;
    auto plain = WindowHost::create(plain_root, options(false, kEditorHostClearRgb));
    REQUIRE(plain);
    CHECK(window_is(*plain, kEditorHostClearRgb));
    CHECK(count_off(plain->capture_back_buffer_png(), kEditorHostClearRgb) == 0);
    CHECK_FALSE(window_is(*plain, kDeclared));
}

#ifdef PULP_HAS_SKIA
TEST_CASE("GPU window host: window, backing layer and first frame are the declared background",
          "[window-host][first-frame][macos][gpu]") {
    [NSApplication sharedApplication];
    View root;
    auto host = WindowHost::create(root, options(true, kDeclared));
    REQUIRE(host);
    if (!host->is_gpu_backed()) SKIP("no Dawn/Metal adapter in this process");

    // The window and the content layer are what composite before the first
    // Metal frame lands; the platform default here was white.
    CHECK(window_is(*host, kDeclared));
    NSView* content = (__bridge NSView*)host->native_content_view_handle();
    REQUIRE(content != nil);
    CHECK(color_is(content.layer.backgroundColor, kDeclared));

    // A frame of an empty tree is the fill under the tree: all background.
    CHECK(count_off(host->capture_back_buffer_png(), kDeclared) == 0);

    View plain_root;
    auto plain = WindowHost::create(plain_root, options(true, kEditorHostClearRgb));
    REQUIRE(plain);
    NSView* plain_content = (__bridge NSView*)plain->native_content_view_handle();
    CHECK(color_is(plain_content.layer.backgroundColor, kEditorHostClearRgb));
    CHECK(count_off(plain->capture_back_buffer_png(), kEditorHostClearRgb) == 0);
}
#endif

#endif  // TARGET_OS_OSX
