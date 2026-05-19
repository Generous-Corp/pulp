#include <pulp/view/plugin_view_host.hpp>

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/canvas/cg_canvas.hpp>
#import <Cocoa/Cocoa.h>
#include <algorithm>
#include <atomic>

#ifdef PULP_HAS_SKIA
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
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
        }
    }

    ~MacPluginViewHost() override {
        root_.set_plugin_view_host(nullptr);
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
    PulpPluginView* view_ = nil;
};

} // namespace pulp::view (close for ObjC declarations)

// ── MacGpuPluginViewHost (Dawn/Skia Graphite) ────────────────────────────────

#ifdef PULP_HAS_SKIA

// CAMetalLayer-backed NSView for DAW-embedded GPU rendering
@interface PulpGpuPluginView : NSView
@property (nonatomic, readonly) CAMetalLayer* metalLayer;
@property (nonatomic, assign) pulp::view::View* rootView;
@end

@implementation PulpGpuPluginView

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
        layer.drawableSize = CGSizeMake(frame.size.width * scale, frame.size.height * scale);
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
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    CGFloat scale = self.window ? self.window.backingScaleFactor
                                : [NSScreen mainScreen].backingScaleFactor;
    self.metalLayer.contentsScale = scale;
    self.metalLayer.drawableSize = CGSizeMake(self.bounds.size.width * scale,
                                              self.bounds.size.height * scale);
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
        : root_(root), size_(size) {
        @autoreleasepool {
            NSRect frame = NSMakeRect(0, 0, size.width, size.height);
            metal_view_ = [[PulpGpuPluginView alloc] initWithFrame:frame];
            metal_view_.rootView = &root_;
            init_gpu(size.width, size.height);
        }
    }

    ~MacGpuPluginViewHost() override {
        root_.set_plugin_view_host(nullptr);
        stop_display_link();
        skia_surface_.reset();
        gpu_surface_.reset();
        metal_view_.rootView = nullptr;
    }

    NativeViewHandle native_handle() override {
        return (__bridge void*)metal_view_;
    }

    void attach_to_parent(NativeViewHandle parent) override {
        @autoreleasepool {
            NSView* parent_view = (__bridge NSView*)parent;
            if (parent_view && metal_view_) {
                [parent_view addSubview:metal_view_];
                start_display_link();
                needs_repaint_.store(true);
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
        }
        if (gpu_surface_) {
            gpu_surface_->resize(width, height);
        }
        if (skia_surface_) {
            CGFloat scale = metal_view_.metalLayer.contentsScale;
            skia_surface_->resize(width, height, static_cast<float>(scale));
        }
        needs_repaint_.store(true);
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
    PulpGpuPluginView* metal_view_ = nil;

    std::unique_ptr<render::GpuSurface> gpu_surface_;
    std::unique_ptr<render::SkiaSurface> skia_surface_;
    CVDisplayLinkRef display_link_ = nullptr;
    std::atomic<bool> needs_repaint_{true};

    void init_gpu(uint32_t width, uint32_t height) {
        gpu_surface_ = render::GpuSurface::create_dawn();
        if (!gpu_surface_) return;

        render::GpuSurface::Config config{};
        config.width = width;
        config.height = height;
        config.native_surface_handle = (__bridge void*)metal_view_.metalLayer;

        if (!gpu_surface_->initialize(config)) {
            gpu_surface_.reset();
            return;
        }

        CGFloat scale = metal_view_.metalLayer.contentsScale;
        render::SkiaSurface::Config skia_config{};
        skia_config.width = width;
        skia_config.height = height;
        skia_config.scale_factor = static_cast<float>(scale);

        skia_surface_ = render::SkiaSurface::create(*gpu_surface_, skia_config);
    }

    void render_frame() {
        if (!gpu_surface_ || !skia_surface_ || !needs_repaint_.load(std::memory_order_relaxed))
            return;

        if (!gpu_surface_->begin_frame()) return;

        if (auto* canvas = skia_surface_->begin_frame()) {
            float w = static_cast<float>(size_.width);
            float h = static_cast<float>(size_.height);
            root_.set_bounds({0, 0, w, h});
            root_.layout_children();

            canvas->set_fill_color(pulp::canvas::Color::rgba8(30, 30, 46));
            canvas->fill_rect(0, 0, w, h);
            root_.paint_all(*canvas);
        }

        skia_surface_->end_frame();
        gpu_surface_->end_frame();
        needs_repaint_.store(false, std::memory_order_relaxed);
    }

    static CVReturn display_link_callback(
        CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*,
        CVOptionFlags, CVOptionFlags*, void* context)
    {
        auto* self = static_cast<MacGpuPluginViewHost*>(context);
        if (self->needs_repaint_.load(std::memory_order_relaxed)) {
            dispatch_async(dispatch_get_main_queue(), ^{
                self->render_frame();
            });
        }
        return kCVReturnSuccess;
    }

    void start_display_link() {
        if (display_link_) return;
        CVDisplayLinkCreateWithActiveCGDisplays(&display_link_);
        CVDisplayLinkSetOutputCallback(display_link_, display_link_callback, this);
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
        root.set_plugin_view_host(host.get());
        return host;
    }
#endif
    auto host = std::make_unique<MacPluginViewHost>(root, options.size);
    root.set_plugin_view_host(host.get());
    return host;
}

} // namespace pulp::view

#endif // TARGET_OS_OSX
