// test_rmlui_native_metal.mm: Vellum's Metal RmlUi renderer on an
// RmlUiSurface: a frame drawn through the surface matches Vellum's
// SoftRenderer within the tolerance Vellum holds the Metal renderer to, at
// backing scale 1 and 2, and the hooks give the renderer the whole drawable
// with one target pixel per context unit. The references are written by
// pulp-rmlui-soft-reference in another process (ctest fixture). Skips, saying
// so, without a Metal device. Compiled with ARC.

#include <catch2/catch_test_macros.hpp>

#include "rmlui_metal_golden.hpp"

#include <pulp/rmlui_native/metal_renderer.hpp>

#include <vellum/rmlui/image_compare.hpp>

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
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
bool draw(const char* document, CGFloat scale, Drawn& out, std::string& failure) {
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
        // The context is already in drawable pixels; a backing scale of 2
        // must not reach the renderer as a second scale.
        REQUIRE(target.scale == 1.0f);
        REQUIRE(target.clear);
    }
}
