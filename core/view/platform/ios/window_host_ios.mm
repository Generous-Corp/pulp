// iOS window host — UIWindow-based standalone window for Pulp apps
// Mirrors window_host_mac.mm but uses UIKit instead of AppKit.

#include <pulp/view/window_host.hpp>

#if TARGET_OS_IOS

#include <pulp/canvas/cg_canvas.hpp>
#import <UIKit/UIKit.h>
#include <unordered_map>

// ── PulpRootView: UIView subclass that paints the View tree ─────────────────

@interface PulpRootView : UIView {
    std::unordered_map<void*, int> _touchIdMap;
    int _nextTouchId;
}
@property (nonatomic, assign) pulp::view::View* rootView;
@end

@implementation PulpRootView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.backgroundColor = [UIColor blackColor];
        self.multipleTouchEnabled = YES;
        self.contentMode = UIViewContentModeRedraw;
        _nextTouchId = 0;
        [self setupHoverIfAvailable];
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

- (void)drawRect:(CGRect)rect {
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    if (!ctx) return;

    CGRect bounds = self.bounds;

    // Flip CG coordinates to match UIKit top-left origin
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, bounds.size.height);
    CGContextScaleCTM(ctx, 1.0, -1.0);

    pulp::canvas::CoreGraphicsCanvas canvas(ctx,
        static_cast<float>(bounds.size.width),
        static_cast<float>(bounds.size.height));

    canvas.set_fill_color(pulp::canvas::Color::rgba8(30, 30, 46));
    canvas.fill_rect(0, 0,
        static_cast<float>(bounds.size.width),
        static_cast<float>(bounds.size.height));

    if (self.rootView) {
        // Account for safe area insets on iOS
        UIEdgeInsets insets = self.safeAreaInsets;
        float sx = static_cast<float>(insets.left);
        float sy = static_cast<float>(insets.top);
        float sw = static_cast<float>(bounds.size.width - insets.left - insets.right);
        float sh = static_cast<float>(bounds.size.height - insets.top - insets.bottom);

        self.rootView->set_bounds({sx, sy, sw, sh});
        self.rootView->layout_children();
        self.rootView->paint_all(canvas);
    }

    CGContextRestoreGState(ctx);
}

- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    [self setNeedsDisplay];
}

// ── Touch events ────────────────────────────────────────────────────────────

- (pulp::view::MouseEvent)mouseEventFromTouch:(UITouch*)touch isDown:(BOOL)isDown {
    CGPoint loc = [touch locationInView:self];
    pulp::view::MouseEvent me;
    me.position = {static_cast<float>(loc.x), static_cast<float>(loc.y)};
    me.window_position = me.position;
    me.button = pulp::view::MouseButton::left;
    me.pointer_id = [self stableIdForTouch:touch];
    me.is_down = isDown;
    me.modifiers = 0x8000; // Touch flag

    // Stylus / pressure data (P3: Apple Pencil support)
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

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:YES];
        self.rootView->on_mouse_down(me);
    }
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:YES];
        self.rootView->on_mouse_drag(me);
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:NO];
        self.rootView->on_mouse_up(me);
        [self removeTouchId:touch];
    }
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:NO];
        me.is_cancelled = true;
        self.rootView->on_mouse_up(me);
        [self removeTouchId:touch];
    }
}

// ── iPadOS hover support (P6: trackpad/mouse connected) ─────────────────

- (void)setupHoverIfAvailable {
    if (@available(iOS 13.0, *)) {
        UIHoverGestureRecognizer *hover =
            [[UIHoverGestureRecognizer alloc] initWithTarget:self action:@selector(handleHover:)];
        [self addGestureRecognizer:hover];
    }
}

- (void)handleHover:(UIHoverGestureRecognizer *)recognizer API_AVAILABLE(ios(13.0)) {
    if (!self.rootView) return;
    CGPoint loc = [recognizer locationInView:self];
    pulp::view::Point pt = {static_cast<float>(loc.x), static_cast<float>(loc.y)};

    if (recognizer.state == UIGestureRecognizerStateChanged ||
        recognizer.state == UIGestureRecognizerStateBegan) {
        self.rootView->simulate_hover(pt);
    } else if (recognizer.state == UIGestureRecognizerStateEnded ||
               recognizer.state == UIGestureRecognizerStateCancelled) {
        self.rootView->simulate_hover({-1, -1});
    }
    [self setNeedsDisplay];
}

@end

// ── IOSWindowHost ───────────────────────────────────────────────────────────

namespace pulp::view {

class IOSWindowHost : public WindowHost {
public:
    IOSWindowHost(View& root, const WindowOptions& options)
        : root_(root) {
        // On iOS, WindowHost is used for standalone apps.
        // The UIWindow is created here but shown in run_event_loop().
        (void)options; // width/height ignored — iOS windows are fullscreen
    }

    ~IOSWindowHost() override = default;

    void show() override {
        // iOS windows are managed by the app lifecycle
    }

    void hide() override {
        // Not applicable on iOS
    }

    bool is_visible() const override {
        return window_ != nil && !window_.isHidden;
    }

    void repaint() override {
        @autoreleasepool {
            [root_view_ setNeedsDisplay];
        }
    }

    void set_close_callback(std::function<void()> cb) override {
        close_callback_ = std::move(cb);
    }

    void run_event_loop() override {
        @autoreleasepool {
            // Create a full-screen window
            UIWindowScene *scene = nil;
            for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
                if ([s isKindOfClass:[UIWindowScene class]]) {
                    scene = (UIWindowScene *)s;
                    break;
                }
            }

            if (scene) {
                window_ = [[UIWindow alloc] initWithWindowScene:scene];
            } else {
                window_ = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
            }

            root_view_ = [[PulpRootView alloc] initWithFrame:window_.bounds];
            root_view_.rootView = &root_;
            root_view_.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

            UIViewController *vc = [[UIViewController alloc] init];
            vc.view = root_view_;
            window_.rootViewController = vc;
            [window_ makeKeyAndVisible];

            // On iOS, the run loop is managed by UIApplicationMain.
            // This method returns immediately — the caller should not
            // expect blocking behavior on iOS.
        }
    }

private:
    View& root_;
    UIWindow* window_ = nil;
    PulpRootView* root_view_ = nil;
    std::function<void()> close_callback_;
};

// ── IOSGpuWindowHost (GPU rendering via Dawn/Skia Graphite) ─────────────

#ifdef PULP_HAS_SKIA
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>

class IOSGpuWindowHost : public WindowHost {
public:
    IOSGpuWindowHost(View& root, const WindowOptions& options)
        : root_(root) {
        (void)options;
    }

    ~IOSGpuWindowHost() override {
        skia_surface_.reset();
        gpu_surface_.reset();
    }

    void show() override {}
    void hide() override {}
    bool is_visible() const override { return window_ != nil && !window_.isHidden; }
    render::GpuSurface* gpu_surface() const override { return gpu_surface_.get(); }

    void repaint() override {
        needs_repaint_ = true;
    }

    void set_close_callback(std::function<void()> cb) override {
        close_callback_ = std::move(cb);
    }

    void run_event_loop() override {
        @autoreleasepool {
            UIWindowScene *scene = nil;
            for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
                if ([s isKindOfClass:[UIWindowScene class]]) {
                    scene = (UIWindowScene *)s;
                    break;
                }
            }

            if (scene) {
                window_ = [[UIWindow alloc] initWithWindowScene:scene];
            } else {
                window_ = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
            }

            // Create Metal-backed view
            metal_view_ = [[PulpMetalView alloc] initWithFrame:window_.bounds];
            metal_view_.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            metal_view_.multipleTouchEnabled = YES;

            CGFloat scale = UIScreen.mainScreen.scale;
            CGSize bounds = window_.bounds.size;
            uint32_t pw = static_cast<uint32_t>(bounds.width * scale);
            uint32_t ph = static_cast<uint32_t>(bounds.height * scale);

            // Initialize GPU surface with Metal layer
            render::GpuSurface::Config gpu_config;
            gpu_config.width = pw;
            gpu_config.height = ph;
            gpu_config.native_surface_handle = (__bridge void*)[metal_view_ metalLayer];

            gpu_surface_ = std::make_unique<render::GpuSurface>();
            if (!gpu_surface_->initialize(gpu_config)) {
                runtime::log_error("iOS GPU: GpuSurface init failed, falling back to CPU");
                gpu_surface_.reset();
            } else {
                // Initialize Skia surface sharing Dawn device
                render::SkiaSurface::Config skia_config;
                skia_config.width = pw;
                skia_config.height = ph;
                skia_config.dawn_device = gpu_surface_->dawn_device_handle();
                skia_config.dawn_queue = gpu_surface_->dawn_queue_handle();
                skia_config.dawn_instance = gpu_surface_->dawn_instance_handle();

                skia_surface_ = std::make_unique<render::SkiaSurface>();
                if (!skia_surface_->initialize(skia_config)) {
                    runtime::log_error("iOS GPU: SkiaSurface init failed");
                    skia_surface_.reset();
                    gpu_surface_.reset();
                }
            }

            UIViewController *vc = [[UIViewController alloc] init];
            vc.view = metal_view_;
            window_.rootViewController = vc;
            [window_ makeKeyAndVisible];

            runtime::log_info("iOS GPU: window host ready ({}x{} physical)", pw, ph);
        }
    }

private:
    View& root_;
    UIWindow* window_ = nil;
    PulpMetalView* metal_view_ = nil;
    std::unique_ptr<render::GpuSurface> gpu_surface_;
    std::unique_ptr<render::SkiaSurface> skia_surface_;
    std::function<void()> close_callback_;
    bool needs_repaint_ = false;

    void render_frame() {
        if (!gpu_surface_ || !skia_surface_) return;
        if (!gpu_surface_->begin_frame()) return;

        auto* canvas = skia_surface_->begin_frame(gpu_surface_->current_texture_handle());
        if (canvas) {
            // Paint the view tree
            auto b = root_.bounds();
            root_.set_bounds({0, 0, b.width, b.height});
            root_.layout_children();
            // TODO: wrap SkCanvas in SkiaCanvas adapter and call root_.paint_all()
        }

        skia_surface_->end_frame();
        gpu_surface_->end_frame();
    }
};

#endif // PULP_HAS_SKIA

std::unique_ptr<WindowHost> WindowHost::create(View& root, const WindowOptions& options) {
#ifdef PULP_HAS_SKIA
    if (options.use_gpu) {
        return std::make_unique<IOSGpuWindowHost>(root, options);
    }
#endif
    return std::make_unique<IOSWindowHost>(root, options);
}

} // namespace pulp::view

#endif // TARGET_OS_IOS
