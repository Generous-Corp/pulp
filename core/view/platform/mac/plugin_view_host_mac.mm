#include <pulp/view/plugin_view_host.hpp>

#include <TargetConditionals.h>
#if TARGET_OS_OSX

// Per-binary-unique ObjC class names (renames PulpPluginView / PulpGpuPluginView
// / PulpAccessibilityElement when a shipped binary defines
// PULP_VIEW_OBJC_SUFFIX). Must precede the first reference to those classes.
#include "pulp_mac_objc_names.h"

#include <pulp/canvas/cg_canvas.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/text_editor.hpp>  // focus-release affordance: single-line check
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/continuous_frames.hpp>  // needs_continuous_frames (CPU + GPU host repaint gate)
#include <pulp/view/platform/ns_role_mapping.hpp>
#include <pulp/view/accessibility.hpp>
#include <pulp/view/window_host.hpp>  // compute_design_viewport_transform
#include <pulp/view/pointer_dispatch.hpp>  // dispatch_context_menu (no-Skia builds too)
#import <Cocoa/Cocoa.h>
// CoreVideo is used unconditionally now: the CPU (CoreGraphics, no-Skia)
// plugin host also drives a CVDisplayLink for continuous frames + the idle
// pump, not just the GPU host. Must be outside the PULP_HAS_SKIA guard.
#import <CoreVideo/CVDisplayLink.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include <functional>
#include <iostream>
#include <memory>

// Reuse the standalone window host's coordinate/event helpers (to_local,
// view_is_in_tree, modifiers_from_ns_flags) — same pulp-view-core lib.
#include "window_host_mac_internal.hpp"

#ifdef PULP_HAS_SKIA
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/host_frame_pump.hpp>
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

extern "C" void pulp_mac_plugin_text_input_client_category_anchor();

// ── PulpPluginView: NSView subclass for DAW embedding ────────────────────────

@interface PulpPluginView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@property (nonatomic, copy) void (^onResize)(uint32_t, uint32_t);
// Fired from -viewDidMoveToWindow so the host can start/stop its CPU frame
// driver only while the view actually lives in a window.
@property (nonatomic, copy) void (^onWindowChange)(void);
// Fired from -setNeedsDisplay: (flag=YES) — the ONE funnel every "this editor
// is dirty" signal already passes through: the event handlers below, the host's
// repaint() override (and therefore View::request_repaint), resize, and AppKit
// itself. The host mirrors it into a link-thread-readable atomic so its
// CVDisplayLink callback can gate a vsync WITHOUT hopping to the main thread
// (see should_dispatch_host_frame). Without a dirty signal the gate could never
// re-open: a static editor would idle forever and never see the hover that woke
// it. Nil-ed in the host destructor before `this` is freed.
@property (nonatomic, copy) void (^onNeedsDisplay)(void);
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
@property (nonatomic, assign) BOOL designTopAlign;
@end

// ── Accessibility element wrapping a Pulp View ──────────────────────────────

@interface PulpAccessibilityElement : NSAccessibilityElement
@property (nonatomic, assign) pulp::view::View* pulpView;
@property (nonatomic, assign) PulpPluginView* hostView;
@end

@implementation PulpAccessibilityElement
- (NSAccessibilityRole)accessibilityRole {
    if (!_pulpView) return NSAccessibilityGroupRole;
    // Shared table with the standalone window host — see
    // platform/ns_role_mapping.hpp. Views with AccessRole::none never reach
    // here (collectAccessibleChildren: skips them).
    return pulp::view::ns_role_for_access_role(_pulpView->access_role());
}

- (NSString*)accessibilityLabel {
    if (!_pulpView || _pulpView->access_label().empty()) return nil;
    return [NSString stringWithUTF8String:_pulpView->access_label().c_str()];
}

- (id)accessibilityValue {
    if (!_pulpView) return nil;
    // Same shared resolver as the standalone window host
    // (accessibility_mac.mm): value interface → text interface → access_value.
    const std::string value = pulp::view::accessibility_value_string(*_pulpView);
    if (value.empty()) return nil;
    return [NSString stringWithUTF8String:value.c_str()];
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

// Forward declaration (defined below): the currently-focused input widget IF it
// belongs to `root`'s tree, else nullptr. Used by the mouse-down focus protocol.
static pulp::view::View* pulp_focus_under_root(pulp::view::View* root);
static pulp::view::View* pulp_plugin_cancel_marked_text_and_revalidate(
    pulp::view::View* root, NSView* host, pulp::view::View* view);

// Dispatched handlers (on_click / on_mouse_* / on_pointer_event / on_drag) are
// std::function callbacks — for a scripted UI they reach into the JS bridge and
// CAN throw. If a C++ exception unwinds out of these functions it crosses the
// AppKit ObjC frame that delivered the event → undefined behavior / host crash.
// Wrap each dispatch in try/catch, exactly as the standalone PulpView mouse
// handlers do (window_host_mac.mm), so a throwing handler is contained.
// Route an event at window-point `pt` to an open ComboBox popup when the point
// falls inside its (flip/scroll/clamp-aware) menu rect. The dropdown paints as an
// overlay OVER sibling views, so a plain hit_test lands on the sibling
// underneath — this mirrors the standalone host's active_popup_ bypass so
// dropdown selection + wheel-scroll work in an EMBEDDED plugin editor (Logic/AU
// etc.), not just the standalone window. `configure` fills the event-specific
// fields. Returns the routed combo, or nullptr when nothing was handled.
pulp::view::ComboBox* pulp_plugin_route_to_open_popup(
    pulp::view::View* root, pulp::view::Point pt,
    const std::function<void(pulp::view::MouseEvent&)>& configure) {
    auto* combo = pulp::view::ComboBox::active_popup_;
    if (!combo || !root) return nullptr;
    float ddx = 0, ddy = 0, ddw = 0, ddh = 0;
    if (!combo->dropdown_window_rect(ddx, ddy, ddw, ddh)) return nullptr;
    if (pt.x < ddx || pt.x > ddx + ddw || pt.y < ddy || pt.y > ddy + ddh) return nullptr;
    pulp::view::MouseEvent me;
    me.position = pulp::view::mac_geometry::to_local(pt, combo, root);
    me.window_position = pt;
    configure(me);
    combo->on_mouse_event(me);
    return combo;
}

void pulp_plugin_mouse_down(NSView* host, pulp::view::View* root, NSEvent* event,
                            pulp::view::Point pt, pulp::view::View** drag_target) {
  try {
    if (!root) return;
    // Route a click inside an open dropdown to the combo BEFORE hit_test (which
    // would otherwise land on the sibling view the menu overlays).
    if (auto* combo = pulp_plugin_route_to_open_popup(
            root, pt, [&](pulp::view::MouseEvent& me) {
                me.button = pulp::view::MouseButton::left;
                me.modifiers = pulp::view::mac_geometry::modifiers_from_ns_flags(
                    event.modifierFlags);
                me.is_down = true;
                me.phase = pulp::view::MousePhase::press;
                me.click_count = static_cast<int>(event.clickCount);
            })) {
        *drag_target = combo;
        return;
    }

    pulp::view::MouseEvent gesture_event;
    gesture_event.position = pt;
    gesture_event.window_position = pt;
    gesture_event.button = pulp::view::MouseButton::left;
    gesture_event.modifiers = pulp::view::mac_geometry::modifiers_from_ns_flags(
        event.modifierFlags);
    gesture_event.is_down = true;
    gesture_event.phase = pulp::view::MousePhase::press;
    gesture_event.click_count = static_cast<int>(event.clickCount);
    if (root->dispatch_gesture_pointer_event(gesture_event)) {
        *drag_target = nullptr;
        return;
    }

    *drag_target = root->hit_test(pt);
    pulp::view::ComboBox::notify_global_click(*drag_target);
    if (!*drag_target) return;
    using namespace pulp::view::mac_geometry;
    auto local = to_local(pt, *drag_target, root);
    // Focus-change protocol — mirror the standalone host (window_host_mac.mm).
    // Claiming input focus alone (the old behavior) routed typing to the widget
    // but never set its VISUAL focus state, so an embedded text field showed no
    // highlight border and no blinking caret in a DAW. Run the full protocol:
    // blur the previously-focused widget, then focus the new one via
    // on_focus_changed(true) (sets has_focus_ → border + caret-blink clock) and
    // claim_input_focus (routes text). Scoped to THIS root (focused_input_ is
    // process-global; never touch another open editor's focus).
    {
        auto* prev = pulp_focus_under_root(root);
        if (auto* te = dynamic_cast<pulp::view::TextEditor*>(prev); te && te->has_marked_text()) {
            prev = pulp_plugin_cancel_marked_text_and_revalidate(root, host, prev);
            *drag_target = root->hit_test(pt);
            if (!*drag_target) return;
            local = to_local(pt, *drag_target, root);
            if (!prev) prev = pulp_focus_under_root(root);
        }
        if ((*drag_target)->focusable()) {
            if (prev && prev != *drag_target) {
                if (!view_is_in_tree(*drag_target, root)) {
                    *drag_target = nullptr;
                    return;
                }
                if (prev && prev != *drag_target) {
                    prev->release_input_focus();
                    prev->on_focus_changed(false);
                    if (!view_is_in_tree(*drag_target, root)) {
                        *drag_target = nullptr;
                        return;
                    }
                }
            }
            (*drag_target)->on_focus_changed(true);
            (*drag_target)->claim_input_focus();
        } else if (prev) {
            // A click on a non-focusable target commits/closes any open type-in.
            if (!view_is_in_tree(*drag_target, root)) {
                *drag_target = nullptr;
                return;
            }
            if (prev) {
                prev->release_input_focus();
                prev->on_focus_changed(false);
                if (!view_is_in_tree(*drag_target, root)) {
                    *drag_target = nullptr;
                    return;
                }
            }
        }
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
    if (!root) return;
    pulp::view::MouseEvent gesture_event;
    gesture_event.position = pt;
    gesture_event.window_position = pt;
    gesture_event.button = pulp::view::MouseButton::left;
    gesture_event.is_down = true;
    gesture_event.phase = pulp::view::MousePhase::drag;
    if (root->dispatch_gesture_pointer_event(gesture_event))
        return;
    if (!*drag_target) return;
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
    if (!root) return;
    pulp::view::MouseEvent gesture_event;
    gesture_event.position = pt;
    gesture_event.window_position = pt;
    gesture_event.button = pulp::view::MouseButton::left;
    gesture_event.modifiers = modifiers_from_ns_flags(event.modifierFlags);
    gesture_event.is_down = false;
    gesture_event.phase = pulp::view::MousePhase::release;
    gesture_event.click_count = static_cast<int>(event.clickCount);
    if (root->dispatch_gesture_pointer_event(gesture_event)) {
        *drag_target = nullptr;
        return;
    }
    if (!*drag_target) return;
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
    // An open dropdown consumes the wheel to scroll its (clamped) item list,
    // ahead of any enclosing ScrollView (whose scroll would close it).
    if (pulp_plugin_route_to_open_popup(
            root, pt, [&](pulp::view::MouseEvent& me) {
                me.is_wheel = true;
                me.scroll_delta_x = static_cast<float>(event.scrollingDeltaX);
                me.scroll_delta_y = static_cast<float>(-event.scrollingDeltaY);
            })) return;
    auto* target = root->hit_test(pt);
    if (!target) {
        if (auto* scroll = pulp::view::find_wheel_scroll_view_at(*root, pt)) {
            pulp::view::MouseEvent me;
            me.position = pt;
            me.window_position = pt;
            me.is_wheel = true;
            me.scroll_delta_x = static_cast<float>(event.scrollingDeltaX);
            me.scroll_delta_y = static_cast<float>(-event.scrollingDeltaY);
            scroll->on_mouse_event(me);
            scroll->layout_children();
        }
        return;
    }
    pulp::view::MouseEvent me;
    me.position = pt;
    me.window_position = pt;
    me.is_wheel = true;
    me.scroll_delta_x = static_cast<float>(event.scrollingDeltaX);
    me.scroll_delta_y = static_cast<float>(-event.scrollingDeltaY);
    if (target->wants_wheel_value()) {
        target->on_wheel(me.scroll_delta_y);
        return;
    }
    for (auto* v = target; v; v = v->parent()) {
        if (v->wants_wheel_scroll()) {
            v->on_mouse_event(me);
            v->layout_children();
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

// Route a keyDown to the currently focused input widget — the embedded analog of
// the standalone PulpView's key handling (window_host_mac.mm). Without this an
// embedded TextEditor (e.g. an imported search field) never receives typing: the
// host NSView is first responder but had no keyDown:, so characters were dropped.
// A click already sets focus (claim_input_focus in mouseDown). Returns true when
// a focused input consumed the event, so the caller only falls through to
// [super keyDown:] (host menu shortcuts etc.) when nothing was focused.
// The input-focus slot (`View::focused_input_`) is a process-GLOBAL static, so
// in a host with two Pulp plugin editors open at once it may point into a
// DIFFERENT editor's view tree. Every focus decision here must be scoped to
// THIS NSView's embedded root, or editor B steals/ misroutes the keyboard when
// editor A holds focus. Returns the focused view only when it is `root` or a
// descendant of it; nullptr otherwise. The static is auto-cleared by ~View
// (#1708), so the pointer is never stale.
static pulp::view::View* pulp_focus_under_root(pulp::view::View* root) {
    auto* fv = pulp::view::View::focused_input_;
    if (!fv || !root) return nullptr;
    for (pulp::view::View* v = fv; v != nullptr; v = v->parent())
        if (v == root) return fv;
    return nullptr;
}

struct PulpFocusIdentity {
    pulp::view::View* view = nullptr;
    std::uint64_t instance_id = 0;
    std::weak_ptr<const std::uint64_t> lifetime;

    bool matches(pulp::view::View* current) const {
        return current && current == view && !lifetime.expired() &&
               current->import_binding_instance_id() == instance_id;
    }
};

PulpFocusIdentity pulp_focus_identity(pulp::view::View* view) {
    return {
        .view = view,
        .instance_id = view ? view->import_binding_instance_id() : 0,
        .lifetime = view ? view->import_binding_lifetime_token()
                         : std::weak_ptr<const std::uint64_t>{},
    };
}

pulp::view::View* pulp_plugin_cancel_marked_text_and_revalidate(pulp::view::View* root,
                                                                NSView* host,
                                                                pulp::view::View* view) {
    if (!view) return nullptr;
    auto identity = pulp_focus_identity(view);
    if (auto* te = dynamic_cast<pulp::view::TextEditor*>(view); te && te->has_marked_text()) {
        try {
            te->set_marked_text("", 0, 0);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[plugin-view-host] IME marked-text cancellation threw: %s\n",
                         e.what());
        } catch (...) {
            std::fprintf(stderr, "[plugin-view-host] IME marked-text cancellation threw (unknown)\n");
        }
        if (host) [[host inputContext] discardMarkedText];
        auto* current = pulp_focus_under_root(root);
        if (!identity.matches(current)) return nullptr;
        return current;
    }
    return view;
}

// Whether to grab the DAW keyboard: ONLY while a focused widget under this root
// actually ACCEPTS TEXT INPUT (a TextEditor type-in). A focusable-but-non-text
// widget — a ComboBox dropdown, a focusable container/root — must NOT make the
// plugin steal the keyboard, or the host loses transport keys (Space/R) AND its
// Musical Typing (e.g. clicking the Direction/Loop dropdown left the editor first
// responder, swallowing every later key so QWERTY notes stopped reaching Logic).
// This matches acceptsFirstResponder's own contract ("ONLY while a pulp text field
// is focused") — the focusable check alone was too broad. (pulp: AU hosted-view
// key routing.)
static bool pulp_text_input_focused_under_root(pulp::view::View* root) {
    auto* fv = pulp_focus_under_root(root);
    return fv != nullptr && fv->accepts_text_input();
}

bool pulp_plugin_event_has_private_use_function_character(NSEvent* event) {
    NSString* chars = event.charactersIgnoringModifiers;
    if (!chars) chars = event.characters;
    for (NSUInteger i = 0; i < chars.length; ++i) {
        const unichar ch = [chars characterAtIndex:i];
        if (ch >= 0xF700 && ch <= 0xF8FF) return true;
    }
    return false;
}

bool pulp_plugin_key_down(NSView* host, pulp::view::View* root, NSEvent* event) {
  try {
    if (!root) return false;
    // Only dispatch to a focused widget that belongs to THIS editor's tree —
    // never another open plugin editor's focused field (focused_input_ is
    // process-global).
    auto* fv = pulp_focus_under_root(root);
    // Only a FOCUSED text field consumes keys in a plugin host. With nothing
    // focused the editor isn't first responder (acceptsFirstResponder is gated on a
    // focused field), so the key never reaches here — it stays with the DAW for
    // transport + Musical Typing. A plugin must NOT route the bare computer keyboard
    // into its own musical typing; that fights the host. (The standalone drives its
    // own QWERTY musical typing through a different window host.)
    if (!fv) return false;
    const auto handled_focus = pulp_focus_identity(fv);

    if (auto* te = dynamic_cast<pulp::view::TextEditor*>(fv)) {
        if (te->has_marked_text()) {
            [host interpretKeyEvents:@[ event ]];
            root->request_repaint();
            return true;
        }
    }

    pulp::view::KeyEvent ke;
    ke.key = pulp::view::mac_geometry::key_code_from_ns(event.keyCode);
    ke.modifiers = pulp::view::mac_geometry::modifiers_from_ns_flags(event.modifierFlags);
    ke.is_down = true;
    ke.is_repeat = event.isARepeat;
    // Navigation / editing commands first (arrows, backspace, enter, escape, …).
    // The return value tells us whether the editor already handled this key as a
    // command — if so it must NOT also be inserted as text.
    const bool consumed = fv->on_key_event(ke);
    fv = pulp_focus_under_root(root);
    if (!handled_focus.matches(fv)) {
        root->request_repaint();
        return true;
    }

    // Text insertion is offered to AppKit's text input manager so dead keys and
    // IME composition reach insertText:/setMarkedText:. Command/control chords
    // stay on the key-command path above and must not also insert text.
    if (!consumed) {
        const NSEventModifierFlags cmd_ctrl =
            NSEventModifierFlagCommand | NSEventModifierFlagControl;
        if ((event.modifierFlags & cmd_ctrl) == 0 &&
            ke.key != pulp::view::KeyCode::tab &&
            !pulp_plugin_event_has_private_use_function_character(event)) {
            [host interpretKeyEvents:@[ event ]];
            fv = pulp_focus_under_root(root);
            if (!handled_focus.matches(fv)) {
                root->request_repaint();
                return true;
            }
        }
    }

    // Focus-release affordance — hand the keyboard back to the DAW. While a text
    // field holds focus the editor view is first responder, so transport keys
    // (Space, R, …) go to the field, not the host. Escape blurs a focused TEXT
    // EDITOR; Tab/Return blur a single-line one (Return having just committed via
    // on_return). Once focus clears, syncKeyFocus / the forward path hands the
    // keyboard back so DAW shortcuts work again until the field is re-focused.
    // Scoped to TextEditor so a focusable NON-text widget (e.g. a custom view
    // that uses Escape for its own purpose) is never force-blurred out from
    // under itself. (pulp: AU hosted-view key routing.)
    if (auto* te = dynamic_cast<pulp::view::TextEditor*>(fv)) {
        const bool blur = (ke.key == pulp::view::KeyCode::escape) ||
                          (!te->multi_line && (ke.key == pulp::view::KeyCode::tab ||
                                               ke.key == pulp::view::KeyCode::enter));
        if (blur) {
            auto* current = pulp_plugin_cancel_marked_text_and_revalidate(root, host, te);
            if (current) {
                current->release_input_focus();
                current->on_focus_changed(false);
            }
        }
    }
    root->request_repaint();
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[plugin-view-host] keyDown handler threw: %s\n", e.what());
    return true;  // swallow; don't beep/propagate a half-handled key
  } catch (...) {
    std::fprintf(stderr, "[plugin-view-host] keyDown handler threw (unknown)\n");
    return true;
  }
}

// The host window's previous first responder, restored when a pulp widget
// releases text-input focus. Built without ARC and holding NO ownership: the
// saved pointer is NEVER dereferenced — it is re-found BY IDENTITY in the
// window's current view tree before use, so a responder freed while we held
// the keyboard degrades to nil (the window) instead of a dangling send. The
// restore matters: handing focus to nil instead of back to the host's own
// view leaves hosts like Logic with dead keyboard routing (Musical Typing
// stays silent after a type-in commit until the user resets the track).
[[maybe_unused]] static NSResponder* pulp_plugin_live_prior_responder(NSResponder* saved, NSWindow* win) {
    if (saved == nil || saved == (NSResponder*)win || win.contentView == nil) return nil;
    NSMutableArray<NSView*>* stack = [NSMutableArray arrayWithObject:win.contentView];
    while (stack.count > 0) {
        NSView* v = stack.lastObject;
        [stack removeLastObject];
        if (v == (NSView*)saved) return saved;
        [stack addObjectsFromArray:v.subviews];
    }
    return nil;
}

// End the focused widget's text input because the HOST moved the keyboard
// away (the user clicked a host control while a type-in was open). Clearing
// the slot first makes the widget's own release_input_focus() a no-op, so
// the on_focus_changed notification can commit/close its editing UI without
// recursing. Returns true when a widget was actually ended (repaint needed).
// Scoped to `root`: a resignFirstResponder on THIS editor must never end a
// type-in owned by another open plugin editor (focused_input_ is global).
static bool pulp_plugin_end_text_input(NSView* host, pulp::view::View* root) {
    auto* fv = pulp_focus_under_root(root);
    if (!fv) return false;
    fv = pulp_plugin_cancel_marked_text_and_revalidate(root, host, fv);
    if (!fv) fv = pulp_focus_under_root(root);
    if (!fv) return true;
    fv->release_input_focus();
    try {
        fv->on_focus_changed(false);
    } catch (...) {
        std::fprintf(stderr, "[plugin-view-host] on_focus_changed(false) threw\n");
    }
    return true;
}

// Route a Cmd-chord (⌘A / ⌘C / ⌘V / ⌘X / ⌘Z …) to the focused editor. macOS
// delivers command-key equivalents through performKeyEquivalent: BEFORE keyDown:
// — without this override the host (e.g. Logic) consumes ⌘A as its own
// "Select All" and the embedded field never sees it, so select-all/copy/paste
// appear dead in the plugin (pulp: AU hosted-view key routing). Scoped to THIS
// editor's tree (focused_input_ is process-global). Returns true when consumed.
bool pulp_plugin_key_equivalent(pulp::view::View* root, NSEvent* event) {
  try {
    if (!root) return false;
    auto* fv = pulp_focus_under_root(root);
    if (!fv) return false;
    // Only intercept actual command chords; let everything else flow normally
    // so host menu shortcuts (⌘W, ⌘Q, ⌘S) keep working.
    if ((event.modifierFlags & NSEventModifierFlagCommand) == 0) return false;
    pulp::view::KeyEvent ke;
    ke.key = pulp::view::mac_geometry::key_code_from_ns(event.keyCode);
    ke.modifiers = pulp::view::mac_geometry::modifiers_from_ns_flags(event.modifierFlags);
    ke.is_down = true;
    ke.is_repeat = event.isARepeat;
    if (fv->on_key_event(ke)) {
        root->request_repaint();
        return true;
    }
    return false;
  } catch (...) {
    std::fprintf(stderr, "[plugin-view-host] performKeyEquivalent handler threw\n");
    return false;
  }
}

// Set the macOS cursor from the view under `local` (root coords). Mirrors the
// standalone host's hover→NSCursor mapping (window_host_mac.mm) for the subset
// that matters in a plugin editor — most importantly IBeam over a text field so
// a focused/hovered TextEditor shows the expected i-beam (pulp: AU hosted-view
// key routing). Unknown styles fall back to the arrow cursor.
void pulp_plugin_apply_hover_cursor(pulp::view::View* root, pulp::view::Point local) {
  try {
    if (!root) return;
    // Route hover over an OPEN dropdown to the combo so every row highlights —
    // the menu overlays sibling views, so a plain hit_test/simulate_hover would
    // land on the sibling under the lower rows and they'd never highlight.
    if (auto* combo = pulp_plugin_route_to_open_popup(
            root, local, [&](pulp::view::MouseEvent&) {})) {
        (void)combo;
        [[NSCursor arrowCursor] set];
        return;
    }
    root->simulate_hover(local);
    auto* target = root->hit_test(local);
    if (!target) { [[NSCursor arrowCursor] set]; return; }
    switch (target->cursor()) {
        case pulp::view::View::CursorStyle::text:
            [[NSCursor IBeamCursor] set]; break;
        case pulp::view::View::CursorStyle::pointer:
            [[NSCursor pointingHandCursor] set]; break;
        case pulp::view::View::CursorStyle::crosshair:
            [[NSCursor crosshairCursor] set]; break;
        case pulp::view::View::CursorStyle::grab:
            [[NSCursor openHandCursor] set]; break;
        case pulp::view::View::CursorStyle::grabbing:
            [[NSCursor closedHandCursor] set]; break;
        case pulp::view::View::CursorStyle::not_allowed:
            [[NSCursor operationNotAllowedCursor] set]; break;
        case pulp::view::View::CursorStyle::horizontal_resize:
            [[NSCursor resizeLeftRightCursor] set]; break;
        case pulp::view::View::CursorStyle::vertical_resize:
            [[NSCursor resizeUpDownCursor] set]; break;
        default:
            [[NSCursor arrowCursor] set]; break;
    }
  } catch (...) {
    // A cursor update must never take down the host process.
  }
}

// Forward a key the plugin did NOT consume to the DAW host, so transport keys
// (Space = play/stop, R = record, …) keep working while a plugin editor is
// frontmost. The DAW embeds our editor as a SUBVIEW of its own container view
// in the SAME window, so we hand the key into the host's responder chain via
// that parent view: make the host view first responder, deliver the key, then
// take first responder back so the editor keeps receiving keys. (Forwarding to
// NSApp.mainWindow instead — a DIFFERENT window — does NOT work: Logic ignores
// it; the in-window superview is the correct target.) Returns true when the
// event was handed off.
static bool pulp_plugin_forward_key_to_host(NSView* self, NSEvent* event) {
  // Re-entrancy guard scoped to the IN-FLIGHT event (by timestamp), not a
  // blanket flag: delivering to the host can bounce the same event back through
  // our keyDown:, which we must not re-forward. But if [host_view keyDown:]
  // spins a nested event loop (host opens a modal in response to the key),
  // UNRELATED later keys must still flow — a blanket bool would drop them until
  // the loop unwinds. Comparing the event timestamp blocks only the same event.
  static NSTimeInterval s_forwarding_ts = -1.0;
  const NSTimeInterval ts = event.timestamp;
  if (ts == s_forwarding_ts) return false;
  const NSTimeInterval prev_ts = s_forwarding_ts;
  @try {
    NSView* host_view = self.superview;
    NSWindow* win = self.window;
    if (host_view == nil || win == nil) return false;
    s_forwarding_ts = ts;
    [win makeFirstResponder:host_view];
    [host_view keyDown:event];
    if (self.superview != nil && self.window != nil)
      [self.window makeFirstResponder:self];
    s_forwarding_ts = prev_ts;
    return true;
  } @catch (...) {
    s_forwarding_ts = prev_ts;
    // Never let a forwarding attempt crash the host.
  }
  return false;
}

} // namespace

@implementation PulpPluginView {
    pulp::view::View* _dragTarget;  // captured at mouseDown, re-validated each event
    NSResponder* _priorResponder;   // identity-validated before use, never deref'd
    NSTrackingArea* _trackingArea;  // hover tracking for the i-beam over text fields
}

- (BOOL)isFlipped { return NO; }
// Keyboard-focus contract (host-etiquette). The editor stays first responder
// while it is shown so it can INTERCEPT every key: keys for a focused text
// field are consumed; everything else is forwarded back to the host (see
// pulp_plugin_forward_key_to_host in -keyDown:). That forward is what lets the
// host keep its keyboard routing — DAW transport (Space/R) AND Logic Musical
// Typing on software-instrument tracks — even while a plugin control is
// focused. (Previously this returned first-responder only while a widget held
// the text-input slot, which fixed Musical Typing by NOT grabbing the keyboard
// but left no path to hand transport keys back after the user left a field;
// the forward supersedes that approach.)
- (BOOL)acceptsFirstResponder {
    // Take the keyboard ONLY while a pulp text field is focused. Otherwise the DAW
    // owns it, so host transport keys (Space/R) and the host's Musical Typing keep
    // working while the plugin editor is open — a plugin must not hijack the DAW
    // keyboard. (The standalone uses a different window host that drives its own
    // QWERTY musical typing; this path is DAW-only.)
    return pulp_text_input_focused_under_root(self.rootView);
}
- (void)syncKeyFocus {
    NSWindow* win = self.window;
    if (!win) return;
    const bool wants = pulp_text_input_focused_under_root(self.rootView);
    if (wants && win.firstResponder != self) {
        [win makeFirstResponder:self];
    } else if (!wants && win.firstResponder == self) {
        // No field focused — hand the keyboard back to the DAW so transport keys
        // and the host's Musical Typing resume immediately (e.g. after a type-in
        // commits). Without this the editor would keep first responder and swallow
        // DAW shortcuts until the user clicked a host control.
        [win makeFirstResponder:nil];
    }
}
// The host moved the keyboard elsewhere (click on a host control, window
// switching) while a widget still held text-input focus: close that text
// input instead of silently re-claiming on the next event — the host's grab
// wins. Without this an open type-in left behind keeps acceptsFirstResponder
// true and steals the keyboard back, which reads as "the plugin killed
// Musical Typing again".
- (BOOL)resignFirstResponder {
    _priorResponder = nil;  // the host chose a new responder; nothing to restore
    if (pulp_plugin_end_text_input(self, self.rootView)) [self setNeedsDisplay:YES];
    return [super resignFirstResponder];
}
- (void)keyDown:(NSEvent*)event {
    if (!pulp_plugin_key_down(self, self.rootView, event)) {
        // No focused field consumed it — try to hand it to the DAW host
        // (transport keys), then fall back to the normal responder chain.
        if (!pulp_plugin_forward_key_to_host(self, event)) {
            [super keyDown:event];
            // The forward couldn't run (no host superview to hand off to — e.g.
            // an out-of-process view-service host). Don't hold the keyboard
            // hostage: with no field focused, resign first responder so the
            // window's normal responder chain / the host receive later keys,
            // instead of every key dead-ending at this editor.
            if (pulp_focus_under_root(self.rootView) == nullptr &&
                self.window.firstResponder == self)
                [self.window makeFirstResponder:nil];
        }
    }
    [self syncKeyFocus];  // commit/escape may have ended text input — release
}
// Cmd-chords (⌘A/⌘C/⌘V/⌘X/⌘Z) arrive here BEFORE keyDown:; route them to the
// focused editor so the host doesn't swallow them. See pulp_plugin_key_equivalent.
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    if (pulp_plugin_key_equivalent(self.rootView, event)) {
        [self setNeedsDisplay:YES];
        return YES;
    }
    return [super performKeyEquivalent:event];
}
// Resolve a window-space event into root-view coords, applying the inverse
// design-viewport transform when set so hit_test runs against design-space
// coords. Identity when pointTransform is nil.
- (pulp::view::Point)localPoint:(NSEvent*)event {
    auto pt = pulp_plugin_local_point(self, event);
    if (self.pointTransform) pt = self.pointTransform(pt);
    return pt;
}
// Hover tracking so the cursor becomes an i-beam over a text field. The
// tracking rect auto-follows the view bounds (NSTrackingInVisibleRect).
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) { [self removeTrackingArea:_trackingArea]; _trackingArea = nil; }
    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(NSTrackingMouseMoved | NSTrackingActiveInKeyWindow |
                      NSTrackingInVisibleRect | NSTrackingCursorUpdate)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}
- (void)mouseMoved:(NSEvent*)event {
    if (!self.rootView) return;
    pulp_plugin_apply_hover_cursor(self.rootView, [self localPoint:event]);
}
- (void)cursorUpdate:(NSEvent*)event {
    if (self.rootView) pulp_plugin_apply_hover_cursor(self.rootView, [self localPoint:event]);
    else [super cursorUpdate:event];
}
- (void)mouseDown:(NSEvent*)event {
    if (!self.rootView) return;
    pulp_plugin_mouse_down(self, self.rootView, event, [self localPoint:event], &_dragTarget);
    [self setNeedsDisplay:YES];
    [self syncKeyFocus];
}
- (void)mouseDragged:(NSEvent*)event {
    pulp_plugin_mouse_drag(self.rootView, [self localPoint:event], &_dragTarget);
    [self setNeedsDisplay:YES];
}
- (void)mouseUp:(NSEvent*)event {
    pulp_plugin_mouse_up(self.rootView, event, [self localPoint:event], &_dragTarget);
    [self setNeedsDisplay:YES];
    [self syncKeyFocus];  // a tap may have entered (or left) type-in
}
// A plugin editor previously had no right-button path at all: mouseDown: hardcodes
// MouseButton::left, and there was no rightMouseDown:, so a right-click inside a
// DAW reached no view. TextEditor's own context menu and every JS onContextMenu
// handler were dead in-host while working in the standalone.
- (void)rightMouseDown:(NSEvent*)event {
  @try {
    try {
        if (!self.rootView) { [super rightMouseDown:event]; return; }
        if (pulp::view::dispatch_context_menu(*self.rootView, [self localPoint:event]))
            [self setNeedsDisplay:YES];
        else
            [super rightMouseDown:event];  // let the host show its own menu
    } catch (const std::exception& e) {
        std::cerr << "PulpPluginView rightMouseDown error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "PulpPluginView rightMouseDown error: unknown exception\n";
    }
  } @catch (NSException* exception) {
    std::cerr << "PulpPluginView rightMouseDown NSException: "
              << [[exception name] UTF8String] << "\n";
  }
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
    // Merge any TextAccessibilityNodes registered through the
    // cross-platform text-a11y scaffold. The Pulp-View-role tree above
    // and the text registry are independent surfaces; without this
    // merge the registry would be a private map and VoiceOver would
    // never discover painted text registered via
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
    if (pulp::view::is_accessibility_element(*view)) {
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

    // An exception escaping drawRect: is FATAL to the entire hosting process:
    // AppKit's _crashOnException kills it, and in a DAW's shared AU sandbox
    // (Logic's AUHostingService) that takes down EVERY plugin in the process —
    // observed live as "adjusting one plugin kills the instrument's audio until
    // it is reopened". Painting one bad frame is always preferable; catch, log
    // once, and keep the process alive.
    @try {
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
            // Matches the standalone host.
            pulp::view::View::paint_overlays(canvas, self.rootView);
            canvas.restore_to_count(saved);
        } else {
            self.rootView->set_bounds({0, 0, bw, bh});
            self.rootView->layout_children();
            self.rootView->paint_all(canvas);
            pulp::view::View::paint_overlays(canvas, self.rootView);
        }
    } @catch (NSException* e) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            pulp::runtime::log_error(
                "[plugin-view] exception during editor paint (suppressed to keep "
                "the host process alive): {} — {}",
                e.name.UTF8String ? e.name.UTF8String : "?",
                e.reason.UTF8String ? e.reason.UTF8String : "?");
        }
    }
}

// Every dirty signal in this file goes through -setNeedsDisplay: — the event
// handlers, the host's repaint()/set_size(), and AppKit's own invalidations.
// Tapping it here is what lets the display-link callback gate on an atomic
// instead of dispatching a tree walk to the main thread every vsync.
- (void)setNeedsDisplay:(BOOL)flag {
    [super setNeedsDisplay:flag];
    if (flag && self.onNeedsDisplay) self.onNeedsDisplay();
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.onWindowChange) self.onWindowChange();
    if (self.window) {
        // Accept dragged files/text so the editor's drop targets work when hosted
        // in a DAW (the NSDraggingDestination behavior lives in drag_drop_mac.mm).
        // Without this, drag-drop worked only in the standalone PulpView.
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL, NSPasteboardTypeString ]];
        // The hover tracking area installed in -updateTrackingAreas carries
        // NSTrackingMouseMoved, but a tracking area only fans -mouseMoved: out to
        // its owner when the window itself accepts mouse-moved events, and NSWindow
        // defaults that flag to NO. This window is owned by the foreign host (a DAW,
        // or a JUCE/iPlug2 embed), not by Pulp, so nothing sets the flag for us and
        // -mouseMoved: never arrives — CSS :hover / on_hover_enter / on_hover_leave
        // stay dead in a hosted editor even though the same tree hovers correctly in
        // the standalone window. Opt the host window into mouse-moved delivery so the
        // shared -mouseMoved: -> simulate_hover path reaches the view tree, matching
        // what the standalone window host does (window_host_mac.mm).
        self.window.acceptsMouseMovedEvents = YES;
    }
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

// Native-child clip masking reuses the shared mac_geometry helper
// (clip_child_view_in_host, window_host_mac_geometry.mm) so the coordinate-flip
// math lives in exactly one place — see window_host_mac_internal.hpp (included
// above). The two plugin-host overrides below forward to it directly.

// ── MacPluginViewHost ────────────────────────────────────────────────────────

namespace pulp::view {

// Both plugin hosts drive frames through the shared frame-pump contract
// (host_frame_pump.hpp): begin_host_frame() measures the dt from the display
// link's own presentation timestamp, pumps the activity probes, and reports
// whether the frame composites; advance_host_frame() then advances the
// FrameClock, the widget animations, and the CSS timelines by that ONE dt.

class MacPluginViewHost : public PluginViewHost {
public:
    MacPluginViewHost(View& root, Size size)
        : root_(root), size_(size) {
        @autoreleasepool {
            pulp_mac_plugin_text_input_client_category_anchor();
            // Parity with the GPU plugin host: the CPU editor path owns a
            // FrameClock too, so FrameClock subscribers (and anything reading
            // frame_clock()->time(), e.g. shader time) actually advance.
            root_.set_frame_clock(&frame_clock_);
            NSRect frame = NSMakeRect(0, 0, size.width, size.height);
            view_ = [[PulpPluginView alloc] initWithFrame:frame];
            view_.rootView = &root_;
            view_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            view_.onResize = ^(uint32_t w, uint32_t h) {
                this->on_native_frame_changed(w, h);
            };
            // Start/stop the CPU frame driver as the view enters/leaves a
            // window (cleared in the destructor before `this` is freed).
            view_.onWindowChange = ^{ this->update_render_link(); };
            // Mirror every dirty signal into the link-thread-readable flag that
            // re-opens the vsync gate. Captures the shared state, NOT `this`, so
            // a late AppKit invalidation after teardown cannot touch freed memory.
            auto state = link_state_;
            view_.onNeedsDisplay = ^{
                state->needs_repaint.store(true, std::memory_order_relaxed);
            };
        }
    }

    ~MacPluginViewHost() override {
        // Stop the frame driver before anything it captures is freed. The
        // shared token is flipped first so any block already in flight on the
        // main queue (or a reentrant one) bails out before touching `this`.
        link_state_->alive.store(false, std::memory_order_release);
        stop_render_link();
        root_.set_frame_clock(nullptr);
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
            view_.onWindowChange = nil;
            view_.onNeedsDisplay = nil;
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
        // Removed from its window → no more frames needed. (viewDidMoveToWindow
        // also fires onWindowChange, but stop explicitly so a host that detaches
        // without a window transition still releases the link.)
        stop_render_link();
    }

    void repaint() override {
        @autoreleasepool {
            [view_ setNeedsDisplay:YES];
        }
    }

    // The CPU host repaints only on input (setNeedsDisplay) by default, which
    // strands live content (a spectrum that animates, values that track host
    // automation, a blinking caret, a widget hover fade) and the idle callback
    // that drains the store's automation listeners. So while the editor is in a
    // window, run a CVDisplayLink that — for each vsync worth dispatching — runs
    // the idle callback if there is one, advances the FrameClock / widget / CSS
    // animations, and marks the CPU view dirty while any view needs continuous
    // frames. "Worth dispatching" is decided on the LINK THREAD from three
    // atomics (dirty / animating / scripted), so a static native editor inside a
    // DAW does zero main-thread work per vsync. Lifecycle-tied + reentrancy-safe
    // to match MacGpuPluginViewHost.
    void set_idle_callback(std::function<void()> callback) override {
        idle_cb_ = std::move(callback);
        // A scripted editor's idle pump (rAF / timers / async results) must run
        // every vsync regardless of native dirtiness, so it holds the gate open.
        link_state_->has_idle_callback.store(static_cast<bool>(idle_cb_),
                                             std::memory_order_release);
        // The window-change hook starts the link on attach; if the view is
        // already in a window (e.g. AU returned it then the DAW attached it
        // before this call), reconcile now.
        update_render_link();
    }

    // Start iff in a window; stop otherwise. Idempotent. See
    // plugin_view_wants_render_link() for why an idle callback is NOT required.
    void update_render_link() {
        if (pulp::view::mac_frame_timing::plugin_view_wants_render_link(
                view_ != nil, view_ != nil && view_.window != nil))
            start_render_link();
        else
            stop_render_link();
    }
    void start_render_link() {
        if (render_link_) return;
        CVDisplayLinkCreateWithActiveCGDisplays(&render_link_);
        if (!render_link_) return;
        CVDisplayLinkSetOutputCallback(render_link_, &render_link_callback, this);
        CVDisplayLinkStart(render_link_);
        if (const float nominal =
                pulp::view::mac_frame_timing::display_link_nominal_dt(render_link_);
            nominal > 0.0f) {
            frame_pump_.set_nominal_dt(nominal);
        }
        // The editor was not being pumped while the link was stopped; the next
        // frame is a resume, not a multi-second frame.
        frame_pump_.suspend();
    }
    void stop_render_link() {
        frame_pump_.suspend();
        if (render_link_) {
            CVDisplayLinkStop(render_link_);   // synchronous: no callback after this
            CVDisplayLinkRelease(render_link_);
            render_link_ = nullptr;
        }
    }

    static CVReturn render_link_callback(CVDisplayLinkRef, const CVTimeStamp*,
                                         const CVTimeStamp* output_time, CVOptionFlags,
                                         CVOptionFlags*, void* ctx) {
        auto* self = static_cast<MacPluginViewHost*>(ctx);
        // The link's own presentation timestamp — captured on the link thread,
        // consumed on main. See mac_frame_timing::display_link_seconds.
        const double frame_time =
            pulp::view::mac_frame_timing::display_link_seconds(output_time);
        // Capture the shared liveness/queue token: it outlives `self`, so the
        // dispatched block never touches a freed host.
        auto state = self->link_state_;
        if (!state->alive.load(std::memory_order_acquire)) return kCVReturnSuccess;

        // Gate on the LINK THREAD — parity with the GPU hosts, and the whole
        // reason a native (unscripted) editor can now run a link at all. The
        // link keeps firing at the display's refresh rate even when the editor
        // is completely static; without this, every one of those vsyncs would
        // dispatch_async to the main thread of someone's DAW and walk the entire
        // view tree (needs_continuous_frames) 60-120x/sec to conclude nothing
        // changed. A skipped vsync is a vsync the pump never measured, so
        // should_dispatch_host_frame records it and the next real frame resumes
        // at one nominal frame instead of measuring the whole idle gap.
        if (!pulp::view::should_dispatch_host_frame(
                self->frame_pump_,
                state->needs_repaint.load(std::memory_order_relaxed),
                state->continuous.load(std::memory_order_relaxed),
                state->has_idle_callback.load(std::memory_order_acquire)))
            return kCVReturnSuccess;

        bool expected = false;
        if (!state->queued.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_relaxed))
            return kCVReturnSuccess;  // a tick is already queued this vsync
        dispatch_async(dispatch_get_main_queue(), ^{
            @autoreleasepool {
                if (!state->alive.load(std::memory_order_acquire)) {
                    state->queued.store(false, std::memory_order_release);
                    return;
                }
                // Copy the idle callback locally so running it can't free the
                // std::function out from under its own call; then drain
                // host-automation listeners to the UI (bind_parameter widgets
                // repaint themselves via request_repaint on change).
                std::function<void()> idle = self->idle_cb_;
                if (idle) idle();
                // Idle work (store pump / scripted poll) can reentrantly close
                // the editor → ~host sets alive=false and frees `self`. Re-check
                // before touching ANY self member; the queue flag lives on the
                // shared state, so leaving it set after teardown is harmless
                // (the link is already stopped, no further callbacks fire).
                if (!state->alive.load(std::memory_order_acquire)) return;
                // Consume the dirty flag AFTER the idle callback, so a repaint it
                // requested lands in THIS frame; anything that dirties the view
                // later (during the advance below) re-arms it for the next one.
                const bool dirty =
                    state->needs_repaint.exchange(false, std::memory_order_relaxed);
                const auto tick = pulp::view::begin_host_frame(
                    &self->root_, self->frame_clock_, self->frame_pump_, frame_time,
                    dirty);
                // Publish the tree's liveness for the next vsync's gate.
                state->continuous.store(tick.continuous, std::memory_order_relaxed);
                if (tick.should_render) {
                    pulp::view::advance_host_frame(&self->root_, self->frame_clock_, tick.dt);
                    // Repaint only for an ANIMATING tree. A dirty-only frame was
                    // already marked dirty by whatever set the flag (AppKit will
                    // paint it), and re-marking it here would re-arm needs_repaint
                    // through -setNeedsDisplay: forever — a dirty flag that feeds
                    // itself is an ungated link with extra steps. When the tree IS
                    // animating the re-arm is exactly right: it keeps the gate open
                    // for the settling frame after the last animation frame.
                    if (tick.continuous && self->view_) [self->view_ setNeedsDisplay:YES];
                }
                state->queued.store(false, std::memory_order_release);
            }
        });
        return kCVReturnSuccess;
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

    bool set_native_child_view_clip(NativeViewHandle child_view,
                                    bool has_clip,
                                    float x,
                                    float y,
                                    float width,
                                    float height) override {
        return mac_geometry::clip_child_view_in_host(view_, child_view, has_clip,
                                                     x, y, width, height);
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
            view_.designTopAlign = design_top_align_;
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

    // Forward design->host transform for embedded native children. The CPU
    // NSView paint centers the design surface (no top-align setter), so this
    // matches the paint transform exactly.
    bool design_viewport_transform(float& sx, float& sy,
                                   float& tx, float& ty) const override {
        if (design_viewport_w_ <= 0.0f || design_viewport_h_ <= 0.0f)
            return false;
        return WindowHost::compute_design_viewport_transform(
            static_cast<float>(size_.width), static_cast<float>(size_.height),
            design_viewport_w_, design_viewport_h_, sx, sy, tx, ty,
            design_top_align_);
    }

    void on_native_frame_changed(uint32_t w, uint32_t h) {
        if (w == size_.width && h == size_.height) return;
        if (w == 0 || h == 0) return;
        set_size(w, h);
        if (resize_cb_) resize_cb_(w, h);
    }

private:
    View& root_;
    FrameClock frame_clock_;
    HostFramePump frame_pump_;
    Size size_;
    PulpPluginView* view_ = nil;
    std::function<void(uint32_t, uint32_t)> resize_cb_;
    // Continuous-frame driver for the CPU (non-GPU) editor path.
    std::function<void()> idle_cb_;
    CVDisplayLinkRef render_link_ = nullptr;
    // Shared liveness + per-vsync queue flag. Lives in a shared_ptr so the
    // CVDisplayLink callback's main-queue block can outlive the host without
    // touching freed memory.
    struct FrameLink {
        std::atomic<bool> alive{true};
        std::atomic<bool> queued{false};
        // The three inputs to the link-thread vsync gate (should_dispatch_host_frame).
        // They live here rather than on the host so the link callback and the
        // -setNeedsDisplay: hook can read/write them without a host pointer.
        //   needs_repaint — host dirty flag; set by -setNeedsDisplay: (input,
        //                   request_repaint, resize), consumed by the frame.
        //                   Starts true: the first mount frame must run.
        //   continuous    — did the last dispatched frame report an animating tree?
        //   has_idle_callback — a scripted editor's pump must run every vsync.
        std::atomic<bool> needs_repaint{true};
        std::atomic<bool> continuous{false};
        std::atomic<bool> has_idle_callback{false};
    };
    std::shared_ptr<FrameLink> link_state_ = std::make_shared<FrameLink>();
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
@property (nonatomic, assign) float designW;
@property (nonatomic, assign) float designH;
@property (nonatomic, assign) BOOL designTopAlign;
@end

@implementation PulpGpuPluginView {
    pulp::view::View* _dragTarget;  // captured at mouseDown, re-validated each event
    NSResponder* _priorResponder;   // identity-validated before use, never deref'd
    NSTrackingArea* _trackingArea;  // hover tracking for the i-beam over text fields
}

// Keyboard-focus contract — see PulpPluginView::acceptsFirstResponder above:
// stay first responder while shown and INTERCEPT keys; consume keys for a
// focused field and forward everything else back to the host (transport +
// Musical Typing) via pulp_plugin_forward_key_to_host in -keyDown:.
- (BOOL)acceptsFirstResponder {
    // DAW-only path: take the keyboard ONLY while a pulp text field is focused, so
    // the host keeps transport keys (Space/R) + Musical Typing. See
    // PulpPluginView::acceptsFirstResponder.
    return pulp_text_input_focused_under_root(self.rootView);
}
- (void)syncKeyFocus {
    NSWindow* win = self.window;
    if (!win) return;
    const bool wants = pulp_text_input_focused_under_root(self.rootView);
    if (wants && win.firstResponder != self) {
        [win makeFirstResponder:self];
    } else if (!wants && win.firstResponder == self) {
        [win makeFirstResponder:nil];  // hand the keyboard back to the DAW
    }
}
// Host took the keyboard while a type-in was open: close it, don't re-claim.
// See PulpPluginView::resignFirstResponder.
- (BOOL)resignFirstResponder {
    _priorResponder = nil;
    if (pulp_plugin_end_text_input(self, self.rootView) && self.rootView)
        self.rootView->request_repaint();
    return [super resignFirstResponder];
}
- (void)keyDown:(NSEvent*)event {
    if (!pulp_plugin_key_down(self, self.rootView, event)) {
        // No focused field consumed it — try to hand it to the DAW host
        // (transport keys), then fall back to the normal responder chain.
        if (!pulp_plugin_forward_key_to_host(self, event)) {
            [super keyDown:event];
            // The forward couldn't run (no host superview to hand off to — e.g.
            // an out-of-process view-service host). Don't hold the keyboard
            // hostage: with no field focused, resign first responder so the
            // window's normal responder chain / the host receive later keys,
            // instead of every key dead-ending at this editor.
            if (pulp_focus_under_root(self.rootView) == nullptr &&
                self.window.firstResponder == self)
                [self.window makeFirstResponder:nil];
        }
    }
    [self syncKeyFocus];
}
// Cmd-chords (⌘A/⌘C/⌘V/⌘X/⌘Z) arrive here BEFORE keyDown:; route them to the
// focused editor so the host doesn't swallow them. See pulp_plugin_key_equivalent.
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    if (pulp_plugin_key_equivalent(self.rootView, event)) {
        self.rootView ? self.rootView->request_repaint() : (void)0;
        return YES;
    }
    return [super performKeyEquivalent:event];
}
// Resolve a window-space event into root-view coords, applying the inverse
// design-viewport transform when set.
- (pulp::view::Point)localPoint:(NSEvent*)event {
    auto pt = pulp_plugin_local_point(self, event);
    if (self.pointTransform) pt = self.pointTransform(pt);
    return pt;
}
// Hover tracking so the cursor becomes an i-beam over a text field. The
// tracking rect auto-follows the view bounds (NSTrackingInVisibleRect).
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) { [self removeTrackingArea:_trackingArea]; _trackingArea = nil; }
    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(NSTrackingMouseMoved | NSTrackingActiveInKeyWindow |
                      NSTrackingInVisibleRect | NSTrackingCursorUpdate)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}
- (void)mouseMoved:(NSEvent*)event {
    if (!self.rootView) return;
    pulp_plugin_apply_hover_cursor(self.rootView, [self localPoint:event]);
}
- (void)cursorUpdate:(NSEvent*)event {
    if (self.rootView) pulp_plugin_apply_hover_cursor(self.rootView, [self localPoint:event]);
    else [super cursorUpdate:event];
}
- (void)mouseDown:(NSEvent*)event {
    if (!self.rootView) return;
    pulp_plugin_mouse_down(self, self.rootView, event, [self localPoint:event], &_dragTarget);
    if (self.rootView) self.rootView->request_repaint();
    [self syncKeyFocus];
}
- (void)mouseDragged:(NSEvent*)event {
    pulp_plugin_mouse_drag(self.rootView, [self localPoint:event], &_dragTarget);
    if (self.rootView) self.rootView->request_repaint();
}
- (void)mouseUp:(NSEvent*)event {
    pulp_plugin_mouse_up(self.rootView, event, [self localPoint:event], &_dragTarget);
    if (self.rootView) self.rootView->request_repaint();
    [self syncKeyFocus];
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
        // that embeds us adds our NSView as a subview of *its own* layer-backed
        // content view (common for hosts whose editor wrapper is layer-backed).
        // Per Apple's NSView docs,
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
        // backing layer, never a wrapped sublayer. (This is the standard
        // backing-layer hook for a Metal-backed NSView.) Trigger it now by
        // requesting layer-backing; AppKit calls -makeBackingLayer synchronously.
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
    if (self.window) {
        // Accept dragged files/text when hosted in a DAW (drop dispatch lives in
        // drag_drop_mac.mm) — same as the CPU host above and the standalone PulpView.
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL, NSPasteboardTypeString ]];
        // Opt the host-owned window into mouse-moved delivery so the hover tracking
        // area's -mouseMoved: -> simulate_hover path fires in a hosted editor. See
        // the identical note in PulpPluginView above: a tracking area only receives
        // -mouseMoved: when the window accepts those events, and a foreign host's
        // window defaults the flag to NO.
        self.window.acceptsMouseMovedEvents = YES;
    }
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
            pulp_mac_plugin_text_input_client_category_anchor();
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

    bool set_native_child_view_clip(NativeViewHandle child_view,
                                    bool has_clip,
                                    float x,
                                    float y,
                                    float width,
                                    float height) override {
        return mac_geometry::clip_child_view_in_host(metal_view_, child_view,
                                                     has_clip, x, y, width,
                                                     height);
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
            metal_view_.designW = design_w;
            metal_view_.designH = design_h;
            metal_view_.designTopAlign = design_top_align_;
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
        @autoreleasepool {
            metal_view_.designTopAlign = top_align;
        }
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

    // Forward design->host transform for embedded native children — mirrors
    // paint_scene()'s letterbox scale (design_top_align_ honored).
    bool design_viewport_transform(float& sx, float& sy,
                                   float& tx, float& ty) const override {
        if (design_viewport_w_ <= 0.0f || design_viewport_h_ <= 0.0f)
            return false;
        return WindowHost::compute_design_viewport_transform(
            static_cast<float>(size_.width), static_cast<float>(size_.height),
            design_viewport_w_, design_viewport_h_, sx, sy, tx, ty,
            design_top_align_);
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
    // Measured-dt source fed by the CVDisplayLink's presentation timestamps.
    HostFramePump frame_pump_;
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
            // the standalone GPU host.
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
            pulp::view::needs_continuous_frames(&root_) || frame_clock_.has_active_subscribers(),
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
        CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp* output_time,
        CVOptionFlags, CVOptionFlags*, void* context)
    {
        auto* self = static_cast<MacGpuPluginViewHost*>(context);
        const double frame_time =
            pulp::view::mac_frame_timing::display_link_seconds(output_time);
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

        // Gate on the link thread — see should_dispatch_host_frame(). A vsync we
        // decline to dispatch is a vsync the pump never measures, so it is
        // recorded as a skip and the next real frame resumes at one nominal
        // frame instead of measuring the whole idle gap. Without that, an editor
        // that idles for 100 ms and is then hovered advances the hover animation
        // by 100 ms on its FIRST frame (an 80 ms fade never plays).
        if (pulp::view::should_dispatch_host_frame(
                self->frame_pump_,
                self->needs_repaint_.load(std::memory_order_relaxed),
                self->continuous_frames_.load(std::memory_order_relaxed),
                self->has_idle_callback_.load(std::memory_order_acquire))) {
            bool expected = false;
            if (!self->render_dispatch_queued_.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return kCVReturnSuccess;  // a render is already queued this vsync
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                @autoreleasepool {
                    if (!alive->load(std::memory_order_acquire)) return;
                    // Copy the idle callback locally so running it can't free the
                    // std::function out from under its own call; pump idle
                    // (scripted poll: async results, timers, rAF) FIRST so any
                    // request_repaint it triggers is seen below.
                    std::function<void()> idle = self->idle_callback_;
                    if (idle) idle();
                    // Idle work can reentrantly close the editor → ~host flips
                    // alive=false and frees `self`. Re-check before touching ANY
                    // self member (parity with the CPU host's render_link_callback).
                    if (!alive->load(std::memory_order_acquire)) return;

                    const auto tick = pulp::view::begin_host_frame(
                        &self->root_, self->frame_clock_, self->frame_pump_, frame_time,
                        self->needs_repaint_.load(std::memory_order_relaxed));
                    if (!tick.should_render) {
                        self->continuous_frames_.store(false, std::memory_order_relaxed);
                        self->render_dispatch_queued_.store(false, std::memory_order_release);
                        return;
                    }
                    pulp::view::advance_host_frame(&self->root_, self->frame_clock_, tick.dt);
                    if (tick.continuous) {
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
        frame_pump_.suspend();  // the editor was not being pumped: next frame is a resume
    }

    // Bind the display link to the screen the embedded view is actually on,
    // so we render at that display's vsync (the host window may live on a
    // secondary monitor), and re-seed the pump's nominal (first-frame / wake)
    // interval from THAT display — 1/120 on ProMotion, not an assumed 1/60.
    // Seeding lives here, not in start_display_link(), because the editor can be
    // dragged to a display with a different refresh rate while the link runs.
    void bind_to_window_screen() {
        if (!display_link_) return;
        NSScreen* screen = metal_view_.window ? metal_view_.window.screen : nil;
        if (!screen) screen = [NSScreen mainScreen];
        NSNumber* num = screen.deviceDescription[@"NSScreenNumber"];
        if (num) {
            CVDisplayLinkSetCurrentCGDisplay(
                display_link_, (CGDirectDisplayID)num.unsignedIntValue);
        }
        if (const float nominal =
                pulp::view::mac_frame_timing::display_link_nominal_dt(display_link_);
            nominal > 0.0f) {
            frame_pump_.set_nominal_dt(nominal);
        }
    }

    void stop_display_link() {
        frame_pump_.suspend();
        if (display_link_) {
            CVDisplayLinkStop(display_link_);
            CVDisplayLinkRelease(display_link_);
            display_link_ = nullptr;
        }
    }

    // The widget/CSS animation walk lives in shared code
    // (pulp::view::advance_host_frame → advance_widget_animations), driven from
    // display_link_callback via begin_host_frame(). The continuous-frame
    // predicate is the shared pulp::view::needs_continuous_frames().
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
        // back to the CoreGraphics host so the editor never disappears.
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
