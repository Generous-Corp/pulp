#include <pulp/view/plugin_view_host.hpp>

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/canvas/cg_canvas.hpp>
#import <Cocoa/Cocoa.h>
#include <algorithm>
#include <atomic>

#include <functional>
#include <memory>

#ifdef PULP_HAS_SKIA
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include "window_host_mac_capture.h"  // mac_capture::encode_rgba_to_png
#import <QuartzCore/CAMetalLayer.h>
#import <CoreVideo/CVDisplayLink.h>
#import <Metal/Metal.h>
#endif

// Forward declaration for the macOS NSAccessibility bridge defined in
// text_accessibility_macos.mm. Lets PulpPluginView merge text-a11y
// elements into -accessibilityChildren without pulling the whole
// scaffold header transitively.
namespace pulp::view {
NSArray* pulp_text_accessibility_all_elements_macos();
}

// ── PulpPluginView: NSView subclass for DAW embedding ────────────────────────

@interface PulpPluginView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@property (nonatomic, copy) void (^onResize)(uint32_t, uint32_t);
@end

// ── Accessibility element wrapping a Pulp View ──────────────────────────────

@interface PulpAccessibilityElement : NSAccessibilityElement
@property (nonatomic, assign) pulp::view::View* pulpView;
@property (nonatomic, assign) PulpPluginView* hostView;
@end

@implementation PulpAccessibilityElement
- (NSAccessibilityRole)accessibilityRole {
    if (!_pulpView) return NSAccessibilityGroupRole;
    using AR = pulp::view::View::AccessRole;
    auto role = _pulpView->access_role();
    if (role == AR::slider) return NSAccessibilitySliderRole;
    if (role == AR::toggle) return NSAccessibilityCheckBoxRole;
    if (role == AR::label)  return NSAccessibilityStaticTextRole;
    if (role == AR::meter)  return NSAccessibilityLevelIndicatorRole;
    if (role == AR::group)  return NSAccessibilityGroupRole;
    if (role == AR::image)  return NSAccessibilityImageRole;
    return NSAccessibilityGroupRole;
}

- (NSString*)accessibilityLabel {
    if (!_pulpView || _pulpView->access_label().empty()) return nil;
    return [NSString stringWithUTF8String:_pulpView->access_label().c_str()];
}

- (id)accessibilityValue {
    if (!_pulpView || _pulpView->access_value().empty()) return nil;
    return [NSString stringWithUTF8String:_pulpView->access_value().c_str()];
}

- (NSRect)accessibilityFrame {
    if (!_pulpView || !_hostView) return NSZeroRect;
    auto b = _pulpView->bounds();
    NSRect localRect = NSMakeRect(b.x, b.y, b.width, b.height);
    return [_hostView convertRect:localRect toView:nil];
}

- (BOOL)isAccessibilityElement { return YES; }
@end

@implementation PulpPluginView

- (BOOL)isFlipped { return NO; }
- (BOOL)isAccessibilityElement { return YES; }
- (NSAccessibilityRole)accessibilityRole { return NSAccessibilityGroupRole; }
- (NSString*)accessibilityLabel { return @"Plugin UI"; }

- (NSArray*)accessibilityChildren {
    NSMutableArray* children = [NSMutableArray array];
    if (self.rootView) {
        [self collectAccessibleChildren:self.rootView into:children];
    }
    // pulp #2255 (font v2 Slice 2.6 macOS) — merge in any
    // TextAccessibilityNodes registered through the cross-platform
    // text-a11y scaffold. The Pulp-View-role tree above and the text
    // registry are independent surfaces; without this merge the
    // registry would be a private map and VoiceOver would never
    // discover painted text registered via
    // register_text_accessibility_node().
    NSArray* text_elements = pulp::view::pulp_text_accessibility_all_elements_macos();
    for (NSAccessibilityElement* el in text_elements) {
        [el setAccessibilityParent:self];
        [children addObject:el];
    }
    return children;
}

- (void)collectAccessibleChildren:(pulp::view::View*)view into:(NSMutableArray*)array {
    if (!view || !view->visible()) return;
    if (view->access_role() != pulp::view::View::AccessRole::none) {
        PulpAccessibilityElement* elem = [PulpAccessibilityElement new];
        elem.pulpView = view;
        elem.hostView = self;
        [elem setAccessibilityParent:self];
        [array addObject:elem];
    }
    for (size_t i = 0; i < view->child_count(); ++i) {
        [self collectAccessibleChildren:view->child_at(i) into:array];
    }
}

- (void)drawRect:(NSRect)dirtyRect {
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    NSRect bounds = self.bounds;

    pulp::canvas::CoreGraphicsCanvas canvas(ctx,
        static_cast<float>(bounds.size.width),
        static_cast<float>(bounds.size.height));

    // Clear with theme background
    canvas.set_fill_color(pulp::canvas::Color::rgba8(30, 30, 46));
    canvas.fill_rect(0, 0,
        static_cast<float>(bounds.size.width),
        static_cast<float>(bounds.size.height));

    if (self.rootView) {
        self.rootView->set_bounds({0, 0,
            static_cast<float>(bounds.size.width),
            static_cast<float>(bounds.size.height)});
        self.rootView->layout_children();
        self.rootView->paint_all(canvas);
    }
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self setNeedsDisplay:YES];
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    if (self.onResize) {
        self.onResize(static_cast<uint32_t>(newSize.width),
                      static_cast<uint32_t>(newSize.height));
    }
}

@end

static NSRect child_view_frame_in_host(NSView* container,
                                       float x,
                                       float y,
                                       float width,
                                       float height) {
    if (!container) {
        return NSZeroRect;
    }

    const auto bounds = container.bounds;
    const CGFloat clipped_width = std::max<CGFloat>(0.0, width);
    const CGFloat clipped_height = std::max<CGFloat>(0.0, height);
    const CGFloat cocoa_y = container.isFlipped
        ? y
        : NSHeight(bounds) - y - clipped_height;
    return NSMakeRect(x, cocoa_y, clipped_width, clipped_height);
}

static bool attach_child_view_to_host(NSView* container,
                                      void* child_view_handle,
                                      float x,
                                      float y,
                                      float width,
                                      float height) {
    if (!container || !child_view_handle) {
        return false;
    }

    NSView* child = (__bridge NSView*)child_view_handle;
    if (!child) {
        return false;
    }

    if (child.superview && child.superview != container) {
        [child removeFromSuperview];
    }

    [child setFrame:child_view_frame_in_host(container, x, y, width, height)];

    if (child.superview != container) {
        [container addSubview:child];
    }

    [child setHidden:NO];
    return true;
}

static bool set_child_view_bounds_in_host(NSView* container,
                                          void* child_view_handle,
                                          float x,
                                          float y,
                                          float width,
                                          float height) {
    if (!container || !child_view_handle) {
        return false;
    }

    NSView* child = (__bridge NSView*)child_view_handle;
    if (!child || child.superview != container) {
        return false;
    }

    [child setFrame:child_view_frame_in_host(container, x, y, width, height)];
    return true;
}

static void detach_child_view_from_host(NSView* container, void* child_view_handle) {
    if (!container || !child_view_handle) {
        return;
    }

    NSView* child = (__bridge NSView*)child_view_handle;
    if (child && child.superview == container) {
        [child removeFromSuperview];
    }
}

// ── MacPluginViewHost ────────────────────────────────────────────────────────

namespace pulp::view {

class MacPluginViewHost : public PluginViewHost {
public:
    MacPluginViewHost(View& root, Size size)
        : root_(root), size_(size) {
        @autoreleasepool {
            NSRect frame = NSMakeRect(0, 0, size.width, size.height);
            view_ = [[PulpPluginView alloc] initWithFrame:frame];
            view_.rootView = &root_;
            view_.onResize = ^(uint32_t w, uint32_t h) {
                this->on_native_frame_changed(w, h);
            };
        }
    }

    ~MacPluginViewHost() override {
        root_.set_plugin_view_host(nullptr);
        @autoreleasepool { view_.onResize = nil; }
        detach();
    }

    NativeViewHandle native_handle() override {
        return (__bridge void*)view_;
    }

    void attach_to_parent(NativeViewHandle parent) override {
        @autoreleasepool {
            NSView* parent_view = (__bridge NSView*)parent;
            if (parent_view && view_) {
                [parent_view addSubview:view_];
                [view_ setNeedsDisplay:YES];
            }
        }
    }

    void detach() override {
        @autoreleasepool {
            if (view_) {
                [view_ removeFromSuperview];
            }
        }
    }

    void repaint() override {
        @autoreleasepool {
            [view_ setNeedsDisplay:YES];
        }
    }

    void set_size(uint32_t width, uint32_t height) override {
        size_ = {width, height};
        @autoreleasepool {
            [view_ setFrameSize:NSMakeSize(width, height)];
            [view_ setNeedsDisplay:YES];
        }
    }

    Size get_size() const override {
        return size_;
    }

    void set_resize_callback(std::function<void(uint32_t, uint32_t)> cb) override {
        resize_cb_ = std::move(cb);
    }

    bool attach_native_child_view(NativeViewHandle child_view,
                                  float x,
                                  float y,
                                  float width,
                                  float height) override {
        return attach_child_view_to_host(view_, child_view, x, y, width, height);
    }

    bool set_native_child_view_bounds(NativeViewHandle child_view,
                                      float x,
                                      float y,
                                      float width,
                                      float height) override {
        return set_child_view_bounds_in_host(view_, child_view, x, y, width, height);
    }

    void detach_native_child_view(NativeViewHandle child_view) override {
        detach_child_view_from_host(view_, child_view);
    }

    void on_native_frame_changed(uint32_t w, uint32_t h) {
        if (w == size_.width && h == size_.height) return;
        if (w == 0 || h == 0) return;
        set_size(w, h);
        if (resize_cb_) resize_cb_(w, h);
    }

private:
    View& root_;
    Size size_;
    PulpPluginView* view_ = nil;
    std::function<void(uint32_t, uint32_t)> resize_cb_;
};

} // namespace pulp::view (close for ObjC declarations)

// ── MacGpuPluginViewHost (Dawn/Skia Graphite) ────────────────────────────────

#ifdef PULP_HAS_SKIA

// CAMetalLayer-backed NSView for DAW-embedded GPU rendering.
//
// Unlike the standalone window host (which owns its NSWindow and starts the
// CVDisplayLink in its constructor), an embedded plugin view is handed a
// parent view by the host and only becomes live once it joins a window. So
// this view exposes window-attach / backing-change callbacks the host wires
// up to start/stop the display link and reconfigure surfaces at the right
// moments. The wrapper paths (AU returns the NSView directly; VST3/CLAP call
// attach_to_parent) never drive `attach_to_parent`-time rendering — they all
// funnel through `-viewDidMoveToWindow`.
@interface PulpGpuPluginView : NSView
@property (nonatomic, readonly) CAMetalLayer* metalLayer;
@property (nonatomic, assign) pulp::view::View* rootView;
@property (nonatomic, copy) void (^onWindowChange)(void);
@property (nonatomic, copy) void (^onBackingChange)(void);
@property (nonatomic, copy) void (^onResize)(uint32_t, uint32_t);
@end

@implementation PulpGpuPluginView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = MTLCreateSystemDefaultDevice();
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        // framebufferOnly = NO so the embedded back buffer can be read back
        // for headless capture (`capture_back_buffer_png`) — matches the
        // standalone MacGpuWindowHost. (Plugin host previously set YES,
        // which blocked readback.)
        layer.framebufferOnly = NO;
        CGFloat scale = self.window ? self.window.backingScaleFactor
                                    : [NSScreen mainScreen].backingScaleFactor;
        layer.contentsScale = scale;
        layer.drawableSize = CGSizeMake(frame.size.width * scale, frame.size.height * scale);
        layer.opaque = YES;
        self.layer = layer;
        _metalLayer = layer;
    }
    return self;
}

- (BOOL)isFlipped { return NO; }

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    CGFloat scale = self.window ? self.window.backingScaleFactor
                                : [NSScreen mainScreen].backingScaleFactor;
    self.metalLayer.contentsScale = scale;
    self.metalLayer.drawableSize = CGSizeMake(newSize.width * scale, newSize.height * scale);
    // AU v2 resizes this NSView directly (no host size callback). Notify the
    // host; it resizes surfaces and forwards to ViewBridge::resize. The host
    // guards against re-entrancy when *it* drove the frame change.
    if (self.onResize) {
        self.onResize(static_cast<uint32_t>(newSize.width),
                      static_cast<uint32_t>(newSize.height));
    }
}

// Start/stop the display link when the view joins or leaves a window. This is
// the embeddable equivalent of the standalone host starting its link in the
// constructor — the wrapper attach paths don't render until this fires.
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.onWindowChange) self.onWindowChange();
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    CGFloat scale = self.window ? self.window.backingScaleFactor
                                : [NSScreen mainScreen].backingScaleFactor;
    self.metalLayer.contentsScale = scale;
    self.metalLayer.drawableSize = CGSizeMake(self.bounds.size.width * scale,
                                              self.bounds.size.height * scale);
    if (self.onBackingChange) self.onBackingChange();
}

// Accessibility
- (BOOL)isAccessibilityElement { return YES; }
- (NSAccessibilityRole)accessibilityRole { return NSAccessibilityGroupRole; }
- (NSString*)accessibilityLabel { return @"Plugin UI"; }

@end

namespace pulp::view { // reopen for C++ classes

class MacGpuPluginViewHost : public PluginViewHost {
public:
    MacGpuPluginViewHost(View& root, Size size)
        : root_(root), size_(size),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        @autoreleasepool {
            root_.set_frame_clock(&frame_clock_);
            NSRect frame = NSMakeRect(0, 0, size.width, size.height);
            metal_view_ = [[PulpGpuPluginView alloc] initWithFrame:frame];
            metal_view_.rootView = &root_;

            // Wire the embeddable lifecycle hooks. The display link starts
            // when the view joins a window and stops when it leaves; the
            // surfaces re-sync on backing (HiDPI) changes. Captured `this`
            // is safe: the view is owned by this host and torn down in the
            // destructor before `this` dies, and the callbacks are cleared
            // there too.
            metal_view_.onWindowChange = ^{ this->handle_window_change(); };
            metal_view_.onBackingChange = ^{ this->handle_backing_change(); };
            metal_view_.onResize = ^(uint32_t w, uint32_t h) {
                this->on_native_frame_changed(w, h);
            };

            init_gpu(static_cast<float>(size.width), static_cast<float>(size.height));
        }
    }

    ~MacGpuPluginViewHost() override {
        // Flip the liveness token FIRST so any display-link block already
        // queued on the main thread becomes a no-op before we free anything
        // (mirrors the #2502 deferred-click token).
        alive_->store(false, std::memory_order_release);
        root_.set_plugin_view_host(nullptr);
        stop_display_link();
        @autoreleasepool {
            metal_view_.onWindowChange = nil;
            metal_view_.onBackingChange = nil;
            metal_view_.onResize = nil;
        }
        skia_surface_.reset();
        gpu_surface_.reset();
        metal_view_.rootView = nullptr;
        root_.set_frame_clock(nullptr);
    }

    NativeViewHandle native_handle() override {
        return (__bridge void*)metal_view_;
    }

    void attach_to_parent(NativeViewHandle parent) override {
        @autoreleasepool {
            NSView* parent_view = (__bridge NSView*)parent;
            if (parent_view && metal_view_) {
                [parent_view addSubview:metal_view_];
                // Display link starts via -viewDidMoveToWindow → handle_window_change().
                needs_repaint_.store(true, std::memory_order_relaxed);
            }
        }
    }

    void detach() override {
        stop_display_link();
        @autoreleasepool {
            if (metal_view_) [metal_view_ removeFromSuperview];
        }
    }

    void repaint() override {
        needs_repaint_.store(true, std::memory_order_relaxed);
    }

    void set_size(uint32_t width, uint32_t height) override {
        size_ = {width, height};
        @autoreleasepool {
            [metal_view_ setFrameSize:NSMakeSize(width, height)];
            // Resize parity with the standalone host: GpuSurface in PHYSICAL
            // pixels (logical * scale, matches the CAMetalLayer drawableSize),
            // SkiaSurface in LOGICAL size + scale factor.
            CGFloat scale = metal_view_.metalLayer.contentsScale;
            if (gpu_surface_) {
                gpu_surface_->resize(static_cast<uint32_t>(width * scale),
                                     static_cast<uint32_t>(height * scale));
            }
            if (skia_surface_) {
                skia_surface_->resize(width, height, static_cast<float>(scale));
            }
        }
        needs_repaint_.store(true, std::memory_order_relaxed);
    }

    Size get_size() const override { return size_; }

    bool is_gpu_backed() const override {
        return gpu_surface_ != nullptr && skia_surface_ != nullptr &&
               skia_surface_->is_available();
    }

    void set_idle_callback(std::function<void()> callback) override {
        idle_callback_ = std::move(callback);
        has_idle_callback_.store(static_cast<bool>(idle_callback_),
                                 std::memory_order_release);
    }

    void set_resize_callback(std::function<void(uint32_t, uint32_t)> cb) override {
        resize_cb_ = std::move(cb);
    }

    // Deterministic GPU back-buffer readback for hidden / headless test
    // hosts (mirrors WindowHost::capture_back_buffer_png, issue #2001).
    // Goes straight through render_frame()'s readback path; never shows a
    // window or touches the compositor. Returns {} on failure.
    std::vector<uint8_t> capture_back_buffer_png() override {
        if (!gpu_surface_ || !skia_surface_) return {};
        needs_repaint_.store(true, std::memory_order_relaxed);
        std::vector<uint8_t> pixels;
        uint32_t pixel_w = 0;
        uint32_t pixel_h = 0;
        if (!render_frame(&pixels, &pixel_w, &pixel_h)) return {};
        return pulp::view::mac_capture::encode_rgba_to_png(
            pixels.data(), pixel_w, pixel_h, static_cast<size_t>(pixel_w) * 4u);
    }

    bool attach_native_child_view(NativeViewHandle child_view,
                                  float x,
                                  float y,
                                  float width,
                                  float height) override {
        return attach_child_view_to_host(metal_view_, child_view, x, y, width, height);
    }

    bool set_native_child_view_bounds(NativeViewHandle child_view,
                                      float x,
                                      float y,
                                      float width,
                                      float height) override {
        return set_child_view_bounds_in_host(metal_view_, child_view, x, y, width, height);
    }

    void detach_native_child_view(NativeViewHandle child_view) override {
        detach_child_view_from_host(metal_view_, child_view);
    }

private:
    View& root_;
    Size size_;
    PulpGpuPluginView* metal_view_ = nil;

    std::unique_ptr<render::GpuSurface> gpu_surface_;
    std::unique_ptr<render::SkiaSurface> skia_surface_;
    CVDisplayLinkRef display_link_ = nullptr;
    FrameClock frame_clock_;
    std::atomic<bool> needs_repaint_{true};
    std::atomic<bool> continuous_frames_{false};
    std::atomic<bool> render_dispatch_queued_{false};
    std::function<void()> idle_callback_;
    std::atomic<bool> has_idle_callback_{false};
    std::function<void(uint32_t, uint32_t)> resize_cb_;
    // Liveness token captured by the display-link main-thread dispatch
    // blocks. Flipped false in the destructor so a queued block after
    // teardown is a no-op (DAW teardown order is not under our control).
    std::shared_ptr<std::atomic<bool>> alive_;
    int frame_ok_count_ = 0;

    void init_gpu(float width, float height) {
        gpu_surface_ = render::GpuSurface::create_dawn();
        if (!gpu_surface_) {
            fprintf(stderr, "[plugin-gpu-host] gpu init failed reason=create_dawn_null "
                            "falling_back=cpu-paint\n");
            return;
        }

        // Configure GpuSurface at PHYSICAL pixel dimensions to match the
        // CAMetalLayer drawableSize (logical * scale).
        CGFloat scale = metal_view_.metalLayer.contentsScale;
        uint32_t phys_w = static_cast<uint32_t>(width * scale);
        uint32_t phys_h = static_cast<uint32_t>(height * scale);

        render::GpuSurface::Config gpu_config{};
        gpu_config.width = phys_w;
        gpu_config.height = phys_h;
        gpu_config.native_surface_handle = (__bridge void*)metal_view_.metalLayer;

        if (!gpu_surface_->initialize(gpu_config)) {
            fprintf(stderr, "[plugin-gpu-host] gpu init failed reason=initialize "
                            "falling_back=cpu-paint\n");
            gpu_surface_.reset();
            return;
        }

        // SkiaSurface uses LOGICAL dimensions + scale factor.
        render::SkiaSurface::Config skia_config{};
        skia_config.width = static_cast<uint32_t>(width);
        skia_config.height = static_cast<uint32_t>(height);
        skia_config.scale_factor = static_cast<float>(scale);
        skia_surface_ = render::SkiaSurface::create(*gpu_surface_, skia_config);
        if (!skia_surface_) {
            fprintf(stderr, "[plugin-gpu-host] gpu init failed reason=skia_create_null "
                            "falling_back=cpu-paint\n");
            gpu_surface_.reset();
            return;
        }
        fprintf(stderr, "[plugin-gpu-host] init requested size=%.0fx%.0f scale=%.1f "
                        "gpu=%ux%u\n", width, height, scale, phys_w, phys_h);
    }

    void paint_scene(canvas::Canvas& canvas) {
        float w = static_cast<float>(size_.width);
        float h = static_cast<float>(size_.height);
        root_.set_bounds({0, 0, w, h});
        root_.layout_children();
        canvas.set_fill_color(pulp::canvas::Color::rgba8(30, 30, 46));
        canvas.fill_rect(0, 0, w, h);
        root_.paint_all(canvas);
    }

    // Render one frame. When capture buffers are supplied, the rendered
    // RGBA back buffer is read back into them (for capture_back_buffer_png).
    bool render_frame(std::vector<uint8_t>* capture_pixels = nullptr,
                      uint32_t* capture_width = nullptr,
                      uint32_t* capture_height = nullptr) {
        if (!gpu_surface_ || !skia_surface_) return false;
        if (!gpu_surface_->begin_frame()) return false;

        auto* canvas = skia_surface_->begin_frame();
        if (!canvas) {
            gpu_surface_->end_frame();
            return false;
        }

        if (frame_ok_count_++ == 0) {
            CGFloat scale = metal_view_.metalLayer.contentsScale;
            fprintf(stderr, "[plugin-gpu-host] first frame logical=%ux%u gpu=%ux%u scale=%.1f\n",
                    size_.width, size_.height, gpu_surface_->width(),
                    gpu_surface_->height(), scale);
        }

        paint_scene(*canvas);

        continuous_frames_.store(
            view_needs_continuous_frames(&root_) || frame_clock_.has_active_subscribers(),
            std::memory_order_relaxed);

        bool captured = true;
        if (capture_pixels && capture_width && capture_height) {
            captured = skia_surface_->read_current_rgba(*capture_pixels,
                                                        *capture_width,
                                                        *capture_height);
        }

        skia_surface_->end_frame();
        gpu_surface_->end_frame();

        needs_repaint_.store(continuous_frames_.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        return captured;
    }

    static CVReturn display_link_callback(
        CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*,
        CVOptionFlags, CVOptionFlags*, void* context)
    {
        auto* self = static_cast<MacGpuPluginViewHost*>(context);
        // Copy the liveness token so the atomic outlives a teardown that
        // races this callback.
        auto alive = self->alive_;
        if (!alive->load(std::memory_order_acquire)) return kCVReturnSuccess;

        const bool has_idle = self->has_idle_callback_.load(std::memory_order_acquire);
        if (self->needs_repaint_.load(std::memory_order_relaxed) ||
            self->continuous_frames_.load(std::memory_order_relaxed) ||
            has_idle) {
            bool expected = false;
            if (!self->render_dispatch_queued_.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return kCVReturnSuccess;  // a render is already queued this vsync
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                @autoreleasepool {
                    if (!alive->load(std::memory_order_acquire)) return;
                    // Pump idle (scripted poll: async results, timers, rAF)
                    // FIRST so any request_repaint they trigger is seen below.
                    if (self->idle_callback_) self->idle_callback_();

                    bool animate = view_needs_continuous_frames(&self->root_);
                    bool tick_subscribers = self->frame_clock_.has_active_subscribers();
                    if (!self->needs_repaint_.load(std::memory_order_relaxed) &&
                        !animate && !tick_subscribers) {
                        self->continuous_frames_.store(false, std::memory_order_relaxed);
                        self->render_dispatch_queued_.store(false, std::memory_order_release);
                        return;
                    }
                    self->frame_clock_.tick(1.0f / 60.0f);
                    advance_widget_animations(&self->root_, 1.0f / 60.0f);
                    if (animate || tick_subscribers) {
                        self->needs_repaint_.store(true, std::memory_order_relaxed);
                    }
                    self->render_frame();
                    self->render_dispatch_queued_.store(false, std::memory_order_release);
                }
            });
        }
        return kCVReturnSuccess;
    }

    // Called when the native NSView frame changed (e.g. AU host resize).
    // Guards re-entrancy: when *we* drove the change via set_size(), size_
    // already matches, so this is a no-op and never recurses through
    // set_size()'s own [metal_view_ setFrameSize:] call.
    void on_native_frame_changed(uint32_t w, uint32_t h) {
        if (w == size_.width && h == size_.height) return;
        if (w == 0 || h == 0) return;
        set_size(w, h);
        if (resize_cb_) resize_cb_(w, h);
    }

    void handle_window_change() {
        if (metal_view_.window) {
            start_display_link();
            needs_repaint_.store(true, std::memory_order_relaxed);
        } else {
            stop_display_link();
        }
    }

    void handle_backing_change() {
        // The view already updated its layer's contentsScale/drawableSize;
        // re-sync the surfaces at the new scale and re-arm a paint.
        set_size(size_.width, size_.height);
    }

    void start_display_link() {
        if (display_link_) {
            // Already running; just make sure it's bound to the current screen.
            bind_to_window_screen();
            return;
        }
        CVDisplayLinkCreateWithActiveCGDisplays(&display_link_);
        CVDisplayLinkSetOutputCallback(display_link_, display_link_callback, this);
        bind_to_window_screen();
        CVDisplayLinkStart(display_link_);
    }

    // Bind the display link to the screen the embedded view is actually on,
    // so we render at that display's vsync (the host window may live on a
    // secondary monitor).
    void bind_to_window_screen() {
        if (!display_link_) return;
        NSScreen* screen = metal_view_.window ? metal_view_.window.screen : nil;
        if (!screen) screen = [NSScreen mainScreen];
        NSNumber* num = screen.deviceDescription[@"NSScreenNumber"];
        if (num) {
            CVDisplayLinkSetCurrentCGDisplay(
                display_link_, (CGDirectDisplayID)num.unsignedIntValue);
        }
    }

    void stop_display_link() {
        if (display_link_) {
            CVDisplayLinkStop(display_link_);
            CVDisplayLinkRelease(display_link_);
            display_link_ = nullptr;
        }
    }

    // ── Continuous-frame / animation drivers (parity with MacGpuWindowHost) ──
    static bool view_needs_continuous_frames(View* view) {
        if (!view) return false;
        if (auto* k = dynamic_cast<Knob*>(view)) {
            if ((k->hover_glow() > 0.01f && k->hover_glow() < 0.99f) || k->shader_uses_time())
                return true;
        }
        if (auto* t = dynamic_cast<Toggle*>(view)) {
            if ((t->thumb_position() > 0.01f && t->thumb_position() < 0.99f) || t->shader_uses_time())
                return true;
        }
        if (auto* f = dynamic_cast<Fader*>(view)) {
            if (f->hover_scale() > 1.01f || f->shader_uses_time())
                return true;
        }
        if (auto* sv = dynamic_cast<ScrollView*>(view)) {
            if (sv->scroll_animating()) return true;
        }
        if (view->animation_play_state() != "paused") {
            for (const auto& a : view->active_animations()) {
                if (a.active) return true;
            }
        }
        for (size_t i = 0; i < view->child_count(); ++i) {
            if (view_needs_continuous_frames(view->child_at(i))) return true;
        }
        return false;
    }

    static void advance_widget_animations(View* view, float dt) {
        if (!view) return;
        if (auto* k = dynamic_cast<Knob*>(view)) k->advance_animations(dt);
        else if (auto* t = dynamic_cast<Toggle*>(view)) t->advance_animations(dt);
        else if (auto* f = dynamic_cast<Fader*>(view)) f->advance_animations(dt);
        else if (auto* sv = dynamic_cast<ScrollView*>(view)) sv->advance_animations(dt);
        else if (auto* tip = dynamic_cast<Tooltip*>(view)) tip->advance_animations(dt);
        view->tick_animations(dt);
        for (size_t i = 0; i < view->child_count(); ++i)
            advance_widget_animations(view->child_at(i), dt);
    }
};

} // namespace pulp::view (close GPU class block)

#endif // PULP_HAS_SKIA

namespace pulp::view { // reopen for factory functions

// ── Factory ──────────────────────────────────────────────────────────────────

std::unique_ptr<PluginViewHost> PluginViewHost::create(View& root, Size size) {
    auto host = std::make_unique<MacPluginViewHost>(root, size);
    root.set_plugin_view_host(host.get());
    return host;
}

std::unique_ptr<PluginViewHost> PluginViewHost::create(View& root, const Options& options) {
#ifdef PULP_HAS_SKIA
    if (options.use_gpu) {
        auto host = std::make_unique<MacGpuPluginViewHost>(root, options.size);
        if (host->is_gpu_backed()) {
            root.set_plugin_view_host(host.get());
            return host;
        }
        // GPU init failed (no Dawn/Metal adapter in this host process) — fall
        // back to the CoreGraphics host so the editor never disappears (item 9).
        // The adapter's runtime scream-guard logs the mismatch loudly.
        host.reset();
    }
#endif
    auto host = std::make_unique<MacPluginViewHost>(root, options.size);
    root.set_plugin_view_host(host.get());
    return host;
}

} // namespace pulp::view

#endif // TARGET_OS_OSX
