#include <pulp/view/window_host.hpp>

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

// ── PulpView: CoreGraphics NSView (CPU rendering path) ───────────────────────

@interface PulpView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@end

@implementation PulpView

- (BOOL)isFlipped { return YES; }

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
    float width_ = 0, height_ = 0;

    void init_gpu(float width, float height) {
        width_ = width;
        height_ = height;

        gpu_surface_ = render::GpuSurface::create_dawn();
        if (!gpu_surface_) return;

        render::GpuSurface::Config gpu_config{};
        gpu_config.width = static_cast<uint32_t>(width);
        gpu_config.height = static_cast<uint32_t>(height);
        gpu_config.native_layer = (__bridge void*)metal_view_.metalLayer;

        if (!gpu_surface_->initialize(gpu_config)) {
            gpu_surface_.reset();
            return;
        }

        CGFloat scale = metal_view_.metalLayer.contentsScale;
        render::SkiaSurface::Config skia_config{};
        skia_config.width = static_cast<uint32_t>(width);
        skia_config.height = static_cast<uint32_t>(height);
        skia_config.scale_factor = static_cast<float>(scale);

        skia_surface_ = render::SkiaSurface::create(*gpu_surface_, skia_config);
    }

    void handle_resize(float width, float height) {
        width_ = width;
        height_ = height;

        if (gpu_surface_) {
            gpu_surface_->resize(static_cast<uint32_t>(width),
                                  static_cast<uint32_t>(height));
        }
        if (skia_surface_) {
            CGFloat scale = metal_view_.metalLayer.contentsScale;
            skia_surface_->resize(static_cast<uint32_t>(width),
                                   static_cast<uint32_t>(height),
                                   static_cast<float>(scale));
        }
        needs_repaint_ = true;
    }

    void render_frame() {
        if (!gpu_surface_ || !skia_surface_ || !needs_repaint_) return;

        // Frame lifecycle: acquire → wrap → render → submit → present
        if (!gpu_surface_->begin_frame()) return;

        if (auto* canvas = skia_surface_->begin_frame()) {
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
