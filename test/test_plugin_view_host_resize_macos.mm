// Live resize through the macOS GPU plug-in host, driven the three ways a
// host resizes an editor:
//
//   flood        the host calls set_size() for every mouse move of a drag;
//   final-only   the host calls set_size() once, with the size it settled on;
//   native       the host resizes the NSView itself and never calls the
//                editor (Logic's AU v2 path), so -setFrameSize: is the only
//                signal.
//
// Each frame configures the swapchain at most once, the processor's resize
// notification runs at most once per frame with the latest size, and once
// the drag stops the swapchain settles at exactly the view's physical size.

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

using namespace pulp::view;

#ifdef PULP_HAS_SKIA

namespace {

using Calls = std::vector<std::pair<uint32_t, uint32_t>>;

struct Editor {
    View root;
    std::unique_ptr<PluginViewHost> host;
    NSWindow* window = nil;
    NSView* view = nil;
    Calls notified;

    // `windowed` puts the view in a composited window, which starts the
    // display link the way a DAW's editor window does.
    bool open(bool windowed) {
        [NSApplication sharedApplication];
        PluginViewHost::Options options;
        options.size = {400, 300};
        options.use_gpu = true;
        host = PluginViewHost::create(root, options);
        if (!host || !host->gpu_surface())
            return false;
        host->set_resize_callback([this](uint32_t w, uint32_t h) { notified.emplace_back(w, h); });
        view = (__bridge NSView*)host->native_handle();
        if (windowed) {
            window = [[NSWindow alloc] initWithContentRect:NSMakeRect(-4000, -4000, 400, 300)
                                                 styleMask:NSWindowStyleMaskBorderless
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
            [window.contentView addSubview:view];
            [window orderFrontRegardless];
        }
        return true;
    }

    ~Editor() {
        [view removeFromSuperview];
        [window orderOut:nil];
    }

    pulp::render::GpuSurface& gpu() {
        return *host->gpu_surface();
    }
    uint64_t configures() {
        return gpu().swapchain_stats().configure_count;
    }

    double scale() const {
        return view.window ? view.window.backingScaleFactor
                           : [NSScreen mainScreen].backingScaleFactor;
    }

    // One presented frame, as the display link would run it.
    bool frame() {
        const bool presented = !host->capture_back_buffer_png().empty();
        [CATransaction flush];
        return presented;
    }

    // Frames until the swapchain has held its size for two of them.
    bool settle() {
        for (int i = 0; i < 6; ++i) {
            if (!frame())
                return false;
            if (gpu().swapchain_stats().settled)
                return true;
        }
        return false;
    }

    bool at_physical_size(uint32_t w, uint32_t h) {
        const auto pw = static_cast<uint32_t>(w * scale());
        const auto ph = static_cast<uint32_t>(h * scale());
        return gpu().width() == pw && gpu().height() == ph;
    }
};

constexpr const char* kNoGpu = "no Dawn/Metal adapter in this process";

// The host's display link needs an active display. A session with none (ssh
// into a Mac with its display asleep) gets no link, so the host has no frame
// to hold a notification for and delivers it at once.
// CVDisplayLink is deprecated but is still the hosts' vsync source.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
bool display_link_available() {
    CVDisplayLinkRef link = nullptr;
    CVDisplayLinkCreateWithActiveCGDisplays(&link);
    if (!link)
        return false;
    CVDisplayLinkRelease(link);
    return true;
}
#pragma clang diagnostic pop

} // namespace

TEST_CASE("GPU plug-in host: a set_size flood configures once per frame",
          "[plugin-view-host][resize][macos][gpu]") {
    @autoreleasepool {
        Editor e;
        if (!e.open(true))
            SKIP(kNoGpu);
        REQUIRE(e.settle());
        const uint64_t before = e.configures();

        // Sixty size events before one frame.
        for (uint32_t i = 1; i <= 60; ++i)
            e.host->set_size(400 + i, 300 + i / 2);
        REQUIRE(e.configures() == before);
        REQUIRE(e.frame());
        CHECK(e.configures() == before + 1);

        // Sixty more inside four frames.
        for (uint32_t frame = 0; frame < 4; ++frame) {
            for (uint32_t i = 1; i <= 15; ++i)
                e.host->set_size(460 + frame * 15 + i, 330);
            REQUIRE(e.frame());
        }
        CHECK(e.configures() == before + 5);

        REQUIRE(e.settle());
        CHECK(e.configures() == before + 5);
        CHECK(e.at_physical_size(520, 330));
        // A host-driven size is the host's own; it is not echoed back.
        CHECK(e.notified.empty());
    }
}

TEST_CASE("GPU plug-in host: a single final-size set_size configures once",
          "[plugin-view-host][resize][macos][gpu]") {
    @autoreleasepool {
        Editor e;
        if (!e.open(true))
            SKIP(kNoGpu);
        REQUIRE(e.settle());
        const uint64_t before = e.configures();

        e.host->set_size(640, 480);
        REQUIRE(e.settle());
        CHECK(e.configures() == before + 1);
        CHECK(e.at_physical_size(640, 480));
    }
}

TEST_CASE("GPU plug-in host: a native resize with no host call notifies once per frame",
          "[plugin-view-host][resize][macos][gpu]") {
    @autoreleasepool {
        Editor e;
        if (!e.open(true))
            SKIP(kNoGpu);
        REQUIRE(e.settle());
        const uint64_t before = e.configures();

        // With a display link, notifications wait for the frame; without one
        // there is no frame to wait for, and each is delivered as it lands.
        const bool link = display_link_available();
        if (!link)
            WARN("no active display: checking the host's no-display-link path");
        const size_t per_frame_burst = link ? 1 : 60;

        // Logic resizes the AU v2 view directly, once per mouse move.
        for (uint32_t i = 1; i <= 60; ++i)
            [e.view setFrameSize:NSMakeSize(400 + i, 300)];
        CHECK(e.notified.size() == (link ? 0u : 60u));
        REQUIRE(e.frame());
        REQUIRE(e.notified.size() == per_frame_burst);
        CHECK(e.notified.back() == std::make_pair(460u, 300u));
        // The swapchain is configured by the frame either way, once.
        CHECK(e.configures() == before + 1);

        // Sixty more inside four frames: one notification per frame, the last
        // one with the final size.
        for (uint32_t frame = 0; frame < 4; ++frame) {
            for (uint32_t i = 1; i <= 15; ++i)
                [e.view setFrameSize:NSMakeSize(460 + frame * 15 + i, 310)];
            REQUIRE(e.frame());
        }
        REQUIRE(e.notified.size() == (link ? 5u : 120u));
        CHECK(e.notified.back() == std::make_pair(520u, 310u));
        CHECK(e.configures() == before + 5);

        REQUIRE(e.settle());
        CHECK(e.notified.size() == (link ? 5u : 120u));
        CHECK(e.at_physical_size(520, 310));
    }
}

TEST_CASE("GPU plug-in host: a native resize with no display link notifies at once",
          "[plugin-view-host][resize][macos][gpu]") {
    @autoreleasepool {
        Editor e;
        if (!e.open(false))
            SKIP(kNoGpu);
        // No window, so no frame is coming: the processor must not wait for one.
        [e.view setFrameSize:NSMakeSize(500, 320)];
        CHECK(e.notified == Calls{{500, 320}});
        REQUIRE(e.settle());
        CHECK(e.at_physical_size(500, 320));
    }
}

// The drawable size belongs to the swapchain: a size event leaves the layer
// holding its last frame, and the next frame both configures the swapchain
// and resizes the drawable. Resizing the drawable on the size event emptied
// the layer, so a drag composited the background between frames.
TEST_CASE("GPU plug-in host: a resize keeps the last frame until the next one draws",
          "[plugin-view-host][resize][macos][gpu]") {
    @autoreleasepool {
        Editor e;
        if (!e.open(true))
            SKIP(kNoGpu);
        REQUIRE(e.settle());
        CAMetalLayer* layer = (CAMetalLayer*)e.view.layer;
        REQUIRE([layer isKindOfClass:[CAMetalLayer class]]);
        // Pinned, not stretched: the last frame stays at its pixel size.
        CHECK(e.view.layerContentsPlacement == NSViewLayerContentsPlacementTopLeft);
        INFO("layer contentsGravity " << layer.contentsGravity.UTF8String);
        CHECK_FALSE([layer.contentsGravity isEqualToString:kCAGravityResize]);
        const double s = e.scale();
        CHECK(layer.drawableSize.width == 400 * s);
        CHECK(layer.contents != nil);

        [e.view setFrameSize:NSMakeSize(500, 360)];
        [CATransaction flush];
        CHECK(layer.drawableSize.width == 400 * s);
        CHECK(layer.drawableSize.height == 300 * s);
        CHECK(layer.contents != nil);

        REQUIRE(e.frame());
        CHECK(layer.drawableSize.width == 500 * s);
        CHECK(layer.drawableSize.height == 360 * s);
        CHECK(layer.contents != nil);
        CHECK(e.at_physical_size(500, 360));
    }
}

#endif // PULP_HAS_SKIA

#endif // TARGET_OS_OSX
