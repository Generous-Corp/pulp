// iOS plugin view host — UIView-based embedding for AUv3 in iOS hosts
// Mirrors plugin_view_host_mac.mm but uses UIKit instead of AppKit.

#include <pulp/view/plugin_view_host.hpp>

#if TARGET_OS_IOS

#include <pulp/canvas/cg_canvas.hpp>
#import <UIKit/UIKit.h>

// ── PulpPluginUIView: UIView subclass for DAW embedding on iOS ──────────────

@interface PulpPluginUIView : UIView
@property (nonatomic, assign) pulp::view::View* rootView;
@end

@implementation PulpPluginUIView

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

    // UIKit has origin at top-left (same as Pulp), but Core Graphics
    // has origin at bottom-left. Flip the coordinate system.
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, bounds.size.height);
    CGContextScaleCTM(ctx, 1.0, -1.0);

    pulp::canvas::CoreGraphicsCanvas canvas(ctx,
        static_cast<float>(bounds.size.width),
        static_cast<float>(bounds.size.height));

    // Clear background
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

    CGContextRestoreGState(ctx);
}

// ── Touch handling → MouseEvent with pointer_id ─────────────────────────────

- (void)dispatchTouches:(NSSet<UITouch *> *)touches phase:(BOOL)isDown {
    if (!self.rootView) return;

    int pointerId = 0;
    for (UITouch *touch in touches) {
        CGPoint loc = [touch locationInView:self];
        pulp::view::MouseEvent event;
        event.position = {static_cast<float>(loc.x), static_cast<float>(loc.y)};
        event.window_position = event.position;
        event.button = pulp::view::MouseButton::left;
        event.pointer_id = pointerId;
        event.is_down = isDown;
        event.modifiers = 0x8000; // Touch flag

        if (isDown) {
            self.rootView->on_mouse_down(event);
        } else {
            self.rootView->on_mouse_up(event);
        }
        ++pointerId;
    }
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self dispatchTouches:touches phase:YES];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    int pointerId = 0;
    for (UITouch *touch in touches) {
        CGPoint loc = [touch locationInView:self];
        pulp::view::MouseEvent me;
        me.position = {static_cast<float>(loc.x), static_cast<float>(loc.y)};
        me.window_position = me.position;
        me.button = pulp::view::MouseButton::left;
        me.pointer_id = pointerId;
        me.is_down = true;
        me.modifiers = 0x8000;
        self.rootView->on_mouse_drag(me);
        ++pointerId;
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self dispatchTouches:touches phase:NO];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self dispatchTouches:touches phase:NO];
}

// ── Safe area insets ────────────────────────────────────────────────────────

- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    [self setNeedsDisplay];
}

@end

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

private:
    View& root_;
    Size size_;
    PulpPluginUIView* view_ = nil;
};

// On iOS, the factory creates an iOS host; on macOS it creates a Mac host.
// The macOS factory is in plugin_view_host_mac.mm — the linker picks the
// correct one based on the platform target.
std::unique_ptr<PluginViewHost> PluginViewHost::create(View& root, Size size) {
    return std::make_unique<IOSPluginViewHost>(root, size);
}

} // namespace pulp::view

#endif // TARGET_OS_IOS
