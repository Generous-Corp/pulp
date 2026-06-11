#include <pulp/view/plugin_view_host.hpp>

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/canvas/cg_canvas.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/script_event_dispatch.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/window_host.hpp>  // compute_design_viewport_transform
#import <Cocoa/Cocoa.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include <functional>
#include <memory>

// Reuse the standalone window host's coordinate/event helpers (to_local,
// view_is_in_tree, modifiers_from_ns_flags) — same pulp-view-core lib.
#include "window_host_mac_internal.hpp"

#ifdef PULP_HAS_SKIA
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include "window_host_mac_capture.h"  // mac_capture::encode_rgba_to_png
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
@property (nonatomic, copy) void (^onResize)(uint32_t, uint32_t);
// Inverse-design-viewport transform applied to every host-space input point
// before hit_test, mirroring the standalone PulpView. nil = identity. Set
// by MacPluginViewHost::set_design_viewport.
@property (nonatomic, copy) pulp::view::Point (^pointTransform)(pulp::view::Point);
// Design viewport size. (0, 0) = identity (paint at host bounds, no
// scale/letterbox). When set, drawRect pins root to (designW, designH),
// fills letterbox bars at host bounds, then translate+scale before
// paint_all so the rendered surface matches the standalone host.
@property (nonatomic, assign) float designW;
@property (nonatomic, assign) float designH;
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

// ── Shared mouse-input dispatch for embedded plugin views ────────────────────
//
// Both PulpPluginView (CPU) and PulpGpuPluginView (GPU) embed a Pulp View tree
// in a DAW editor window and must route native mouse events into it. This is
// the same hit_test → on_mouse_event + on_mouse_down/drag/up + W3C pointer/drag
// bubbling that the standalone MacGpuWindowHost's PulpView does — without it the
// editor paints but swallows every click (the bug seen in Logic). Kept as free
// functions so the two ObjC view classes share one implementation.
namespace {

pulp::view::Point pulp_plugin_local_point(NSView* self, NSEvent* event) {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    // NSView is not flipped (both plugin views return isFlipped NO): Y=0 is the
    // bottom, so flip into the view tree's top-down space.
    const float h = static_cast<float>(self.bounds.size.height);
    return pulp::view::Point{static_cast<float>(p.x), h - static_cast<float>(p.y)};
}

// Dispatched handlers (on_click / on_mouse_* / on_pointer_event / on_drag) are
// std::function callbacks — for a scripted UI they reach into the JS bridge and
// CAN throw. If a C++ exception unwinds out of these functions it crosses the
// AppKit ObjC frame that delivered the event → undefined behavior / host crash.
// Wrap each dispatch in try/catch, exactly as the standalone PulpView mouse
// handlers do (window_host_mac.mm), so a throwing handler is contained.
void pulp_plugin_mouse_down(pulp::view::View* root, NSEvent* event,
                            pulp::view::Point pt, pulp::view::View** drag_target) {
  try {
    if (!root) return;
    *drag_target = root->hit_test(pt);
    pulp::view::ComboBox::notify_global_click(*drag_target);
    if (!*drag_target) return;
    using namespace pulp::view::mac_geometry;
    auto local = to_local(pt, *drag_target, root);
    if ((*drag_target)->focusable()) {
        auto* focused = pulp::view::View::focused_input_;
        if (focused != *drag_target) {
            if (focused && view_is_in_tree(focused, root))
                focused->on_focus_changed(false);
            (*drag_target)->on_focus_changed(true);
        }
        (*drag_target)->claim_input_focus();
    } else if (auto* focused = pulp::view::View::focused_input_;
               focused && view_is_in_tree(focused, root)) {
        focused->on_focus_changed(false);
        focused->release_input_focus();
    }

    pulp::view::MouseEvent me;
    me.position = local;
    me.window_position = pt;
    me.button = pulp::view::MouseButton::left;
    me.modifiers = modifiers_from_ns_flags(event.modifierFlags);
    me.is_down = true;
    me.phase = pulp::view::MousePhase::press;
    me.click_count = static_cast<int>(event.clickCount);
    (*drag_target)->on_mouse_event(me);
    (*drag_target)->on_mouse_down(local);
    // Bubble pointerdown to ancestors that registered on_pointer_event (React).
    for (auto* b = (*drag_target)->parent(); b; b = b->parent()) {
        if (!b->on_pointer_event) continue;
        pulp::view::MouseEvent bme = me;
        bme.position = to_local(pt, b, root);
        b->on_pointer_event(bme);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[plugin-view-host] mouseDown handler threw: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "[plugin-view-host] mouseDown handler threw (unknown)\n");
  }
}

void pulp_plugin_mouse_drag(pulp::view::View* root, pulp::view::Point pt,
                            pulp::view::View** drag_target) {
  try {
    using namespace pulp::view::mac_geometry;
    if (!*drag_target || !root) return;
    if (!view_is_in_tree(*drag_target, root)) { *drag_target = nullptr; return; }
    auto local = to_local(pt, *drag_target, root);
    (*drag_target)->on_mouse_drag(local);
    if ((*drag_target)->on_drag) (*drag_target)->on_drag(local);
    for (auto* b = (*drag_target)->parent(); b; b = b->parent()) {
        if (!b->on_drag) continue;
        (*b).on_drag(to_local(pt, b, root));
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[plugin-view-host] mouseDragged handler threw: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "[plugin-view-host] mouseDragged handler threw (unknown)\n");
  }
}

void pulp_plugin_mouse_up(pulp::view::View* root, NSEvent* event,
                          pulp::view::Point pt, pulp::view::View** drag_target) {
  try {
    using namespace pulp::view::mac_geometry;
    if (!*drag_target || !root) return;
    if (!view_is_in_tree(*drag_target, root)) { *drag_target = nullptr; return; }
    auto local = to_local(pt, *drag_target, root);
    auto* released = root->hit_test(pt);
    pulp::view::View* click_target = *drag_target;
    while (click_target && !click_target->on_click) click_target = click_target->parent();
    auto click_handler = click_target ? click_target->on_click : std::function<void()>{};

    (*drag_target)->on_mouse_up(local);
    pulp::view::MouseEvent up;
    up.position = local;
    up.window_position = pt;
    up.button = pulp::view::MouseButton::left;
    up.modifiers = modifiers_from_ns_flags(event.modifierFlags);
    up.is_down = false;
    up.phase = pulp::view::MousePhase::release;
    up.click_count = static_cast<int>(event.clickCount);
    (*drag_target)->on_mouse_event(up);
    for (auto* b = (*drag_target)->parent(); b; b = b->parent()) {
        if (!b->on_pointer_event) continue;
        pulp::view::MouseEvent bme = up;
        bme.position = to_local(pt, b, root);
        b->on_pointer_event(bme);
    }
    if (released == *drag_target && click_handler) click_handler();
    *drag_target = nullptr;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[plugin-view-host] mouseUp handler threw: %s\n", e.what());
    if (drag_target) *drag_target = nullptr;
  } catch (...) {
    std::fprintf(stderr, "[plugin-view-host] mouseUp handler threw (unknown)\n");
    if (drag_target) *drag_target = nullptr;
  }
}

void pulp_plugin_wheel(pulp::view::View* root, pulp::view::Point pt, NSEvent* event) {
  try {
    if (!root) return;
    auto* target = root->hit_test(pt);
    if (!target) return;
    pulp::view::MouseEvent me;
    me.position = pt;
    me.window_position = pt;
    me.is_wheel = true;
    me.scroll_delta_x = static_cast<float>(event.scrollingDeltaX);
    me.scroll_delta_y = static_cast<float>(-event.scrollingDeltaY);
    for (auto* v = target; v; v = v->parent()) {
        if (auto* sv = dynamic_cast<pulp::view::ScrollView*>(v)) {
            sv->on_mouse_event(me);
            sv->layout_children();
            return;
        }
        if (v->on_pointer_event) v->on_mouse_event(me);
    }
    target->on_mouse_event(me);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[plugin-view-host] scrollWheel handler threw: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "[plugin-view-host] scrollWheel handler threw (unknown)\n");
  }
}

bool pulp_plugin_event_has_text_input_modifier(const pulp::view::KeyEvent& event) {
    return event.isMainModifier() || event.isCtrlDown() || event.isAltDown();
}

pulp::view::KeyEvent pulp_plugin_key_event_from_ns(NSEvent* event) {
    pulp::view::KeyEvent ke;
    ke.key = pulp::view::mac_geometry::key_code_from_ns(event.keyCode);
    ke.modifiers = pulp::view::mac_geometry::modifiers_from_ns_flags(event.modifierFlags);
    ke.is_down = true;
    ke.is_repeat = event.isARepeat;
    return ke;
}

// Route a native key event to the currently focused input widget. The embedded
// plugin views are shared by AU/VST3/CLAP editors, so keyboard parity belongs
// here rather than in individual examples. Returns true only when the focused
// widget consumed the key or accepted printable text.
bool pulp_plugin_dispatch_focused_key(pulp::view::View* root,
                                      NSEvent* event,
                                      bool allow_text_input) {
  try {
    if (!root) return false;
    // Auto-clearing static: cleared by ~View when the focused widget is freed,
    // so this never derefs freed memory (pulp #1708 rationale).
    auto* fv = pulp::view::View::focused_input_;
    if (!fv) return false;
    using namespace pulp::view::mac_geometry;
    if (!view_is_in_tree(fv, root)) return false;

    const auto ke = pulp_plugin_key_event_from_ns(event);
    if (fv->on_key_event(ke)) {
        root->request_repaint();
        return true;
    }

    if (allow_text_input && fv->accepts_text_input()
        && !pulp_plugin_event_has_text_input_modifier(ke)) {
        // Printable characters → text insertion. (Full IME / marked-text via
        // NSTextInputClient is a follow-up; this covers ASCII + most Latin typing.)
        NSString* chars = event.characters;
        if (chars.length > 0) {
            unichar c0 = [chars characterAtIndex:0];
            if (c0 >= 0x20 && c0 != 0x7f) {
                pulp::view::TextInputEvent te;
                te.text = chars.UTF8String;
                fv->on_text_input(te);
                root->request_repaint();
                return true;
            }
        }
    }

    return false;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[plugin-view-host] keyDown handler threw: %s\n", e.what());
    return true;  // swallow; don't beep/propagate a half-handled key
  } catch (...) {
    std::fprintf(stderr, "[plugin-view-host] keyDown handler threw (unknown)\n");
    return true;
  }
}

bool pulp_plugin_key_down(pulp::view::View* root, NSEvent* event) {
    if (pulp_plugin_dispatch_focused_key(root, event, /*allow_text_input=*/true)) {
        return true;
    }
    if (!root) return false;
    const auto ke = pulp_plugin_key_event_from_ns(event);
    if (pulp::view::View::call_inspector_key_hook(ke)) {
        root->request_repaint();
        return true;
    }
    if (root->on_global_key && root->on_global_key(ke)) {
        root->request_repaint();
        return true;
    }
    pulp::view::script_events::dispatch_global_key(static_cast<int>(ke.key),
                                                   ke.modifiers,
                                                   /*is_down=*/true);
    return false;
}

bool pulp_plugin_perform_key_equivalent(pulp::view::View* root, NSEvent* event) {
    if (pulp_plugin_dispatch_focused_key(root, event, /*allow_text_input=*/false)) {
        return true;
    }
    if (!root) return false;
    const auto ke = pulp_plugin_key_event_from_ns(event);
    if (root->on_global_key && root->on_global_key(ke)) {
        root->request_repaint();
        return true;
    }
    pulp::view::script_events::dispatch_global_key(static_cast<int>(ke.key),
                                                   ke.modifiers,
                                                   /*is_down=*/true);
    return false;
}

} // namespace

@implementation PulpPluginView {
    pulp::view::View* _dragTarget;  // captured at mouseDown, re-validated each event
}

- (BOOL)isFlipped { return NO; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    return pulp_plugin_perform_key_equivalent(self.rootView, event) ? YES : NO;
}
- (void)keyDown:(NSEvent*)event {
    if (!pulp_plugin_key_down(self.rootView, event)) [super keyDown:event];
}
// Resolve a window-space event into root-view coords, applying the inverse
// design-viewport transform when set so hit_test runs against design-space
// coords. Identity when pointTransform is nil.
- (pulp::view::Point)localPoint:(NSEvent*)event {
    auto pt = pulp_plugin_local_point(self, event);
    if (self.pointTransform) pt = self.pointTransform(pt);
    return pt;
}
- (void)mouseDown:(NSEvent*)event {
    if (!self.rootView) return;
    if (self.window.firstResponder != self) [self.window makeFirstResponder:self];
    pulp_plugin_mouse_down(self.rootView, event, [self localPoint:event], &_dragTarget);
    [self setNeedsDisplay:YES];
}
- (void)mouseDragged:(NSEvent*)event {
    pulp_plugin_mouse_drag(self.rootView, [self localPoint:event], &_dragTarget);
    [self setNeedsDisplay:YES];
}
- (void)mouseUp:(NSEvent*)event {
    pulp_plugin_mouse_up(self.rootView, event, [self localPoint:event], &_dragTarget);
    [self setNeedsDisplay:YES];
}
- (void)scrollWheel:(NSEvent*)event {
    if (!self.rootView) return;
    pulp_plugin_wheel(self.rootView, [self localPoint:event], event);
    [self setNeedsDisplay:YES];
}
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
    const float bw = static_cast<float>(bounds.size.width);
    const float bh = static_cast<float>(bounds.size.height);

    pulp::canvas::CoreGraphicsCanvas canvas(ctx, bw, bh);

    // Clear at host bounds so the letterbox bars (visible only when the
    // OS aspect-lock briefly diverges during user drag) share the design
    // background color — same approach as the standalone host.
    canvas.set_fill_color(pulp::canvas::Color::rgba8(30, 30, 46));
    canvas.fill_rect(0, 0, bw, bh);

    if (!self.rootView) return;

    // Design viewport: pin root at design size, apply translate+scale
    // before paint_all so content renders proportionally. Otherwise lay
    // out at host bounds. Mirrors WindowHost paint_scene.
    float sx, sy, tx, ty;
    const bool has_viewport = self.designW > 0.0f && self.designH > 0.0f &&
        pulp::view::WindowHost::compute_design_viewport_transform(
            bw, bh, self.designW, self.designH, sx, sy, tx, ty);

    if (has_viewport) {
        self.rootView->set_bounds({0, 0, self.designW, self.designH});
        self.rootView->layout_children();
        const int saved = canvas.save_count();
        canvas.save();
        canvas.translate(tx, ty);
        canvas.scale(sx, sy);
        self.rootView->paint_all(canvas);
        // paint_overlays MUST run inside the design transform — overlays
        // (ComboBox dropdowns, inspector layer) draw in ROOT coordinates,
        // and mouse input inverse-maps window→root via pointTransform
        // before hit_test. Painting them outside the transform would put
        // them at root coords in window space → visually misaligned and
        // non-clickable at any host size that isn't exactly design size.
        // Matches the standalone host (pulp PR #1984 Codex P1).
        pulp::view::View::paint_overlays(canvas, self.rootView);
        canvas.restore_to_count(saved);
    } else {
        self.rootView->set_bounds({0, 0, bw, bh});
        self.rootView->layout_children();
        self.rootView->paint_all(canvas);
        pulp::view::View::paint_overlays(canvas, self.rootView);
    }
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self setNeedsDisplay:YES];
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    if (self.onResize) {
        self.onResize(static_cast<uint32_t>(newSize.width),
                      static_cast<uint32_t>(newSize.height));
    }
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
            view_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            view_.onResize = ^(uint32_t w, uint32_t h) {
                this->on_native_frame_changed(w, h);
            };
        }
    }

    ~MacPluginViewHost() override {
        root_.set_plugin_view_host(nullptr);
        // CRITICAL: clear pointTransform BEFORE the host C++ object is freed.
        // The block captures `this` by raw pointer. If the DAW retains the
        // NSView after host disposal (it routinely does — `attach_to_parent`
        // hands the view to the DAW's view hierarchy), a later mouseDown:
        // would invoke the block on freed memory → host crash. Mirrors the
        // #2502 deferred-click teardown pattern.
        @autoreleasepool {
            view_.pointTransform = nil;
            view_.onResize = nil;
        }
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

    bool is_attached() const noexcept override {
        // Truthful native check: the view is parented iff it sits in a view
        // hierarchy. `attach_to_parent` only ever adds `view_` to the supplied
        // parent, so a non-nil superview means the attach took.
        return view_ != nil && view_.superview != nil;
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

    void set_resize_callback(std::function<void(uint32_t, uint32_t)> cb) override {
        resize_cb_ = std::move(cb);
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

    void set_fixed_aspect_ratio(float ratio) override {
        // Plugin hosts don't own the OS window — DAW enforces the aspect
        // via the per-format resize-hint path. Stored for API parity.
        fixed_aspect_ratio_ = ratio;
    }

    void set_design_viewport(float design_w, float design_h) override {
        design_viewport_w_ = design_w;
        design_viewport_h_ = design_h;
        @autoreleasepool {
            view_.designW = design_w;
            view_.designH = design_h;
            if (design_w > 0.0f && design_h > 0.0f) {
                __block MacPluginViewHost* host = this;
                view_.pointTransform = ^pulp::view::Point(pulp::view::Point pt) {
                    return host->window_to_root_point(pt);
                };
            } else {
                view_.pointTransform = nil;
            }
            [view_ setNeedsDisplay:YES];
        }
    }

    Point window_to_root_point(Point pt) const override {
        float sx, sy, tx, ty;
        if (!WindowHost::compute_design_viewport_transform(
                static_cast<float>(size_.width),
                static_cast<float>(size_.height),
                design_viewport_w_, design_viewport_h_,
                sx, sy, tx, ty, design_top_align_)) {
            return pt;
        }
        if (sx <= 0.0f || sy <= 0.0f) return pt;
        return { (pt.x - tx) / sx, (pt.y - ty) / sy };
    }

    void on_native_frame_changed(uint32_t w, uint32_t h) {
        if (w == size_.width && h == size_.height) return;
        if (w == 0 || h == 0) return;
        set_size(w, h);
        if (resize_cb_) resize_cb_(w, h);
    }

private:
    View& root_;
    Size size_;
    PulpPluginView* view_ = nil;
    std::function<void(uint32_t, uint32_t)> resize_cb_;
    // Design viewport: when (>0, >0) root paints at design size and the
    // canvas applies translate+scale to fit the host bounds. Mouse coords
    // are inverse-mapped via window_to_root_point().
    float design_viewport_w_ = 0.0f;
    float design_viewport_h_ = 0.0f;
    float fixed_aspect_ratio_ = 0.0f;
    // CPU-host fallback stays vertically CENTERED (no setter override). The
    // member exists only so window_to_root_point's shared call compiles; the
    // CPU NSView paint also centers, so paint + input stay consistent.
    bool design_top_align_ = false;
};

} // namespace pulp::view (close for ObjC declarations)

// ── MacGpuPluginViewHost (Dawn/Skia Graphite) ────────────────────────────────

#ifdef PULP_HAS_SKIA

// CAMetalLayer-backed NSView for DAW-embedded GPU rendering.
//
// Unlike the standalone window host (which owns its NSWindow and starts the
// CVDisplayLink in its constructor), an embedded plugin view is handed a
// parent view by the host and only becomes live once it joins a window. So
// this view exposes window-attach / backing-change callbacks the host wires
// up to start/stop the display link and reconfigure surfaces at the right
// moments. The wrapper paths (AU returns the NSView directly; VST3/CLAP call
// attach_to_parent) never drive `attach_to_parent`-time rendering — they all
// funnel through `-viewDidMoveToWindow`.
@interface PulpGpuPluginView : NSView
@property (nonatomic, readonly) CAMetalLayer* metalLayer;
@property (nonatomic, assign) pulp::view::View* rootView;
@property (nonatomic, copy) void (^onWindowChange)(void);
@property (nonatomic, copy) void (^onBackingChange)(void);
@property (nonatomic, copy) void (^onResize)(uint32_t, uint32_t);
// Inverse-design-viewport transform applied to every host-space input point
// before hit_test. Mirrors PulpPluginView + the standalone host. nil =
// identity. Set by MacGpuPluginViewHost::set_design_viewport.
@property (nonatomic, copy) pulp::view::Point (^pointTransform)(pulp::view::Point);
@end

@implementation PulpGpuPluginView {
    pulp::view::View* _dragTarget;  // captured at mouseDown, re-validated each event
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    return pulp_plugin_perform_key_equivalent(self.rootView, event) ? YES : NO;
}
- (void)keyDown:(NSEvent*)event {
    if (!pulp_plugin_key_down(self.rootView, event)) [super keyDown:event];
}
// Resolve a window-space event into root-view coords, applying the inverse
// design-viewport transform when set.
- (pulp::view::Point)localPoint:(NSEvent*)event {
    auto pt = pulp_plugin_local_point(self, event);
    if (self.pointTransform) pt = self.pointTransform(pt);
    return pt;
}
- (void)mouseDown:(NSEvent*)event {
    if (!self.rootView) return;
    if (self.window.firstResponder != self) [self.window makeFirstResponder:self];
    pulp_plugin_mouse_down(self.rootView, event, [self localPoint:event], &_dragTarget);
    if (self.rootView) self.rootView->request_repaint();
}
- (void)mouseDragged:(NSEvent*)event {
    pulp_plugin_mouse_drag(self.rootView, [self localPoint:event], &_dragTarget);
    if (self.rootView) self.rootView->request_repaint();
}
- (void)mouseUp:(NSEvent*)event {
    pulp_plugin_mouse_up(self.rootView, event, [self localPoint:event], &_dragTarget);
    if (self.rootView) self.rootView->request_repaint();
}
- (void)scrollWheel:(NSEvent*)event {
    if (!self.rootView) return;
    pulp_plugin_wheel(self.rootView, [self localPoint:event], event);
    if (self.rootView) self.rootView->request_repaint();
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        // Follow the host's editor-container resize. AU v2 hosts (and VST3/CLAP
        // when they resize the parent) move our frame; flexible autoresizing
        // makes -setFrameSize: fire, which resizes the surfaces + relays out.
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        // BLACK-SCREEN ROOT CAUSE (pulp foreign-host-embed): a foreign DAW host
        // that embeds us via juce::NSViewComponent adds our NSView as a subview
        // of *its own* layer-backed content view. Per Apple's NSView docs,
        // "Creating a layer-backed view implicitly causes the entire view
        // hierarchy under that view to become layer-backed." So AppKit forces
        // THIS view layer-backed and — if we merely assign `self.layer` in init
        // — wraps our content in an AppKit-owned `NSViewBackingLayer` and demotes
        // our CAMetalLayer to a *sublayer*. Dawn presents into the CAMetalLayer
        // every vsync (the GPU renders correct, non-black frames — proven via
        // the env-gated live-stat readback), but the demoted sublayer's
        // presented drawable is not what the window server composites → the
        // editor stays BLACK on screen while headless capture looks perfect.
        //
        // The robust fix is to make our CAMetalLayer the view's genuine BACKING
        // layer by returning it from -makeBackingLayer (the documented hook
        // AppKit calls to create the backing layer). This survives forced
        // layer-backing under any foreign parent: our CAMetalLayer IS the
        // backing layer, never a wrapped sublayer. This is the same mechanism
        // JUCE itself uses for its Metal peer. Trigger it now by requesting
        // layer-backing; AppKit calls -makeBackingLayer synchronously.
        self.wantsLayer = YES;
    }
    return self;
}

// AppKit calls this to create the view's backing layer (on first
// `wantsLayer = YES`, and again if the view is forced layer-backed by a
// layer-backed ancestor). Returning our configured CAMetalLayer guarantees it
// is the view's real backing layer in every embedding, not a demoted sublayer.
- (CALayer*)makeBackingLayer {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = MTLCreateSystemDefaultDevice();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    // framebufferOnly = NO so the embedded back buffer can be read back for
    // headless capture (`capture_back_buffer_png`) — matches MacGpuWindowHost.
    layer.framebufferOnly = NO;
    CGFloat scale = self.window ? self.window.backingScaleFactor
                                : [NSScreen mainScreen].backingScaleFactor;
    layer.contentsScale = scale;
    layer.drawableSize = CGSizeMake(self.bounds.size.width * scale,
                                    self.bounds.size.height * scale);

    // pulp #1382 — opaque + seeded dark background (RGB 30,30,46 = 0x1E1E2E),
    // mirroring the standalone PulpMetalView, so there is no clear/undefined
    // composite while the foreign host reparents and relayers the view.
    layer.opaque = YES;
    const CGFloat dark[4] = { 30.0 / 255.0, 30.0 / 255.0, 46.0 / 255.0, 1.0 };
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    layer.backgroundColor = CGColorCreate(cs, dark);
    CGColorSpaceRelease(cs);

    _metalLayer = layer;
    return layer;
}

// pulp #1382 — `wantsUpdateLayer = YES` puts AppKit on the layer-update path
// (calls -updateLayer instead of -drawRect:) and stops it from auto-clearing
// the backing layer between updates, so the most-recent Metal frame stays
// presented until the next display-link tick. Matches PulpMetalView.
- (BOOL)wantsUpdateLayer { return YES; }

// No-op: Metal frames are produced by MacGpuPluginViewHost::render_frame off
// the display link, NOT inside AppKit's update cycle. We only need these to
// exist so AppKit honors wantsUpdateLayer and skips its own paint pipeline.
- (void)updateLayer {}
- (void)drawRect:(NSRect)dirtyRect { (void)dirtyRect; }

- (BOOL)isFlipped { return NO; }

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    CGFloat scale = self.window ? self.window.backingScaleFactor
                                : [NSScreen mainScreen].backingScaleFactor;
    self.metalLayer.contentsScale = scale;
    self.metalLayer.drawableSize = CGSizeMake(newSize.width * scale, newSize.height * scale);
    // AU v2 resizes this NSView directly (no host size callback). Notify the
    // host; it resizes surfaces and forwards to ViewBridge::resize. The host
    // guards against re-entrancy when *it* drove the frame change.
    if (self.onResize) {
        self.onResize(static_cast<uint32_t>(newSize.width),
                      static_cast<uint32_t>(newSize.height));
    }
}

// Start/stop the display link when the view joins or leaves a window. This is
// the embeddable equivalent of the standalone host starting its link in the
// constructor — the wrapper attach paths don't render until this fires.
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.onWindowChange) self.onWindowChange();
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    CGFloat scale = self.window ? self.window.backingScaleFactor
                                : [NSScreen mainScreen].backingScaleFactor;
    self.metalLayer.contentsScale = scale;
    self.metalLayer.drawableSize = CGSizeMake(self.bounds.size.width * scale,
                                              self.bounds.size.height * scale);
    if (self.onBackingChange) self.onBackingChange();
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
        : root_(root), size_(size),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        @autoreleasepool {
            root_.set_frame_clock(&frame_clock_);
            NSRect frame = NSMakeRect(0, 0, size.width, size.height);
            metal_view_ = [[PulpGpuPluginView alloc] initWithFrame:frame];
            metal_view_.rootView = &root_;

            // Wire the embeddable lifecycle hooks. The display link starts
            // when the view joins a window and stops when it leaves; the
            // surfaces re-sync on backing (HiDPI) changes. Captured `this`
            // is safe: the view is owned by this host and torn down in the
            // destructor before `this` dies, and the callbacks are cleared
            // there too.
            metal_view_.onWindowChange = ^{ this->handle_window_change(); };
            metal_view_.onBackingChange = ^{ this->handle_backing_change(); };
            metal_view_.onResize = ^(uint32_t w, uint32_t h) {
                this->on_native_frame_changed(w, h);
            };

            init_gpu(static_cast<float>(size.width), static_cast<float>(size.height));
        }
    }

    ~MacGpuPluginViewHost() override {
        // Flip the liveness token FIRST so any display-link block already
        // queued on the main thread becomes a no-op before we free anything
        // (mirrors the #2502 deferred-click token).
        alive_->store(false, std::memory_order_release);
        root_.set_plugin_view_host(nullptr);
        stop_display_link();
        // CRITICAL: clear pointTransform BEFORE the host C++ object is freed.
        // The block captures `this` by raw pointer; if the DAW retains the
        // NSView after host disposal, a later mouseDown: would call the
        // block on freed memory → host crash.
        @autoreleasepool {
            metal_view_.pointTransform = nil;
            metal_view_.onWindowChange = nil;
            metal_view_.onBackingChange = nil;
            metal_view_.onResize = nil;
        }
        skia_surface_.reset();
        gpu_surface_.reset();
        metal_view_.rootView = nullptr;
        root_.set_frame_clock(nullptr);
    }

    NativeViewHandle native_handle() override {
        return (__bridge void*)metal_view_;
    }

    void attach_to_parent(NativeViewHandle parent) override {
        @autoreleasepool {
            NSView* parent_view = (__bridge NSView*)parent;
            if (parent_view && metal_view_) {
                [parent_view addSubview:metal_view_];
                // Display link starts via -viewDidMoveToWindow → handle_window_change().
                needs_repaint_.store(true, std::memory_order_relaxed);
            }
        }
    }

    bool is_attached() const noexcept override {
        return metal_view_ != nil && metal_view_.superview != nil;
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
            // Resize parity with the standalone host: GpuSurface in PHYSICAL
            // pixels (logical * scale, matches the CAMetalLayer drawableSize),
            // SkiaSurface in LOGICAL size + scale factor.
            CGFloat scale = metal_view_.metalLayer.contentsScale;
            if (gpu_surface_) {
                gpu_surface_->resize(static_cast<uint32_t>(width * scale),
                                     static_cast<uint32_t>(height * scale));
            }
            if (skia_surface_) {
                skia_surface_->resize(width, height, static_cast<float>(scale));
            }
        }
        needs_repaint_.store(true, std::memory_order_relaxed);
    }

    Size get_size() const override { return size_; }

    bool is_gpu_backed() const override {
        return gpu_surface_ != nullptr && skia_surface_ != nullptr &&
               skia_surface_->is_available();
    }

    // Phase iOS-D.3b Slice 1 (planning/2026-05-29-ios-d3b-threejs-webgpu-program.md).
    // Mirrors WindowHost::gpu_surface() so a scripted UI mounted inside an
    // AUv3 / VST3 / CLAP editor can route navigator.gpu /
    // canvas.getContext('webgpu') through the same wgpu::Surface that
    // paints the editor.
    render::GpuSurface* gpu_surface() const override {
        return gpu_surface_.get();
    }

    void set_idle_callback(std::function<void()> callback) override {
        idle_callback_ = std::move(callback);
        has_idle_callback_.store(static_cast<bool>(idle_callback_),
                                 std::memory_order_release);
    }

    void set_resize_callback(std::function<void(uint32_t, uint32_t)> cb) override {
        resize_cb_ = std::move(cb);
    }

    // Deterministic GPU back-buffer readback for hidden / headless test
    // hosts (mirrors WindowHost::capture_back_buffer_png, issue #2001).
    // Goes straight through render_frame()'s readback path; never shows a
    // window or touches the compositor. Returns {} on failure.
    std::vector<uint8_t> capture_back_buffer_png() override {
        if (!gpu_surface_ || !skia_surface_) return {};
        needs_repaint_.store(true, std::memory_order_relaxed);
        std::vector<uint8_t> pixels;
        uint32_t pixel_w = 0;
        uint32_t pixel_h = 0;
        if (!render_frame(&pixels, &pixel_w, &pixel_h)) return {};
        return pulp::view::mac_capture::encode_rgba_to_png(
            pixels.data(), pixel_w, pixel_h, static_cast<size_t>(pixel_w) * 4u);
    }

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

    void set_fixed_aspect_ratio(float ratio) override {
        // Plugin hosts don't own the OS window — DAW enforces via the
        // per-format resize-hint path. Stored for API parity.
        fixed_aspect_ratio_ = ratio;
    }

    void set_design_viewport(float design_w, float design_h) override {
        design_viewport_w_ = design_w;
        design_viewport_h_ = design_h;
        @autoreleasepool {
            if (design_w > 0.0f && design_h > 0.0f) {
                __block MacGpuPluginViewHost* host = this;
                metal_view_.pointTransform = ^pulp::view::Point(pulp::view::Point pt) {
                    return host->window_to_root_point(pt);
                };
            } else {
                metal_view_.pointTransform = nil;
            }
        }
        needs_repaint_.store(true, std::memory_order_relaxed);
    }

    void set_design_viewport_top_align(bool top_align) override {
        design_top_align_ = top_align;
        needs_repaint_.store(true, std::memory_order_relaxed);
    }

    Point window_to_root_point(Point pt) const override {
        float sx, sy, tx, ty;
        if (!WindowHost::compute_design_viewport_transform(
                static_cast<float>(size_.width),
                static_cast<float>(size_.height),
                design_viewport_w_, design_viewport_h_,
                sx, sy, tx, ty, design_top_align_)) {
            return pt;
        }
        if (sx <= 0.0f || sy <= 0.0f) return pt;
        return { (pt.x - tx) / sx, (pt.y - ty) / sy };
    }

private:
    View& root_;
    Size size_;
    PulpGpuPluginView* metal_view_ = nil;
    bool design_top_align_ = false;

    std::unique_ptr<render::GpuSurface> gpu_surface_;
    std::unique_ptr<render::SkiaSurface> skia_surface_;
    CVDisplayLinkRef display_link_ = nullptr;
    FrameClock frame_clock_;
    std::atomic<bool> needs_repaint_{true};
    std::atomic<bool> continuous_frames_{false};
    std::atomic<bool> render_dispatch_queued_{false};
    std::function<void()> idle_callback_;
    std::atomic<bool> has_idle_callback_{false};
    std::function<void(uint32_t, uint32_t)> resize_cb_;
    // Liveness token captured by the display-link main-thread dispatch
    // blocks. Flipped false in the destructor so a queued block after
    // teardown is a no-op (DAW teardown order is not under our control).
    std::shared_ptr<std::atomic<bool>> alive_;
    int frame_ok_count_ = 0;
    int display_link_ticks_ = 0;  // env-gated diagnostic counter only
    // Design viewport: when (>0, >0) root paints at design size and the
    // Skia canvas applies translate+scale to fit the host bounds. Mouse
    // coords are inverse-mapped via window_to_root_point().
    float design_viewport_w_ = 0.0f;
    float design_viewport_h_ = 0.0f;
    float fixed_aspect_ratio_ = 0.0f;

    // FIRST-PAINT SIZE matters: the (width,height) this surface is created at
    // becomes the first painted frame's size. In an out-of-process plugin host
    // (notably Logic AU v3) the host may NOT have delivered the editor view's
    // real window size yet at attach time, so creating the surface at the
    // DESIGN size paints an oversized first frame that the host clips/composites
    // into a smaller window until a resize/reopen. Callers embedding this host
    // in such a DAW must defer creation until the view reports a real settled
    // size and pass THAT as the initial size (see
    // core/format/src/au_view_controller_mac.mm `-createViewHostIfReady` and
    // `.agents/skills/auv3/SKILL.md → "Logic OOP first-paint clip"`).
    void init_gpu(float width, float height) {
        gpu_surface_ = render::GpuSurface::create_dawn();
        if (!gpu_surface_) {
            fprintf(stderr, "[plugin-gpu-host] gpu init failed reason=create_dawn_null "
                            "falling_back=cpu-paint\n");
            return;
        }

        // Configure GpuSurface at PHYSICAL pixel dimensions to match the
        // CAMetalLayer drawableSize (logical * scale).
        CGFloat scale = metal_view_.metalLayer.contentsScale;
        uint32_t phys_w = static_cast<uint32_t>(width * scale);
        uint32_t phys_h = static_cast<uint32_t>(height * scale);

        render::GpuSurface::Config gpu_config{};
        gpu_config.width = phys_w;
        gpu_config.height = phys_h;
        gpu_config.native_surface_handle = (__bridge void*)metal_view_.metalLayer;

        if (!gpu_surface_->initialize(gpu_config)) {
            fprintf(stderr, "[plugin-gpu-host] gpu init failed reason=initialize "
                            "falling_back=cpu-paint\n");
            gpu_surface_.reset();
            return;
        }

        // SkiaSurface uses LOGICAL dimensions + scale factor.
        render::SkiaSurface::Config skia_config{};
        skia_config.width = static_cast<uint32_t>(width);
        skia_config.height = static_cast<uint32_t>(height);
        skia_config.scale_factor = static_cast<float>(scale);
        skia_surface_ = render::SkiaSurface::create(*gpu_surface_, skia_config);
        if (!skia_surface_) {
            fprintf(stderr, "[plugin-gpu-host] gpu init failed reason=skia_create_null "
                            "falling_back=cpu-paint\n");
            gpu_surface_.reset();
            return;
        }
        fprintf(stderr, "[plugin-gpu-host] init requested size=%.0fx%.0f scale=%.1f "
                        "gpu=%ux%u\n", width, height, scale, phys_w, phys_h);
    }

    void paint_scene(canvas::Canvas& canvas) {
        const float w = static_cast<float>(size_.width);
        const float h = static_cast<float>(size_.height);

        // Letterbox bg first at host bounds so the bars (visible only when
        // the OS aspect-lock briefly diverges during user drag) share the
        // design background color. Matches the standalone host.
        canvas.set_fill_color(pulp::canvas::Color::rgba8(30, 30, 46));
        canvas.fill_rect(0, 0, w, h);

        float sx, sy, tx, ty;
        const bool has_viewport = design_viewport_w_ > 0.0f && design_viewport_h_ > 0.0f &&
            WindowHost::compute_design_viewport_transform(
                w, h, design_viewport_w_, design_viewport_h_, sx, sy, tx, ty,
                design_top_align_);

        if (has_viewport) {
            root_.set_bounds({0, 0, design_viewport_w_, design_viewport_h_});
            root_.layout_children();
            const int saved = canvas.save_count();
            canvas.save();
            canvas.translate(tx, ty);
            canvas.scale(sx, sy);
            root_.paint_all(canvas);
            // paint_overlays MUST run inside the design transform —
            // overlays (ComboBox dropdowns, inspector layer) draw in ROOT
            // coords, mouse input inverse-maps window→root before hit_test.
            // Painting outside the transform would visually misalign + make
            // overlays non-clickable at any non-design-size host. Matches
            // the standalone GPU host (pulp PR #1984 Codex P1).
            View::paint_overlays(canvas, &root_);
            canvas.restore_to_count(saved);
        } else {
            root_.set_bounds({0, 0, w, h});
            root_.layout_children();
            root_.paint_all(canvas);
            View::paint_overlays(canvas, &root_);
        }
    }

    // Render one frame. When capture buffers are supplied, the rendered
    // RGBA back buffer is read back into them (for capture_back_buffer_png).
    bool render_frame(std::vector<uint8_t>* capture_pixels = nullptr,
                      uint32_t* capture_width = nullptr,
                      uint32_t* capture_height = nullptr) {
        if (!gpu_surface_ || !skia_surface_) return false;
        if (!gpu_surface_->begin_frame()) return false;

        auto* canvas = skia_surface_->begin_frame();
        if (!canvas) {
            gpu_surface_->end_frame();
            return false;
        }

        if (frame_ok_count_++ == 0) {
            CGFloat scale = metal_view_.metalLayer.contentsScale;
            fprintf(stderr, "[plugin-gpu-host] first frame logical=%ux%u gpu=%ux%u scale=%.1f\n",
                    size_.width, size_.height, gpu_surface_->width(),
                    gpu_surface_->height(), scale);
        }

        paint_scene(*canvas);

        continuous_frames_.store(
            view_needs_continuous_frames(&root_) || frame_clock_.has_active_subscribers(),
            std::memory_order_relaxed);

        // PULP_EMBED_GPU_FRAME_STAT — env-gated LIVE display-link present-path
        // pixel proof (NOT the forced capture path). Every Nth on-screen frame,
        // flush the Graphite recording and read the back buffer back, then log a
        // coarse luma / non-black stat. This proves the live present cadence is
        // emitting real, non-black content frame after frame — impossible to
        // confirm with screencapture in a permission-blocked env. Off by default;
        // costs a full GPU readback on the sampled frames only.
        static const int kStatEvery = [] {
            if (const char* e = std::getenv("PULP_EMBED_GPU_FRAME_STAT")) {
                int n = std::atoi(e);
                return n > 0 ? n : 30;
            }
            return 0;
        }();
        // Once-per-run on-screen geometry/visibility proof. A foreign host that
        // embeds us can leave the view at 0x0 (its NSViewComponent wrapper is
        // never sized) or relayer it so the CAMetalLayer is no longer the view's
        // backing layer — both render the editor black even though the GPU emits
        // perfect frames. view_size > 0, not hidden, backing==metal confirms the
        // full on-screen compositing chain is intact (the thing screencapture
        // would otherwise verify).
        if (kStatEvery > 0 && !capture_pixels && frame_ok_count_ == 5) {
            CAMetalLayer* ml = metal_view_.metalLayer;
            CALayer* backing = metal_view_.layer;
            NSRect inWin = [metal_view_ convertRect:metal_view_.bounds toView:nil];
            fprintf(stderr,
                    "[plugin-gpu-host][onscreen] view_size=%.0fx%.0f "
                    "frame_in_window=(%.0f,%.0f %.0fx%.0f) backing_is_metal=%d "
                    "hidden=%d alpha=%.2f has_window=%d\n",
                    metal_view_.bounds.size.width, metal_view_.bounds.size.height,
                    inWin.origin.x, inWin.origin.y, inWin.size.width, inWin.size.height,
                    (backing == ml) ? 1 : 0,
                    metal_view_.hidden ? 1 : 0,
                    (double)metal_view_.alphaValue,
                    metal_view_.window != nil ? 1 : 0);
        }
        if (kStatEvery > 0 && !capture_pixels &&
            (frame_ok_count_ % kStatEvery) == 1) {
            std::vector<uint8_t> px;
            uint32_t pw = 0, ph = 0;
            // read_current_rgba() snaps + submits the in-progress Graphite
            // recording itself before reading, so the readback sees the painted
            // frame rather than a blank/cleared backing.
            if (skia_surface_->read_current_rgba(px, pw, ph) && !px.empty()) {
                double luma_sum = 0.0;
                size_t non_black = 0;
                size_t n = static_cast<size_t>(pw) * ph;
                // Coarse unique-color estimate via a small bucketed signature
                // set (top 3 bits per channel → 512 buckets).
                std::array<bool, 512> seen{};
                size_t unique = 0;
                for (size_t i = 0; i < n; ++i) {
                    uint8_t r = px[i * 4 + 0], g = px[i * 4 + 1], b = px[i * 4 + 2];
                    double l = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                    luma_sum += l;
                    if (r > 8 || g > 8 || b > 8) ++non_black;
                    int bucket = ((r >> 5) << 6) | ((g >> 5) << 3) | (b >> 5);
                    if (!seen[bucket]) { seen[bucket] = true; ++unique; }
                }
                fprintf(stderr,
                        "[plugin-gpu-host][live-stat] frame=%d size=%ux%u "
                        "mean_luma=%.1f non_black=%.1f%% color_buckets=%zu\n",
                        frame_ok_count_, pw, ph,
                        luma_sum / static_cast<double>(n),
                        100.0 * static_cast<double>(non_black) /
                            static_cast<double>(n),
                        unique);
            }
        }

        bool captured = true;
        if (capture_pixels && capture_width && capture_height) {
            captured = skia_surface_->read_current_rgba(*capture_pixels,
                                                        *capture_width,
                                                        *capture_height);
        }

        skia_surface_->end_frame();
        gpu_surface_->end_frame();

        needs_repaint_.store(continuous_frames_.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        return captured;
    }

    static CVReturn display_link_callback(
        CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*,
        CVOptionFlags, CVOptionFlags*, void* context)
    {
        auto* self = static_cast<MacGpuPluginViewHost*>(context);
        // Copy the liveness token so the atomic outlives a teardown that
        // races this callback.
        auto alive = self->alive_;
        if (!alive->load(std::memory_order_acquire)) return kCVReturnSuccess;

        // PULP_EMBED_GPU_FRAME_STAT — env-gated proof that the CVDisplayLink is
        // actually ticking in the embedded case. Logs every ~120 vsyncs.
        if (std::getenv("PULP_EMBED_GPU_FRAME_STAT")) {
            int n = ++self->display_link_ticks_;
            if (n == 1 || (n % 120) == 0) {
                fprintf(stderr, "[plugin-gpu-host][dl] display-link tick=%d "
                                "needs_repaint=%d continuous=%d\n",
                        n,
                        self->needs_repaint_.load(std::memory_order_relaxed) ? 1 : 0,
                        self->continuous_frames_.load(std::memory_order_relaxed) ? 1 : 0);
            }
        }

        const bool has_idle = self->has_idle_callback_.load(std::memory_order_acquire);
        if (self->needs_repaint_.load(std::memory_order_relaxed) ||
            self->continuous_frames_.load(std::memory_order_relaxed) ||
            has_idle) {
            bool expected = false;
            if (!self->render_dispatch_queued_.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return kCVReturnSuccess;  // a render is already queued this vsync
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                @autoreleasepool {
                    if (!alive->load(std::memory_order_acquire)) return;
                    // Pump idle (scripted poll: async results, timers, rAF)
                    // FIRST so any request_repaint they trigger is seen below.
                    if (self->idle_callback_) self->idle_callback_();

                    bool animate = view_needs_continuous_frames(&self->root_);
                    bool tick_subscribers = self->frame_clock_.has_active_subscribers();
                    if (!self->needs_repaint_.load(std::memory_order_relaxed) &&
                        !animate && !tick_subscribers) {
                        self->continuous_frames_.store(false, std::memory_order_relaxed);
                        self->render_dispatch_queued_.store(false, std::memory_order_release);
                        return;
                    }
                    self->frame_clock_.tick(1.0f / 60.0f);
                    advance_widget_animations(&self->root_, 1.0f / 60.0f);
                    if (animate || tick_subscribers) {
                        self->needs_repaint_.store(true, std::memory_order_relaxed);
                    }
                    self->render_frame();
                    self->render_dispatch_queued_.store(false, std::memory_order_release);
                }
            });
        }
        return kCVReturnSuccess;
    }

    // Called when the native NSView frame changed (e.g. AU host resize).
    // Guards re-entrancy: when *we* drove the change via set_size(), size_
    // already matches, so this is a no-op and never recurses through
    // set_size()'s own [metal_view_ setFrameSize:] call.
    void on_native_frame_changed(uint32_t w, uint32_t h) {
        if (w == size_.width && h == size_.height) return;
        if (w == 0 || h == 0) return;
        set_size(w, h);
        if (resize_cb_) resize_cb_(w, h);
    }

    void handle_window_change() {
        if (metal_view_.window) {
            start_display_link();
            needs_repaint_.store(true, std::memory_order_relaxed);
        } else {
            stop_display_link();
        }
    }

    void handle_backing_change() {
        // The view already updated its layer's contentsScale/drawableSize;
        // re-sync the surfaces at the new scale and re-arm a paint.
        set_size(size_.width, size_.height);
    }

    void start_display_link() {
        if (display_link_) {
            // Already running; just make sure it's bound to the current screen.
            bind_to_window_screen();
            return;
        }
        CVDisplayLinkCreateWithActiveCGDisplays(&display_link_);
        CVDisplayLinkSetOutputCallback(display_link_, display_link_callback, this);
        bind_to_window_screen();
        CVDisplayLinkStart(display_link_);
    }

    // Bind the display link to the screen the embedded view is actually on,
    // so we render at that display's vsync (the host window may live on a
    // secondary monitor).
    void bind_to_window_screen() {
        if (!display_link_) return;
        NSScreen* screen = metal_view_.window ? metal_view_.window.screen : nil;
        if (!screen) screen = [NSScreen mainScreen];
        NSNumber* num = screen.deviceDescription[@"NSScreenNumber"];
        if (num) {
            CVDisplayLinkSetCurrentCGDisplay(
                display_link_, (CGDirectDisplayID)num.unsignedIntValue);
        }
    }

    void stop_display_link() {
        if (display_link_) {
            CVDisplayLinkStop(display_link_);
            CVDisplayLinkRelease(display_link_);
            display_link_ = nullptr;
        }
    }

    // ── Continuous-frame / animation drivers (parity with MacGpuWindowHost) ──
    static bool view_needs_continuous_frames(View* view) {
        if (!view) return false;
        if (auto* k = dynamic_cast<Knob*>(view)) {
            if ((k->hover_glow() > 0.01f && k->hover_glow() < 0.99f) || k->shader_uses_time())
                return true;
        }
        if (auto* t = dynamic_cast<Toggle*>(view)) {
            if ((t->thumb_position() > 0.01f && t->thumb_position() < 0.99f) || t->shader_uses_time())
                return true;
        }
        if (auto* f = dynamic_cast<Fader*>(view)) {
            if (f->hover_scale() > 1.01f || f->shader_uses_time())
                return true;
        }
        if (auto* sv = dynamic_cast<ScrollView*>(view)) {
            if (sv->scroll_animating()) return true;
        }
        if (view->animation_play_state() != "paused") {
            for (const auto& a : view->active_animations()) {
                if (a.active) return true;
            }
        }
        for (size_t i = 0; i < view->child_count(); ++i) {
            if (view_needs_continuous_frames(view->child_at(i))) return true;
        }
        return false;
    }

    static void advance_widget_animations(View* view, float dt) {
        if (!view) return;
        if (auto* k = dynamic_cast<Knob*>(view)) k->advance_animations(dt);
        else if (auto* t = dynamic_cast<Toggle*>(view)) t->advance_animations(dt);
        else if (auto* f = dynamic_cast<Fader*>(view)) f->advance_animations(dt);
        else if (auto* sv = dynamic_cast<ScrollView*>(view)) sv->advance_animations(dt);
        else if (auto* tip = dynamic_cast<Tooltip*>(view)) tip->advance_animations(dt);
        view->tick_animations(dt);
        for (size_t i = 0; i < view->child_count(); ++i)
            advance_widget_animations(view->child_at(i), dt);
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
        if (host->is_gpu_backed()) {
            root.set_plugin_view_host(host.get());
            return host;
        }
        // GPU init failed (no Dawn/Metal adapter in this host process) — fall
        // back to the CoreGraphics host so the editor never disappears (item 9).
        // The adapter's runtime scream-guard logs the mismatch loudly.
        host.reset();
    }
#endif
    auto host = std::make_unique<MacPluginViewHost>(root, options.size);
    root.set_plugin_view_host(host.get());
    return host;
}

} // namespace pulp::view

#endif // TARGET_OS_OSX
