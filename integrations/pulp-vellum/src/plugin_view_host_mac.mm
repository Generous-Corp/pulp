// macOS editor host for a Vellum view tree: a layer-backed NSView that paints
// through Vellum's raster renderer and routes mouse and wheel input through
// Vellum's pointer delivery. See pulp/vellum/plugin_view_host.hpp.
//
// The NSView subclass is registered at run time under a name unique to this
// image. Every plug-in links its own copy of this file, and a DAW loads many
// plug-ins into one Objective-C runtime; a class name compiled in would be
// shared by all of them, and the runtime would pick one image's class for
// every plug-in.

#include <pulp/vellum/plugin_view_host.hpp>

#include <vellum/view/input_events.hpp>
#include <vellum/view/pointer_dispatch.hpp>
#include <vellum/view/screenshot.hpp>
#include <vellum/view/view.hpp>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <objc/runtime.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if !__has_feature(objc_arc)
#error "plugin_view_host_mac.mm must be compiled with -fobjc-arc"
#endif

namespace pulp::vellum::detail {
namespace {

namespace vv = ::vellum::view;

class MacRasterPluginViewHost;

Ivar host_ivar();

MacRasterPluginViewHost* host_of(id view) {
    Ivar ivar = host_ivar();
    if (view == nil || ivar == nullptr)
        return nullptr;
    auto* base = static_cast<char*>((__bridge void*)view);
    return *reinterpret_cast<MacRasterPluginViewHost**>(base + ivar_getOffset(ivar));
}

void set_host_of(id view, MacRasterPluginViewHost* host) {
    Ivar ivar = host_ivar();
    if (view == nil || ivar == nullptr)
        return;
    auto* base = static_cast<char*>((__bridge void*)view);
    *reinterpret_cast<MacRasterPluginViewHost**>(base + ivar_getOffset(ivar)) = host;
}

std::uint16_t modifiers_of(NSEvent* event) {
    const NSEventModifierFlags flags = event.modifierFlags;
    std::uint16_t mods = vv::kModNone;
    if (flags & NSEventModifierFlagShift)
        mods |= vv::kModShift;
    if (flags & NSEventModifierFlagControl)
        mods |= vv::kModCtrl;
    if (flags & NSEventModifierFlagOption)
        mods |= vv::kModAlt;
    if (flags & NSEventModifierFlagCommand)
        mods |= vv::kModCmd | vv::kModMeta;
    return mods;
}

class MacRasterPluginViewHost final : public vv::PluginViewHost {
  public:
    MacRasterPluginViewHost(vv::View& root, const Options& options);
    ~MacRasterPluginViewHost() override;

    vv::NativeViewHandle native_handle() override {
        return (__bridge void*)view_;
    }

    void attach_to_parent(vv::NativeViewHandle parent) override {
        auto* parent_view = (__bridge NSView*)parent;
        if (parent_view == nil || view_ == nil)
            return;
        if (view_.superview != parent_view) {
            [view_ removeFromSuperview];
            [view_ setFrameOrigin:NSZeroPoint];
            [parent_view addSubview:view_];
        }
        repaint();
    }

    bool is_attached() const noexcept override {
        return view_ != nil && view_.superview != nil;
    }

    void detach() override {
        if (view_ != nil)
            [view_ removeFromSuperview];
    }

    void repaint() override {
        NSView* view = view_;
        if (view == nil)
            return;
        if ([NSThread isMainThread]) {
            view.needsDisplay = YES;
        } else {
            dispatch_async(dispatch_get_main_queue(), ^{
              view.needsDisplay = YES;
            });
        }
    }

    void set_size(std::uint32_t width, std::uint32_t height) override {
        if (width == 0 || height == 0)
            return;
        size_ = {width, height};
        [view_ setFrameSize:NSMakeSize(width, height)];
        repaint();
    }

    Size get_size() const override {
        return size_;
    }

    bool present_first_frame() override {
        return paint();
    }

    std::vector<std::uint8_t> capture_back_buffer_png() override {
        return vv::render_to_png(root_, size_.width, size_.height, backing_scale());
    }

    /// Render the tree into the layer. Called from -updateLayer, and from
    /// present_first_frame() before the host first composites the view.
    /// Returns true when a frame reached the layer.
    bool paint();

    void mouse_down(NSEvent* event);
    void mouse_dragged(NSEvent* event);
    void mouse_up(NSEvent* event);
    void scroll_wheel(NSEvent* event);

  private:
    float backing_scale() const {
        NSWindow* window = view_.window;
        return window != nil ? static_cast<float>(window.backingScaleFactor) : 1.0f;
    }

    vv::Point point_of(NSEvent* event) const {
        const NSPoint p = [view_ convertPoint:event.locationInWindow fromView:nil];
        return {static_cast<float>(p.x), static_cast<float>(p.y)};
    }

    vv::View& root_;
    NSView* view_ = nil;
    Size size_;
    std::uint32_t background_rgb_;
    vv::View* press_target_ = nullptr;
};

// Instance methods of the runtime-registered NSView subclass. The view is
// flipped so its coordinates match the view tree's top-left origin.
BOOL view_is_flipped(id, SEL) {
    return YES;
}
BOOL view_wants_update_layer(id, SEL) {
    return YES;
}
BOOL view_accepts_first_mouse(id, SEL, NSEvent*) {
    return YES;
}
void view_update_layer(id self, SEL) {
    if (auto* host = host_of(self))
        host->paint();
}
void view_mouse_down(id self, SEL, NSEvent* event) {
    if (auto* host = host_of(self))
        host->mouse_down(event);
}
void view_mouse_dragged(id self, SEL, NSEvent* event) {
    if (auto* host = host_of(self))
        host->mouse_dragged(event);
}
void view_mouse_up(id self, SEL, NSEvent* event) {
    if (auto* host = host_of(self))
        host->mouse_up(event);
}
void view_scroll_wheel(id self, SEL, NSEvent* event) {
    if (auto* host = host_of(self))
        host->scroll_wheel(event);
}

Class editor_view_class() {
    static Class cls = [] {
        // The address of a function in this image names the class, so each
        // plug-in image registers its own.
        char name[96];
        std::snprintf(
            name, sizeof name, "PulpVellumEditorView_%llx",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&editor_view_class)));
        Class existing = objc_getClass(name);
        if (existing != Nil)
            return existing;
        Class created = objc_allocateClassPair([NSView class], name, 0);
        if (created == Nil)
            return Class{Nil};
        class_addIvar(created, "pulp_vellum_host", sizeof(void*), alignof(void*) == 8 ? 3 : 2,
                      "^v");
        class_addMethod(created, @selector(isFlipped), reinterpret_cast<IMP>(view_is_flipped),
                        "c@:");
        class_addMethod(created, @selector(wantsUpdateLayer),
                        reinterpret_cast<IMP>(view_wants_update_layer), "c@:");
        class_addMethod(created, @selector(acceptsFirstMouse:),
                        reinterpret_cast<IMP>(view_accepts_first_mouse), "c@:@");
        class_addMethod(created, @selector(updateLayer), reinterpret_cast<IMP>(view_update_layer),
                        "v@:");
        class_addMethod(created, @selector(mouseDown:), reinterpret_cast<IMP>(view_mouse_down),
                        "v@:@");
        class_addMethod(created, @selector(mouseDragged:),
                        reinterpret_cast<IMP>(view_mouse_dragged), "v@:@");
        class_addMethod(created, @selector(mouseUp:), reinterpret_cast<IMP>(view_mouse_up), "v@:@");
        class_addMethod(created, @selector(scrollWheel:), reinterpret_cast<IMP>(view_scroll_wheel),
                        "v@:@");
        objc_registerClassPair(created);
        return created;
    }();
    return cls;
}

Ivar host_ivar() {
    static Ivar ivar = [] {
        Class cls = editor_view_class();
        return cls != Nil ? class_getInstanceVariable(cls, "pulp_vellum_host") : nullptr;
    }();
    return ivar;
}

MacRasterPluginViewHost::MacRasterPluginViewHost(vv::View& root, const Options& options)
    : root_(root), size_(options.size), background_rgb_(options.background_rgb) {
    if (size_.width == 0 || size_.height == 0)
        size_ = {400, 300};
    Class cls = editor_view_class();
    if (cls == Nil)
        return;
    view_ = [[cls alloc] initWithFrame:NSMakeRect(0, 0, size_.width, size_.height)];
    set_host_of(view_, this);
    view_.wantsLayer = YES;
    view_.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
    const CGFloat r = ((background_rgb_ >> 16) & 0xff) / 255.0;
    const CGFloat g = ((background_rgb_ >> 8) & 0xff) / 255.0;
    const CGFloat b = (background_rgb_ & 0xff) / 255.0;
    view_.layer.backgroundColor = [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0].CGColor;
    view_.layer.contentsGravity = kCAGravityTopLeft;
}

MacRasterPluginViewHost::~MacRasterPluginViewHost() {
    if (root_.plugin_view_host() == this)
        root_.set_plugin_view_host(nullptr);
    if (view_ != nil) {
        set_host_of(view_, nullptr);
        [view_ removeFromSuperview];
        view_ = nil;
    }
}

bool MacRasterPluginViewHost::paint() {
    if (view_ == nil)
        return false;
    const NSSize bounds = view_.bounds.size;
    const auto width = static_cast<std::uint32_t>(bounds.width);
    const auto height = static_cast<std::uint32_t>(bounds.height);
    if (width == 0 || height == 0)
        return false;
    const float scale = backing_scale();
    std::uint32_t pixel_w = 0;
    std::uint32_t pixel_h = 0;
    std::vector<std::uint8_t> pixels =
        vv::render_to_rgba(root_, width, height, scale, &pixel_w, &pixel_h);
    clear_pending_dirty();
    if (pixels.empty() || pixel_w == 0 || pixel_h == 0)
        return false;

    CFDataRef data = CFDataCreate(nullptr, pixels.data(), static_cast<CFIndex>(pixels.size()));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image =
        CGImageCreate(pixel_w, pixel_h, 8, 32, static_cast<size_t>(pixel_w) * 4u, space,
                      static_cast<CGBitmapInfo>(kCGBitmapByteOrderDefault) |
                          static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast),
                      provider, nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    CFRelease(data);
    if (image == nullptr)
        return false;
    view_.layer.contentsScale = scale;
    view_.layer.contents = (__bridge id)image;
    CGImageRelease(image);
    return view_.layer.contents != nil;
}

void MacRasterPluginViewHost::mouse_down(NSEvent* event) {
    const vv::Point pt = point_of(event);
    press_target_ = root_.hit_test(pt);
    if (press_target_ == nullptr)
        return;
    vv::deliver_mouse_down(root_, press_target_, pt, modifiers_of(event),
                           static_cast<int>(event.clickCount));
    repaint();
}

void MacRasterPluginViewHost::mouse_dragged(NSEvent* event) {
    if (press_target_ == nullptr)
        return;
    vv::deliver_mouse_drag(root_, press_target_, point_of(event), modifiers_of(event),
                           static_cast<int>(event.clickCount));
    repaint();
}

void MacRasterPluginViewHost::mouse_up(NSEvent* event) {
    vv::View* target = press_target_;
    press_target_ = nullptr;
    if (target == nullptr)
        return;
    vv::MouseUpHost host;
    host.fire_click = [](const std::function<void()>& click, const std::string&, std::uint16_t) {
        if (click)
            click();
    };
    vv::deliver_mouse_up(root_, target, point_of(event), modifiers_of(event),
                         static_cast<int>(event.clickCount), host);
    repaint();
}

void MacRasterPluginViewHost::scroll_wheel(NSEvent* event) {
    vv::WheelHost host;
    host.request_repaint = [this] { repaint(); };
    // NSEvent's deltas are bottom-up; the view tree's convention is negated.
    vv::deliver_mouse_wheel(root_, point_of(event), static_cast<float>(event.scrollingDeltaX),
                            -static_cast<float>(event.scrollingDeltaY), modifiers_of(event), host);
}

std::atomic<bool> g_gpu_request_logged{false};

} // namespace

std::unique_ptr<::vellum::view::PluginViewHost>
make_platform_plugin_view_host(::vellum::view::View& root,
                               const ::vellum::view::PluginViewHost::Options& options) {
    if (options.use_gpu && !g_gpu_request_logged.exchange(true))
        std::fprintf(stderr, "[pulp-vellum] this editor asked for a GPU host; the installed "
                             "Vellum SDK exposes none, so it opens on the raster host\n");
    auto host = std::make_unique<MacRasterPluginViewHost>(root, options);
    if (host->native_handle() == nullptr)
        return nullptr;
    return host;
}

bool platform_plugin_view_host_available() noexcept {
    return true;
}

} // namespace pulp::vellum::detail
