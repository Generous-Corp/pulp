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
#include <dlfcn.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <thread>
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

// ── Content-first open ───────────────────────────────────────────────────────
//
// A content-first open mounts the document inside the host's view-creation
// call and presents that frame before the host shows the view. The GPU host
// paints into its CAMetalLayer with no window at all, so the layer already
// holds the document when the host (or AUHostingService, out of process)
// composites it; the display link's later frames take over from there.

namespace {

class CountingRoot : public View {
  public:
    int paints = 0;
    float last_w = 0, last_h = 0;
    void paint(pulp::canvas::Canvas&) override {
        ++paints;
        last_w = bounds().width;
        last_h = bounds().height;
    }
};

} // namespace

#ifdef PULP_HAS_SKIA
TEST_CASE("GPU plug-in host: present_first_frame paints the tree before the view has a window",
          "[plugin-view-host][first-frame][content-first][macos][gpu]") {
    [NSApplication sharedApplication];
    CountingRoot root;
    auto host = PluginViewHost::create(root, options(true, kDeclared));
    REQUIRE(host);
    if (host->gpu_surface() == nullptr)
        SKIP("no Dawn/Metal adapter in this process");
    NSView* view = (__bridge NSView*)host->native_handle();
    REQUIRE(view.window == nil);

    CHECK(root.paints == 0);
    CHECK(host->present_first_frame());
    // The mounted tree was painted into the layer, not just marked dirty.
    CHECK(root.paints == 1);
    // A presented drawable is the layer's content from here on, window or not.
    [CATransaction flush];
    CHECK(view.layer.contents != nil);

    // Once a frame went out, the display link owns painting: a second call
    // only asks for one and never presents out of turn.
    CHECK_FALSE(host->present_first_frame());
    CHECK(root.paints == 1);
}

TEST_CASE("GPU plug-in host: a resize before the display link paints re-presents at the new size",
          "[plugin-view-host][first-frame][content-first][macos][gpu]") {
    [NSApplication sharedApplication];
    CountingRoot root;
    auto host = PluginViewHost::create(root, options(true, kDeclared));
    REQUIRE(host);
    if (host->gpu_surface() == nullptr)
        SKIP("no Dawn/Metal adapter in this process");
    // In a window that is never ordered in: the host treats the view as shown,
    // and nothing appears on screen.
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 64, 40)
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.releasedWhenClosed = NO;
    NSView* view = (__bridge NSView*)host->native_handle();
    [window.contentView addSubview:view];
    host->set_idle_callback(nullptr);
    REQUIRE(host->present_first_frame());
    REQUIRE(root.paints == 1);
    CHECK(root.last_w == 64.0f);

    // Before the view has a window too: a host that sizes its container
    // before showing it must not composite an emptied layer.
    {
        CountingRoot bare;
        auto windowless = PluginViewHost::create(bare, options(true, kDeclared));
        REQUIRE(windowless->present_first_frame());
        windowless->set_size(40, 25);
        CHECK(bare.paints == 2);
        CHECK(bare.last_w == 40.0f);
    }

    // A host that resizes the editor right after open (a restored or minimum
    // size, a container settling): each new size is presented with the resize,
    // not that frame stretched until the display link paints.
    host->set_size(56, 35);
    CHECK(root.paints == 2);
    CHECK(root.last_w == 56.0f);
    host->set_size(48, 30);
    CHECK(root.paints == 3);
    CHECK(root.last_w == 48.0f);
    CHECK(root.last_h == 30.0f);

    host->detach();
    host.reset();
    [window close];
}
#endif

// ── A host resize queued behind the document mount ────────────────────────────
//
// Out of process (Logic's AUHostingService) the host applies the size its
// window negotiated by sending it to the plug-in's main thread, and that
// message can arrive while the main thread is mounting the document. The frame
// painted right after the mount used to go out at the size the view had BEFORE
// the queued resize, and the host showed it cropped into its smaller window.
// This repro puts the resize on the main queue from another thread while the
// idle pump is mounting, exactly as the cross-process message lands, and checks
// every painted frame of the mounted document: the layout size it was painted
// at equals the view's bounds and the container's (host's) bounds, and no host
// resize was still waiting.

namespace {

struct FramePaint {
    bool mounted = false;
    bool resize_pending = false;
    float layout_w = 0, layout_h = 0;
    double view_w = 0, view_h = 0;
    double container_w = 0, container_h = 0;
};

// The editor root. Records the geometry of every frame it is painted in.
class RecordingRoot : public View {
public:
    std::vector<FramePaint> paints;
    bool mounted = false;
    std::atomic<bool>* resize_pending = nullptr;
    NSView* host_view = nil;
    NSView* container = nil;

    void paint(pulp::canvas::Canvas&) override {
        FramePaint f;
        f.mounted = mounted;
        f.resize_pending = resize_pending && resize_pending->load();
        f.layout_w = bounds().width;
        f.layout_h = bounds().height;
        f.view_w = host_view.bounds.size.width;
        f.view_h = host_view.bounds.size.height;
        f.container_w = container.bounds.size.width;
        f.container_h = container.bounds.size.height;
        paints.push_back(f);
    }
};

void spin_main(double seconds) {
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

// Opens a host at the plug-in's preferred size in a container, mounts the
// "document" from the idle pump on its third tick while a smaller host size is
// queued on the main queue, and returns every frame painted.
std::vector<FramePaint> run_resize_during_mount(bool use_gpu, bool& had_gpu) {
    constexpr double kOpenW = 240, kOpenH = 156;   // preferred size
    constexpr double kHostW = 192, kHostH = 124;   // what the host negotiates
    [NSApplication sharedApplication];

    RecordingRoot root;
    std::atomic<bool> resize_pending{false};
    root.resize_pending = &resize_pending;

    PluginViewHost::Options o;
    o.size = {static_cast<uint32_t>(kOpenW), static_cast<uint32_t>(kOpenH)};
    o.use_gpu = use_gpu;
    o.background_rgb = kDeclared;
    auto host = PluginViewHost::create(root, o);
    REQUIRE(host);
    had_gpu = host->gpu_surface() != nullptr;

    // Never ordered in, so the test cannot take focus or show a window; the
    // display link still runs once the view is in one.
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, kOpenW, kOpenH)
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.releasedWhenClosed = NO;
    NSView* container =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, kOpenW, kOpenH)];
    container.autoresizesSubviews = YES;
    [window.contentView addSubview:container];

    NSView* view = (__bridge NSView*)host->native_handle();
    root.host_view = view;
    root.container = container;

    int idle_ticks = 0;
    host->set_idle_callback([&] {
        if (++idle_ticks != 3) return;
        // The document mounts on this tick. The host's resize is sent from
        // another thread and queued on the main queue while the mount runs.
        root.mounted = true;
        root.request_repaint();
        resize_pending.store(true);
        dispatch_semaphore_t queued = dispatch_semaphore_create(0);
        std::thread sender([&] {
            dispatch_async(dispatch_get_main_queue(), ^{
                resize_pending.store(false);
                [container setFrameSize:NSMakeSize(kHostW, kHostH)];
            });
            dispatch_semaphore_signal(queued);
        });
        dispatch_semaphore_wait(queued, DISPATCH_TIME_FOREVER);
        sender.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));  // mount work
    });

    [container addSubview:view];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto mounted_frames = [&] {
        std::size_t n = 0;
        for (const auto& f : root.paints) n += f.mounted ? 1 : 0;
        return n;
    };
    while (mounted_frames() < 2 && std::chrono::steady_clock::now() < deadline) {
        if (mounted_frames() == 1) root.request_repaint();
        spin_main(0.02);
    }

    host->set_idle_callback(nullptr);
    host->detach();
    host.reset();
    [window close];
    return root.paints;
}

void check_frames_after_mount(const std::vector<FramePaint>& paints) {
    std::size_t mounted = 0;
    for (const auto& f : paints) {
        if (!f.mounted) continue;
        ++mounted;
        INFO("frame " << mounted << ": layout " << f.layout_w << "x" << f.layout_h
                      << ", view " << f.view_w << "x" << f.view_h << ", host "
                      << f.container_w << "x" << f.container_h
                      << (f.resize_pending ? ", host resize still queued" : ""));
        CHECK_FALSE(f.resize_pending);
        CHECK(f.layout_w == static_cast<float>(f.view_w));
        CHECK(f.layout_h == static_cast<float>(f.view_h));
        CHECK(f.view_w == f.container_w);
        CHECK(f.view_h == f.container_h);
        CHECK(f.container_w == 192.0);
        CHECK(f.container_h == 124.0);
    }
    REQUIRE(mounted >= 1);
}

}  // namespace

TEST_CASE("CPU plug-in host: a host resize queued during the mount lands before the next frame",
          "[plugin-view-host][first-frame][macos]") {
    bool had_gpu = false;
    const auto paints = run_resize_during_mount(false, had_gpu);
    if (paints.empty()) SKIP("no frame was painted: no display link in this process");
    check_frames_after_mount(paints);
}

#ifdef PULP_HAS_SKIA
TEST_CASE("GPU plug-in host: a host resize queued during the mount lands before the next frame",
          "[plugin-view-host][first-frame][macos][gpu]") {
    bool had_gpu = false;
    const auto paints = run_resize_during_mount(true, had_gpu);
    if (!had_gpu) SKIP("no Dawn/Metal adapter in this process");
    if (paints.empty()) SKIP("no frame was presented: no display link in this process");
    check_frames_after_mount(paints);
}
#endif

#ifdef PULP_HAS_SKIA
namespace {

class SolidRoot : public View {
  public:
    void paint(pulp::canvas::Canvas& c) override {
        c.set_fill_color(pulp::canvas::Color::hex(0x00C000));
        c.fill_rect(0, 0, bounds().width, bounds().height);
    }
};

typedef CGImageRef (*WindowImageFn)(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption);

// What the window server composites for `window` (a process may read its own
// windows): the share of pixels in each colour class.
struct Composite {
    double magenta = 0, background = 0, content = 0;
};
bool composite_of(NSWindow* window, Composite& out) {
    static auto fn = (WindowImageFn)dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
    if (!fn)
        return false;
    CGImageRef img =
        fn(CGRectNull, kCGWindowListOptionIncludingWindow, (CGWindowID)window.windowNumber,
           kCGWindowImageBoundsIgnoreFraming | kCGWindowImageNominalResolution);
    if (!img)
        return false;
    size_t w = 0, h = 0;
    const auto px = srgb_pixels(img, w, h);
    CGImageRelease(img);
    if (w == 0 || h == 0)
        return false;
    size_t m = 0, b = 0, c = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        if (near_rgb(px[i], px[i + 1], px[i + 2], 0xFF00FF))
            ++m;
        else if (near_rgb(px[i], px[i + 1], px[i + 2], kDeclared))
            ++b;
        else if (px[i + 1] > 150 && px[i] < 60)
            ++c;
    }
    const double n = double(w * h);
    out = {m / n, b / n, c / n};
    return true;
}

} // namespace

// A host whose window is already on screen when it attaches the editor (VST3
// attached(), CLAP set_parent(), AU v3): the first image the window server
// composites after the content-first frame must be that frame, never the
// backing layer's colour for a vsync while the drawable is still in flight.
// Hidden: it orders a borderless window in off every screen and reads it back,
// which needs a window server; run it explicitly with "[composite]".
TEST_CASE(
    "GPU plug-in host: an in-window first frame composites as the frame, not the backing colour",
    "[.][composite][content-first][macos][gpu]") {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    // Whether the backing colour wins the race shows up on some opens and not
    // others, so open several times and count every image.
    constexpr int kOpens = 6;
    int background_images = 0, content_images = 0, opens = 0;
    for (int n = 0; n < kOpens; ++n) {
        SolidRoot root;
        PluginViewHost::Options o = options(true, kDeclared);
        o.size = {240, 160};
        auto host = PluginViewHost::create(root, o);
        REQUIRE(host);
        if (host->gpu_surface() == nullptr)
            SKIP("no Dawn/Metal adapter in this process");
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(-30000, -30000, 240, 160)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        window.releasedWhenClosed = NO;
        window.animationBehavior = NSWindowAnimationBehaviorNone;
        window.backgroundColor = [NSColor colorWithSRGBRed:1 green:0 blue:1 alpha:1];
        [window orderFrontRegardless];
        // The host's window is on screen, showing only itself, before it
        // attaches the editor.
        Composite before;
        const auto shown = std::chrono::steady_clock::now();
        while ((!composite_of(window, before) || before.magenta < 0.99) &&
               std::chrono::steady_clock::now() - shown < std::chrono::seconds(2))
            spin_main(0.01);
        if (before.magenta < 0.99)
            SKIP("no window server composite in this session");

        host->attach_to_parent((__bridge void*)window.contentView);
        REQUIRE(host->present_first_frame());
        ++opens;
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250)) {
            Composite c;
            if (composite_of(window, c)) {
                if (c.background > 0.5)
                    ++background_images;
                if (c.content > 0.5)
                    ++content_images;
            }
            spin_main(0.002);
        }
        host->detach();
        host.reset();
        [window orderOut:nil];
        [window close];
    }
    INFO(opens << " opens; images showing only the backing colour: " << background_images);
    CHECK(content_images >= opens);
    CHECK(background_images == 0);
}
#endif

#endif  // TARGET_OS_OSX
