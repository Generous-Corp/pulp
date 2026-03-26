#include <pulp/view/window_host.hpp>

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/canvas/cg_canvas.hpp>
#import <Cocoa/Cocoa.h>

// ── PulpView: NSView subclass that paints the View tree ──────────────────────

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

    // Clear background
    canvas.set_fill_color(pulp::canvas::Color::rgba(30, 30, 46)); // Dark theme bg
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

// ── PulpWindowDelegate ───────────────────────────────────────────────────────

@interface PulpWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, copy) void (^onClose)(void);
@end

@implementation PulpWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
    if (self.onClose) self.onClose();
    [NSApp terminate:nil];
    return YES;
}

@end

// ── MacWindowHost ────────────────────────────────────────────────────────────

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

    void show() override {
        [window_ makeKeyAndOrderFront:nil];
    }

    void hide() override {
        [window_ orderOut:nil];
    }

    bool is_visible() const override {
        return [window_ isVisible];
    }

    void repaint() override {
        [view_ setNeedsDisplay:YES];
    }

    void set_close_callback(std::function<void()> cb) override {
        close_callback_ = std::move(cb);
        delegate_.onClose = ^{
            if (close_callback_) close_callback_();
        };
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

std::unique_ptr<WindowHost> WindowHost::create(View& root, const WindowOptions& options) {
    return std::make_unique<MacWindowHost>(root, options);
}

} // namespace pulp::view

#endif // TARGET_OS_OSX
