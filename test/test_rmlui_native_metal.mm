// test_rmlui_native_metal.mm: Vellum's Metal RmlUi renderer on an
// RmlUiSurface: a frame drawn through the surface matches Vellum's
// SoftRenderer within the tolerance Vellum holds the Metal renderer to, at
// backing scale 1 and 2, the hooks give the renderer the whole drawable
// with one target pixel per context unit, and a new surface's first frame,
// drawn before any scissor was set, is complete. The references are written by
// pulp-rmlui-soft-reference in another process (ctest fixture). Skips, saying
// so, without a Metal device. Compiled with ARC.

#include <catch2/catch_test_macros.hpp>

#include "rmlui_metal_golden.hpp"

#include <pulp/rmlui_native/metal_renderer.hpp>

#include <vellum/rmlui/image_compare.hpp>

#include <RmlUi/Core/Context.h>

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::rmlui_native;
namespace golden = pulp_rmlui_golden;

@interface PulpRmlMetalTestWindow : NSWindow
@property(nonatomic) CGFloat testScale;
@end
@implementation PulpRmlMetalTestWindow
- (CGFloat)backingScaleFactor {
    return self.testScale > 0 ? self.testScale : [super backingScaleFactor];
}
@end

namespace {

#define REQUIRE_METAL()                                                                            \
    do {                                                                                           \
        if (!MTLCreateSystemDefaultDevice())                                                       \
            SKIP("no Metal device on this host; the Metal renderer was not exercised");            \
    } while (0)

/// Copies the drawable of one requested frame into shared memory after the
/// renderer has drawn it, then hands it back as premultiplied RGBA8.
struct FrameCapture {
    std::atomic<bool> wanted{false};
    std::atomic<bool> done{false};
    id<MTLCommandBuffer> command_buffer = nil;
    id<MTLTexture> texture = nil;
    id<MTLBuffer> buffer = nil;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    RendererHooks wrap(RendererHooks inner) {
        RendererHooks hooks;
        hooks.begin_frame = [this, inner](const FrameTarget& frame) {
            command_buffer = (__bridge id<MTLCommandBuffer>)frame.command_buffer;
            texture = (__bridge id<MTLTexture>)frame.texture;
            inner.begin_frame(frame);
        };
        hooks.end_frame = [this, inner] {
            inner.end_frame();
            if (!wanted.exchange(false))
                return;
            width = static_cast<std::uint32_t>(texture.width);
            height = static_cast<std::uint32_t>(texture.height);
            buffer = [texture.device newBufferWithLength:std::size_t(width) * height * 4u
                                                 options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
            [blit copyFromTexture:texture
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(width, height, 1)
                                toBuffer:buffer
                       destinationOffset:0
                  destinationBytesPerRow:std::size_t(width) * 4u
                destinationBytesPerImage:std::size_t(width) * height * 4u];
            [blit endEncoding];
            std::atomic<bool>* flag = &done;
            [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
              flag->store(true);
            }];
        };
        return hooks;
    }

    /// The captured BGRA8 drawable as RGBA8, top row first.
    std::vector<std::uint8_t> rgba() const {
        std::vector<std::uint8_t> out(std::size_t(width) * height * 4u);
        const auto* bgra = static_cast<const std::uint8_t*>(buffer.contents);
        for (std::size_t p = 0; p < out.size() / 4u; ++p) {
            out[p * 4 + 0] = bgra[p * 4 + 2];
            out[p * 4 + 1] = bgra[p * 4 + 1];
            out[p * 4 + 2] = bgra[p * 4 + 0];
            out[p * 4 + 3] = bgra[p * 4 + 3];
        }
        return out;
    }
};

template <class Pred> bool wait_for(Pred done, double seconds = 10.0) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!done()) {
        if (std::chrono::steady_clock::now() > until)
            return false;
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    return true;
}

struct Drawn {
    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t unsupported = 0;
};

/// Opens `document` in an editor window at `scale`, lets layout settle, and
/// captures one frame drawn by Vellum's Metal renderer through the surface.
bool draw(const char* document, CGFloat scale, Drawn& out, std::string& failure,
          bool leave_density_ratio_at_one = false) {
    [NSApplication sharedApplication];
    PulpRmlMetalTestWindow* window = [[PulpRmlMetalTestWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, golden::kWidthPoints, golden::kHeightPoints)
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.releasedWhenClosed = NO;
    window.testScale = scale;

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    golden::GoldenAssets assets(PULP_RMLUI_GOLDEN_FONT);
    vellum::rmlui::MetalRenderInterface renderer(device);
    if (!renderer.valid()) {
        failure = "the Metal renderer did not build its pipelines";
        return false;
    }
    FrameCapture capture;
    RmlUiSurfaceOptions options;
    options.document = document;
    options.font_faces = {golden::kFontAsset};
    options.device = (__bridge void*)device;
    options.rate_cap_hz = 120.0;
    bool ok = false;
    {
        RmlUiSurface ui(assets, renderer, capture.wrap(metal_renderer_hooks(renderer)), options);
        if (!ui.attach((__bridge void*)window.contentView)) {
            failure = "attach failed";
        } else {
            if (ui.surface()->device() != (__bridge void*)device) {
                failure = "surface and renderer use different Metal devices";
                ui.detach();
                return false;
            }
            if (leave_density_ratio_at_one && scale > 1.0) {
                if (!ui.with_context([](Rml::Context& context) {
                        context.SetDensityIndependentPixelRatio(1.0f);
                    })) {
                    failure = "could not set the control density ratio";
                    ui.detach();
                    return false;
                }
            }
            // The surface draws on demand. Two frames first, as the reference
            // renders twice, so layout has settled; then the captured one.
            if (!wait_for([&] { return ui.frames() >= 1; })) {
                failure = "no first frame";
            } else {
                ui.request_frame();
                if (!wait_for([&] { return ui.frames() >= 2; })) {
                    failure = "no second frame";
                } else {
                    capture.wanted.store(true);
                    ui.request_frame();
                    ok = wait_for([&] { return capture.done.load(); });
                    if (!ok)
                        failure = "the captured frame never completed";
                }
            }
            ui.detach();
        }
    }
    // The window exists only to host the surface; it is never shown.
    REQUIRE_FALSE(window.isVisible);
    [window close];
    if (!ok)
        return false;
    out.rgba = capture.rgba();
    out.width = capture.width;
    out.height = capture.height;
    out.unsupported = renderer.unsupported_calls();
    return true;
}

std::vector<std::uint8_t> reference(const char* document, std::uint32_t width,
                                    std::uint32_t height) {
    std::vector<std::uint8_t> pixels;
    const auto file = std::filesystem::path(PULP_RMLUI_SOFT_REFERENCES) /
                      golden::reference_name(document, width, height);
    if (!golden::read_rgba(file, width, height, pixels))
        pixels.clear();
    return pixels;
}

// The first frame a new surface draws must be complete. A renderer that
// takes a zero-sized scissor for "no scissor set yet" drops everything on that
// frame, large images included, until something forces a redraw.
constexpr const char* kFirstFrameDocument = "first_frame.rml";
constexpr const char* kLargeImage = "large.png";
constexpr const char* kFirstFrameRml = R"(<rml>
<head>
<style>
body { width: 100%; height: 100%; margin: 0; background-color: #20242c; }
#image { position: absolute; left: 10px; top: 10px; width: 300px; height: 220px; }
#panel { position: absolute; left: 0px; top: 232px; width: 320px; height: 8px;
         background-color: #8fb3ff; }
</style>
</head>
<body><img id="image" src="large.png"/><div id="panel"/></body>
</rml>)";

// One quadrant colour each, opaque, so a quadrant's interior is exact after
// scaling.
constexpr std::uint8_t kQuadrants[4][3] = {
    {220, 40, 40}, {40, 200, 60}, {40, 80, 220}, {230, 210, 40}};

// A 512x512 PNG of four solid quadrants (top-left, top-right, bottom-left,
// bottom-right).
std::string large_png() {
    constexpr int kSize = 512;
    std::vector<std::uint8_t> rgba(std::size_t(kSize) * kSize * 4u);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const int q = (y >= kSize / 2 ? 2 : 0) + (x >= kSize / 2 ? 1 : 0);
            std::uint8_t* px = &rgba[(std::size_t(y) * kSize + x) * 4u];
            px[0] = kQuadrants[q][0];
            px[1] = kQuadrants[q][1];
            px[2] = kQuadrants[q][2];
            px[3] = 255;
        }
    }
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef bitmap = CGBitmapContextCreate(rgba.data(), kSize, kSize, 8, kSize * 4, srgb,
                                                kCGImageAlphaPremultipliedLast);
    CGImageRef image = CGBitmapContextCreateImage(bitmap);
    NSMutableData* data = [NSMutableData data];
    CGImageDestinationRef destination = CGImageDestinationCreateWithData(
        (__bridge CFMutableDataRef)data, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    CGImageDestinationAddImage(destination, image, nullptr);
    const bool ok = CGImageDestinationFinalize(destination);
    CFRelease(destination);
    CGImageRelease(image);
    CGContextRelease(bitmap);
    CGColorSpaceRelease(srgb);
    if (!ok)
        return {};
    return std::string(static_cast<const char*>(data.bytes), data.length);
}

class FirstFrameAssets final : public vellum::rmlui::AssetProvider {
  public:
    explicit FirstFrameAssets(golden::GoldenAssets& fallback)
        : fallback_(fallback), png_(large_png()) {}

    vellum::rmlui::Status load(std::string_view path, vellum::rmlui::Asset& out) override {
        out.path = std::string(path);
        if (path == kFirstFrameDocument) {
            out.bytes = kFirstFrameRml;
            return vellum::rmlui::Status::ok;
        }
        if (path == kLargeImage) {
            if (png_.empty())
                return vellum::rmlui::Status::missing_asset;
            out.bytes = png_;
            return vellum::rmlui::Status::ok;
        }
        return fallback_.load(path, out);
    }

  private:
    golden::GoldenAssets& fallback_;
    std::string png_;
};

/// Captures the very first frame a new surface draws, then the second, from
/// one surface.
bool draw_first_two(CGFloat scale, Drawn& first, Drawn& second, std::string& failure) {
    [NSApplication sharedApplication];
    PulpRmlMetalTestWindow* window = [[PulpRmlMetalTestWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, golden::kWidthPoints, golden::kHeightPoints)
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.releasedWhenClosed = NO;
    window.testScale = scale;

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    golden::GoldenAssets fonts(PULP_RMLUI_GOLDEN_FONT);
    FirstFrameAssets assets(fonts);
    vellum::rmlui::MetalRenderInterface renderer(device);
    if (!renderer.valid()) {
        failure = "the Metal renderer did not build its pipelines";
        return false;
    }
    FrameCapture capture;
    // Armed before the surface exists, so the capture is the surface's first
    // frame: nothing has drawn, and no scissor has been set.
    capture.wanted.store(true);
    RmlUiSurfaceOptions options;
    options.document = kFirstFrameDocument;
    options.font_faces = {golden::kFontAsset};
    options.device = (__bridge void*)device;
    options.rate_cap_hz = 120.0;
    bool ok = false;
    {
        RmlUiSurface ui(assets, renderer, capture.wrap(metal_renderer_hooks(renderer)), options);
        if (!ui.attach((__bridge void*)window.contentView)) {
            failure = "attach failed";
        } else if (!wait_for([&] { return capture.done.load(); })) {
            failure = "the first frame never completed";
        } else {
            first.rgba = capture.rgba();
            first.width = capture.width;
            first.height = capture.height;
            capture.done.store(false);
            capture.wanted.store(true);
            ui.request_frame();
            ok = wait_for([&] { return capture.done.load(); });
            if (!ok) {
                failure = "the second frame never completed";
            } else {
                second.rgba = capture.rgba();
                second.width = capture.width;
                second.height = capture.height;
            }
        }
        ui.detach();
    }
    // The window exists only to host the surface; it is never shown.
    REQUIRE_FALSE(window.isVisible);
    [window close];
    first.unsupported = second.unsupported = renderer.unsupported_calls();
    return ok;
}

bool near_rgb(const std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t x,
              std::uint32_t y, const std::uint8_t (&expected)[3]) {
    const std::uint8_t* px = &rgba[(std::size_t(y) * width + x) * 4u];
    for (int c = 0; c < 3; ++c)
        if (std::abs(int(px[c]) - int(expected[c])) > 2)
            return false;
    return px[3] == 255;
}

} // namespace

TEST_CASE("a frame drawn through the Metal renderer matches the soft reference",
          "[native-surface][rmlui][metal]") {
    REQUIRE_METAL();
    for (const auto& document : golden::kDocuments) {
        for (const auto scale : golden::kScales) {
            @autoreleasepool {
                INFO(document.name << " at backing scale " << scale);
                Drawn drawn;
                std::string failure;
                const bool drew = draw(document.name, scale, drawn, failure);
                INFO(failure);
                REQUIRE(drew);
                REQUIRE(drawn.width == golden::kWidthPoints * scale);
                REQUIRE(drawn.height == golden::kHeightPoints * scale);
                REQUIRE(drawn.unsupported == 0);
                const auto expected = reference(document.name, drawn.width, drawn.height);
                REQUIRE_FALSE(expected.empty());
                // The frame drew the document, not a cleared target.
                REQUIRE(golden::covered_fraction(drawn.rgba) > 0.9);
                const auto difference = vellum::rmlui::compare_rgba(drawn.rgba, expected);
                INFO("mean " << difference.mean << ", within " << difference.within);
                CHECK(vellum::rmlui::within_renderer_tolerance(difference));
            }
        }
    }
}

TEST_CASE("the comparison tells one document's frame from another's",
          "[native-surface][rmlui][metal]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Drawn drawn;
        std::string failure;
        const bool drew = draw(golden::kDocuments[0].name, 1, drawn, failure);
        INFO(failure);
        REQUIRE(drew);
        const auto other = reference(golden::kDocuments[1].name, drawn.width, drawn.height);
        REQUIRE_FALSE(other.empty());
        const auto difference = vellum::rmlui::compare_rgba(drawn.rgba, other);
        INFO("mean " << difference.mean << ", within " << difference.within);
        REQUIRE_FALSE(vellum::rmlui::within_renderer_tolerance(difference));
    }
}

TEST_CASE("the renderer is handed the whole drawable in context pixels, cleared",
          "[native-surface][rmlui][metal]") {
    REQUIRE_METAL();
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandBuffer> command_buffer = [[device newCommandQueue] commandBuffer];
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:640
                                                              height:480
                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];

        FrameTarget frame;
        frame.command_buffer = (__bridge void*)command_buffer;
        frame.texture = (__bridge void*)texture;
        frame.width = 640;
        frame.height = 480;
        frame.scale = 2.0;
        const auto target = metal_frame_target(frame);
        REQUIRE(target.command_buffer == command_buffer);
        REQUIRE(target.target == texture);
        REQUIRE(target.viewport.originX == 0.0);
        REQUIRE(target.viewport.originY == 0.0);
        REQUIRE(target.viewport.width == 640.0);
        REQUIRE(target.viewport.height == 480.0);
        REQUIRE(target.viewport.znear == 0.0);
        REQUIRE(target.viewport.zfar == 1.0);
        REQUIRE(target.scale == 1.0f);
        REQUIRE(target.clear);
    }
}

TEST_CASE("a 2x context left at a 1x density ratio fails the parity check",
          "[native-surface][rmlui][metal]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Drawn drawn;
        std::string failure;
        const auto& document = golden::kDocuments[0];
        REQUIRE(draw(document.name, 2, drawn, failure, true));
        const auto expected = reference(document.name, drawn.width, drawn.height);
        REQUIRE_FALSE(expected.empty());
        const auto difference = vellum::rmlui::compare_rgba(drawn.rgba, expected);
        INFO("mean " << difference.mean << ", within " << difference.within);
        REQUIRE_FALSE(vellum::rmlui::within_renderer_tolerance(difference));
    }
}

TEST_CASE("the first frame draws a large image fully, as the second frame does",
          "[native-surface][rmlui][metal][first-frame]") {
    REQUIRE_METAL();
    REQUIRE_FALSE(large_png().empty());
    for (const auto scale : golden::kScales) {
        @autoreleasepool {
            INFO("backing scale " << scale);
            Drawn first;
            Drawn second;
            std::string failure;
            const bool drew = draw_first_two(scale, first, second, failure);
            INFO(failure);
            REQUIRE(drew);
            REQUIRE(first.unsupported == 0);
            REQUIRE(first.width == golden::kWidthPoints * scale);
            REQUIRE(first.height == golden::kHeightPoints * scale);
            // The image's four quadrants are on the first frame. The image
            // spans context pixels (10, 10) to (310, 230); sample each
            // quadrant's interior.
            const std::uint32_t xs[2] = {10 + 75, 10 + 225};
            const std::uint32_t ys[2] = {10 + 55, 10 + 165};
            for (int q = 0; q < 4; ++q) {
                INFO("quadrant " << q);
                CHECK(near_rgb(first.rgba, first.width, xs[q % 2], ys[q / 2], kQuadrants[q]));
            }
            // And the first frame is the frame a redraw produces.
            REQUIRE(second.width == first.width);
            REQUIRE(second.height == first.height);
            const auto difference = vellum::rmlui::compare_rgba(first.rgba, second.rgba);
            INFO("mean " << difference.mean << ", within " << difference.within);
            CHECK(first.rgba == second.rgba);
        }
    }
}
