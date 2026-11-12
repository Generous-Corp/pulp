// test_native_metal_surface.mm — pulp::native::MetalSurface, the native surface
// an editor with its own Metal renderer draws into.
//
// Each case runs in an offscreen NSWindow on the real system Metal device and
// skips, saying so, on a host with no device or no window server. Compiled with
// ARC.

#include <catch2/catch_test_macros.hpp>

#include <pulp/native/metal_surface.hpp>

#include "support/thread_progress.hpp"
#include <pulp/platform/objc_unique_class.hpp>

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

using namespace pulp::native;
using Clock = std::chrono::steady_clock;

// A window whose backing scale a test can change, standing in for a move to a
// display with another scale.
@interface PulpNativeSurfaceTestWindow : NSWindow
@property(nonatomic) CGFloat testScale;
@end
@implementation PulpNativeSurfaceTestWindow
- (CGFloat)backingScaleFactor {
    return self.testScale > 0 ? self.testScale : [super backingScaleFactor];
}
@end

namespace {

PulpNativeSurfaceTestWindow* make_window(CGFloat w = 320, CGFloat h = 200) {
    [NSApplication sharedApplication];
    auto* window =
        [[PulpNativeSurfaceTestWindow alloc] initWithContentRect:NSMakeRect(0, 0, w, h)
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
    window.releasedWhenClosed = NO;
    return window;
}

std::unique_ptr<MetalSurface> make_surface(const MetalSurfaceOptions& options = {}) {
    if (!MTLCreateSystemDefaultDevice())
        return nullptr;
    return MetalSurface::create(options);
}

#define REQUIRE_SURFACE(s)                                                                         \
    do {                                                                                           \
        if (!(s))                                                                                  \
            SKIP("no Metal device on this host; the native surface was not exercised");            \
    } while (0)

/// Clears the drawable to one colour with the client's own queue and presents
/// it through that command buffer.
bool clear_and_present(id<MTLCommandQueue> queue, MetalDrawable& drawable) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)drawable.texture();
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.4, 0.6, 1.0);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
    [enc endEncoding];
    const bool ok = drawable.present((__bridge void*)cb);
    [cb commit];
    return ok;
}

/// A client render thread that acquires and presents until told to stop.
struct RenderThread {
    MetalSurface& surface;
    id<MTLCommandQueue> queue;
    std::atomic<bool> stop{false};
    std::atomic<int> frames{0};
    std::thread thread;

    RenderThread(MetalSurface& s, id<MTLCommandQueue> q) : surface(s), queue(q) {
        thread = std::thread([this] {
            while (!stop.load()) {
                @autoreleasepool {
                    auto drawable = surface.acquire_drawable();
                    if (drawable && clear_and_present(queue, drawable))
                        frames.fetch_add(1);
                    else
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
        });
    }
    void join() {
        stop.store(true);
        if (thread.joinable())
            thread.join();
    }
    ~RenderThread() {
        join();
    }
};

/// Waits up to `seconds` for `done`, so a stalled client fails the case
/// instead of hanging the binary.
template <class Pred> bool wait_until(Pred done, double seconds = 10.0) {
    const auto deadline = Clock::now() + std::chrono::duration<double>(seconds);
    while (!done()) {
        if (Clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void pump_main(double seconds) {
    const auto until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    [[NSRunLoop currentRunLoop] runUntilDate:until];
}

} // namespace

TEST_CASE("a metal surface exposes its device, format and a binary-unique view class",
          "[native-surface][metal]") {
    @autoreleasepool {
        auto surface = make_surface();
        REQUIRE_SURFACE(surface);
        id<MTLDevice> device = (__bridge id<MTLDevice>)surface->device();
        REQUIRE(device != nil);
        NSView* view = (__bridge NSView*)surface->view();
        auto* layer = (CAMetalLayer*)view.layer;
        REQUIRE([layer isKindOfClass:[CAMetalLayer class]]);
        REQUIRE(layer.device == device);
        REQUIRE(surface->pixel_format() == MTLPixelFormatBGRA8Unorm);
        REQUIRE(layer.framebufferOnly == NO);
        REQUIRE(layer.allowsNextDrawableTimeout == YES);
        REQUIRE(surface->command_queue() != nullptr);
        const std::string expected =
            pulp::platform::mac::unique_objc_class_name("PulpNativeSurfaceView");
        REQUIRE(MetalSurface::view_class_name() == expected);
        REQUIRE(std::string(object_getClassName(view)) == expected);
        REQUIRE(expected != "PulpNativeSurfaceView");
    }
}

TEST_CASE("a metal surface refuses options Core Animation would reject",
          "[native-surface][metal]") {
    @autoreleasepool {
        if (!MTLCreateSystemDefaultDevice())
            SKIP("no Metal device on this host");
        MetalSurfaceOptions bad_format;
        bad_format.pixel_format = MTLPixelFormatR8Unorm;
        REQUIRE(MetalSurface::create(bad_format) == nullptr);
        MetalSurfaceOptions bad_count;
        bad_count.maximum_drawable_count = 4;
        REQUIRE(MetalSurface::create(bad_count) == nullptr);
        MetalSurfaceOptions srgb;
        srgb.pixel_format = MTLPixelFormatBGRA8Unorm_sRGB;
        auto surface = MetalSurface::create(srgb);
        REQUIRE(surface);
        REQUIRE(surface->pixel_format() == MTLPixelFormatBGRA8Unorm_sRGB);
    }
}

TEST_CASE("a surface attached before its parent has a window is not ready until it does",
          "[native-surface][metal][lifecycle]") {
    @autoreleasepool {
        auto surface = make_surface();
        REQUIRE_SURFACE(surface);
        auto* window = make_window();
        NSView* parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 150)];
        int window_events = 0;
        surface->set_listener([&](const SurfaceEvent& e) {
            if (e.kind == SurfaceEventKind::window_changed)
                ++window_events;
        });

        REQUIRE(surface->attach((__bridge void*)parent));
        REQUIRE_FALSE(surface->geometry().in_window);
        REQUIRE_FALSE(surface->acquire_drawable());
        REQUIRE(window_events == 0);

        [window.contentView addSubview:parent];
        REQUIRE(window_events == 1);
        const auto g = surface->geometry();
        REQUIRE(g.in_window);
        REQUIRE(g.width == 300.0);
        REQUIRE(g.height == 150.0);
        REQUIRE(g.scale > 0.0);
        REQUIRE(g.drawable_width == static_cast<std::uint32_t>(std::lround(300.0 * g.scale)));
        REQUIRE(g.drawable_height == static_cast<std::uint32_t>(std::lround(150.0 * g.scale)));
        auto drawable = surface->acquire_drawable();
        REQUIRE(drawable);
        REQUIRE(drawable.width() == g.drawable_width);
        surface->detach();
        [window close];
    }
}

TEST_CASE("a client presents its own drawables from its own thread",
          "[native-surface][metal][client-presents]") {
    @autoreleasepool {
        auto surface = make_surface();
        REQUIRE_SURFACE(surface);
        auto* window = make_window();
        REQUIRE(surface->attach((__bridge void*)window.contentView));
        // The client's own queue on the surface's device.
        id<MTLDevice> device = (__bridge id<MTLDevice>)surface->device();
        id<MTLCommandQueue> queue = [device newCommandQueue];

        bool presented = false;
        bool second_present = true;
        std::uint32_t format = 0;
        std::thread client([&] {
            @autoreleasepool {
                auto drawable = surface->acquire_drawable();
                if (!drawable)
                    return;
                format = static_cast<std::uint32_t>(
                    ((__bridge id<MTLTexture>)drawable.texture()).pixelFormat);
                presented = clear_and_present(queue, drawable);
                // A second present of the same drawable is refused.
                second_present = drawable.present();
            }
        });
        client.join();
        REQUIRE(presented);
        REQUIRE_FALSE(second_present);
        REQUIRE(format == surface->pixel_format());
        surface->detach();
        [window close];
    }
}

TEST_CASE("framebuffer_only decides whether a client can blit from the drawable",
          "[native-surface][metal]") {
    @autoreleasepool {
        auto* window = make_window(64, 64);
        for (const bool framebuffer_only : {false, true}) {
            MetalSurfaceOptions options;
            options.framebuffer_only = framebuffer_only;
            auto surface = make_surface(options);
            REQUIRE_SURFACE(surface);
            REQUIRE(surface->attach((__bridge void*)window.contentView));
            auto drawable = surface->acquire_drawable();
            REQUIRE(drawable);
            id<MTLTexture> texture = (__bridge id<MTLTexture>)drawable.texture();
            REQUIRE(texture.framebufferOnly == framebuffer_only);
            if (!framebuffer_only) {
                id<MTLDevice> device = (__bridge id<MTLDevice>)surface->device();
                const NSUInteger row = texture.width * 4;
                id<MTLBuffer> buffer = [device newBufferWithLength:row * texture.height
                                                           options:MTLResourceStorageModeShared];
                id<MTLCommandBuffer> cb =
                    [(__bridge id<MTLCommandQueue>)surface->command_queue() commandBuffer];
                id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
                [blit copyFromTexture:texture
                                 sourceSlice:0
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(0, 0, 0)
                                  sourceSize:MTLSizeMake(texture.width, texture.height, 1)
                                    toBuffer:buffer
                           destinationOffset:0
                      destinationBytesPerRow:row
                    destinationBytesPerImage:row * texture.height];
                [blit endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                REQUIRE(cb.status == MTLCommandBufferStatusCompleted);
            }
            surface->detach();
        }
        [window close];
    }
}

TEST_CASE("a backing-scale change resizes the drawable before the next one is handed out",
          "[native-surface][metal][geometry]") {
    @autoreleasepool {
        auto surface = make_surface();
        REQUIRE_SURFACE(surface);
        auto* window = make_window(200, 100);
        window.testScale = 1.0;
        NSView* content = window.contentView;
        REQUIRE(surface->attach((__bridge void*)content));
        NSView* view = (__bridge NSView*)surface->view();
        [view viewDidChangeBackingProperties];
        REQUIRE(surface->geometry().drawable_width == 200);

        window.testScale = 2.0;
        [view viewDidChangeBackingProperties];
        const auto g = surface->geometry();
        REQUIRE(g.scale == 2.0);
        REQUIRE(g.drawable_width == 400);
        REQUIRE(g.drawable_height == 200);
        auto drawable = surface->acquire_drawable();
        REQUIRE(drawable);
        REQUIRE(drawable.width() == 400);
        REQUIRE(drawable.height() == 200);
        surface->detach();
        [window close];
    }
}

TEST_CASE("in client sizing the surface never changes the drawable size itself",
          "[native-surface][metal][geometry]") {
    @autoreleasepool {
        auto* window = make_window(200, 100);
        window.testScale = 1.0;
        auto run_storm = [&](DrawableSizing sizing) {
            MetalSurfaceOptions options;
            options.sizing = sizing;
            auto surface = make_surface(options);
            if (!surface)
                return std::vector<std::uint32_t>{};
            surface->attach((__bridge void*)window.contentView);
            if (sizing == DrawableSizing::client)
                REQUIRE(surface->set_drawable_size(640, 480));
            else
                REQUIRE_FALSE(surface->set_drawable_size(640, 480));
            NSView* view = (__bridge NSView*)surface->view();
            std::vector<std::uint32_t> widths;
            for (int i = 0; i < 200; ++i) {
                [view setFrameSize:NSMakeSize(100 + i, 50 + i)];
                widths.push_back(surface->geometry().drawable_width);
            }
            surface->detach();
            return widths;
        };
        const auto client = run_storm(DrawableSizing::client);
        if (client.empty())
            SKIP("no Metal device on this host");
        for (auto w : client)
            REQUIRE(w == 640);
        // Control: automatic sizing follows every resize.
        const auto automatic = run_storm(DrawableSizing::automatic);
        REQUIRE(automatic.front() == 100);
        REQUIRE(automatic.back() == 299);
        [window close];
    }
}

TEST_CASE("a reader thread always sees one consistent geometry snapshot",
          "[native-surface][metal][geometry]") {
    @autoreleasepool {
        auto surface = make_surface();
        REQUIRE_SURFACE(surface);
        auto* window = make_window(200, 100);
        window.testScale = 2.0;
        REQUIRE(surface->attach((__bridge void*)window.contentView));
        NSView* view = (__bridge NSView*)surface->view();
        std::atomic<bool> stop{false};
        std::atomic<int> torn{0};
        std::atomic<int> reads{0};
        std::thread reader([&] {
            while (!stop.load()) {
                const auto g = surface->geometry();
                const auto w = static_cast<std::uint32_t>(std::lround(g.width * g.scale));
                const auto h = static_cast<std::uint32_t>(std::lround(g.height * g.scale));
                if (g.in_window && (w != g.drawable_width || h != g.drawable_height))
                    torn.fetch_add(1);
                reads.fetch_add(1);
            }
        });
        // The reader must be running before the resizes start, and must read
        // many times while they run, or "never torn" proves nothing. Neither
        // can be left to the scheduler: on a loaded or sanitized runner the
        // reader may not get a turn during a fixed number of resizes. So wait
        // for it to start, and keep resizing until enough reads overlapped.
        const bool reader_started =
            pulp::test::wait_for_condition([&] { return reads.load() > 0; });
        const int before = reads.load();
        constexpr int kOverlappingReads = 200;
        int resizes = 0;
        for (; resizes < 500 || (reads.load() - before < kOverlappingReads && resizes < 200000);
             ++resizes)
            [view setFrameSize:NSMakeSize(50 + (resizes % 97), 40 + (resizes % 53))];
        const int overlapped = reads.load() - before;
        stop.store(true);
        reader.join();
        INFO(resizes << " resizes, " << overlapped << " reads during them");
        REQUIRE(reader_started);
        REQUIRE(overlapped >= kOverlappingReads);
        REQUIRE(torn.load() == 0);
        surface->detach();
        [window close];
    }
}

TEST_CASE("teardown asks the client to stop before the view leaves its parent",
          "[native-surface][metal][lifecycle]") {
    @autoreleasepool {
        auto surface = make_surface();
        REQUIRE_SURFACE(surface);
        auto* window = make_window();
        REQUIRE(surface->attach((__bridge void*)window.contentView));
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)surface->command_queue();
        auto renderer = std::make_unique<RenderThread>(*surface, queue);
        REQUIRE(wait_until([&] { return renderer->frames.load() >= 3; }));

        NSView* view = (__bridge NSView*)surface->view();
        bool view_was_in_parent = false;
        surface->set_release_handler([&] {
            view_was_in_parent = view.superview == window.contentView;
            renderer->join();
        });
        surface->detach();
        REQUIRE(view_was_in_parent);
        REQUIRE(view.superview == nil);
        REQUIRE_FALSE(surface->acquire_drawable());
        [window close];
    }
}

TEST_CASE("teardown returns promptly while a client is mid-acquisition",
          "[native-surface][metal][lifecycle]") {
    // Core Animation hands this host a drawable in milliseconds even when every
    // drawable is held or queued, so a wait that reaches the bound cannot be
    // produced here. What can be shown: teardown never waits on the surface
    // itself while a client is acquiring and presenting as fast as it can.
    @autoreleasepool {
        auto surface = make_surface();
        REQUIRE_SURFACE(surface);
        auto* window = make_window(64, 64);
        REQUIRE(surface->attach((__bridge void*)window.contentView));
        auto renderer = std::make_unique<RenderThread>(
            *surface, (__bridge id<MTLCommandQueue>)surface->command_queue());
        REQUIRE(wait_until([&] { return renderer->frames.load() >= 20; }));

        surface->set_release_handler([&] { renderer->join(); });
        const auto start = Clock::now();
        surface->detach();
        const auto teardown = Clock::now() - start;
        REQUIRE(teardown < MetalSurface::drawable_wait_bound);
        const int frames = renderer->frames.load();
        REQUIRE_FALSE(surface->acquire_drawable());
        REQUIRE(renderer->frames.load() == frames);
        [window close];
    }
}

TEST_CASE("a client can rebuild its renderer on a live attachment",
          "[native-surface][metal][lifecycle]") {
    @autoreleasepool {
        auto surface = make_surface();
        REQUIRE_SURFACE(surface);
        auto* window = make_window(128, 128);
        REQUIRE(surface->attach((__bridge void*)window.contentView));
        id<MTLDevice> device = (__bridge id<MTLDevice>)surface->device();
        for (int rebuild = 0; rebuild < 20; ++rebuild) {
            id<MTLCommandQueue> queue = [device newCommandQueue];
            RenderThread renderer(*surface, queue);
            REQUIRE(wait_until([&] { return renderer.frames.load() >= 2; }));
            renderer.join();
        }
        REQUIRE(surface->attached());
        REQUIRE(surface->acquire_drawable());
        surface->detach();
        [window close];
    }
}

TEST_CASE("several surfaces in one process run independently",
          "[native-surface][metal][lifecycle]") {
    @autoreleasepool {
        auto* window = make_window(400, 100);
        std::vector<std::unique_ptr<MetalSurface>> surfaces;
        std::vector<NSView*> parents;
        for (int i = 0; i < 4; ++i) {
            auto surface = make_surface();
            REQUIRE_SURFACE(surface);
            NSView* parent = [[NSView alloc] initWithFrame:NSMakeRect(i * 100, 0, 100, 100)];
            [window.contentView addSubview:parent];
            REQUIRE(surface->attach((__bridge void*)parent));
            surfaces.push_back(std::move(surface));
            parents.push_back(parent);
        }
        std::vector<std::unique_ptr<RenderThread>> renderers;
        for (auto& s : surfaces)
            renderers.push_back(std::make_unique<RenderThread>(
                *s, (__bridge id<MTLCommandQueue>)s->command_queue()));
        for (auto& r : renderers)
            REQUIRE(wait_until([&] { return r->frames.load() >= 2; }));

        surfaces[1]->set_release_handler([&] { renderers[1]->join(); });
        surfaces[1]->detach();
        std::vector<int> before;
        for (auto& r : renderers)
            before.push_back(r->frames.load());
        // Wait (bounded, so a loaded host cannot fail it on timing alone) until
        // every surface still attached has drawn more frames.
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        auto others_advanced = [&] {
            for (std::size_t i = 0; i < renderers.size(); ++i)
                if (i != 1 && renderers[i]->frames.load() <= before[i])
                    return false;
            return true;
        };
        while (!others_advanced() && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        for (std::size_t i = 0; i < renderers.size(); ++i) {
            if (i == 1)
                REQUIRE(renderers[i]->frames.load() == before[i]);
            else
                REQUIRE(renderers[i]->frames.load() > before[i]);
        }
        for (std::size_t i = 0; i < surfaces.size(); ++i) {
            surfaces[i]->set_release_handler([&renderers, i] { renderers[i]->join(); });
            surfaces[i]->detach();
        }
        [window close];
    }
}

TEST_CASE("invalidate after the parent is gone touches nothing and is idempotent",
          "[native-surface][metal][lifecycle]") {
    auto surface = make_surface();
    REQUIRE_SURFACE(surface);
    int released = 0;
    surface->set_release_handler([&] { ++released; });
    @autoreleasepool {
        auto* window = make_window();
        NSView* parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
        [window.contentView addSubview:parent];
        REQUIRE(surface->attach((__bridge void*)parent));
        [parent removeFromSuperview];
        [window close];
        parent = nil;
        window = nil;
    }
    surface->invalidate();
    surface->invalidate();
    REQUIRE(released == 1);
    REQUIRE_FALSE(surface->attached());
    REQUIRE(surface->view() == nullptr);
    REQUIRE_FALSE(surface->acquire_drawable());
    @autoreleasepool {
        NSView* other = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
        REQUIRE_FALSE(surface->attach((__bridge void*)other));
    }
}

TEST_CASE("the surface reports when its window is shown and hidden",
          "[native-surface][metal][visibility]") {
    @autoreleasepool {
        auto surface = make_surface();
        REQUIRE_SURFACE(surface);
        auto* window = make_window();
        REQUIRE(surface->attach((__bridge void*)window.contentView));
        REQUIRE_FALSE(surface->on_screen().window_visible);
        std::vector<OnScreenState> seen;
        surface->set_listener([&](const SurfaceEvent& e) {
            if (e.kind == SurfaceEventKind::visibility_changed)
                seen.push_back(e.visibility);
        });
        [window orderFrontRegardless];
        pump_main(0.2);
        if (!window.isVisible)
            SKIP("no window server; window visibility was not exercised");
        REQUIRE(surface->on_screen().window_visible);

        surface->poll_visibility();
        REQUIRE(seen.size() == 1);
        REQUIRE(seen.back().window_visible);
        // Nothing moved, so a second poll reports nothing.
        surface->poll_visibility();
        REQUIRE(seen.size() == 1);

        [window orderOut:nil];
        pump_main(0.2);
        REQUIRE_FALSE(surface->on_screen().on_screen());
        surface->poll_visibility();
        REQUIRE(seen.size() == 2);
        REQUIRE_FALSE(seen.back().window_visible);
        REQUIRE_FALSE(seen.back().on_screen());
        surface->detach();
        [window close];
    }
}
