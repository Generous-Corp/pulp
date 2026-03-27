#include <pulp/view/window_host.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/canvas/cg_canvas.hpp>
#import <Cocoa/Cocoa.h>
#include <atomic>

#ifdef PULP_HAS_SKIA
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreVideo/CVDisplayLink.h>
#import <Metal/Metal.h>
#endif

// ── App menu helper ──────────────────────────────────────────────────────────

static void install_app_menu(NSString* appName) {
    NSMenu* menuBar = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [menuBar addItem:appItem];
    [NSApp setMainMenu:menuBar];

    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:appName]
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
}

// ── PulpView: CoreGraphics NSView (CPU rendering path) ───────────────────────

@interface PulpView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@property (nonatomic, assign) pulp::view::FrameClock* frameClock;
@property (nonatomic, strong) NSTimer* animationTimer;
@property (nonatomic, strong) NSTrackingArea* trackingArea;
@end

@implementation PulpView

- (BOOL)isFlipped { return NO; }  // CG canvas already flips coordinates
- (BOOL)acceptsFirstResponder { return YES; }

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (self.trackingArea) [self removeTrackingArea:self.trackingArea];
    self.trackingArea = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                      NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:self.trackingArea];
}

- (pulp::view::Point)localPoint:(NSEvent*)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    // NSView is not flipped, so Y=0 is bottom. Convert to top-down for the view tree.
    float viewHeight = static_cast<float>(self.bounds.size.height);
    return {static_cast<float>(p.x), viewHeight - static_cast<float>(p.y)};
}

- (void)scrollWheel:(NSEvent*)event {
    if (!self.rootView) return;
    auto pt = [self localPoint:event];
    auto* target = self.rootView->hit_test(pt);
    if (!target) return;

    pulp::view::MouseEvent me;
    me.position = pt;
    me.is_wheel = true;
    me.scroll_delta_x = static_cast<float>(event.scrollingDeltaX);
    me.scroll_delta_y = static_cast<float>(event.scrollingDeltaY);
    target->on_mouse_event(me);
    [self startAnimationTimerIfNeeded];
    [self setNeedsDisplay:YES];
}

- (void)mouseDown:(NSEvent*)event {
    if (!self.rootView) return;
    auto pt = [self localPoint:event];
    self.rootView->simulate_click(pt);
    [self setNeedsDisplay:YES];
}

- (void)mouseMoved:(NSEvent*)event {
    if (!self.rootView) return;
    auto pt = [self localPoint:event];
    self.rootView->simulate_hover(pt);
    [self startAnimationTimerIfNeeded];
    [self setNeedsDisplay:YES];
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    if (!self.rootView) return;
    // Clear all hover states
    self.rootView->simulate_hover({-1, -1});
    [self startAnimationTimerIfNeeded];
    [self setNeedsDisplay:YES];
}

- (void)startAnimationTimerIfNeeded {
    if (self.animationTimer) return;
    // 60fps timer to drive animations
    self.animationTimer = [NSTimer scheduledTimerWithTimeInterval:1.0/60.0
        repeats:YES block:^(NSTimer* timer) {
            if (self.frameClock) {
                self.frameClock->tick(1.0f / 60.0f);
            }
            // Advance widget animations
            [self advanceWidgetAnimations:self.rootView dt:1.0f/60.0f];
            [self setNeedsDisplay:YES];

            // Stop timer when no animations are active
            if (self.frameClock && !self.frameClock->has_active_subscribers()) {
                if (![self hasActiveAnimations:self.rootView]) {
                    [self.animationTimer invalidate];
                    self.animationTimer = nil;
                }
            }
        }];
}

- (void)advanceWidgetAnimations:(pulp::view::View*)view dt:(float)dt {
    if (!view) return;
    // Try each animated widget type
    if (auto* k = dynamic_cast<pulp::view::Knob*>(view)) k->advance_animations(dt);
    else if (auto* t = dynamic_cast<pulp::view::Toggle*>(view)) t->advance_animations(dt);
    else if (auto* f = dynamic_cast<pulp::view::Fader*>(view)) f->advance_animations(dt);
    else if (auto* sv = dynamic_cast<pulp::view::ScrollView*>(view)) sv->advance_animations(dt);
    else if (auto* tip = dynamic_cast<pulp::view::Tooltip*>(view)) tip->advance_animations(dt);

    for (size_t i = 0; i < view->child_count(); ++i)
        [self advanceWidgetAnimations:view->child_at(i) dt:dt];
}

- (BOOL)hasActiveAnimations:(pulp::view::View*)view {
    if (!view) return NO;
    if (auto* k = dynamic_cast<pulp::view::Knob*>(view))
        if (k->hover_glow() > 0.01f || k->hover_glow() < 0.99f) return YES;
    if (auto* t = dynamic_cast<pulp::view::Toggle*>(view))
        if (t->thumb_position() > 0.01f && t->thumb_position() < 0.99f) return YES;
    if (auto* f = dynamic_cast<pulp::view::Fader*>(view))
        if (f->hover_scale() > 1.01f) return YES;

    for (size_t i = 0; i < view->child_count(); ++i)
        if ([self hasActiveAnimations:view->child_at(i)]) return YES;
    return NO;
}

- (void)drawRect:(NSRect)dirtyRect {
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    NSRect bounds = self.bounds;

    pulp::canvas::CoreGraphicsCanvas canvas(ctx,
        static_cast<float>(bounds.size.width),
        static_cast<float>(bounds.size.height));

    canvas.set_fill_color(pulp::canvas::Color::rgba(30, 30, 46));
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

- (void)dealloc {
    [self.animationTimer invalidate];
}

@end

// ── PulpMetalView: CAMetalLayer-backed NSView (GPU rendering path) ───────────

#ifdef PULP_HAS_SKIA

@interface PulpMetalView : NSView
@property (nonatomic, readonly) CAMetalLayer* metalLayer;
@property (nonatomic, assign) pulp::view::View* rootView;
// C++ render state is managed by MacGpuWindowHost, not the view
@end

@implementation PulpMetalView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;

        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = MTLCreateSystemDefaultDevice();
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        CGFloat scale = self.window ? self.window.backingScaleFactor
                                    : [NSScreen mainScreen].backingScaleFactor;
        layer.contentsScale = scale;
        CGSize backing = NSMakeSize(frame.size.width * scale, frame.size.height * scale);
        layer.drawableSize = backing;

        self.layer = layer;
        _metalLayer = layer;
    }
    return self;
}

- (BOOL)isFlipped { return YES; }

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    CGFloat scale = self.window ? self.window.backingScaleFactor
                                : [NSScreen mainScreen].backingScaleFactor;
    self.metalLayer.contentsScale = scale;
    self.metalLayer.drawableSize = CGSizeMake(newSize.width * scale, newSize.height * scale);
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    CGFloat scale = self.window ? self.window.backingScaleFactor
                                : [NSScreen mainScreen].backingScaleFactor;
    self.metalLayer.contentsScale = scale;
    CGSize backing = CGSizeMake(self.bounds.size.width * scale, self.bounds.size.height * scale);
    self.metalLayer.drawableSize = backing;
}

@end

#endif // PULP_HAS_SKIA

// ── PulpWindowDelegate ───────────────────────────────────────────────────────

@interface PulpWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, copy) void (^onClose)(void);
@property (nonatomic, copy) void (^onResize)(float, float);
@end

@implementation PulpWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
    if (self.onClose) self.onClose();
    [NSApp terminate:nil];
    return YES;
}

- (void)windowDidResize:(NSNotification*)notification {
    if (self.onResize) {
        NSWindow* window = notification.object;
        NSSize size = window.contentView.bounds.size;
        self.onResize(static_cast<float>(size.width), static_cast<float>(size.height));
    }
}

@end

// ── MacWindowHost (CoreGraphics) ─────────────────────────────────────────────

namespace pulp::view {

class MacWindowHost : public WindowHost {
public:
    MacWindowHost(View& root, const WindowOptions& options)
        : root_(root) {
        @autoreleasepool {
            NSRect frame = NSMakeRect(100, 100, options.width, options.height);
            NSWindowStyleMask style = NSWindowStyleMaskTitled
                | NSWindowStyleMaskClosable
                | NSWindowStyleMaskMiniaturizable;
            if (options.resizable)
                style |= NSWindowStyleMaskResizable;

            window_ = [[NSWindow alloc] initWithContentRect:frame
                                        styleMask:style
                                        backing:NSBackingStoreBuffered
                                        defer:NO];

            [window_ setTitle:[NSString stringWithUTF8String:options.title.c_str()]];

            view_ = [[PulpView alloc] initWithFrame:frame];
            view_.rootView = &root_;
            view_.frameClock = root_.frame_clock();
            [window_ setContentView:view_];

            delegate_ = [[PulpWindowDelegate alloc] init];
            [window_ setDelegate:delegate_];
        }
    }

    ~MacWindowHost() override = default;

    void show() override { [window_ makeKeyAndOrderFront:nil]; }
    void hide() override { [window_ orderOut:nil]; }
    bool is_visible() const override { return [window_ isVisible]; }
    void repaint() override { [view_ setNeedsDisplay:YES]; }

    void set_close_callback(std::function<void()> cb) override {
        close_callback_ = std::move(cb);
        delegate_.onClose = ^{ if (close_callback_) close_callback_(); };
    }

    void run_event_loop() override {
        @autoreleasepool {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            install_app_menu([window_ title]);
            show();
            [NSApp activateIgnoringOtherApps:YES];
            [NSApp run];
        }
    }

private:
    View& root_;
    NSWindow* window_ = nil;
    PulpView* view_ = nil;
    PulpWindowDelegate* delegate_ = nil;
    std::function<void()> close_callback_;
};

// ── MacGpuWindowHost (Dawn/Skia Graphite) ────────────────────────────────────

#ifdef PULP_HAS_SKIA

class MacGpuWindowHost : public WindowHost {
public:
    MacGpuWindowHost(View& root, const WindowOptions& options)
        : root_(root) {
        @autoreleasepool {
            NSRect frame = NSMakeRect(100, 100, options.width, options.height);
            NSWindowStyleMask style = NSWindowStyleMaskTitled
                | NSWindowStyleMaskClosable
                | NSWindowStyleMaskMiniaturizable;
            if (options.resizable)
                style |= NSWindowStyleMaskResizable;

            window_ = [[NSWindow alloc] initWithContentRect:frame
                                        styleMask:style
                                        backing:NSBackingStoreBuffered
                                        defer:NO];
            [window_ setTitle:[NSString stringWithUTF8String:options.title.c_str()]];

            // Create CAMetalLayer-backed view
            metal_view_ = [[PulpMetalView alloc] initWithFrame:frame];
            metal_view_.rootView = &root_;
            [window_ setContentView:metal_view_];

            delegate_ = [[PulpWindowDelegate alloc] init];
            delegate_.onResize = ^(float w, float h) {
                handle_resize(w, h);
            };
            [window_ setDelegate:delegate_];

            // Initialize GPU render stack
            init_gpu(options.width, options.height);
        }
    }

    ~MacGpuWindowHost() override {
        stop_display_link();
        skia_surface_.reset();
        gpu_surface_.reset();
        metal_view_.rootView = nullptr;
    }

    void show() override { [window_ makeKeyAndOrderFront:nil]; }
    void hide() override { [window_ orderOut:nil]; }
    bool is_visible() const override { return [window_ isVisible]; }

    void repaint() override {
        needs_repaint_ = true;
    }

    void set_close_callback(std::function<void()> cb) override {
        close_callback_ = std::move(cb);
        delegate_.onClose = ^{ if (close_callback_) close_callback_(); };
    }

    void run_event_loop() override {
        @autoreleasepool {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            install_app_menu([window_ title]);

            // Start display-linked render loop
            start_display_link();

            show();
            [NSApp activateIgnoringOtherApps:YES];
            [NSApp run];
        }
    }

private:
    View& root_;
    NSWindow* window_ = nil;
    PulpMetalView* metal_view_ = nil;
    PulpWindowDelegate* delegate_ = nil;
    std::function<void()> close_callback_;

    std::unique_ptr<render::GpuSurface> gpu_surface_;
    std::unique_ptr<render::SkiaSurface> skia_surface_;
    CVDisplayLinkRef display_link_ = nullptr;
    std::atomic<bool> needs_repaint_{true};
    int frame_fail_count_ = 0;
    int frame_ok_count_ = 0;
    float width_ = 0, height_ = 0;

    void init_gpu(float width, float height) {
        width_ = width;
        height_ = height;

        gpu_surface_ = render::GpuSurface::create_dawn();
        if (!gpu_surface_) return;

        // Configure GpuSurface at PHYSICAL pixel dimensions to match
        // the CAMetalLayer's drawableSize (which is logical * scale)
        CGFloat scale = metal_view_.metalLayer.contentsScale;
        uint32_t phys_w = static_cast<uint32_t>(width * scale);
        uint32_t phys_h = static_cast<uint32_t>(height * scale);

        render::GpuSurface::Config gpu_config{};
        gpu_config.width = phys_w;
        gpu_config.height = phys_h;
        gpu_config.native_surface_handle = (__bridge void*)metal_view_.metalLayer;

        if (!gpu_surface_->initialize(gpu_config)) {
            gpu_surface_.reset();
            return;
        }

        // SkiaSurface uses logical dimensions + scale factor.
        // The scale transform maps logical coordinates (800x550) to
        // physical pixels (1600x1100) in the GPU texture.
        render::SkiaSurface::Config skia_config{};
        skia_config.width = static_cast<uint32_t>(width);
        skia_config.height = static_cast<uint32_t>(height);
        skia_config.scale_factor = static_cast<float>(scale);

        skia_surface_ = render::SkiaSurface::create(*gpu_surface_, skia_config);
    }

    void handle_resize(float width, float height) {
        width_ = width;
        height_ = height;

        CGFloat scale = metal_view_.metalLayer.contentsScale;
        uint32_t phys_w = static_cast<uint32_t>(width * scale);
        uint32_t phys_h = static_cast<uint32_t>(height * scale);

        if (gpu_surface_) {
            gpu_surface_->resize(phys_w, phys_h);
        }
        if (skia_surface_) {
            skia_surface_->resize(static_cast<uint32_t>(width),
                                   static_cast<uint32_t>(height),
                                   static_cast<float>(scale));
        }
        needs_repaint_ = true;
    }

    void render_frame() {
        if (!gpu_surface_ || !skia_surface_) return;

        if (!gpu_surface_->begin_frame()) {
            frame_fail_count_++;
            if (frame_fail_count_ <= 3)
                fprintf(stderr, "[gpu-host] begin_frame failed (%d)\n", frame_fail_count_);
            return;
        }

        auto* canvas = skia_surface_->begin_frame();
        if (!canvas) {
            fprintf(stderr, "[gpu-host] skia begin_frame returned null\n");
            gpu_surface_->end_frame();
            return;
        }

        if (frame_ok_count_++ == 0) {
            CGFloat scale = metal_view_.metalLayer.contentsScale;
            fprintf(stderr, "[gpu-host] first frame: logical=%.0fx%.0f gpu=%ux%u scale=%.1f\n",
                width_, height_, gpu_surface_->width(), gpu_surface_->height(), scale);
        }

        {
            // Layout and paint the view tree
            root_.set_bounds({0, 0, width_, height_});
            root_.layout_children();

            // Clear background
            canvas->set_fill_color(canvas::Color::rgba(30, 30, 46));
            canvas->fill_rect(0, 0, width_, height_);

            root_.paint_all(*canvas);
        }

        skia_surface_->end_frame();   // submit Graphite recording
        gpu_surface_->end_frame();    // present to Metal surface

        needs_repaint_ = false;
    }

    // CVDisplayLink callback — fires on the display's vsync
    static CVReturn display_link_callback(
        CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*,
        CVOptionFlags, CVOptionFlags*, void* context)
    {
        auto* self = static_cast<MacGpuWindowHost*>(context);
        if (self->needs_repaint_.load(std::memory_order_relaxed)) {
            // Dispatch rendering to the main thread (required for Cocoa + Metal)
            dispatch_async(dispatch_get_main_queue(), ^{
                self->render_frame();
            });
        }
        return kCVReturnSuccess;
    }

    void start_display_link() {
        CVDisplayLinkCreateWithActiveCGDisplays(&display_link_);
        CVDisplayLinkSetOutputCallback(display_link_, display_link_callback, this);

        // Match the display the window is on
        CGDirectDisplayID display_id =
            (CGDirectDisplayID)[[window_.screen deviceDescription][@"NSScreenNumber"] unsignedIntValue];
        CVDisplayLinkSetCurrentCGDisplay(display_link_, display_id);
        CVDisplayLinkStart(display_link_);
    }

    void stop_display_link() {
        if (display_link_) {
            CVDisplayLinkStop(display_link_);
            CVDisplayLinkRelease(display_link_);
            display_link_ = nullptr;
        }
    }
};

#endif // PULP_HAS_SKIA

// ── Factory ──────────────────────────────────────────────────────────────────

std::unique_ptr<WindowHost> WindowHost::create(View& root, const WindowOptions& options) {
#ifdef PULP_HAS_SKIA
    if (options.use_gpu) {
        return std::make_unique<MacGpuWindowHost>(root, options);
    }
#endif
    return std::make_unique<MacWindowHost>(root, options);
}

} // namespace pulp::view

#endif // TARGET_OS_OSX
