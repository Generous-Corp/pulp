// iOS window host — UIWindow-based standalone window for Pulp apps
// Mirrors window_host_mac.mm but uses UIKit instead of AppKit.

#include <pulp/view/window_host.hpp>

#if TARGET_OS_IOS

#include <pulp/canvas/cg_canvas.hpp>
#import <UIKit/UIKit.h>

// ── PulpRootView: UIView subclass that paints the View tree ─────────────────

@interface PulpRootView : UIView
@property (nonatomic, assign) pulp::view::View* rootView;
@end

@implementation PulpRootView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.backgroundColor = [UIColor blackColor];
        self.multipleTouchEnabled = YES;
        self.contentMode = UIViewContentModeRedraw;
    }
    return self;
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

    canvas.set_fill_color(pulp::canvas::Color::rgba(30, 30, 46));
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

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    int pid = 0;
    for (UITouch *touch in touches) {
        CGPoint loc = [touch locationInView:self];
        pulp::view::MouseEvent me;
        me.position = {static_cast<float>(loc.x), static_cast<float>(loc.y)};
        me.window_position = me.position;
        me.button = pulp::view::MouseButton::left;
        me.pointer_id = pid++;
        me.is_down = true;
        me.modifiers = 0x8000;
        self.rootView->on_mouse_down(me);
    }
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    int pid = 0;
    for (UITouch *touch in touches) {
        CGPoint loc = [touch locationInView:self];
        pulp::view::MouseEvent me;
        me.position = {static_cast<float>(loc.x), static_cast<float>(loc.y)};
        me.window_position = me.position;
        me.button = pulp::view::MouseButton::left;
        me.pointer_id = pid++;
        me.is_down = true;
        me.modifiers = 0x8000;
        self.rootView->on_mouse_drag(me);
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    int pid = 0;
    for (UITouch *touch in touches) {
        CGPoint loc = [touch locationInView:self];
        pulp::view::MouseEvent me;
        me.position = {static_cast<float>(loc.x), static_cast<float>(loc.y)};
        me.window_position = me.position;
        me.button = pulp::view::MouseButton::left;
        me.pointer_id = pid++;
        me.is_down = false;
        me.modifiers = 0x8000;
        self.rootView->on_mouse_up(me);
    }
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self touchesEnded:touches withEvent:event];
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

std::unique_ptr<WindowHost> WindowHost::create(View& root, const WindowOptions& options) {
    return std::make_unique<IOSWindowHost>(root, options);
}

} // namespace pulp::view

#endif // TARGET_OS_IOS
