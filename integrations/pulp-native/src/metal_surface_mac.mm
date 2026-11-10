// MetalSurface on macOS: a runtime-registered NSView subclass backed by a
// CAMetalLayer. Compiled with ARC.

#include <pulp/native/metal_surface.hpp>

#include "surface_input_mac.h"

#include <pulp/platform/objc_unique_class.hpp>

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>

#if !__has_feature(objc_arc)
#error "metal_surface_mac.mm must be compiled with -fobjc-arc"
#endif

namespace pulp::native {

namespace {

constexpr const char* kViewClassBase = "PulpNativeSurfaceView";

bool supported_pixel_format(std::uint32_t format) {
    switch (static_cast<MTLPixelFormat>(format)) {
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRGBA16Float:
    case MTLPixelFormatRGB10A2Unorm:
    case MTLPixelFormatBGR10A2Unorm:
        return true;
    default:
        return false;
    }
}

} // namespace

struct MetalSurface::Impl {
    MetalSurfaceOptions options;
    detail::SurfaceInput input;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    CAMetalLayer* layer = nil;
    NSView* view = nil;
    NSWindow* observed_window = nil;
    NSMutableArray* window_observers = [NSMutableArray array];

    bool attached = false;
    bool invalidated = false;
    std::function<void(const SurfaceEvent&)> listener;
    std::function<void()> release_handler;
    OnScreenState last_visibility;

    // Read from any thread; written on the main thread.
    mutable std::mutex state_mutex;
    SurfaceGeometry geometry;
    bool accepting = false;

    // Main thread: recompute the size and publish it.
    void update_geometry() {
        NSWindow* window = view.window;
        const double scale = window ? window.backingScaleFactor : 0.0;
        const NSSize points = view ? view.bounds.size : NSZeroSize;
        if (window && scale > 0.0) {
            layer.contentsScale = scale;
            if (options.sizing == DrawableSizing::automatic) {
                const CGSize pixels =
                    CGSizeMake(std::round(points.width * scale), std::round(points.height * scale));
                if (pixels.width >= 1.0 && pixels.height >= 1.0 &&
                    !CGSizeEqualToSize(pixels, layer.drawableSize))
                    layer.drawableSize = pixels;
            }
        }
        const CGSize drawable = layer.drawableSize;
        std::lock_guard lock(state_mutex);
        SurfaceGeometry next = geometry;
        next.width = points.width;
        next.height = points.height;
        next.scale = scale;
        next.drawable_width = static_cast<std::uint32_t>(drawable.width);
        next.drawable_height = static_cast<std::uint32_t>(drawable.height);
        next.in_window = window != nil;
        const bool changed = next.width != geometry.width || next.height != geometry.height ||
                             next.scale != geometry.scale ||
                             next.drawable_width != geometry.drawable_width ||
                             next.drawable_height != geometry.drawable_height ||
                             next.in_window != geometry.in_window;
        if (changed)
            ++next.generation;
        geometry = next;
        accepting = attached && window != nil;
    }

    OnScreenState visibility_now() const {
        OnScreenState state;
        NSWindow* window = view.window;
        state.in_window = window != nil;
        if (window) {
            state.window_visible = window.isVisible;
            state.minimized = window.isMiniaturized;
            state.occluded = (window.occlusionState & NSWindowOcclusionStateVisible) == 0;
        }
        return state;
    }

    void emit(SurfaceEventKind kind) {
        if (!listener)
            return;
        SurfaceEvent event;
        event.kind = kind;
        {
            std::lock_guard lock(state_mutex);
            event.geometry = geometry;
        }
        event.visibility = last_visibility;
        // Copy first: the listener may replace itself.
        auto callback = listener;
        callback(event);
    }

    void check_visibility() {
        const auto now = visibility_now();
        if (now == last_visibility)
            return;
        last_visibility = now;
        emit(SurfaceEventKind::visibility_changed);
    }

    void stop_observing() {
        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
        for (id token in window_observers)
            [center removeObserver:token];
        [window_observers removeAllObjects];
        observed_window = nil;
    }

    void observe_window(NSWindow* window) {
        if (window == observed_window)
            return;
        stop_observing();
        observed_window = window;
        if (!window)
            return;
        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
        Impl* owner = this;
        auto watch = ^(NSNotificationName name) {
          id token = [center addObserverForName:name
                                         object:window
                                          queue:nil
                                     usingBlock:^(NSNotification*) {
                                       owner->check_visibility();
                                     }];
          [window_observers addObject:token];
        };
        watch(NSWindowDidChangeOcclusionStateNotification);
        watch(NSWindowDidMiniaturizeNotification);
        watch(NSWindowDidDeminiaturizeNotification);
        watch(NSWindowDidChangeScreenNotification);

        auto focus = ^(NSNotificationName name, id object, bool focused, FocusReason reason) {
          id token = [center addObserverForName:name
                                         object:object
                                          queue:nil
                                     usingBlock:^(NSNotification*) {
                                       owner->input.emit_focus(focused, reason);
                                     }];
          [window_observers addObject:token];
        };
        focus(NSWindowDidBecomeKeyNotification, window, true, FocusReason::window);
        focus(NSWindowDidResignKeyNotification, window, false, FocusReason::window);
        focus(NSApplicationDidBecomeActiveNotification, NSApp, true, FocusReason::application);
        focus(NSApplicationDidResignActiveNotification, NSApp, false, FocusReason::application);
    }

    // View callbacks (main thread).
    void window_changed() {
        observe_window(view.window);
        update_geometry();
        emit(SurfaceEventKind::window_changed);
        check_visibility();
    }
    void geometry_changed() {
        SurfaceGeometry before;
        {
            std::lock_guard lock(state_mutex);
            before = geometry;
        }
        update_geometry();
        std::uint64_t after = 0;
        {
            std::lock_guard lock(state_mutex);
            after = geometry.generation;
        }
        if (after != before.generation)
            emit(SurfaceEventKind::geometry_changed);
    }

    // The first phase of every teardown.
    void stop_drawables() {
        {
            std::lock_guard lock(state_mutex);
            accepting = false;
        }
        if (release_handler) {
            auto handler = release_handler;
            handler();
        }
    }
};

namespace {

MetalSurface::Impl* impl_of(id view) {
    return static_cast<MetalSurface::Impl*>(
        pulp::platform::mac::instance_context((__bridge void*)view));
}

Class view_class();

void call_super(id self, SEL sel) {
    using Fn = void (*)(id, SEL);
    if (auto fn = reinterpret_cast<Fn>(pulp::platform::mac::super_implementation(
            (__bridge void*)view_class(), sel_getName(sel))))
        fn(self, sel);
}

CALayer* imp_make_backing_layer(id self, SEL) {
    if (auto* impl = impl_of(self))
        return impl->layer;
    return [CALayer layer];
}

BOOL imp_yes(id, SEL) {
    return YES;
}

BOOL imp_is_opaque(id self, SEL) {
    auto* impl = impl_of(self);
    return impl ? impl->options.opaque : NO;
}

void imp_view_did_move_to_window(id self, SEL sel) {
    call_super(self, sel);
    if (auto* impl = impl_of(self))
        impl->window_changed();
}

void imp_view_did_change_backing_properties(id self, SEL sel) {
    call_super(self, sel);
    if (auto* impl = impl_of(self))
        impl->geometry_changed();
}

void imp_set_frame_size(id self, SEL sel, NSSize size) {
    using Fn = void (*)(id, SEL, NSSize);
    if (auto fn = reinterpret_cast<Fn>(pulp::platform::mac::super_implementation(
            (__bridge void*)view_class(), sel_getName(sel))))
        fn(self, sel, size);
    if (auto* impl = impl_of(self))
        impl->geometry_changed();
}

void configure_view_class(void* cls_ptr, void*) {
    Class cls = (__bridge Class)cls_ptr;
    class_addMethod(cls, @selector(makeBackingLayer), (IMP)imp_make_backing_layer, "@@:");
    class_addMethod(cls, @selector(wantsUpdateLayer), (IMP)imp_yes, "c@:");
    class_addMethod(cls, @selector(isFlipped), (IMP)imp_yes, "c@:");
    class_addMethod(cls, @selector(isOpaque), (IMP)imp_is_opaque, "c@:");
    class_addMethod(cls, @selector(viewDidMoveToWindow), (IMP)imp_view_did_move_to_window, "v@:");
    class_addMethod(cls, @selector(viewDidChangeBackingProperties),
                    (IMP)imp_view_did_change_backing_properties, "v@:");
    class_addMethod(cls, @selector(setFrameSize:), (IMP)imp_set_frame_size,
                    (std::string("v@:") + @encode(NSSize)).c_str());
    detail::add_input_methods(cls);
}

Class view_class() {
    static Class cls = (__bridge Class)pulp::platform::mac::unique_objc_class(
        kViewClassBase, (__bridge void*)[NSView class], &configure_view_class);
    return cls;
}

} // namespace

namespace detail {

SurfaceInput* surface_input_of(id view) {
    auto* impl = impl_of(view);
    return impl ? &impl->input : nullptr;
}

Class surface_view_class() {
    return view_class();
}

} // namespace detail

// ── MetalDrawable ────────────────────────────────────────────────────────────

MetalDrawable::MetalDrawable(MetalDrawable&& other) noexcept
    : drawable_(std::exchange(other.drawable_, nullptr)),
      presented_(std::exchange(other.presented_, false)) {}

MetalDrawable& MetalDrawable::operator=(MetalDrawable&& other) noexcept {
    if (this != &other) {
        reset();
        drawable_ = std::exchange(other.drawable_, nullptr);
        presented_ = std::exchange(other.presented_, false);
    }
    return *this;
}

MetalDrawable::~MetalDrawable() {
    reset();
}

void MetalDrawable::reset() noexcept {
    if (drawable_)
        CFRelease(drawable_);
    drawable_ = nullptr;
    presented_ = false;
}

void* MetalDrawable::texture() const noexcept {
    if (!drawable_)
        return nullptr;
    return (__bridge void*)((__bridge id<CAMetalDrawable>)drawable_).texture;
}

std::uint32_t MetalDrawable::width() const noexcept {
    if (!drawable_)
        return 0;
    return static_cast<std::uint32_t>(((__bridge id<CAMetalDrawable>)drawable_).texture.width);
}

std::uint32_t MetalDrawable::height() const noexcept {
    if (!drawable_)
        return 0;
    return static_cast<std::uint32_t>(((__bridge id<CAMetalDrawable>)drawable_).texture.height);
}

bool MetalDrawable::present(void* command_buffer) {
    if (!drawable_ || presented_)
        return false;
    @autoreleasepool {
        id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>)drawable_;
        if (command_buffer)
            [(__bridge id<MTLCommandBuffer>)command_buffer presentDrawable:drawable];
        else
            [drawable present];
    }
    presented_ = true;
    return true;
}

// ── MetalSurface ─────────────────────────────────────────────────────────────

std::unique_ptr<MetalSurface> MetalSurface::create(const MetalSurfaceOptions& options) {
    if (!supported_pixel_format(options.pixel_format))
        return nullptr;
    if (options.maximum_drawable_count < 2 || options.maximum_drawable_count > 3)
        return nullptr;
    Class cls = view_class();
    if (!cls)
        return nullptr;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
        return nullptr;

    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->device = device;
    impl->queue = [device newCommandQueue];

    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = device;
    layer.pixelFormat = static_cast<MTLPixelFormat>(options.pixel_format);
    layer.framebufferOnly = options.framebuffer_only;
    layer.opaque = options.opaque;
    layer.maximumDrawableCount = options.maximum_drawable_count;
    // Bounds every nextDrawable wait, so teardown cannot hang on one.
    layer.allowsNextDrawableTimeout = YES;
    layer.needsDisplayOnBoundsChange = NO;
    impl->layer = layer;

    NSView* view = [[cls alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
    // The context goes in before layer backing is turned on, so
    // makeBackingLayer already returns this surface's layer.
    pulp::platform::mac::set_instance_context((__bridge void*)view, impl.get());
    view.wantsLayer = YES;
    view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    impl->view = view;
    impl->update_geometry();
    return std::unique_ptr<MetalSurface>(new MetalSurface(std::move(impl)));
}

MetalSurface::MetalSurface(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MetalSurface::~MetalSurface() {
    if (impl_->attached)
        detach();
    impl_->stop_observing();
    if (impl_->view)
        pulp::platform::mac::set_instance_context((__bridge void*)impl_->view, nullptr);
}

bool MetalSurface::attach(void* parent) {
    if (!parent || impl_->attached || impl_->invalidated || !impl_->view)
        return false;
    NSView* parent_view = (__bridge NSView*)parent;
    impl_->view.frame = parent_view.bounds;
    impl_->attached = true;
    [parent_view addSubview:impl_->view];
    // Adding the view to a parent that is already in a window delivers
    // viewDidMoveToWindow; for one that is not, the window comes later.
    impl_->observe_window(impl_->view.window);
    impl_->update_geometry();
    impl_->last_visibility = impl_->visibility_now();
    return true;
}

void MetalSurface::detach() {
    if (!impl_->attached)
        return;
    impl_->stop_drawables();
    impl_->attached = false;
    impl_->stop_observing();
    [impl_->view removeFromSuperview];
    impl_->update_geometry();
}

void MetalSurface::invalidate() {
    if (impl_->invalidated)
        return;
    impl_->stop_drawables();
    impl_->attached = false;
    impl_->invalidated = true;
    impl_->stop_observing();
    if (impl_->view) {
        // The parent still holds the view; leave it there untouched and make
        // every callback it delivers from now on a no-op.
        pulp::platform::mac::set_instance_context((__bridge void*)impl_->view, nullptr);
        impl_->view = nil;
    }
    std::lock_guard lock(impl_->state_mutex);
    impl_->accepting = false;
}

bool MetalSurface::attached() const noexcept {
    return impl_->attached;
}

void* MetalSurface::view() const noexcept {
    return (__bridge void*)impl_->view;
}

void* MetalSurface::device() const noexcept {
    return (__bridge void*)impl_->device;
}

void* MetalSurface::command_queue() const noexcept {
    return (__bridge void*)impl_->queue;
}

std::uint32_t MetalSurface::pixel_format() const noexcept {
    return static_cast<std::uint32_t>(impl_->layer.pixelFormat);
}

DrawableSizing MetalSurface::sizing() const noexcept {
    return impl_->options.sizing;
}

SurfaceGeometry MetalSurface::geometry() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->geometry;
}

OnScreenState MetalSurface::on_screen() const {
    return impl_->visibility_now();
}

void MetalSurface::poll_visibility() {
    impl_->check_visibility();
}

MetalDrawable MetalSurface::acquire_drawable() {
    CAMetalLayer* layer = nil;
    {
        std::lock_guard lock(impl_->state_mutex);
        if (!impl_->accepting)
            return {};
        // A strong local reference: the layer outlives this call even if
        // teardown starts while nextDrawable waits.
        layer = impl_->layer;
    }
    @autoreleasepool {
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable)
            return {};
        return MetalDrawable((__bridge_retained void*)drawable);
    }
}

bool MetalSurface::set_drawable_size(std::uint32_t width, std::uint32_t height) {
    if (impl_->options.sizing != DrawableSizing::client || width == 0 || height == 0)
        return false;
    impl_->layer.drawableSize = CGSizeMake(width, height);
    impl_->geometry_changed();
    return true;
}

void MetalSurface::set_listener(std::function<void(const SurfaceEvent&)> listener) {
    impl_->listener = std::move(listener);
}

void MetalSurface::set_release_handler(std::function<void()> handler) {
    impl_->release_handler = std::move(handler);
}

void MetalSurface::set_input_handler(InputHandler handler) {
    impl_->input.handler = std::move(handler);
}

void MetalSurface::set_cursor(Cursor cursor) {
    impl_->input.cursor = cursor;
    if (impl_->input.pointer_inside)
        [(__bridge NSCursor*)ns_cursor_for(cursor) set];
}

bool MetalSurface::set_text_focus(bool held) {
    NSView* view = impl_->view;
    NSWindow* window = view.window;
    if (!view || !window)
        return false;
    if (held) {
        impl_->input.text_focus = true;
        if (window.firstResponder == view)
            return true;
        NSResponder* prior = window.firstResponder;
        if ([window makeFirstResponder:view]) {
            impl_->input.prior_responder = prior;
            return true;
        }
        impl_->input.text_focus = false;
        return false;
    }
    impl_->input.text_focus = false;
    if (window.firstResponder != view)
        return true;
    // Hand the keyboard back to whatever held it before, not to nil: a host
    // whose key routing hangs off its own responder (Musical Typing) goes
    // silent otherwise.
    NSResponder* prior = impl_->input.prior_responder;
    impl_->input.prior_responder = nil;
    if (prior && prior != view && [window makeFirstResponder:prior])
        return true;
    return [window makeFirstResponder:nil];
}

bool MetalSurface::text_focus() const noexcept {
    return impl_->input.text_focus;
}

std::string MetalSurface::view_class_name() {
    return pulp::platform::mac::unique_objc_class_name(kViewClassBase);
}

} // namespace pulp::native
