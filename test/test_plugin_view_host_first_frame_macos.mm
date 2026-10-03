// The first thing a DAW shows of a plug-in editor is the plug-in's own colour.
//
// A host-embedded editor opens view-first: the host gets a sized view at once
// and the document mounts a couple of frames later. Until then the window
// server composites the view's backing layer, and the first presented frames
// paint an empty tree. Both used to be a fixed framework colour, so every
// plug-in opened on the same navy before its own UI appeared. These tests pin
// the contract on both macOS plug-in hosts: the declared background
// (PluginViewHost::Options::background_rgb) is the backing layer's colour and
// every pixel of a frame painted before the document mounts.

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace pulp::view;

namespace {

constexpr std::uint32_t kDeclared = 0x05070A;

bool near_channel(int a, int b) { return std::abs(a - b) <= 1; }

bool near_rgb(int r, int g, int b, std::uint32_t rgb) {
    return near_channel(r, int((rgb >> 16) & 0xff)) && near_channel(g, int((rgb >> 8) & 0xff)) &&
           near_channel(b, int(rgb & 0xff));
}

// Render a CGImage into a tightly packed sRGB RGBA buffer, so the comparison
// is in the colour space the declaration is written in.
std::vector<std::uint8_t> srgb_pixels(CGImageRef image, size_t& w, size_t& h) {
    w = CGImageGetWidth(image);
    h = CGImageGetHeight(image);
    std::vector<std::uint8_t> px(w * h * 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs,
                                             kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    return px;
}

std::size_t count_off(const std::vector<std::uint8_t>& px, std::uint32_t rgb) {
    std::size_t off = 0;
    for (std::size_t i = 0; i + 3 < px.size(); i += 4)
        if (!near_rgb(px[i], px[i + 1], px[i + 2], rgb)) ++off;
    return off;
}

// The CoreGraphics host paints in -drawRect:; caching the view's display runs
// exactly that, with no window and nothing on screen.
std::vector<std::uint8_t> cpu_frame(NSView* view, size_t& w, size_t& h) {
    NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
    return srgb_pixels(rep.CGImage, w, h);
}

bool layer_is(CALayer* layer, std::uint32_t rgb) {
    if (!layer || !layer.backgroundColor) return false;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGColorRef srgb = CGColorCreateCopyByMatchingToColorSpace(
        cs, kCGRenderingIntentDefault, layer.backgroundColor, nullptr);
    CGColorSpaceRelease(cs);
    if (!srgb) return false;
    const CGFloat* c = CGColorGetComponents(srgb);
    const bool ok = CGColorGetNumberOfComponents(srgb) >= 3 &&
                    near_rgb(int(c[0] * 255.0 + 0.5), int(c[1] * 255.0 + 0.5),
                             int(c[2] * 255.0 + 0.5), rgb);
    CGColorRelease(srgb);
    return ok;
}

PluginViewHost::Options options(bool use_gpu, std::uint32_t background) {
    PluginViewHost::Options o;
    o.size = {64, 40};
    o.use_gpu = use_gpu;
    o.background_rgb = background;
    return o;
}

}  // namespace

TEST_CASE("CPU plug-in host: a frame before the document mounts is the declared background",
          "[plugin-view-host][first-frame][macos]") {
    [NSApplication sharedApplication];
    View root;  // a view-first editor whose document has not mounted yet
    auto host = PluginViewHost::create(root, options(false, kDeclared));
    REQUIRE(host);
    NSView* view = (__bridge NSView*)host->native_handle();
    REQUIRE(view != nil);

    size_t w = 0, h = 0;
    const auto px = cpu_frame(view, w, h);
    REQUIRE(w > 0);
    REQUIRE(h > 0);
    CHECK(count_off(px, kDeclared) == 0);

    // Control: the same host without a declaration paints the documented
    // default, so the pass above is the option's doing, not a coincidence.
    View plain_root;
    auto plain = PluginViewHost::create(plain_root, options(false, kEditorHostClearRgb));
    const auto plain_px = cpu_frame((__bridge NSView*)plain->native_handle(), w, h);
    CHECK(count_off(plain_px, kEditorHostClearRgb) == 0);
    CHECK(count_off(plain_px, kDeclared) == plain_px.size() / 4);
}

#ifdef PULP_HAS_SKIA
TEST_CASE("GPU plug-in host: backing layer and first frame are the declared background",
          "[plugin-view-host][first-frame][macos][gpu]") {
    [NSApplication sharedApplication];
    View root;
    auto host = PluginViewHost::create(root, options(true, kDeclared));
    REQUIRE(host);
    if (host->gpu_surface() == nullptr) SKIP("no Dawn/Metal adapter in this process");

    // Before any Metal frame the window server composites the backing layer,
    // so its colour is the very first thing a DAW shows.
    NSView* view = (__bridge NSView*)host->native_handle();
    REQUIRE(view != nil);
    CHECK(layer_is(view.layer, kDeclared));

    // The first presented frame paints an empty tree: all background.
    const auto png = host->capture_back_buffer_png();
    REQUIRE_FALSE(png.empty());
    NSBitmapImageRep* rep = [NSBitmapImageRep
        imageRepWithData:[NSData dataWithBytes:png.data() length:png.size()]];
    REQUIRE(rep != nil);
    size_t w = 0, h = 0;
    const auto px = srgb_pixels(rep.CGImage, w, h);
    REQUIRE(w > 0);
    CHECK(count_off(px, kDeclared) == 0);

    // Control: an undeclared host still seeds the documented default.
    View plain_root;
    auto plain = PluginViewHost::create(plain_root, options(true, kEditorHostClearRgb));
    REQUIRE(plain);
    CHECK(layer_is(((__bridge NSView*)plain->native_handle()).layer, kEditorHostClearRgb));
    CHECK_FALSE(layer_is(((__bridge NSView*)plain->native_handle()).layer, kDeclared));
}
#endif

#endif  // TARGET_OS_OSX
