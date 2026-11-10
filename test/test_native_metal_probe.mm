// test_native_metal_probe.mm: the metal-probe editor driven through the
// editor-host contract, as every format adapter opens it: a drag edits the gain
// as one host gesture, the captured frame matches the drawable and shows the
// gain, and open/close cycles leave nothing behind. Skips, saying so, without a
// Metal device. Compiled with ARC.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "metal_probe_processor.hpp"

#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/host_parameter_edit.hpp>
#include <pulp/native/frame_pump.hpp>
#include <pulp/native/metal_surface.hpp>
#include <pulp/state/store.hpp>

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#include <mach/mach.h>

#include <memory>
#include <string>
#include <vector>

using namespace pulp::format;
using pulp::state::ParamID;

// A Retina window on any host, so the capture size is checked at 2x even on a
// display-less build machine.
@interface PulpMetalProbeTestWindow : NSWindow
@end
@implementation PulpMetalProbeTestWindow
- (CGFloat)backingScaleFactor {
    return 2.0;
}
@end

namespace {

struct HostLog {
    std::vector<std::string> entries;
    HostParameterEditBridge::Callbacks callbacks() {
        return {
            [this](ParamID id) { entries.push_back("B" + std::to_string(id)); },
            [this](ParamID id, float, float) { entries.push_back("V" + std::to_string(id)); },
            [this](ParamID id) { entries.push_back("E" + std::to_string(id)); },
        };
    }
};

struct Probe {
    pulp::state::StateStore store;
    pulp_native_metal_probe::MetalProbeProcessor processor;
    HostLog host;
    std::unique_ptr<HostParameterEditBridge> bridge;
    NSWindow* window = nil;
    NSView* parent = nil;
    std::unique_ptr<ContractEditorSession> session;

    Probe() {
        processor.define_parameters(store);
        bridge = std::make_unique<HostParameterEditBridge>(store, host.callbacks());
        [NSApplication sharedApplication];
        window = [[PulpMetalProbeTestWindow alloc] initWithContentRect:NSMakeRect(0, 0, 360, 240)
                                                             styleMask:NSWindowStyleMaskBorderless
                                                               backing:NSBackingStoreBuffered
                                                                 defer:NO];
        window.releasedWhenClosed = NO;
        parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 360, 240)];
        window.contentView = parent;
    }
    ~Probe() {
        close();
        [window close];
    }

    bool open() {
        session = ContractEditorSession::create(processor, store,
                                                *pulp_native_metal_probe::editor_factory(), {},
                                                ContractEditorSession::IdleSource::host_driven);
        return session &&
               session->attach({NativeParentKind::cocoa_view, (__bridge void*)parent, 0});
    }
    void close() {
        if (session)
            session->close_for_owner();
        session.reset();
    }

    NSView* surface_view() const {
        for (NSView* v in parent.subviews)
            if ([NSStringFromClass([v class]) hasPrefix:@"PulpNativeSurfaceView_"])
                return v;
        return nil;
    }

    NSEvent* mouse(NSEventType type, double x, double y) const {
        return [NSEvent mouseEventWithType:type
                                  location:NSMakePoint(x, 240 - y)
                             modifierFlags:0
                                 timestamp:1.0
                              windowNumber:window.windowNumber
                                   context:nil
                               eventNumber:0
                                clickCount:1
                                  pressure:1.0];
    }
};

bool metal_available() {
    return MTLCreateSystemDefaultDevice() != nil;
}

/// Width, height and one pixel (RGBA) of a PNG.
struct DecodedPng {
    std::size_t width = 0;
    std::size_t height = 0;
    std::uint8_t rgba[4] = {};
};

bool decode_png(const std::vector<std::uint8_t>& png, std::size_t px, std::size_t py,
                DecodedPng& out) {
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
    // CG's bitmap rows run top to bottom in memory.
    const std::size_t index = (py * out.width + px) * 4;
    for (int i = 0; i < 4; ++i)
        out.rgba[i] = pixels[index + i];
    CGContextRelease(ctx);
    CGColorSpaceRelease(space);
    CGImageRelease(image);
    return true;
}

std::size_t resident_bytes() {
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS)
        return 0;
    return info.resident_size;
}

} // namespace

TEST_CASE("the metal-probe editor opens through the contract into a native surface",
          "[native-surface][metal-probe]") {
    if (!metal_available())
        SKIP("no Metal device on this host; the metal probe was not exercised");
    @autoreleasepool {
        Probe p;
        REQUIRE(p.open());
        REQUIRE(p.surface_view() != nil);
        REQUIRE(p.session->editor()->automation() != nullptr);
    }
}

TEST_CASE("a drag on the probe is one host gesture that raises the gain",
          "[native-surface][metal-probe]") {
    if (!metal_available())
        SKIP("no Metal device on this host");
    @autoreleasepool {
        Probe p;
        REQUIRE(p.open());
        NSView* view = p.surface_view();
        REQUIRE(view != nil);
        const float before = p.store.get_value(pulp_native_metal_probe::kGain);
        [view mouseDown:p.mouse(NSEventTypeLeftMouseDown, 100, 200)];
        [view mouseDragged:p.mouse(NSEventTypeLeftMouseDragged, 100, 170)];
        [view mouseDragged:p.mouse(NSEventTypeLeftMouseDragged, 100, 140)];
        [view mouseUp:p.mouse(NSEventTypeLeftMouseUp, 100, 140)];
        const float after = p.store.get_value(pulp_native_metal_probe::kGain);
        REQUIRE(after > before);
        // 60 points up a 240-point surface is a quarter of the range.
        REQUIRE(after == Catch::Approx(before + 0.25f).margin(0.01f));
        REQUIRE_FALSE(p.host.entries.empty());
        REQUIRE(p.host.entries.front() == "B1");
        REQUIRE(p.host.entries.back() == "E1");
        int begins = 0, ends = 0, values = 0;
        for (const auto& e : p.host.entries) {
            begins += e == "B1";
            ends += e == "E1";
            values += e == "V1";
        }
        REQUIRE(begins == 1);
        REQUIRE(ends == 1);
        REQUIRE(values >= 1);
    }
}

TEST_CASE("the captured frame has the drawable's size and shows the gain",
          "[native-surface][metal-probe]") {
    if (!metal_available())
        SKIP("no Metal device on this host");
    @autoreleasepool {
        Probe p;
        REQUIRE(p.open());
        p.session->tick();
        auto* automation = p.session->editor()->automation();
        const double scale = p.window.backingScaleFactor;
        REQUIRE(scale == 2.0);
        for (const float gain : {0.0f, 1.0f}) {
            p.store.set_value(pulp_native_metal_probe::kGain, gain);
            std::vector<std::uint8_t> png;
            REQUIRE(automation->capture_png(png));
            DecodedPng decoded;
            // A corner pixel, away from the triangle.
            REQUIRE(decode_png(png, 2, 2, decoded));
            CHECK(decoded.width == static_cast<std::size_t>(360 * scale));
            CHECK(decoded.height == static_cast<std::size_t>(240 * scale));
            if (gain == 0.0f) {
                CHECK(decoded.rgba[0] < 20);
                CHECK(decoded.rgba[2] > 235);
            } else {
                CHECK(decoded.rgba[0] > 235);
                CHECK(decoded.rgba[2] < 20);
            }
        }
    }
}

TEST_CASE("ten open and close cycles leave no tick source and little memory behind",
          "[native-surface][metal-probe][lifecycle]") {
    if (!metal_available())
        SKIP("no Metal device on this host");
    Probe p;
    // One warm-up cycle loads Metal, the shader compiler and AppKit's caches.
    @autoreleasepool {
        REQUIRE(p.open());
        p.close();
    }
    const auto sources = pulp::native::FramePump::live_tick_sources();
    const auto surfaces = pulp::native::MetalSurface::live_surfaces();
    const std::size_t before = resident_bytes();
    for (int i = 0; i < 10; ++i) {
        @autoreleasepool {
            REQUIRE(p.open());
            REQUIRE(pulp::native::FramePump::live_tick_sources() == sources + 1);
            REQUIRE(pulp::native::MetalSurface::live_surfaces() == surfaces + 1);
            p.session->tick();
            p.close();
            REQUIRE(pulp::native::FramePump::live_tick_sources() == sources);
            REQUIRE(pulp::native::MetalSurface::live_surfaces() == surfaces);
            REQUIRE(p.surface_view() == nil);
        }
    }
    const std::size_t after = resident_bytes();
    REQUIRE(before > 0);
    INFO("resident before " << before << " after " << after);
    const auto growth = after > before ? after - before : 0;
    REQUIRE(growth < 5u * 1024u * 1024u);
}
