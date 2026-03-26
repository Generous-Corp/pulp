#include <pulp/view/plugin_view_host.hpp>

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/canvas/cg_canvas.hpp>
#import <Cocoa/Cocoa.h>

// ── PulpPluginView: NSView subclass for DAW embedding ────────────────────────

@interface PulpPluginView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
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

- (BOOL)isFlipped { return YES; }
- (BOOL)isAccessibilityElement { return YES; }
- (NSAccessibilityRole)accessibilityRole { return NSAccessibilityGroupRole; }
- (NSString*)accessibilityLabel { return @"Plugin UI"; }

- (NSArray*)accessibilityChildren {
    if (!self.rootView) return @[];
    NSMutableArray* children = [NSMutableArray array];
    [self collectAccessibleChildren:self.rootView into:children];
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

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self setNeedsDisplay:YES];
}

@end

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
        }
    }

    ~MacPluginViewHost() override {
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

private:
    View& root_;
    Size size_;
    PulpPluginView* view_ = nil;
};

std::unique_ptr<PluginViewHost> PluginViewHost::create(View& root, Size size) {
    return std::make_unique<MacPluginViewHost>(root, size);
}

} // namespace pulp::view

#endif // TARGET_OS_OSX
