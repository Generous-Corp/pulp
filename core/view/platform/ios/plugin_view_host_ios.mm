// iOS plugin view host — UIView-based embedding for AUv3 in iOS hosts
// Mirrors plugin_view_host_mac.mm but uses UIKit instead of AppKit.

#include <pulp/view/plugin_view_host.hpp>

#if TARGET_OS_IOS

#include <pulp/canvas/cg_canvas.hpp>
#import <UIKit/UIKit.h>
#include <algorithm>
#include <atomic>
#include <unordered_map>

// ── PulpPluginUIView: UIView subclass for DAW embedding on iOS ──────────────

@interface PulpPluginUIView : UIView {
    std::unordered_map<void*, int> _touchIdMap;
    int _nextTouchId;
}
@property (nonatomic, assign) pulp::view::View* rootView;
@end

@implementation PulpPluginUIView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.backgroundColor = [UIColor blackColor];
        self.multipleTouchEnabled = YES;
        self.contentMode = UIViewContentModeRedraw;
        _nextTouchId = 0;
    }
    return self;
}

- (int)stableIdForTouch:(UITouch*)touch {
    void* key = (__bridge void*)touch;
    auto it = _touchIdMap.find(key);
    if (it != _touchIdMap.end()) return it->second;
    int newId = _nextTouchId++;
    _touchIdMap[key] = newId;
    return newId;
}

- (void)removeTouchId:(UITouch*)touch {
    _touchIdMap.erase((__bridge void*)touch);
    if (_touchIdMap.empty()) _nextTouchId = 0;
}

- (pulp::view::MouseEvent)mouseEventFromTouch:(UITouch*)touch isDown:(BOOL)isDown {
    CGPoint loc = [touch locationInView:self];
    pulp::view::MouseEvent me;
    me.position = {static_cast<float>(loc.x), static_cast<float>(loc.y)};
    me.window_position = me.position;
    me.button = pulp::view::MouseButton::left;
    me.pointer_id = [self stableIdForTouch:touch];
    me.is_down = isDown;
    me.modifiers = 0x8000;

    if (touch.maximumPossibleForce > 0)
        me.pressure = static_cast<float>(touch.force / touch.maximumPossibleForce);
    if (touch.type == UITouchTypePencil) {
        me.pointer_type = pulp::view::PointerType::pen;
        me.altitude_angle = static_cast<float>(touch.altitudeAngle);
        me.azimuth_angle = static_cast<float>([touch azimuthAngleInView:self]);
    } else {
        me.pointer_type = pulp::view::PointerType::touch;
    }

    return me;
}

- (void)drawRect:(CGRect)rect {
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    if (!ctx) return;

    CGRect bounds = self.bounds;

    // UIKit has origin at top-left (same as Pulp), but Core Graphics
    // has origin at bottom-left. Flip the coordinate system.
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, bounds.size.height);
    CGContextScaleCTM(ctx, 1.0, -1.0);

    pulp::canvas::CoreGraphicsCanvas canvas(ctx,
        static_cast<float>(bounds.size.width),
        static_cast<float>(bounds.size.height));

    // Clear background
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

    CGContextRestoreGState(ctx);
}

// ── Touch handling → MouseEvent with stable pointer identity ────────────────

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:YES];
        self.rootView->on_mouse_down(me.position);
    }
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:YES];
        self.rootView->on_mouse_drag(me.position);
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:NO];
        self.rootView->on_mouse_up(me.position);
        [self removeTouchId:touch];
    }
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:NO];
        me.is_cancelled = true;
        self.rootView->on_mouse_cancel(me.position);
        [self removeTouchId:touch];
    }
}

// ── Safe area insets ────────────────────────────────────────────────────────

- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    [self setNeedsDisplay];
}

@end

static CGRect child_view_frame_in_host(UIView* container,
                                       float x,
                                       float y,
                                       float width,
                                       float height) {
    if (!container) {
        return CGRectZero;
    }

    const CGFloat clipped_width = std::max<CGFloat>(0.0, width);
    const CGFloat clipped_height = std::max<CGFloat>(0.0, height);
    return CGRectMake(x, y, clipped_width, clipped_height);
}

static bool attach_child_view_to_host(UIView* container,
                                      void* child_view_handle,
                                      float x,
                                      float y,
                                      float width,
                                      float height) {
    if (!container || !child_view_handle) {
        return false;
    }

    UIView* child = (__bridge UIView*)child_view_handle;
    if (!child) {
        return false;
    }

    if (child.superview && child.superview != container) {
        [child removeFromSuperview];
    }

    child.frame = child_view_frame_in_host(container, x, y, width, height);
    if (child.superview != container) {
        [container addSubview:child];
    }
    child.hidden = NO;
    return true;
}

static bool set_child_view_bounds_in_host(UIView* container,
                                          void* child_view_handle,
                                          float x,
                                          float y,
                                          float width,
                                          float height) {
    if (!container || !child_view_handle) {
        return false;
    }

    UIView* child = (__bridge UIView*)child_view_handle;
    if (!child || child.superview != container) {
        return false;
    }

    child.frame = child_view_frame_in_host(container, x, y, width, height);
    return true;
}

static void detach_child_view_from_host(UIView* container, void* child_view_handle) {
    if (!container || !child_view_handle) {
        return;
    }

    UIView* child = (__bridge UIView*)child_view_handle;
    if (child && child.superview == container) {
        [child removeFromSuperview];
    }
}

// ── iOSPluginViewHost ─────────────────────────────────────────────────────────

namespace pulp::view {

class IOSPluginViewHost : public PluginViewHost {
public:
    IOSPluginViewHost(View& root, Size size)
        : root_(root), size_(size) {
        @autoreleasepool {
            CGRect frame = CGRectMake(0, 0, size.width, size.height);
            view_ = [[PulpPluginUIView alloc] initWithFrame:frame];
            view_.rootView = &root_;
        }
    }

    ~IOSPluginViewHost() override {
        root_.set_plugin_view_host(nullptr);
        detach();
    }

    NativeViewHandle native_handle() override {
        return (__bridge void*)view_;
    }

    void attach_to_parent(NativeViewHandle parent) override {
        @autoreleasepool {
            UIView* parent_view = (__bridge UIView*)parent;
            if (parent_view && view_) {
                [parent_view addSubview:view_];
                [view_ setNeedsDisplay];
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
            [view_ setNeedsDisplay];
        }
    }

    void set_size(uint32_t width, uint32_t height) override {
        size_ = {width, height};
        @autoreleasepool {
            view_.frame = CGRectMake(0, 0, width, height);
            [view_ setNeedsDisplay];
        }
    }

    Size get_size() const override {
        return size_;
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

private:
    View& root_;
    Size size_;
    PulpPluginUIView* view_ = nil;
};

// ── GPU-accelerated iOS plugin view host ─────────────────────────────────

#ifdef PULP_HAS_SKIA
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

class IOSGpuPluginViewHost;

}  // namespace pulp::view

// Metal-backed UIView for GPU rendering in AUv3 hosts
@interface PulpMetalPluginView : UIView
@end

@implementation PulpMetalPluginView
+ (Class)layerClass { return [CAMetalLayer class]; }
- (CAMetalLayer *)metalLayer { return (CAMetalLayer *)self.layer; }
@end

// CADisplayLink target — relays the vsync tick to the C++ host.
@interface PulpIOSPluginDisplayLinkTarget : NSObject
@property (nonatomic, assign) pulp::view::IOSGpuPluginViewHost* host;
- (void)tick:(CADisplayLink*)link;
@end

namespace pulp::view {

class IOSGpuPluginViewHost : public PluginViewHost {
public:
    IOSGpuPluginViewHost(View& root, const Options& opts)
        : root_(root), size_(opts.size) {
        @autoreleasepool {
            CGRect frame = CGRectMake(0, 0, opts.size.width, opts.size.height);
            metal_view_ = [[PulpMetalPluginView alloc] initWithFrame:frame];
            metal_view_.multipleTouchEnabled = YES;
            metal_view_.autoresizingMask =
                UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

            CGFloat scale = UIScreen.mainScreen.scale;
            uint32_t pw = static_cast<uint32_t>(opts.size.width * scale);
            uint32_t ph = static_cast<uint32_t>(opts.size.height * scale);

            CAMetalLayer* layer = [metal_view_ metalLayer];
            layer.device = MTLCreateSystemDefaultDevice();
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer.drawableSize = CGSizeMake(pw, ph);
            layer.contentsScale = scale;

            render::GpuSurface::Config gpu_cfg;
            gpu_cfg.width = pw;
            gpu_cfg.height = ph;
            gpu_cfg.native_surface_handle = (__bridge void*)layer;

            gpu_surface_ = std::make_unique<render::GpuSurface>();
            if (gpu_surface_->initialize(gpu_cfg)) {
                render::SkiaSurface::Config skia_cfg;
                skia_cfg.width = pw;
                skia_cfg.height = ph;
                skia_cfg.dawn_device = gpu_surface_->dawn_device_handle();
                skia_cfg.dawn_queue = gpu_surface_->dawn_queue_handle();
                skia_cfg.dawn_instance = gpu_surface_->dawn_instance_handle();

                skia_surface_ = std::make_unique<render::SkiaSurface>();
                if (!skia_surface_->initialize(skia_cfg)) {
                    skia_surface_.reset();
                    gpu_surface_.reset();
                }
            } else {
                gpu_surface_.reset();
            }
        }
    }

    ~IOSGpuPluginViewHost() override {
        root_.set_plugin_view_host(nullptr);
        stop_display_link();
    }

    NativeViewHandle native_handle() override { return (__bridge void*)metal_view_; }

    void attach_to_parent(NativeViewHandle parent) override {
        @autoreleasepool {
            UIView* pv = (__bridge UIView*)parent;
            if (pv && metal_view_) {
                [pv addSubview:metal_view_];
                start_display_link();
                needs_repaint_.store(true, std::memory_order_relaxed);
            }
        }
    }

    void detach() override {
        @autoreleasepool {
            stop_display_link();
            [metal_view_ removeFromSuperview];
        }
    }

    void repaint() override {
        needs_repaint_.store(true, std::memory_order_relaxed);
    }

    void tick() {
        if (needs_repaint_.exchange(false, std::memory_order_relaxed)) {
            render_frame();
        }
    }

    void set_size(uint32_t w, uint32_t h) override {
        size_ = {w, h};
        @autoreleasepool {
            metal_view_.frame = CGRectMake(0, 0, w, h);
            CGFloat scale = UIScreen.mainScreen.scale;
            [metal_view_ metalLayer].drawableSize = CGSizeMake(w * scale, h * scale);
            if (skia_surface_) {
                skia_surface_->resize(static_cast<uint32_t>(w * scale),
                                      static_cast<uint32_t>(h * scale),
                                      static_cast<float>(scale));
            }
        }
        needs_repaint_.store(true, std::memory_order_relaxed);
    }

    Size get_size() const override { return size_; }

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
    PulpMetalPluginView* metal_view_ = nil;
    std::unique_ptr<render::GpuSurface> gpu_surface_;
    std::unique_ptr<render::SkiaSurface> skia_surface_;
    std::atomic<bool> needs_repaint_{false};
    CADisplayLink* display_link_ = nil;
    PulpIOSPluginDisplayLinkTarget* display_link_target_ = nil;

    void start_display_link() {
        if (display_link_) return;
        @autoreleasepool {
            display_link_target_ = [[PulpIOSPluginDisplayLinkTarget alloc] init];
            display_link_target_.host = this;
            display_link_ = [CADisplayLink displayLinkWithTarget:display_link_target_
                                                        selector:@selector(tick:)];
            [display_link_ addToRunLoop:[NSRunLoop mainRunLoop]
                                forMode:NSRunLoopCommonModes];
        }
    }

    void stop_display_link() {
        @autoreleasepool {
            if (display_link_) {
                [display_link_ invalidate];
                display_link_ = nil;
            }
            display_link_target_ = nil;
        }
    }

    void render_frame() {
        if (!gpu_surface_ || !skia_surface_) return;
        if (!gpu_surface_->begin_frame()) return;

        auto* cv = skia_surface_->begin_frame();
        if (cv) {
            root_.set_bounds({0, 0,
                              static_cast<float>(size_.width),
                              static_cast<float>(size_.height)});
            root_.layout_children();
            cv->set_fill_color(canvas::Color::rgba8(30, 30, 46));
            cv->fill_rect(0, 0,
                          static_cast<float>(size_.width),
                          static_cast<float>(size_.height));
            root_.paint_all(*cv);
            View::paint_overlays(*cv, &root_);
        }

        skia_surface_->end_frame();
        gpu_surface_->end_frame();
    }
};

}  // namespace pulp::view

@implementation PulpIOSPluginDisplayLinkTarget
- (void)tick:(CADisplayLink*)link {
    (void)link;
    if (_host) _host->tick();
}
@end

namespace pulp::view {

#endif // PULP_HAS_SKIA

// Factory functions
std::unique_ptr<PluginViewHost> PluginViewHost::create(View& root, Size size) {
    auto host = std::make_unique<IOSPluginViewHost>(root, size);
    root.set_plugin_view_host(host.get());
    return host;
}

std::unique_ptr<PluginViewHost> PluginViewHost::create(View& root, const Options& options) {
#ifdef PULP_HAS_SKIA
    if (options.use_gpu) {
        auto host = std::make_unique<IOSGpuPluginViewHost>(root, options);
        root.set_plugin_view_host(host.get());
        return host;
    }
#endif
    auto host = std::make_unique<IOSPluginViewHost>(root, options.size);
    root.set_plugin_view_host(host.get());
    return host;
}

} // namespace pulp::view

#endif // TARGET_OS_IOS
