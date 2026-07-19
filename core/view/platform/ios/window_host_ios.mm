// iOS window host — UIWindow-based standalone window for Pulp apps
// Mirrors window_host_mac.mm but uses UIKit instead of AppKit.

#include <pulp/view/window_host.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/host_frame_pump.hpp>
#include <pulp/events/main_thread_dispatcher.hpp>

#if TARGET_OS_IOS

#include <pulp/canvas/cg_canvas.hpp>
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <utility>

// ── PulpRootView: UIView subclass that paints the View tree ─────────────────

// Forward declaration from accessibility_ios.mm
namespace pulp::view {
NSArray<UIAccessibilityElement *>* create_accessibility_elements(View& root, UIView* container);
}

static pulp::events::MainThreadDispatcher::Backend make_uikit_main_thread_backend() {
    return {
        [](pulp::events::Task task) -> bool {
            if (!task) return false;
            auto* heap_task = new pulp::events::Task(std::move(task));
            dispatch_async(dispatch_get_main_queue(), ^{
                std::unique_ptr<pulp::events::Task> owned(heap_task);
                if (*owned) (*owned)();
            });
            return true;
        },
        [] {
            return [NSThread isMainThread];
        },
    };
}

@interface PulpRootView : UIView {
    std::unordered_map<void*, int> _touchIdMap;
    int _nextTouchId;
    NSArray<UIAccessibilityElement *>* _cachedAccessibilityElements;
    CADisplayLink* _gestureDisplayLink;
}
@property (nonatomic, assign) pulp::view::View* rootView;
@property (nonatomic, copy) void (^onResize)(float, float);
@end

@implementation PulpRootView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.backgroundColor = [UIColor blackColor];
        self.multipleTouchEnabled = YES;
        self.contentMode = UIViewContentModeRedraw;
        _nextTouchId = 0;
        _cachedAccessibilityElements = nil;
        _gestureDisplayLink = nil;
        [self setupHoverIfAvailable];
    }
    return self;
}

- (void)dealloc {
    [_gestureDisplayLink invalidate];
    _gestureDisplayLink = nil;
    [super dealloc];
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

- (void)layoutSubviews {
    [super layoutSubviews];
    if (self.onResize) {
        CGRect bounds = self.bounds;
        self.onResize(static_cast<float>(bounds.size.width),
                      static_cast<float>(bounds.size.height));
    }
}

- (void)syncGestureDisplayLink {
    const bool wants_frames =
        self.rootView && self.rootView->has_time_driven_gestures();
    if (wants_frames) {
        if (_gestureDisplayLink) return;
        _gestureDisplayLink = [CADisplayLink displayLinkWithTarget:self
                                                           selector:@selector(gestureDisplayLinkTick:)];
        [_gestureDisplayLink addToRunLoop:[NSRunLoop mainRunLoop]
                                   forMode:NSRunLoopCommonModes];
        return;
    }
    if (_gestureDisplayLink) {
        [_gestureDisplayLink invalidate];
        _gestureDisplayLink = nil;
    }
}

- (void)gestureDisplayLinkTick:(CADisplayLink*)link {
    (void)link;
    if (!self.rootView) {
        [self syncGestureDisplayLink];
        return;
    }
    self.rootView->advance_gesture_recognizers();
    [self setNeedsDisplay];
    [self syncGestureDisplayLink];
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

    // Stylus / pressure data (Apple Pencil support)
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
        me.phase = pulp::view::MousePhase::press;
        if (self.rootView->dispatch_gesture_pointer_event(me)) {
            [self syncGestureDisplayLink];
            [self setNeedsDisplay];
            continue;
        }
        self.rootView->on_mouse_down(me.position);
    }
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:YES];
        me.phase = pulp::view::MousePhase::drag;
        if (self.rootView->dispatch_gesture_pointer_event(me)) {
            [self syncGestureDisplayLink];
            [self setNeedsDisplay];
            continue;
        }
        self.rootView->on_mouse_drag(me.position);
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:NO];
        me.phase = pulp::view::MousePhase::release;
        if (self.rootView->dispatch_gesture_pointer_event(me)) {
            [self removeTouchId:touch];
            [self syncGestureDisplayLink];
            [self setNeedsDisplay];
            continue;
        }
        self.rootView->on_mouse_up(me.position);
        [self removeTouchId:touch];
    }
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:NO];
        me.is_cancelled = true;
        me.phase = pulp::view::MousePhase::release;
        if (self.rootView->dispatch_gesture_pointer_event(me)) {
            [self removeTouchId:touch];
            [self syncGestureDisplayLink];
            [self setNeedsDisplay];
            continue;
        }
        self.rootView->on_mouse_cancel(me.position);
        [self removeTouchId:touch];
    }
}

// ── iPadOS hover support (trackpad/mouse connected) ─────────────────────

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

// ── UIAccessibilityContainer ────────────────────────────────────────────

- (BOOL)isAccessibilityElement {
    return NO;  // Container, not an element itself
}

- (NSArray *)accessibilityElements {
    if (!_rootView) return @[];
    // Rebuild on each query — VoiceOver caches this per focus change
    _cachedAccessibilityElements = pulp::view::create_accessibility_elements(*_rootView, self);
    return _cachedAccessibilityElements;
}

- (NSInteger)accessibilityElementCount {
    return [[self accessibilityElements] count];
}

- (id)accessibilityElementAtIndex:(NSInteger)index {
    NSArray* elements = [self accessibilityElements];
    if (index >= 0 && index < (NSInteger)elements.count) return elements[index];
    return nil;
}

- (NSInteger)indexOfAccessibilityElement:(id)element {
    return [[self accessibilityElements] indexOfObject:element];
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

    ~IOSWindowHost() override {
        if (host_liveness_)
            host_liveness_->store(false, std::memory_order_release);
        pulp::events::MainThreadDispatcher::unregister_backend(dispatcher_token_);
        @autoreleasepool {
            if (root_view_) {
                root_view_.onResize = nil;
                root_view_.rootView = nullptr;
                root_view_ = nil;
            }
            if (window_) {
                [window_ setHidden:YES];
                window_.rootViewController = nil;
                window_ = nil;
            }
        }
        root_.set_window_host(nullptr);
    }

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

    ContentSize get_content_size() const override {
        CGRect bounds = root_view_ ? root_view_.bounds : UIScreen.mainScreen.bounds;
        return {
            static_cast<uint32_t>(bounds.size.width > 0 ? bounds.size.width : 0),
            static_cast<uint32_t>(bounds.size.height > 0 ? bounds.size.height : 0),
        };
    }

    void set_close_callback(std::function<void()> cb) override {
        close_callback_ = std::move(cb);
    }

    void set_resize_callback(ResizeCallback cb) override {
        resize_callback_ = std::move(cb);
        if (root_view_) {
            auto alive_token = host_liveness_;
            auto* host = this;
            root_view_.onResize = ^(float w, float h) {
                if (!alive_token || !alive_token->load(std::memory_order_acquire))
                    return;
                if (host->resize_callback_) {
                    host->resize_callback_(
                        static_cast<uint32_t>(w > 0 ? w : 0),
                        static_cast<uint32_t>(h > 0 ? h : 0));
                }
            };
        }
    }

    void run_event_loop() override {
        @autoreleasepool {
            pulp::events::MainThreadDispatcher::unregister_backend(dispatcher_token_);
            dispatcher_token_ = 0;

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
            auto alive_token = host_liveness_;
            auto* host = this;
            root_view_.onResize = ^(float w, float h) {
                if (!alive_token || !alive_token->load(std::memory_order_acquire))
                    return;
                if (host->resize_callback_) {
                    host->resize_callback_(
                        static_cast<uint32_t>(w > 0 ? w : 0),
                        static_cast<uint32_t>(h > 0 ? h : 0));
                }
            };

            UIViewController *vc = [[UIViewController alloc] init];
            vc.view = root_view_;
            window_.rootViewController = vc;
            [window_ makeKeyAndVisible];
            dispatcher_token_ = pulp::events::MainThreadDispatcher::register_backend(
                make_uikit_main_thread_backend());

            // On iOS, the run loop is managed by UIApplicationMain.
            // This method returns immediately — the caller should not
            // expect blocking behavior on iOS.
        }
    }

private:
    // No FrameClock / HostFramePump here: this CPU host owns no CADisplayLink,
    // so it has nothing to pump. The GPU host (IOSGpuWindowHost) is the one that
    // drives a frame clock. Advancing animations on this host would need a frame
    // source first — see the iOS gap noted in docs/guides/animation.md.
    View& root_;
    UIWindow* window_ = nil;
    PulpRootView* root_view_ = nil;
    std::function<void()> close_callback_;
    ResizeCallback resize_callback_;
    std::shared_ptr<std::atomic<bool>> host_liveness_ =
        std::make_shared<std::atomic<bool>>(true);
    pulp::events::MainThreadDispatcher::Token dispatcher_token_ = 0;
};

// ── IOSGpuWindowHost (GPU rendering via Dawn/Skia Graphite) ─────────────

// Close the `namespace pulp::view {` opened around IOSWindowHost so the
// Skia / Dawn / runtime headers below resolve at the global ::pulp::*
// scope. When these includes live INSIDE the open namespace, everything
// is nested as `pulp::view::pulp::render::*`, which makes every later use
// of `render::GpuSurface` fail to resolve (it becomes
// `pulp::view::render::GpuSurface`, which does not exist). The bug only
// affects GPU-enabled iOS builds because CPU-only iOS builds never
// include these headers.
}  // namespace pulp::view

#ifdef PULP_HAS_SKIA
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/runtime/log.hpp>

namespace pulp::view {

// Forward decl for the display-link bridge.
class IOSGpuWindowHost;

}  // namespace pulp::view

// Metal-backed UIView — file-local class so we don't depend on the
// private PulpMetalView in metal_surface_ios.mm.
@interface PulpMetalWindowView : UIView
@property (nonatomic, copy) void (^onResize)(float, float);
@end

@implementation PulpMetalWindowView
+ (Class)layerClass { return [CAMetalLayer class]; }
- (CAMetalLayer *)metalLayer { return (CAMetalLayer *)self.layer; }
- (void)layoutSubviews {
    [super layoutSubviews];
    if (self.onResize) {
        CGRect bounds = self.bounds;
        self.onResize(static_cast<float>(bounds.size.width),
                      static_cast<float>(bounds.size.height));
    }
}
@end

// CADisplayLink target — relays the vsync tick to the C++ host.
@interface PulpIOSDisplayLinkTarget : NSObject
@property (nonatomic, assign) pulp::view::IOSGpuWindowHost* host;
- (void)tick:(CADisplayLink*)link;
@end

namespace pulp::view {

class IOSGpuWindowHost : public WindowHost {
public:
    IOSGpuWindowHost(View& root, const WindowOptions& options)
        : root_(root) {
        (void)options;
        // Parity with macOS: the iOS standalone window owns a FrameClock. It
        // previously owned none, so on iOS a standalone app pumped only gesture
        // recognizers — FrameClock subscribers, widget animations, and CSS
        // animation timelines never advanced at all.
        root_.set_frame_clock(&frame_clock_);
    }

    ~IOSGpuWindowHost() override {
        root_.set_frame_clock(nullptr);
        if (host_liveness_)
            host_liveness_->store(false, std::memory_order_release);
        pulp::events::MainThreadDispatcher::unregister_backend(dispatcher_token_);
        if (metal_view_)
            metal_view_.onResize = nil;
        if (display_link_target_)
            display_link_target_.host = nullptr;
        stop_display_link();
        skia_surface_.reset();
        gpu_surface_.reset();
        root_.set_window_host(nullptr);
    }

    void show() override {}
    void hide() override {}
    bool is_visible() const override { return window_ != nil && !window_.isHidden; }
    render::GpuSurface* gpu_surface() const override { return gpu_surface_.get(); }
    ContentSize get_content_size() const override {
        CGRect bounds = metal_view_ ? metal_view_.bounds : UIScreen.mainScreen.bounds;
        return {
            static_cast<uint32_t>(bounds.size.width > 0 ? bounds.size.width : 0),
            static_cast<uint32_t>(bounds.size.height > 0 ? bounds.size.height : 0),
        };
    }

    void repaint() override {
        needs_repaint_.store(true, std::memory_order_relaxed);
    }

    // Real backing scale for this device's screen. The base default returned a
    // hardcoded 1.0, so convert_to_logical/native was identity on EVERY Retina
    // iOS device (2x/3x) — a live coordinate bug. Prefer the window's own
    // UIScreen (correct on external displays / multi-scene) and fall back to the
    // main screen.
    float dpi_scale() const override {
        UIScreen* screen = window_ ? window_.screen : UIScreen.mainScreen;
        if (!screen) screen = UIScreen.mainScreen;
        return static_cast<float>(screen.scale);
    }

    // Fixed "design viewport": pin the root to (design_w x design_h) and letterbox
    // it into the window at paint time, exactly like the macOS GPU host. Mirrors
    // set_design_viewport there; the letterbox math is the shared, platform-neutral
    // WindowHost::compute_design_viewport_transform (unit-tested x-platform in
    // test_view_design_viewport.cpp). Pass (0,0) to disable.
    void set_design_viewport(float design_w, float design_h) override {
        design_viewport_w_ = design_w;
        design_viewport_h_ = design_h;
        needs_repaint_.store(true, std::memory_order_relaxed);
    }

    // Report the active design->window transform for embedded native children
    // (mirrors the paint-time letterbox), matching the macOS GPU host. Returns
    // false (identity) when no viewport is set.
    bool design_viewport_transform(float& sx, float& sy,
                                   float& tx, float& ty) const override {
        return design_transform(sx, sy, tx, ty);
    }

    /// Seed the pump's nominal (first-frame / wake) interval from the display's
    /// real refresh period. Called by the CADisplayLink target.
    void set_nominal_frame_dt(float dt) { frame_pump_.set_nominal_dt(dt); }

    /// `frame_time` is the CADisplayLink's targetTimestamp (when this frame is
    /// actually presented), in the CACurrentMediaTime timebase.
    void tick(double frame_time) {
        // pulp #1402 / #1387 (iOS parity with macOS) — pump JS rAF /
        // setTimeout / async-result queues each vsync. Without this,
        // requestAnimationFrame(cb) callbacks queue forever and only fire
        // when an unrelated touch event triggers a poll. Run the idle
        // callback first so any request_repaint it triggers arms
        // needs_repaint_ for the same vsync tick.
        auto alive_token = host_liveness_;
        if (idle_callback_) {
            idle_callback_();
            if (!alive_token || !alive_token->load(std::memory_order_acquire))
                return;
        }
        // One measured dt to every consumer — the same contract the macOS hosts
        // use (host_frame_pump.hpp). This host used to advance nothing but the
        // gesture recognizers.
        const auto tick_result = begin_host_frame(
            &root_, frame_clock_, frame_pump_, frame_time,
            needs_repaint_.load(std::memory_order_relaxed));
        if (tick_result.should_render) {
            advance_host_frame(&root_, frame_clock_, tick_result.dt);
            needs_repaint_.store(true, std::memory_order_relaxed);
        }
        if (needs_repaint_.exchange(false, std::memory_order_relaxed)) {
            render_frame();
        }
    }

    void set_close_callback(std::function<void()> cb) override {
        close_callback_ = std::move(cb);
    }

    void set_idle_callback(std::function<void()> cb) override {
        // pulp #1402 — wire the host's idle callback into the CADisplayLink
        // dispatch (see tick()). Without this override the base-class no-op
        // silently drops standalone's `scripted_ui->poll()`, so JS animation
        // queues never get a vsync-paced pump.
        idle_callback_ = std::move(cb);
    }

    void set_resize_callback(ResizeCallback cb) override {
        resize_callback_ = std::move(cb);
        if (metal_view_) {
            auto alive_token = host_liveness_;
            auto* host = this;
            metal_view_.onResize = ^(float w, float h) {
                if (!alive_token || !alive_token->load(std::memory_order_acquire))
                    return;
                host->handle_resize(w, h);
            };
        }
    }

    void run_event_loop() override {
        @autoreleasepool {
            pulp::events::MainThreadDispatcher::unregister_backend(dispatcher_token_);
            dispatcher_token_ = 0;

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
            metal_view_ = [[PulpMetalWindowView alloc] initWithFrame:window_.bounds];
            metal_view_.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            metal_view_.multipleTouchEnabled = YES;
            auto alive_token = host_liveness_;
            auto* host = this;
            metal_view_.onResize = ^(float w, float h) {
                if (!alive_token || !alive_token->load(std::memory_order_acquire))
                    return;
                host->handle_resize(w, h);
            };

            CGFloat scale = UIScreen.mainScreen.scale;
            CGSize bounds = window_.bounds.size;
            uint32_t pw = static_cast<uint32_t>(bounds.width * scale);
            uint32_t ph = static_cast<uint32_t>(bounds.height * scale);

            // Initialize GPU surface with Metal layer.
            // GpuSurface is an abstract interface — use the create_dawn()
            // factory, not std::make_unique. SkiaSurface::create() takes
            // a GpuSurface& and a Config struct that exposes only width,
            // height, and scale_factor (it queries the GPU side for the
            // Dawn device/queue handles internally).
            gpu_surface_ = render::GpuSurface::create_dawn();
            if (!gpu_surface_) {
                runtime::log_error("iOS GPU: GpuSurface::create_dawn() returned null, "
                                   "falling back to CPU");
            } else {
                render::GpuSurface::Config gpu_config{};
                gpu_config.width = pw;
                gpu_config.height = ph;
                gpu_config.native_surface_handle = (__bridge void*)[metal_view_ metalLayer];

                if (!gpu_surface_->initialize(gpu_config)) {
                    runtime::log_error("iOS GPU: GpuSurface init failed, falling back to CPU");
                    gpu_surface_.reset();
                } else {
                    CGFloat layer_scale = metal_view_.metalLayer.contentsScale;
                    render::SkiaSurface::Config skia_config{};
                    skia_config.width = static_cast<uint32_t>(bounds.width);
                    skia_config.height = static_cast<uint32_t>(bounds.height);
                    skia_config.scale_factor = static_cast<float>(layer_scale);

                    skia_surface_ = render::SkiaSurface::create(*gpu_surface_, skia_config);
                    if (!skia_surface_) {
                        runtime::log_error("iOS GPU: SkiaSurface::create returned null");
                        gpu_surface_.reset();
                    }
                }
            }

            UIViewController *vc = [[UIViewController alloc] init];
            vc.view = metal_view_;
            window_.rootViewController = vc;
            [window_ makeKeyAndVisible];

            dispatcher_token_ = pulp::events::MainThreadDispatcher::register_backend(
                make_uikit_main_thread_backend());
            start_display_link();
            needs_repaint_.store(true, std::memory_order_relaxed);

            runtime::log_info("iOS GPU: window host ready ({}x{} physical)", pw, ph);
        }
    }

private:
    View& root_;
    FrameClock frame_clock_;
    // Measured-dt source, fed by the CADisplayLink's targetTimestamp.
    HostFramePump frame_pump_;
    UIWindow* window_ = nil;
    PulpMetalWindowView* metal_view_ = nil;
    std::unique_ptr<render::GpuSurface> gpu_surface_;
    std::unique_ptr<render::SkiaSurface> skia_surface_;
    std::function<void()> close_callback_;
    // pulp #1402 — idle callback wiring for iOS GPU host. tick() reads
    // and invokes on the main thread (CADisplayLink fires on main
    // run loop in NSRunLoopCommonModes), so no atomic is needed.
    std::function<void()> idle_callback_;
    std::atomic<bool> needs_repaint_{false};
    std::shared_ptr<std::atomic<bool>> host_liveness_ =
        std::make_shared<std::atomic<bool>>(true);
    CADisplayLink* display_link_ = nil;
    PulpIOSDisplayLinkTarget* display_link_target_ = nil;
    ResizeCallback resize_callback_;
    pulp::events::MainThreadDispatcher::Token dispatcher_token_ = 0;
    float design_viewport_w_ = 0.0f;
    float design_viewport_h_ = 0.0f;

    // Compute the active design->window transform, delegating to the shared
    // header-only math so the host and its unit tests agree. Returns false
    // (identity) when no design viewport is set.
    bool design_transform(float& sx, float& sy, float& tx, float& ty) const {
        if (design_viewport_w_ <= 0.0f || design_viewport_h_ <= 0.0f) return false;
        CGSize logical = window_ ? window_.bounds.size
                                 : UIScreen.mainScreen.bounds.size;
        return WindowHost::compute_design_viewport_transform(
            static_cast<float>(logical.width), static_cast<float>(logical.height),
            design_viewport_w_, design_viewport_h_, sx, sy, tx, ty);
    }

    void start_display_link() {
        if (display_link_) return;
        frame_pump_.suspend();  // (re)start is a resume, not a multi-second frame
        @autoreleasepool {
            display_link_target_ = [[PulpIOSDisplayLinkTarget alloc] init];
            display_link_target_.host = this;
            display_link_ = [CADisplayLink displayLinkWithTarget:display_link_target_
                                                        selector:@selector(tick:)];
            [display_link_ addToRunLoop:[NSRunLoop mainRunLoop]
                                forMode:NSRunLoopCommonModes];
        }
    }

    void stop_display_link() {
        frame_pump_.suspend();
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

        auto* canvas = skia_surface_->begin_frame();
        if (canvas) {
            // Keep bounds aligned with logical window size, not the
            // pixel-scaled GPU surface. paint_all() handles content scale.
            CGSize logical = window_.bounds.size;
            const float win_w = static_cast<float>(logical.width);
            const float win_h = static_cast<float>(logical.height);

            // Design viewport: pin the root to design size and letterbox it into
            // the window (aspect-correct scale + centered translate), mirroring
            // the macOS GPU host's paint_scene. The letterbox fill covers the
            // whole window first so the bars match the design background.
            float sx, sy, tx, ty;
            const bool has_viewport = design_transform(sx, sy, tx, ty);
            if (has_viewport) {
                root_.set_bounds({0, 0, design_viewport_w_, design_viewport_h_});
            } else {
                root_.set_bounds({0, 0, win_w, win_h});
            }
            root_.layout_children();

            canvas->set_fill_color(canvas::Color::rgba8(30, 30, 46));
            canvas->fill_rect(0, 0, win_w, win_h);

            if (has_viewport) {
                const int saved = canvas->save_count();
                canvas->save();
                canvas->translate(tx, ty);
                canvas->scale(sx, sy);
                root_.paint_all(*canvas);
                View::paint_overlays(*canvas, &root_);
                canvas->restore_to_count(saved);
            } else {
                root_.paint_all(*canvas);
                View::paint_overlays(*canvas, &root_);
            }
        }

        skia_surface_->end_frame();
        gpu_surface_->end_frame();
    }

    void handle_resize(float width, float height) {
        const CGFloat scale = UIScreen.mainScreen.scale;
        const uint32_t logical_w = static_cast<uint32_t>(width > 0 ? width : 0);
        const uint32_t logical_h = static_cast<uint32_t>(height > 0 ? height : 0);
        const uint32_t phys_w = static_cast<uint32_t>((width > 0 ? width : 0) * scale);
        const uint32_t phys_h = static_cast<uint32_t>((height > 0 ? height : 0) * scale);

        if (gpu_surface_) {
            gpu_surface_->resize(phys_w, phys_h);
        }
        if (skia_surface_) {
            skia_surface_->resize(logical_w, logical_h, static_cast<float>(scale));
        }
        if (resize_callback_) {
            auto alive_token = host_liveness_;
            resize_callback_(logical_w, logical_h);
            if (!alive_token || !alive_token->load(std::memory_order_acquire))
                return;
        }
        needs_repaint_.store(true, std::memory_order_relaxed);
    }
};

}  // namespace pulp::view

@implementation PulpIOSDisplayLinkTarget
- (void)tick:(CADisplayLink*)link {
    if (!_host) return;
    // targetTimestamp = when this frame will be on screen; duration = this
    // display's nominal frame period (1/120 on ProMotion). Feeding a hardcoded
    // 1/60 here is what made every animation run at double speed on those
    // displays.
    if (link.duration > 0) _host->set_nominal_frame_dt(static_cast<float>(link.duration));
    const double frame_time = link.targetTimestamp > 0 ? link.targetTimestamp
                                                       : CACurrentMediaTime();
    _host->tick(frame_time);
}
@end

#endif // PULP_HAS_SKIA

namespace pulp::view {

std::unique_ptr<WindowHost> WindowHost::create(View& root, const WindowOptions& options) {
#ifdef PULP_HAS_SKIA
    if (options.use_gpu) {
        auto host = std::make_unique<IOSGpuWindowHost>(root, options);
        root.set_window_host(host.get());
        return host;
    }
#endif
    auto host = std::make_unique<IOSWindowHost>(root, options);
    root.set_window_host(host.get());
    return host;
}

} // namespace pulp::view

#endif // TARGET_OS_IOS
