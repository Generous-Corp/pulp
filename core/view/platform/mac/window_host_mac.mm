// Per-binary-unique ObjC class names (see header).
#include "pulp_mac_objc_names.h"
#include <pulp/view/window_host.hpp>
#include <pulp/view/window_manager.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/runtime/trace.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/continuous_frames.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/script_event_dispatch.hpp>
#include <pulp/events/main_thread_dispatcher.hpp>

#include "window_host_mac_capture.h"
#include "window_host_mac_internal.hpp"
#include "window_host_mac_view.h"

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/canvas/cg_canvas.hpp>
#import <Cocoa/Cocoa.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>   // shared_ptr liveness token for deferred clicks
#include <mutex>
#include <utility>
#include <vector>

#ifdef PULP_HAS_SKIA
#include <pulp/render/dirty_tracker.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreVideo/CVDisplayLink.h>
#import <Metal/Metal.h>
#endif

// ── Cursor hide/unhide balance ───────────────────────────────────────────────
// NSCursor hide/unhide calls are reference-counted by AppKit, so we must track
// our own hidden state and always unhide before setting a different cursor.
static bool s_cursor_hidden = false;

// ── Diagonal resize cursor ───────────────────────────────────────────────────
// AppKit exposes no PUBLIC diagonal corner-resize cursor, but NSCursor carries
// the private `_windowResizeNorthWestSouthEastCursor` (↖↘) and
// `_windowResizeNorthEastSouthWestCursor` (↗↙) selectors that NSWindow itself
// uses for corner resizing. We reach them via respondsToSelector so a future
// SDK that renames/removes them degrades to a crosshair instead of crashing.
// `nwse == YES` → the ↖↘ (TL/BR) variant; NO → the ↗↙ (TR/BL) variant.
static NSCursor* pulp_diagonal_resize_cursor(BOOL nwse) {
    SEL sel = nwse
        ? NSSelectorFromString(@"_windowResizeNorthWestSouthEastCursor")
        : NSSelectorFromString(@"_windowResizeNorthEastSouthWestCursor");
    if ([NSCursor respondsToSelector:sel]) {
        id cur = [NSCursor performSelector:sel];
        if ([cur isKindOfClass:[NSCursor class]]) return (NSCursor*)cur;
    }
    return [NSCursor crosshairCursor];
}

// ── App menu helper ──────────────────────────────────────────────────────────

static std::mutex& cocoa_dispatcher_liveness_mutex() {
    static std::mutex mutex;
    return mutex;
}

static std::vector<std::weak_ptr<std::atomic<bool>>>& cocoa_dispatcher_liveness_tokens() {
    static std::vector<std::weak_ptr<std::atomic<bool>>> tokens;
    return tokens;
}

static void register_cocoa_dispatcher_liveness(
    const std::shared_ptr<std::atomic<bool>>& alive) {
    auto& mutex = cocoa_dispatcher_liveness_mutex();
    auto& tokens = cocoa_dispatcher_liveness_tokens();
    std::lock_guard lock(mutex);
    tokens.erase(std::remove_if(tokens.begin(), tokens.end(),
                     [](const auto& token) { return token.expired(); }),
                 tokens.end());
    tokens.push_back(alive);
}

static void mark_cocoa_dispatchers_stopping() {
    auto& mutex = cocoa_dispatcher_liveness_mutex();
    auto& tokens = cocoa_dispatcher_liveness_tokens();
    std::lock_guard lock(mutex);
    tokens.erase(std::remove_if(tokens.begin(), tokens.end(),
                     [](const auto& token) {
                         auto alive = token.lock();
                         if (!alive)
                             return true;
                         alive->store(false, std::memory_order_release);
                         return false;
                     }),
                 tokens.end());
}

static void post_cocoa_stop_event() {
    NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:NO];
}

static void request_cocoa_app_stop() {
    mark_cocoa_dispatchers_stopping();
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp stop:nil];
        post_cocoa_stop_event();
    });
}

static void request_hidden_cocoa_window_close(NSWindow* window) {
    mark_cocoa_dispatchers_stopping();
    dispatch_async(dispatch_get_main_queue(), ^{
        if (window != nil)
            [window close];
        [NSApp stop:nil];
        post_cocoa_stop_event();
    });
}

@interface PulpAppTerminationHandler : NSObject
+ (instancetype)sharedHandler;
- (void)quit:(id)sender;
@end

@implementation PulpAppTerminationHandler

+ (instancetype)sharedHandler {
    static PulpAppTerminationHandler* handler = nil;
    static dispatch_once_t once_token;
    dispatch_once(&once_token, ^{
        handler = [[PulpAppTerminationHandler alloc] init];
    });
    return handler;
}

- (void)quit:(id)sender {
    (void)sender;
    // cmd+Q / Quit terminates the WHOLE app in one press. Previously this did
    // performClose on only the KEY window, so with the floating inspector
    // focused cmd+Q closed just the inspector and a second cmd+Q was needed.
    // [NSApp stop:nil] returns from the [NSApp run] in run_event_loop, main()
    // unwinds, and every WindowHost destructor closes its window (canvas +
    // inspector + any others) — a single, clean quit. (cmd+W still closes one
    // window via the standard responder chain.)
    request_cocoa_app_stop();
}

@end

// Window setup, geometry, event, and gesture helpers live in
// window_host_mac_geometry.mm; use them via pulp::view::mac_geometry.
using namespace pulp::view::mac_geometry;

extern "C" void pulp_mac_text_input_client_category_anchor();

static bool dispatch_mouse_down_if_live(PulpView* host,
                                        pulp::view::View*& target,
                                        const pulp::view::MouseEvent& event,
                                        pulp::view::Point local) {
    if (!host || !target) return false;
    auto* root = [host rootView];
    if (!root) {
        target = nullptr;
        return false;
    }

    target->on_mouse_event(event);
    root = [host rootView];
    if (!view_is_in_tree(target, root)) {
        target = nullptr;
        return false;
    }

    target->on_mouse_down(local);
    root = [host rootView];
    if (!view_is_in_tree(target, root)) {
        target = nullptr;
        return false;
    }

    return true;
}

static pulp::events::MainThreadDispatcher::Backend make_cocoa_main_thread_backend(
    std::shared_ptr<std::atomic<bool>> alive) {
    return {
        [alive](pulp::events::Task task) -> bool {
            if (!task) return false;
            if (!alive || !alive->load(std::memory_order_acquire)) return false;
            auto* heap_task = new pulp::events::Task(std::move(task));
            dispatch_async(dispatch_get_main_queue(), ^{
                std::unique_ptr<pulp::events::Task> owned(heap_task);
                if (*owned) (*owned)();
            });
            return true;
        },
        [alive] {
            if (!alive || !alive->load(std::memory_order_acquire)) return false;
            return [NSThread isMainThread];
        },
    };
}

static void install_app_menu(NSString* appName) {
    NSMenu* menuBar = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [menuBar addItem:appItem];
    [NSApp setMainMenu:menuBar];

    NSMenu* appMenu = [[NSMenu alloc] init];
    (void)appName;  // available for override if needed
    NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit"
                                                      action:@selector(quit:)
                                               keyEquivalent:@"q"];
    [quitItem setTarget:[PulpAppTerminationHandler sharedHandler]];
    [appMenu addItem:quitItem];
    [appItem setSubmenu:appMenu];
}

// ── PulpView: CoreGraphics NSView (CPU rendering path) ───────────────────────

@implementation PulpView {
    pulp::view::View* _dragTarget;
    pulp::view::View* _focusedView;
    pulp::view::Point _relativeMouseWindowPoint;
    BOOL _relativeMouseMode;
    // Liveness token for `mouseUp:`'s deferred-click blocks.
    // The block dispatched onto the main queue captures a COPY of this
    // `shared_ptr`, which keeps the `atomic<bool>` alive independently of
    // the PulpView. `prepareForTeardown` flips it to false; any block that
    // later drains sees false and no-ops instead of invoking a
    // `std::function` whose closure references a freed WidgetBridge /
    // ScriptEngine. A `shared_ptr<atomic<bool>>` is used (rather than
    // `dispatch_block_cancel`) because this file is compiled MRC and the
    // deferred-click blocks must survive being copied onto the queue —
    // `dispatch_block_create` blocks do not, and cancelling a non-dispatch
    // block aborts.
    std::shared_ptr<std::atomic<bool>> _deferredClickAlive;
    BOOL _pendingFirstMouse;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        pulp_mac_text_input_client_category_anchor();
        // Ensure the content view tracks the window's content rect
        // so AppKit resizes our frame when the user drags the window edge.
        // Without this, the view's bounds stay at the original frame and Yoga
        // never reflows on resize.
        self.autoresizesSubviews = YES;
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _deferredClickAlive = std::make_shared<std::atomic<bool>>(true);
    }
    return self;
}

// Invalidate queued callbacks before the owning C++ host and View
// tree can be destroyed. After this returns, deferred click blocks are
// guaranteed no-ops and the AppKit animation timer can no longer tick the host's
// FrameClock after it is freed.
- (void)prepareForTeardown {
    [self.animationTimer invalidate];
    self.animationTimer = nil;
    self.frameClock = nullptr;
    self.rootView = nullptr;
    _dragTarget = nullptr;
    _relativeMouseMode = NO;
    if (_deferredClickAlive)
        _deferredClickAlive->store(false);
}

- (void)setRelativeMouseMode:(BOOL)enabled {
    _relativeMouseMode = enabled;
}

- (BOOL)isFlipped { return NO; }
// drawRect fills the entire bounds with rgba8(30,30,46)
// before painting the view tree, so PulpView is opaque by definition.
// Override isOpaque=YES so AppKit doesn't composite NSWindow.backgroundColor
// (white in light mode) under us on hover/focus repaints. Without this,
// the Spectr filterbank flashes WHITE on mouse-over because AppKit clears
// the dirty region with the window bg before invoking drawRect.
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e {
    (void)e;
    // Keep AppKit's default first-mouse behavior: the click that activates an
    // inactive window foregrounds it but is not delivered as mouseDown, so it
    // cannot also select a view in the inspector overlay. Once the window is
    // key, hover and click delivery proceed normally. Trade-off: a first click
    // on a widget while the window is inactive foregrounds rather than
    // interacts, which is correct for an inspect surface.
    return NO;
}

// The Obj-C `_focusedView` ivar is a parallel pointer to
// `pulp::view::View::focused_input_`. The static is auto-cleared by ~View()
// when the focused widget is destroyed (e.g. React unmount of a clicked
// widget); the ivar is not. mouseDown re-syncs at the top, but its own
// work (overlay dispatch, ComboBox
// routing, on_mouse_event → React unmount) can destroy the focused view
// MID-FUNCTION, after which subsequent _focusedView derefs PAC-fault on
// the vtable load. Read the live pointer through this accessor everywhere
// _focusedView could be reached after a callback that might destroy a view.
- (pulp::view::View*)liveFocusedView {
    if (_focusedView != pulp::view::View::focused_input_) {
        _focusedView = pulp::view::View::focused_input_;
    }
    return _focusedView;
}

- (void)clearInteractionState {
    _dragTarget = nullptr;
    if (auto* fv = [self liveFocusedView]) fv->release_input_focus();
    _focusedView = nullptr;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (self.trackingArea) [self removeTrackingArea:self.trackingArea];
    self.trackingArea = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                      NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:self.trackingArea];
}

- (pulp::view::Point)localPoint:(NSEvent*)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    // NSView is not flipped, so Y=0 is bottom. Convert to top-down for the view tree.
    float viewHeight = static_cast<float>(self.bounds.size.height);
    pulp::view::Point pt{static_cast<float>(p.x), viewHeight - static_cast<float>(p.y)};
    if (_relativeMouseMode) {
        _relativeMouseWindowPoint.x += static_cast<float>(event.deltaX);
        _relativeMouseWindowPoint.y += static_cast<float>(event.deltaY);
        pt = _relativeMouseWindowPoint;
    } else {
        _relativeMouseWindowPoint = pt;
    }
    // When a design viewport is in effect, inverse-
    // transform window-space coords into root-space before hit_test sees
    // them. Identity when no viewport is set.
    if (self.pointTransform) pt = self.pointTransform(pt);
    return pt;
}

// Route an event at window-point `pt` to an open ComboBox popup when the point
// is inside the popup's (flip/scroll/clamp-aware) menu rect — the single source
// of truth shared with paint + hit_test. `configure` fills the event-specific
// fields (button/is_down, or is_wheel + deltas). Returns the routed combo (for
// drag-target bookkeeping) or nullptr when nothing was handled. Shared by
// mouseDown and scrollWheel so an open menu takes precedence over sibling views
// and any enclosing ScrollView (whose scroll would otherwise close the popup).
- (pulp::view::ComboBox*)routeToOpenComboPopup:(pulp::view::Point)pt
                                     configure:(void (^)(pulp::view::MouseEvent&))configure {
    auto* combo = pulp::view::ComboBox::active_popup_;
    if (!combo) return nullptr;
    float ddx = 0, ddy = 0, ddw = 0, ddh = 0;
    if (!combo->dropdown_window_rect(ddx, ddy, ddw, ddh)) return nullptr;
    if (pt.x < ddx || pt.x > ddx + ddw || pt.y < ddy || pt.y > ddy + ddh) return nullptr;
    pulp::view::MouseEvent me;
    me.position = to_local(pt, combo, self.rootView);
    me.window_position = pt;
    if (configure) configure(me);
    combo->on_mouse_event(me);
    [self setNeedsDisplay:YES];
    return combo;
}

- (void)scrollWheel:(NSEvent*)event {
    if (!self.rootView) return;
    auto pt = [self localPoint:event];

    // An open ComboBox popup consumes the wheel to scroll its (clamped) item
    // list, ahead of any enclosing ScrollView (whose scroll would close it).
    if ([self routeToOpenComboPopup:pt configure:^(pulp::view::MouseEvent& me) {
            me.is_wheel = true;
            me.scroll_delta_x = static_cast<float>(event.scrollingDeltaX);
            me.scroll_delta_y = static_cast<float>(-event.scrollingDeltaY);
        }]) return;

    auto* target = self.rootView->hit_test(pt);
    if (!target) {
        // Hovering over empty background inside a scroll pane returns no hit
        // because there is no hit-testable child under the point. Route it to
        // the scroll container the cursor is over so scrolling works anywhere in
        // the pane without a click first.
        if (auto* scroll = pulp::view::find_wheel_scroll_view_at(*self.rootView, pt)) {
            pulp::view::MouseEvent me;
            me.position = pt;
            me.window_position = pt;
            me.is_wheel = true;
            me.scroll_delta_x = static_cast<float>(event.scrollingDeltaX);
            me.scroll_delta_y = static_cast<float>(-event.scrollingDeltaY);
            scroll->on_mouse_event(me);
            scroll->layout_children();
            [self setNeedsDisplay:YES];
        }
        return;
    }

    pulp::view::MouseEvent me;
    me.position = pt;
    // Set window_position so the WidgetBridge wheel registrar can emit
    // valid clientX/clientY — without this JSX `onWheel` handlers that
    // do `e.clientX - rect.left` (e.g. anchor-frequency for trackpad
    // zoom) get 0 - rect.left and the wrong frequency anchor.
    me.window_position = pt;
    me.is_wheel = true;
    me.scroll_delta_x = static_cast<float>(event.scrollingDeltaX);
    me.scroll_delta_y = static_cast<float>(-event.scrollingDeltaY);

    // Value widgets (knob / fader / slider / stepper / pan) under the cursor
    // consume the wheel to adjust their value, taking precedence over an
    // enclosing ScrollView — so "hover + scroll" tweaks the control rather than
    // scrolling the page.
    if (target->wants_wheel_value()) {
        target->on_wheel(me.scroll_delta_y);
        [self setNeedsDisplay:YES];
        return;
    }

    // Walk up from target to find nearest native scroll container.
    // W3C wheel bubble: dispatch to every ancestor with on_pointer_event
    // set. Each handler self-filters on me.is_wheel:
    //   - registerPointer's lambda short-circuits when is_wheel == true
    //     (returns early without dispatching pointerdown/up/move/cancel)
    //   - registerWheel's lambda short-circuits when is_wheel == false
    // So a view that registered both gets both halves; a view that
    // registered only one ignores the other. The PRIOR "stop at first
    // ancestor with on_pointer_event" approach (from c29fa49f) was
    // wrong because it stopped at the canvas child that registered
    // ONLY pointer events — the wheel event never reached the
    // ancestor wrap-div that registered the zoom handler. ScrollView
    // ancestor still takes precedence.
    auto* v = target;
    while (v) {
        if (v->wants_wheel_scroll()) {
            v->on_mouse_event(me);
            v->layout_children();
            [self setNeedsDisplay:YES];
            return;
        }
        if (v->on_pointer_event) {
            v->on_mouse_event(me);
        }
        v = v->parent();
    }
    // No ancestor handled the wheel — deliver to the deepest hit so any
    // default behavior still runs.
    target->on_mouse_event(me);
    [self setNeedsDisplay:YES];
}

- (void)mouseDown:(NSEvent*)event {
    @try {
        try {
            if (!self.rootView) return;
            if (self.window.firstResponder != self) {
                [self.window makeFirstResponder:self];
            }
            auto pt = [self localPoint:event];

            // The Obj-C `_focusedView` ivar is a parallel
            // pointer to `pulp::view::View::focused_input_`. The static is
            // auto-cleared by ~View() when the focused widget
            // is destroyed (e.g., a React unmount of a clicked widget); the
            // ivar is not. This top-level sync is not enough by itself:
            // mouseDown's own work (overlay dispatch, ComboBox
            // routing, on_mouse_event → React unmount) can destroy the focused
            // view MID-FUNCTION, so subsequent _focusedView derefs PAC-faulted
            // on the vtable load. Every deref below routes through
            // -[liveFocusedView], which performs the re-sync at each access
            // site. This top-level re-sync is now redundant with the per-call
            // accessor, but kept as defense-in-depth for any future deref that
            // forgets to use the accessor.
            if (_focusedView && _focusedView != pulp::view::View::focused_input_) {
                _focusedView = pulp::view::View::focused_input_;
            }

        // Inspector intercept — consume clicks when inspector is active.
        //
        {
            auto mods = modifiers_from_ns_flags(event.modifierFlags);
            pulp::view::MouseEvent me;
            me.position = {pt.x, pt.y};
            me.modifiers = mods;
            me.is_down = true;
            me.phase = pulp::view::MousePhase::press;
            // Pass this window's root so the installed hook
            // gates to the inspected canvas root; events in a secondary window
            // (the floating InspectorWindow) must not touch the canvas overlay.
            if (pulp::view::View::call_inspector_mouse_hook(me, self.rootView)) {
                [self setNeedsDisplay:YES];
                return;
            }
        }

        // Route a click inside an active ComboBox dropdown overlay to the combo.
        // The dropdown renders as a paint overlay with no view backing, so a
        // normal hit_test would find the view behind it; the shared helper's
        // flip/scroll/clamp-aware rect ensures a click never falls through to a
        // sibling under the menu or misses a scrolled/flipped-up row.
        if (auto* combo = [self routeToOpenComboPopup:pt configure:^(pulp::view::MouseEvent& me) {
                me.button = pulp::view::MouseButton::left;
                me.is_down = true; me.phase = pulp::view::MousePhase::press;
                me.click_count = 1;
            }]) {
            _dragTarget = combo;
            return;
        }

        // Generalized overlay-click routing for React popovers.
        // Any View that called `claim_overlay()` (e.g. via @pulp/react's
        // `<View overlay>` JSX prop) is checked AFTER the ComboBox path
        // (which stays exact-as-was per regression test in
        // test_combo_dropdown.cpp [issue-overlay]) and BEFORE the regular
        // tree hit_test. If the click falls inside the overlay's window
        // rect we route it directly there so absolutely-positioned popover
        // children get the click instead of whatever sibling/ancestor view
        // happens to occupy that pixel.
        if (auto* overlay = pulp::view::View::active_overlay_) {
            if (view_is_in_tree(overlay, self.rootView) &&
                overlay->overlay_contains({pt.x, pt.y})) {
                // Hit-test inside the overlay subtree so nested buttons /
                // labels still receive the click.
                //
                // Only dispatch when
                // hit_test returns a real view. If hit_test returns
                // nullptr, the overlay (or some ancestor in its
                // subtree) failed the visible / enabled / hit_testable
                // / pointer_events check; force-dispatching to the
                // overlay anyway would bypass those guards. Fall
                // through to the standard hit_test below instead.
                auto local_to_overlay = to_local(pt, overlay, self.rootView);
                if (auto* sub = overlay->hit_test(local_to_overlay)) {
                    // When the
                    // overlay handles a click, the standard ComboBox
                    // outside-click notification at the bottom of
                    // mouseDown: is bypassed via the early return
                    // below. Run it here so an open ComboBox dropdown
                    // still closes when the user clicks on a separate
                    // active overlay.
                    pulp::view::ComboBox::notify_global_click(sub);

                    _dragTarget = sub;
                    auto local = to_local(pt, _dragTarget, self.rootView);

                    if (_dragTarget->focusable()) {
                        if (auto* fv = [self liveFocusedView]; fv && fv != _dragTarget)
                            fv->on_focus_changed(false);
                        _focusedView = _dragTarget;
                        _focusedView->on_focus_changed(true);
                        _focusedView->claim_input_focus();
                    } else if (auto* fv = [self liveFocusedView]) {
                        fv->on_focus_changed(false);
                        fv->release_input_focus();
                        _focusedView = nullptr;
                    }

                    pulp::view::MouseEvent me;
                    me.position = local;
                    me.window_position = pt;
                    me.button = pulp::view::MouseButton::left;
                    me.modifiers = modifiers_from_ns_flags(event.modifierFlags);
                    me.is_down = true; me.phase = pulp::view::MousePhase::press;
                    me.click_count = static_cast<int>(event.clickCount);
                    (void)dispatch_mouse_down_if_live(self, _dragTarget, me, local);
                    [self setNeedsDisplay:YES];
                    return;
                }
                // hit_test returned null — the overlay's guards rejected
                // the click. Don't dispatch and don't release_overlay
                // (the overlay is still mounted, just not currently
                // interactive at this position). Fall through to the
                // standard hit_test path below.
            } else {
                // Click landed outside the overlay — auto-release so the
                // overlay's "dismiss on outside click" semantics work without
                // every JSX caller needing a global click listener. The next
                // mount cycle will re-claim if the popover is still open.
                //
                // Go through dismiss_active_overlay() (not the
                // bare release_overlay()) so React state can flip
                // setOpen(false) via on_overlay_dismissed. Falls through to
                // the standard hit_test below so the click also activates
                // whatever view is underneath, matching existing WebView
                // behavior where outside-click closes-and-clicks-through.
                pulp::view::View::dismiss_active_overlay();
            }
        }

        if (dispatch_mac_gesture_pointer_event(self.rootView, pt, event, pulp::view::MousePhase::press, true)) { _dragTarget = nullptr; [self startAnimationTimerIfNeeded]; [self setNeedsDisplay:YES]; return; }
        _dragTarget = self.rootView->hit_test(pt);
        pulp::view::ComboBox::notify_global_click(_dragTarget);

        if (_dragTarget) {
            auto local = to_local(pt, _dragTarget, self.rootView);

            if (_dragTarget->focusable()) {
                if (auto* fv = [self liveFocusedView]; fv && fv != _dragTarget)
                    fv->on_focus_changed(false);
                _focusedView = _dragTarget;
                _focusedView->on_focus_changed(true);
                _focusedView->claim_input_focus();
            } else if (auto* fv = [self liveFocusedView]) {
                fv->on_focus_changed(false);
                fv->release_input_focus();
                _focusedView = nullptr;
            }

            pulp::view::MouseEvent me;
            me.position = local;
            me.window_position = pt;
            me.button = pulp::view::MouseButton::left;
            me.modifiers = modifiers_from_ns_flags(event.modifierFlags);
            me.is_down = true; me.phase = pulp::view::MousePhase::press;
            me.click_count = static_cast<int>(event.clickCount);
            const bool target_alive = dispatch_mouse_down_if_live(self, _dragTarget, me, local);

            // Bubble pointerdown through ancestors that subscribed via
            // registerPointer. on_mouse_event is the W3C bubbling
            // channel; on_mouse_down stays deepest-wins. Without this,
            // a wrap-div around a canvas child (Spectr's FilterBank
            // band-drawer is this exact pattern) never sees the down
            // event because the canvas child wins hit_test. Recompute
            // each ancestor's local-coord position by re-toLocal'ing.
            if (target_alive) {
                for (auto* bubble = _dragTarget->parent(); bubble; bubble = bubble->parent()) {
                    if (!bubble->on_pointer_event) continue;
                    pulp::view::MouseEvent bme = me;
                    bme.position = to_local(pt, bubble, self.rootView);
                    bubble->on_pointer_event(bme);
                }
            }
        }
            [self setNeedsDisplay:YES];
            // A click can kick off a widget animation (e.g. a Toggle's thumb
            // slide, a Knob detent). mouseDown only paints one frame, so without
            // starting the 60fps driver here the animation would not advance until
            // some later unrelated event repaints — which reads as a "stuck" /
            // slow-to-respond control. The timer self-invalidates once no widget
            // animation remains active (see -startAnimationTimerIfNeeded).
            [self startAnimationTimerIfNeeded];
        } catch (const std::exception& e) {
            std::cerr << "MacWindowHost mouseDown error: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "MacWindowHost mouseDown error: unknown exception\n";
        }
    } @catch (NSException* exception) {
        std::cerr << "MacWindowHost mouseDown NSException: "
                  << [[exception name] UTF8String] << " - "
                  << [[exception reason] UTF8String] << "\n";
    }
}

- (void)mouseDragged:(NSEvent*)event {
    @try {
        try {
            // Inspector intercept — route drag-ticks to the in-canvas overlay
            // so its move/resize gesture state machine sees the drag stream.
            // mouseDown alone is not enough: without this the overlay gets the
            // press but never the drag or release, so a drag "sort of works"
            // but never completes/commits. Consume when the overlay handles it.
            {
                auto pt_i = [self localPoint:event];
                auto mods = modifiers_from_ns_flags(event.modifierFlags);
                pulp::view::MouseEvent me;
                me.position = {pt_i.x, pt_i.y};
                me.modifiers = mods;
                me.is_down = true;  // button still held during drag
                // State the gesture phase explicitly so the
                // overlay's move/resize machine treats this as a DRAG TICK,
                // not a release. Without this the overlay (which historically
                // inferred drag-vs-release from is_down) ended the gesture on
                // the first mac drag tick and fell through to re-selection.
                me.phase = pulp::view::MousePhase::drag;
                // Gate to this window's root (see press).
                if (pulp::view::View::call_inspector_mouse_hook(me, self.rootView)) {
                    [self setNeedsDisplay:YES];
                    return;
                }
            }
            // _dragTarget is captured in mouseDown but the View
            // it points to may be unmounted (and freed) before the next
            // drag event arrives, e.g. when a click triggers a React state
            // change that destroys the widget. Re-validate against the
            // live tree before any deref.
            if (!self.rootView) return;
            auto pt = [self localPoint:event];
            if (dispatch_mac_gesture_pointer_event(self.rootView, pt, event, pulp::view::MousePhase::drag, true)) { [self startAnimationTimerIfNeeded]; [self setNeedsDisplay:YES]; return; }
            if (!_dragTarget) return;
            if (!view_is_in_tree(_dragTarget, self.rootView)) {
                _dragTarget = nullptr;
                return;
            }
            auto local = to_local(pt, _dragTarget, self.rootView);
            _dragTarget->on_mouse_drag(local);
            if (_dragTarget->on_drag) _dragTarget->on_drag(local);

            // Bubble on_drag up to ancestors with on_drag set. Mirrors what
            // mouseDown already does for on_pointer_event.
            // Without this, JSX patterns where the click target is an
            // inner presentational widget (Chainer's XY pad dot, the
            // 5px slider track) but the drag handler is on an outer
            // wrapper get a one-shot pointerdown but no drag stream.
            // Re-toLocal per ancestor so the event coords are in that
            // ancestor's local space.
            for (auto* bubble = _dragTarget->parent(); bubble; bubble = bubble->parent()) {
                if (!bubble->on_drag) continue;
                auto bubble_local = to_local(pt, bubble, self.rootView);
                bubble->on_drag(bubble_local);
            }
            [self setNeedsDisplay:YES];
        } catch (const std::exception& e) {
            std::cerr << "MacWindowHost mouseDragged error: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "MacWindowHost mouseDragged error: unknown exception\n";
        }
    } @catch (NSException* exception) {
        std::cerr << "MacWindowHost mouseDragged NSException: "
                  << [[exception name] UTF8String] << " - "
                  << [[exception reason] UTF8String] << "\n";
    }
}

- (void)mouseUp:(NSEvent*)event {
    @try {
        try {
            // Inspector intercept — route the release to the overlay so its
            // move/resize gesture commits (and records its undo entry).
            // Mirrors mouseDown/mouseDragged. Consume when handled.
            {
                auto pt_i = [self localPoint:event];
                auto mods = modifiers_from_ns_flags(event.modifierFlags);
                pulp::view::MouseEvent me;
                me.position = {pt_i.x, pt_i.y};
                me.modifiers = mods;
                me.is_down = false;  // release
                me.phase = pulp::view::MousePhase::release;
                // Gate to this window's root (see press).
                if (pulp::view::View::call_inspector_mouse_hook(me, self.rootView)) {
                    [self setNeedsDisplay:YES];
                    return;
                }
            }
            auto pt = [self localPoint:event];
            if (dispatch_mac_gesture_pointer_event(self.rootView, pt, event, pulp::view::MousePhase::release, false)) { _dragTarget = nullptr; [self setNeedsDisplay:YES]; return; }
            if (_dragTarget) {
                // _dragTarget may point at a freed View if
                // the mouseDown handler triggered a React unmount of the
                // clicked widget (every dropdown selection in Spectr does
                // this — clicking a band-count item flushes the React
                // tree and the popover Views are dropped before mouseUp
                // ever arrives). Drop the up event silently if the
                // captured pointer is no longer in the live tree, rather
                // than dereference garbage memory and SIGSEGV.
                if (!view_is_in_tree(_dragTarget, self.rootView)) {
                    _dragTarget = nullptr;
                    [self setNeedsDisplay:YES];
                    return;
                }
                auto local = to_local(pt, _dragTarget, self.rootView);
                auto released_target = self.rootView ? self.rootView->hit_test(pt) : nullptr;
                // DOM-style click bubbling. `hit_test` returns
                // the deepest hit-testable view under the cursor, but the
                // `onClick` handler (registered via `registerClick(id)`) may
                // live on an ancestor. The classic reproducer: @pulp/react
                // turns `<button onClick=...>Clear</button>` into a
                // `<View onClick=...>` parent with a `<Label>Clear</Label>`
                // child (Spectr's dom-adapter wraps string children in
                // synthetic Labels). Clicking the visible "Clear" text
                // hits the Label, which has no `on_click`, so capturing only
                // `_dragTarget` silently drops the click.
                // Walk up the parent chain to find the nearest ancestor
                // (including `_dragTarget` itself) with a registered
                // handler — mirrors the browser behaviour @pulp/react users
                // expect.
                pulp::view::View* click_target = _dragTarget;
                while (click_target && !click_target->on_click) {
                    click_target = click_target->parent();
                }
                auto click_handler = click_target ? click_target->on_click : std::function<void()>{};
                auto global_click = self.rootView ? self.rootView->on_global_click : std::function<void(const std::string&, uint16_t)>{};
                // global_click reports the immediate hit (matches existing
                // inspect-click behaviour: Cmd-click on a text label tells
                // the inspector exactly which view was hit, not the
                // bubbled-to ancestor).
                auto clicked_id = _dragTarget->id();
                auto modifiers = modifiers_from_ns_flags(event.modifierFlags);
                _dragTarget->on_mouse_up(local);

                // Bubble pointerup through ancestors (W3C pointer event
                // bubbling — mirrors mouseDown bubble above). Same
                // rationale: wrap-divs with on_pointer_event subscribed
                // never see pointerup otherwise, breaking drag-release
                // for things like FilterBank band finalization.
                pulp::view::MouseEvent up_me;
                up_me.position = local;
                up_me.window_position = pt;
                up_me.button = pulp::view::MouseButton::left;
                up_me.modifiers = modifiers;
                up_me.is_down = false; up_me.phase = pulp::view::MousePhase::release;
                up_me.click_count = static_cast<int>(event.clickCount);
                _dragTarget->on_mouse_event(up_me);
                for (auto* bubble = _dragTarget->parent(); bubble; bubble = bubble->parent()) {
                    if (!bubble->on_pointer_event) continue;
                    pulp::view::MouseEvent bme = up_me;
                    bme.position = to_local(pt, bubble, self.rootView);
                    bubble->on_pointer_event(bme);
                }
                if (released_target == _dragTarget && (click_handler || global_click)) {
                    // `click_handler` / `global_click` are
                    // `std::function`s whose closures reference the
                    // WidgetBridge / ScriptEngine that built them. Deferring
                    // their invocation via a bare `dispatch_async` block left
                    // an unbounded lifetime hazard: if the bridge/engine were
                    // freed (e.g. a test-scoped owner going out of scope)
                    // before the block drained, the block ran a dangling
                    // closure → intermittent SIGSEGV.
                    //
                    // Fix: the deferred block captures a COPY of the view's
                    // `_deferredClickAlive` liveness token (a
                    // `shared_ptr<atomic<bool>>`). The copy keeps the
                    // `atomic<bool>` alive even after the PulpView itself is
                    // gone. `-prepareForTeardown` flips the token to false. A
                    // block that drains after teardown sees `false` and no-ops.
                    // Do not capture or dereference `self` from this block:
                    // this file is compiled MRC, and hidden test windows can be
                    // torn down before AppKit drains every main-queue callback.
                    // The copied handlers must keep their own WidgetBridge /
                    // ScriptEngine liveness guards; this block intentionally
                    // owns only the teardown token, and mouseUp already marks
                    // the view dirty immediately after scheduling it.
                    std::shared_ptr<std::atomic<bool>> aliveToken = _deferredClickAlive;
                    dispatch_async(dispatch_get_main_queue(), ^{
                        // Defused after teardown: do not invoke handlers whose
                        // backing WidgetBridge / ScriptEngine may already be
                        // freed.
                        if (!aliveToken || !aliveToken->load())
                            return;
                        @try {
                            try {
                                if (click_handler) click_handler();
                                if (global_click) global_click(clicked_id, modifiers);
                            } catch (const std::exception& e) {
                                std::cerr << "MacWindowHost deferred click error: " << e.what() << "\n";
                            } catch (...) {
                                std::cerr << "MacWindowHost deferred click error: unknown exception\n";
                            }
                        } @catch (NSException* exception) {
                            std::cerr << "MacWindowHost deferred click NSException: "
                                      << [[exception name] UTF8String] << " - "
                                      << [[exception reason] UTF8String] << "\n";
                        }
                    });
                }
                _dragTarget = nullptr;
            }
            [self setNeedsDisplay:YES];
        } catch (const std::exception& e) {
            std::cerr << "MacWindowHost mouseUp error: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "MacWindowHost mouseUp error: unknown exception\n";
        }
    } @catch (NSException* exception) {
        std::cerr << "MacWindowHost mouseUp NSException: "
                  << [[exception name] UTF8String] << " - "
                  << [[exception reason] UTF8String] << "\n";
    }
}

- (void)rightMouseDown:(NSEvent*)event {
    @try {
        try {
            if (!self.rootView) return;
            auto pt = [self localPoint:event];
            pulp::view::dispatch_context_menu(*self.rootView, pt);
            [self setNeedsDisplay:YES];
        } catch (const std::exception& e) {
            std::cerr << "MacWindowHost rightMouseDown error: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "MacWindowHost rightMouseDown error: unknown exception\n";
        }
    } @catch (NSException* exception) {
        std::cerr << "MacWindowHost rightMouseDown NSException: "
                  << [[exception name] UTF8String] << " - "
                  << [[exception reason] UTF8String] << "\n";
    }
}

// ── Keyboard input ───────────────────────────────────────────────

// NSResponder routes Cmd-modified chords through
// performKeyEquivalent:, NOT keyDown:. Without this override every Cmd
// chord is consumed by the responder chain before View::on_global_key or
// the script dispatcher, leaving Cmd shortcut listeners silently dead.
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    if (!self.rootView) return NO;
    auto key  = key_code_from_ns(event.keyCode);
    auto mods = modifiers_from_ns_flags(event.modifierFlags);
    pulp::view::KeyEvent gke;
    gke.key = key;
    gke.modifiers = mods;
    gke.is_down = true;
    gke.is_repeat = event.isARepeat;

    if (auto* fv = [self liveFocusedView]) {
        if (fv->on_key_event(gke)) {
            [self startAnimationTimerIfNeeded];
            [self setNeedsDisplay:YES];
            return YES;
        }
    }

    // Honor consumption: a chord the root hook claims (e.g. the shell's
    // CommandRegistry dispatch) returns YES — no menu fallthrough, and no
    // script fan-out so the command can't double-fire a JS 'keydown'.
    if (self.rootView->on_global_key && self.rootView->on_global_key(gke)) {
        [self setNeedsDisplay:YES];
        return YES;
    }
    // Unconsumed: additive script fan-out (no-op when the script target is
    // not linked); NO keeps menu shortcuts (Cmd+W, Cmd+Q) working.
    pulp::view::script_events::dispatch_global_key(
        static_cast<int>(key), mods, /*is_down=*/true);
    return NO;
}

- (void)keyDown:(NSEvent*)event {
    @try {
        try {
            auto key = key_code_from_ns(event.keyCode);
            auto mods = modifiers_from_ns_flags(event.modifierFlags);

            // Re-sync the Obj-C `_focusedView` ivar from the
            // auto-clearing `View::focused_input_` static. Same rationale
            // as mouseDown: if the focused View was unmounted between the
            // last input event and this one (~View clears the static),
            // the ivar still dangles and any deref below would PAC-fault on
            // freed memory.
            if (_focusedView && _focusedView != pulp::view::View::focused_input_) {
                _focusedView = pulp::view::View::focused_input_;
            }

        // Inspector intercept — check before all other key handling
        {
            pulp::view::KeyEvent ike;
            ike.key = key;
            ike.modifiers = mods;
            ike.is_down = true;
            ike.is_repeat = event.isARepeat;
            if (pulp::view::View::call_inspector_key_hook(ike)) {
                [self setNeedsDisplay:YES];
                return;
            }
        }

        if (key == pulp::view::KeyCode::tab && self.rootView) {
            if (auto* fv = [self liveFocusedView]) {
                pulp::view::KeyEvent ke;
                ke.key = key;
                ke.modifiers = mods;
                ke.is_down = true;
                ke.is_repeat = event.isARepeat;
                if (fv->on_key_event(ke)) {
                    [self startAnimationTimerIfNeeded];
                    [self setNeedsDisplay:YES];
                    return;
                }
            }

            // Re-sync via liveFocusedView so a destroyed prior
            // focused view doesn't leak a dangling ivar into focus_next/prev
            // and the on_focus_changed/release_input_focus calls below.
            auto* old = [self liveFocusedView];
            pulp::view::View* next = nullptr;
            if (mods & pulp::view::kModShift)
                next = pulp::view::View::focus_prev(*self.rootView, old);
            else
                next = pulp::view::View::focus_next(*self.rootView, old);
            if (next && next != old) {
                if (old) {
                    old->on_focus_changed(false);
                    old->release_input_focus();
                }
                _focusedView = next;
                _focusedView->on_focus_changed(true);
                _focusedView->claim_input_focus();
            }
            [self setNeedsDisplay:YES];
            return;
        }

        // Global key callback — checked after inspector but before tab/escape
        if (self.rootView && self.rootView->on_global_key) {
            pulp::view::KeyEvent gke;
            gke.key = key;
            gke.modifiers = mods;
            gke.is_down = true;
            gke.is_repeat = event.isARepeat;
            if (self.rootView->on_global_key(gke)) {
                [self setNeedsDisplay:YES];
                return;
            }
        }

        // Fan out to every live WidgetBridge so
        // `@pulp/react` apps (Spectr) receive bare-key shortcuts like
        // 'S' or Escape that React effects bind via
        // `window.addEventListener('keydown', ...)`. The bridge's own
        // registered-shortcut path suppresses bare keys when a text-input
        // widget owns focus (see WidgetBridge::forward_key_event), so
        // typing `?` into a search box doesn't trigger a `?` shortcut.
        // This is additive — focused-view delivery still runs below.
        pulp::view::script_events::dispatch_global_key(static_cast<int>(key),
                                                       mods,
                                                       /*is_down=*/true);

        if (key == pulp::view::KeyCode::escape && self.rootView) {
            if (auto* modal = find_topmost_modal(self.rootView)) {
                pulp::view::KeyEvent ke;
                ke.key = key;
                ke.modifiers = mods;
                ke.is_down = true;
                ke.is_repeat = event.isARepeat;
                if (modal->on_key_event(ke)) {
                    [self startAnimationTimerIfNeeded];
                    [self setNeedsDisplay:YES];
                    return;
                }
            }
            // Host-level ESC fallback for an open ComboBox dropdown
            // whose focus has been stolen by a sibling JS-driven element
            // (common pattern: React mounts a popover next to the combo,
            // grabs focus inside it, but the user hits ESC expecting the
            // dropdown they can still see to close). The ComboBox's own
            // on_key_event handler only fires when ComboBox owns focus,
            // so a stolen-focus case wedges the dropdown open with no
            // keyboard escape route. Close at host level the same way
            // active_overlay_ does below.
            if (pulp::view::ComboBox::active_popup_) {
                pulp::view::ComboBox::close_active_popup();
                [self startAnimationTimerIfNeeded];
                [self setNeedsDisplay:YES];
                return;
            }
            // Generic active_overlay_ ESC dismissal. ModalOverlay,
            // ComboBox, and CallOutBox already have their own ESC handlers
            // (ComboBox/CallOutBox sit on the focused view and consume their
            // own KeyCode::escape; modals are handled above). The generic
            // `<View overlay>` path has no widget-specific ESC owner — wire
            // it here so React popovers built from active_overlay_ close on
            // ESC like every other popover surface.
            if (pulp::view::View::active_overlay_) {
                pulp::view::View::dismiss_active_overlay();
                [self startAnimationTimerIfNeeded];
                [self setNeedsDisplay:YES];
                return;
            }
        }

        [self interpretKeyEvents:@[event]];

            // interpretKeyEvents above can dispatch IME / app
            // commands that ultimately destroy the focused widget (e.g.,
            // a JS key handler triggers a React unmount). Re-sync via
            // liveFocusedView so we don't deref a freed view here.
            if (auto* fv = [self liveFocusedView]) {
                pulp::view::KeyEvent ke;
                ke.key = key;
                ke.modifiers = mods;
                ke.is_down = true;
                ke.is_repeat = event.isARepeat;
                fv->on_key_event(ke);
                [self startAnimationTimerIfNeeded];
                [self setNeedsDisplay:YES];
            }
        } catch (const std::exception& e) {
            std::cerr << "MacWindowHost keyDown error: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "MacWindowHost keyDown error: unknown exception\n";
        }
    } @catch (NSException* exception) {
        std::cerr << "MacWindowHost keyDown NSException: "
                  << [[exception name] UTF8String] << " - "
                  << [[exception reason] UTF8String] << "\n";
    }
}

// Symmetric key-up so released keys reach handlers that need both edges — most
// importantly musical-typing note-off (without this, computer-keyboard notes
// stick). Routes the global-key hook (is_down=false) then the focused view.
- (void)keyUp:(NSEvent*)event {
    @try {
        try {
            pulp::view::KeyEvent ke;
            ke.key = key_code_from_ns(event.keyCode);
            ke.modifiers = modifiers_from_ns_flags(event.modifierFlags);
            ke.is_down = false;
            if (self.rootView && self.rootView->on_global_key)
                self.rootView->on_global_key(ke);
            if (auto* fv = [self liveFocusedView]) fv->on_key_event(ke);
            [self setNeedsDisplay:YES];
        } catch (const std::exception& e) {
            std::cerr << "MacWindowHost keyUp error: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "MacWindowHost keyUp error: unknown exception\n";
        }
    } @catch (NSException* exception) {
        std::cerr << "MacWindowHost keyUp NSException: "
                  << [[exception name] UTF8String] << "\n";
    }
}

- (void)mouseMoved:(NSEvent*)event {
    @try {
        try {
            if (!self.rootView) return;
            auto pt = [self localPoint:event];

            // Inspector hover intercept
            {
                pulp::view::MouseEvent me;
                me.position = {pt.x, pt.y};
                me.is_down = false;
                me.phase = pulp::view::MousePhase::hover;
                // Gate to this window's root so hovering the
                // floating InspectorWindow does not highlight the canvas.
                pulp::view::View::call_inspector_mouse_hook(me, self.rootView);
                // Don't consume — let normal hover handling continue for cursor changes
            }

            if (pulp::view::ComboBox::active_popup_) {
                auto* combo = pulp::view::ComboBox::active_popup_;
                float cx = 0, cy = 0;
                auto* v = static_cast<pulp::view::View*>(combo);
                while (v) { cx += v->bounds().x; cy += v->bounds().y; v = v->parent(); }
                pulp::view::MouseEvent me;
                me.position = {pt.x - cx, pt.y - cy};
                me.is_down = false;
                combo->on_mouse_event(me);
            }

            self.rootView->simulate_hover(pt);

            auto* target = self.rootView->hit_test(pt);
            // The inspector overlay may override the cursor for
            // its move/resize affordances (it owns mouse-move before normal
            // hit-testing). A returned style >= 0 wins over the hit view's
            // own cursor(); -1 defers to the normal path below.
            int inspector_cursor = -1;
            {
                pulp::view::MouseEvent cme;
                cme.position = {pt.x, pt.y};
                cme.is_down = false;
                // Gate to this window's root so the canvas
                // overlay's cursor affordance is not driven by moves inside a
                // secondary window.
                inspector_cursor =
                    pulp::view::View::call_inspector_cursor_hook(cme, self.rootView);
            }
            if (target || inspector_cursor >= 0) {
                auto style = inspector_cursor >= 0
                    ? static_cast<pulp::view::View::CursorStyle>(inspector_cursor)
                    : target->cursor();
                // Unhide cursor before switching to any non-invisible style
                if (style != pulp::view::View::CursorStyle::invisible && s_cursor_hidden) {
                    [NSCursor unhide];
                    s_cursor_hidden = false;
                }
                switch (style) {
                    case pulp::view::View::CursorStyle::pointer:
                        [[NSCursor pointingHandCursor] set]; break;
                    case pulp::view::View::CursorStyle::crosshair:
                        [[NSCursor crosshairCursor] set]; break;
                    case pulp::view::View::CursorStyle::text:
                        [[NSCursor IBeamCursor] set]; break;
                    case pulp::view::View::CursorStyle::grab:
                        [[NSCursor openHandCursor] set]; break;
                    case pulp::view::View::CursorStyle::grabbing:
                        [[NSCursor closedHandCursor] set]; break;
                    case pulp::view::View::CursorStyle::not_allowed:
                        [[NSCursor operationNotAllowedCursor] set]; break;
                    case pulp::view::View::CursorStyle::invisible:
                        if (!s_cursor_hidden) {
                            [NSCursor hide];
                            s_cursor_hidden = true;
                        }
                        break;
                    case pulp::view::View::CursorStyle::horizontal_resize:
                        [[NSCursor resizeLeftRightCursor] set]; break;
                    case pulp::view::View::CursorStyle::vertical_resize:
                        [[NSCursor resizeUpDownCursor] set]; break;
                    case pulp::view::View::CursorStyle::top_left_resize:
                    case pulp::view::View::CursorStyle::bottom_right_resize:
                        // Proper diagonal ↖↘ resize cursor. AppKit ships no public diagonal
                        // resize cursor, but the private
                        // `_windowResizeNorthWestSouthEastCursor` selector
                        // is the standard NSWindow corner-resize arrow.
                        // Guard with respondsToSelector and fall back to
                        // crosshair if the (undocumented) symbol is gone.
                        [pulp_diagonal_resize_cursor(YES) set]; break;
                    case pulp::view::View::CursorStyle::top_right_resize:
                    case pulp::view::View::CursorStyle::bottom_left_resize:
                        // Proper diagonal ↗↙
                        // resize cursor (private
                        // `_windowResizeNorthEastSouthWestCursor`).
                        [pulp_diagonal_resize_cursor(NO) set]; break;
                    case pulp::view::View::CursorStyle::multi_directional_resize:
                        [[NSCursor openHandCursor] set]; break;
                    // CSS cursor keywords with native NSCursor backings.
                    //   alias → dragLinkCursor (macOS 10.6+).
                    //   copy → dragCopyCursor (macOS 10.6+).
                    //   zoom-in / zoom-out → zoomInCursor / zoomOutCursor
                    //     (macOS 10.15+; defensive respondsToSelector
                    //     check in case the symbol is weak-linked or
                    //     unavailable on the runtime OS).
                    //   context-menu → contextualMenuCursor (macOS 10.6+).
                    case pulp::view::View::CursorStyle::alias:
                        [[NSCursor dragLinkCursor] set]; break;
                    case pulp::view::View::CursorStyle::copy:
                        [[NSCursor dragCopyCursor] set]; break;
                    case pulp::view::View::CursorStyle::zoom_in:
                        if ([NSCursor respondsToSelector:@selector(zoomInCursor)]) {
                            [[NSCursor performSelector:@selector(zoomInCursor)] set];
                        } else {
                            [[NSCursor arrowCursor] set];
                        }
                        break;
                    case pulp::view::View::CursorStyle::zoom_out:
                        if ([NSCursor respondsToSelector:@selector(zoomOutCursor)]) {
                            [[NSCursor performSelector:@selector(zoomOutCursor)] set];
                        } else {
                            [[NSCursor arrowCursor] set];
                        }
                        break;
                    case pulp::view::View::CursorStyle::context_menu:
                        [[NSCursor contextualMenuCursor] set]; break;
                    default:
                        [[NSCursor arrowCursor] set]; break;
                }
            }

            [self setNeedsDisplay:YES];
        } catch (const std::exception& e) {
            std::cerr << "MacWindowHost mouseMoved error: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "MacWindowHost mouseMoved error: unknown exception\n";
        }
    } @catch (NSException* exception) {
        std::cerr << "MacWindowHost mouseMoved NSException: "
                  << [[exception name] UTF8String] << " - "
                  << [[exception reason] UTF8String] << "\n";
    }
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    @try {
        try {
            if (!self.rootView) return;
            self.rootView->simulate_hover({-1, -1});
            [self startAnimationTimerIfNeeded];
            [self setNeedsDisplay:YES];
        } catch (const std::exception& e) {
            std::cerr << "MacWindowHost mouseExited error: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "MacWindowHost mouseExited error: unknown exception\n";
        }
    } @catch (NSException* exception) {
        std::cerr << "MacWindowHost mouseExited NSException: "
                  << [[exception name] UTF8String] << " - "
                  << [[exception reason] UTF8String] << "\n";
    }
}

// ── Trackpad gestures (pinch/rotate) ───────────────────────────────

- (void)magnifyWithEvent:(NSEvent*)event {
    if (!self.rootView) return;
    auto pt = [self localPoint:event];
    auto* target = self.rootView->hit_test(pt);
    if (!target) return;

    pulp::view::GestureEvent ge;
    if (event.phase == NSEventPhaseBegan)        ge.phase = pulp::view::GesturePhase::began;
    else if (event.phase == NSEventPhaseEnded)   ge.phase = pulp::view::GesturePhase::ended;
    else if (event.phase == NSEventPhaseCancelled) ge.phase = pulp::view::GesturePhase::cancelled;
    else                                          ge.phase = pulp::view::GesturePhase::changed;

    ge.delta_scale = static_cast<float>(event.magnification);
    ge.scale = 1.0f + ge.delta_scale;
    ge.position = pt;
    target->on_gesture_event(ge);
    [self setNeedsDisplay:YES];
}

- (void)rotateWithEvent:(NSEvent*)event {
    if (!self.rootView) return;
    auto pt = [self localPoint:event];
    auto* target = self.rootView->hit_test(pt);
    if (!target) return;

    pulp::view::GestureEvent ge;
    if (event.phase == NSEventPhaseBegan)        ge.phase = pulp::view::GesturePhase::began;
    else if (event.phase == NSEventPhaseEnded)   ge.phase = pulp::view::GesturePhase::ended;
    else if (event.phase == NSEventPhaseCancelled) ge.phase = pulp::view::GesturePhase::cancelled;
    else                                          ge.phase = pulp::view::GesturePhase::changed;

    ge.delta_rotation = static_cast<float>(event.rotation) * (3.14159265f / 180.0f); // degrees → radians
    ge.rotation = ge.delta_rotation;
    ge.position = pt;
    target->on_gesture_event(ge);
    [self setNeedsDisplay:YES];
}

- (void)startAnimationTimerIfNeeded {
    if (self.animationTimer) return;
    // 60fps timer to drive animations
    self.animationTimer = [NSTimer scheduledTimerWithTimeInterval:1.0/60.0
        repeats:YES block:^(NSTimer* timer) {
            if (self.frameClock) self.frameClock->tick(1.0f / 60.0f);
            if (self.rootView) self.rootView->advance_gesture_recognizers();
            // Advance widget animations
            [self advanceWidgetAnimations:self.rootView dt:1.0f/60.0f];
            [self setNeedsDisplay:YES];

            // Stop timer when no animations are active
            if (self.frameClock && !self.frameClock->has_active_subscribers() &&
                (!self.rootView || !self.rootView->has_time_driven_gestures()) &&
                ![self hasActiveAnimations:self.rootView]) {
                [self.animationTimer invalidate];
                self.animationTimer = nil;
            }
        }];
}

- (void)advanceWidgetAnimations:(pulp::view::View*)view dt:(float)dt {
    if (!view) return;
    // Try each animated widget type
    if (auto* k = dynamic_cast<pulp::view::Knob*>(view)) k->advance_animations(dt);
    else if (auto* t = dynamic_cast<pulp::view::Toggle*>(view)) t->advance_animations(dt);
    else if (auto* f = dynamic_cast<pulp::view::Fader*>(view)) f->advance_animations(dt);
    else if (auto* sv = dynamic_cast<pulp::view::ScrollView*>(view)) sv->advance_animations(dt);
    else if (auto* tip = dynamic_cast<pulp::view::Tooltip*>(view)) tip->advance_animations(dt);

    for (size_t i = 0; i < view->child_count(); ++i)
        [self advanceWidgetAnimations:view->child_at(i) dt:dt];
}

- (BOOL)hasActiveAnimations:(pulp::view::View*)view {
    if (!view) return NO;
    if (auto* k = dynamic_cast<pulp::view::Knob*>(view))
        if (k->hover_glow() > 0.01f && k->hover_glow() < 0.99f) return YES;
    if (auto* t = dynamic_cast<pulp::view::Toggle*>(view))
        if (t->thumb_position() > 0.01f && t->thumb_position() < 0.99f) return YES;
    if (auto* f = dynamic_cast<pulp::view::Fader*>(view))
        if (f->hover_scale() > 1.01f) return YES;
    if (auto* sv = dynamic_cast<pulp::view::ScrollView*>(view))
        if (sv->scroll_animating()) return YES;

    for (size_t i = 0; i < view->child_count(); ++i)
        if ([self hasActiveAnimations:view->child_at(i)]) return YES;
    return NO;
}

- (void)drawRect:(NSRect)dirtyRect {
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    NSRect bounds = self.bounds;

    pulp::canvas::CoreGraphicsCanvas canvas(ctx,
        static_cast<float>(bounds.size.width),
        static_cast<float>(bounds.size.height));

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
        pulp::view::View::paint_overlays(canvas, self.rootView);

        // Inspector overlay is painted automatically via View::paint_overlays()
    }
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    // AppKit resizes us during a window resize. Push the new
    // bounds into the root View immediately and relayout so hit testing,
    // tracking-area updates, and the next paint all see the new geometry.
    if (self.rootView) {
        self.rootView->set_bounds({0, 0,
            static_cast<float>(newSize.width),
            static_cast<float>(newSize.height)});
        self.rootView->layout_children();
    }
    [self setNeedsDisplay:YES];
}

- (void)dealloc {
    [self.animationTimer invalidate];
    // Belt-and-suspenders: even if a host teardown path forgot
    // to call -prepareForTeardown, cancel any still-queued deferred-click
    // blocks here so a dangling block can never outlive this view.
    [self prepareForTeardown];
}

@end

// ── PulpMetalView: CAMetalLayer-backed NSView (GPU rendering path) ───────────

#ifdef PULP_HAS_SKIA

@interface PulpMetalView : PulpView
@property (nonatomic, readonly) CAMetalLayer* metalLayer;
@property (nonatomic, copy) dispatch_block_t repaintBlock;
// C++ render state is managed by MacGpuWindowHost, not the view
@end

@implementation PulpMetalView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;

        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = MTLCreateSystemDefaultDevice();
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;
        CGFloat scale = self.window ? self.window.backingScaleFactor
                                    : [NSScreen mainScreen].backingScaleFactor;
        layer.contentsScale = scale;
        CGSize backing = NSMakeSize(frame.size.width * scale, frame.size.height * scale);
        layer.drawableSize = backing;

        // Declare the layer opaque and seed its background to
        // the standalone-app's base dark fill (matches paint_scene RGB
        // 30,30,46 = 0x1E1E2E). Without this, AppKit auto-clears the
        // layer to its default `backgroundColor` (clear/white-equivalent
        // through the window) every time `setNeedsDisplay:YES` fires —
        // which any hover / mouse event triggers — and the user sees an
        // opaque-white flash in the canvas region between when AppKit
        // invalidates and when the next display-link tick produces a
        // Metal frame. The "live paints white but headless paints dark"
        // symptom comes from this gap, not from the canvas command list.
        //
        // BGRA8Unorm at full opacity, sRGB encoded as the pixel format
        // expects (0x1E/255 ≈ 0.118, etc).
        layer.opaque = YES;
        const CGFloat dark[4] = { 30.0/255.0, 30.0/255.0, 46.0/255.0, 1.0 };
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        layer.backgroundColor = CGColorCreate(cs, dark);
        CGColorSpaceRelease(cs);

        // Pin the most-recent drawable to the top-left during a live resize
        // instead of letting Core Animation's default kCAGravityResize STRETCH
        // it to the new layer bounds. The default makes the canvas appear to
        // zoom in/out as you drag a window edge (the old frame is scaled to the
        // new size in the gap before the next Metal frame lands). With
        // top-left gravity the content stays put at its native scale and the
        // newly-exposed area shows the dark background until the next frame —
        // i.e. the canvas "stays in its fixed position" as the user expects.
        // Non-flipped NSView layer: max-Y is visually the top, so TopLeft is
        // the upper-left corner where our (0,0)-origin UI begins.
        layer.contentsGravity = kCAGravityTopLeft;

        self.layer = layer;
        _metalLayer = layer;
    }
    return self;
}

// `wantsUpdateLayer = YES` tells AppKit to use the
// layer-based drawing path (calls `-updateLayer` instead of
// `-drawRect:`) and, critically, NOT to auto-clear the backing layer
// to opaque background between updates. Combined with `layer.opaque
// = YES` above, this makes setNeedsDisplay-triggered invalidations a
// no-op for the layer's own contents — the most-recent Metal frame
// stays presented until the next display-link tick produces a new one.
- (BOOL)wantsUpdateLayer {
    return YES;
}

- (void)updateLayer {
    // No-op: Metal frames are produced by MacGpuWindowHost::render_frame
    // off the display link callback, NOT inside AppKit's update cycle.
    // We just need this method to exist so AppKit honors
    // wantsUpdateLayer and skips its own paint pipeline.
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
}

- (void)setNeedsDisplay:(BOOL)needsDisplay {
    [super setNeedsDisplay:needsDisplay];
    if (needsDisplay && self.repaintBlock) self.repaintBlock();
}

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
    CGSize backing = CGSizeMake(self.bounds.size.width * scale, self.bounds.size.height * scale);
    self.metalLayer.drawableSize = backing;
}

@end

#endif // PULP_HAS_SKIA

// ── PulpWindowDelegate ───────────────────────────────────────────────────────

@interface PulpWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, copy) void (^onClose)(void);
@property (nonatomic, copy) void (^onResize)(float, float);
/// Real-time aspect-lock snap during user drag.
/// macOS `setContentAspectRatio:` is unreliable in practice (it does
/// not engage during edge-drag on every monitor configuration, and is
/// bypassed entirely by the green zoom button / programmatic resize).
/// Implementing `windowWillResize:toSize:` constrains every proposed
/// resize before AppKit commits it, so the live drag is locked to the
/// design aspect regardless of drag handle or zoom path. When 0, no
/// constraint is applied.
@property (nonatomic, assign) CGFloat aspectRatio;
/// Explicit window role for the close policy. A SECONDARY
/// window (the floating inspector) only orders itself out on close and never
/// stops the app; a PRIMARY window (the main canvas, the default) stops the
/// app on close regardless of whatever secondary windows remain visible. This
/// replaces the previous visible-window-count heuristic, which left the app
/// running with only the inspector after the main window closed.
@property (nonatomic, assign) BOOL isSecondaryWindow;
@end

@implementation PulpWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
    if (self.onClose) self.onClose();
    [sender orderOut:nil];
    // Role-based close policy (replaces the visible-count
    // heuristic). A secondary window (e.g. the floating inspector) closing only
    // orders itself out and leaves the main window + app running. A primary
    // window (the main canvas) closing stops the app regardless of any
    // secondary windows still visible.
    if (!self.isSecondaryWindow) request_cocoa_app_stop();
    return YES;
}

- (void)windowDidResize:(NSNotification*)notification {
    if (self.onResize) {
        NSWindow* window = notification.object;
        NSSize size = window.contentView.bounds.size;
        self.onResize(static_cast<float>(size.width), static_cast<float>(size.height));
    }
}

- (NSSize)windowWillResize:(NSWindow*)sender toSize:(NSSize)frameSize {
    // No-op when aspect-lock isn't requested (the scroll-mode showcase and
    // every non-design-viewport window take this path — resize freely).
    if (self.aspectRatio <= 0) return frameSize;

    // Convert frame size → content size (NSWindow gives us frame; we
    // want to constrain the *content* aspect, since the design viewport
    // and our paint math are in content coordinates).
    NSRect frameRect  = NSMakeRect(0, 0, frameSize.width, frameSize.height);
    NSRect contentRect = [sender contentRectForFrameRect:frameRect];
    CGFloat targetW = contentRect.size.width;
    CGFloat targetH = contentRect.size.height;
    if (targetW <= 0 || targetH <= 0) return frameSize;

    NSSize currentContent = sender.contentView.bounds.size;
    CGFloat dw = std::fabs(targetW - currentContent.width);
    CGFloat dh = std::fabs(targetH - currentContent.height);

    // Snap to aspect by picking the dominant drag axis: whichever
    // dimension changed more from the current size drives the other.
    // This lets the user grow OR shrink by dragging any edge or
    // corner; the perpendicular dimension follows proportionally.
    if (dw >= dh) {
        targetH = targetW / self.aspectRatio;
    } else {
        targetW = targetH * self.aspectRatio;
    }

    // Convert back content → frame so AppKit gets a frame size.
    NSRect newContent = NSMakeRect(0, 0, targetW, targetH);
    NSRect newFrame   = [sender frameRectForContentRect:newContent];
    return newFrame.size;
}

@end

// configure_window_type and the child-view geometry helpers
// (child_view_frame_in_host, attach_child_view_to_host,
// set_child_view_bounds_in_host, detach_child_view_from_host) were
// extracted to window_host_mac_geometry.mm — see
// window_host_mac_internal.hpp. Reached here via the file-scope
// `using namespace pulp::view::mac_geometry` above.

// ── MacWindowHost (CoreGraphics) ─────────────────────────────────────────────

namespace pulp::view {

class MacWindowHost : public WindowHost {
public:
    MacWindowHost(View& root, const WindowOptions& options)
        : root_(root) {
        @autoreleasepool {
            root_.set_frame_clock(&frame_clock_);
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
            [window_ setReleasedWhenClosed:NO];

            // NSWindow's default backgroundColor is
            // [NSColor windowBackgroundColor] which is white in macOS
            // light-mode. AppKit composites this beneath the contentView
            // on dirty-rect repaints, even when the contentView is opaque.
            // Set the window backgroundColor to match PulpView's clear color
            // so any compositing race / partial-paint window shows dark, not
            // white. Belt-and-suspenders alongside PulpView isOpaque=YES.
            [window_ setBackgroundColor:[NSColor colorWithCalibratedRed:30.0/255.0
                                                                  green:30.0/255.0
                                                                   blue:46.0/255.0
                                                                  alpha:1.0]];

            [window_ setTitle:[NSString stringWithUTF8String:options.title.c_str()]];

            // Apply multi-window type configuration.
            configure_window_type(window_, options);

            if (options.min_width > 0 || options.min_height > 0)
                [window_ setContentMinSize:NSMakeSize(options.min_width, options.min_height)];

            options_initially_hidden_ = options.initially_hidden;

            view_ = [[PulpView alloc] initWithFrame:frame];
            view_.rootView = &root_;
            view_.frameClock = &frame_clock_;
            [window_ setContentView:view_];

            // The CPU host backs the floating inspector
            // window. Its PulpView tracking area carries NSTrackingMouseMoved,
            // but a tracking area only fans -mouseMoved: out to its owner when
            // the window itself accepts mouse-moved events; NSWindow defaults
            // that flag to NO. The main GPU canvas window happened to get moves
            // anyway (primary key window, continuous run-loop pump), so the
            // shared -mouseMoved: -> rootView->simulate_hover(pt) path that
            // drives View::on_hover_move() (and thus the ToolStrip's per-button
            // tooltip) never fired for the secondary inspector window. Opting
            // the window into mouse-moved delivery makes hover reach the strip
            // so the "Select (V)" / "Text (T)" tooltips paint live.
            [window_ setAcceptsMouseMovedEvents:YES];

            delegate_ = [[PulpWindowDelegate alloc] init];
            // Role drives the close policy (see
            // windowShouldClose:). Default primary; secondary windows (the
            // floating inspector) opt in via WindowOptions::secondary_window.
            delegate_.isSecondaryWindow = options.secondary_window ? YES : NO;
            delegate_.onResize = ^(float w, float h) {
                // Belt-and-suspenders relayout: PulpView's
                // setFrameSize: already pushes new bounds + relayouts when
                // AppKit resizes the content view, but if a host changed
                // the frame in some other path we still want the root view
                // synced before the user's callback fires and before the
                // next paint.
                root_.set_bounds({0, 0, w, h});
                root_.layout_children();
                [view_ setNeedsDisplay:YES];
                if (resize_callback_) {
                    resize_callback_(
                        static_cast<uint32_t>(std::max(0.0f, w)),
                        static_cast<uint32_t>(std::max(0.0f, h)));
                }
            };
            [window_ setDelegate:delegate_];
        }
    }

    ~MacWindowHost() override {
        if (idle_timer_) {
            [idle_timer_ invalidate];
            idle_timer_ = nil;
        }
        idle_callback_ = nullptr;

        // Cancel any queued deferred-click blocks before the
        // root View tree / WidgetBridge / ScriptEngine they captured can be
        // freed. This destructor runs synchronously, with no run-loop pump
        // between here and the caller's earlier-destroyed bridge/engine, so
        // cancelling now reliably catches every still-queued block.
        [view_ prepareForTeardown];
        // Fully detach the NSWindow when the host is destroyed (e.g. the Cmd+I
        // toggle's reset()). Without this the window LINGERED on screen with
        // its C++ root torn out (so it looked "cleared"), the unique_ptr went
        // null so the next Cmd+I stacked ANOTHER inspector, and the lingering
        // window's delegate still pointed at the freed close_callback_ — so
        // closing it invoked a dangling callback (UAF / pointer-auth crash).
        // Clear the callback + delegate FIRST so close can't re-enter
        // windowShouldClose:/onClose during teardown; releasedWhenClosed=NO so
        // our ARC strong ref controls the final dealloc.
        delegate_.onClose = nil;
        [window_ setDelegate:nil];
        [window_ setReleasedWhenClosed:NO];
        [window_ close];
        root_.set_window_host(nullptr);
        root_.set_frame_clock(nullptr);
    }

    void show() override { [window_ makeKeyAndOrderFront:nil]; }
    void hide() override { [window_ orderOut:nil]; }
    bool is_visible() const override { return [window_ isVisible]; }
    void repaint() override { [view_ setNeedsDisplay:YES]; }

    void position_beside(WindowHost* other) override {
        if (!other) return;
        auto* other_nswin = (__bridge NSWindow*)(other->native_window_handle());
        if (!other_nswin || !window_) return;

        auto other_frame = [other_nswin frame];
        auto screen_frame = [[other_nswin screen] visibleFrame];
        auto my_size = [window_ frame].size;

        // Align top of inspector with top of other window (macOS uses bottom-left origin)
        CGFloat target_y = other_frame.origin.y + other_frame.size.height - my_size.height;
        // Clamp vertically to screen bounds
        CGFloat screen_bottom = screen_frame.origin.y;
        CGFloat screen_top = screen_frame.origin.y + screen_frame.size.height;
        if (target_y + my_size.height > screen_top) target_y = screen_top - my_size.height;
        if (target_y < screen_bottom) target_y = screen_bottom;

        // Try right side first
        CGFloat right_x = other_frame.origin.x + other_frame.size.width + 8;
        if (right_x + my_size.width <= screen_frame.origin.x + screen_frame.size.width) {
            [window_ setFrameOrigin:NSMakePoint(right_x, target_y)];
        } else {
            // Fall back to left side
            CGFloat left_x = other_frame.origin.x - my_size.width - 8;
            [window_ setFrameOrigin:NSMakePoint(std::max(left_x, screen_frame.origin.x), target_y)];
        }
    }

    void* native_window_handle() const override { return (__bridge void*) window_; }
    void* native_content_view_handle() const override { return (__bridge void*) view_; }
    ContentSize get_content_size() const override {
        NSSize size = view_ ? view_.bounds.size : NSZeroSize;
        return {
            static_cast<uint32_t>(std::max<CGFloat>(0.0, size.width)),
            static_cast<uint32_t>(std::max<CGFloat>(0.0, size.height)),
        };
    }
    bool attach_native_child_view(void* child_view,
                                  float x,
                                  float y,
                                  float width,
                                  float height) override {
        return attach_child_view_to_host(view_, child_view, x, y, width, height);
    }
    bool set_native_child_view_bounds(void* child_view,
                                      float x,
                                      float y,
                                      float width,
                                      float height) override {
        return set_child_view_bounds_in_host(view_, child_view, x, y, width, height);
    }
    void detach_native_child_view(void* child_view) override {
        detach_child_view_from_host(view_, child_view);
    }
    bool set_native_child_view_clip(void* child_view,
                                    bool has_clip,
                                    float x,
                                    float y,
                                    float width,
                                    float height) override {
        return clip_child_view_in_host(view_, child_view, has_clip, x, y, width,
                                       height);
    }
    std::vector<uint8_t> capture_png() override {
        auto live = pulp::view::mac_capture::capture_window_screencapture_png(window_);
        return !live.empty() ? live : pulp::view::mac_capture::capture_window_content_png(window_, view_);
    }

    // Host-managed pixels only. CG-backed host has no GPU
    // back-buffer, so the deterministic surface is the rasterized content
    // view. Skips screencapture so hidden windows still produce bytes.
    std::vector<uint8_t> capture_back_buffer_png() override {
        return pulp::view::mac_capture::capture_window_content_png(window_, view_);
    }

    void invalidate_input_state() override { [view_ clearInteractionState]; }
    void request_close() override {
        if (options_initially_hidden_ && window_) {
            request_hidden_cocoa_window_close(window_);
            return;
        }
        request_app_close(window_);
    }

    void set_close_callback(std::function<void()> cb) override {
        close_callback_ = std::move(cb);
        delegate_.onClose = ^{ if (close_callback_) close_callback_(); };
    }

    void set_idle_callback(std::function<void()> cb) override {
        idle_callback_ = std::move(cb);
        if (idle_callback_ && !idle_timer_) {
            idle_timer_ = [NSTimer scheduledTimerWithTimeInterval:1.0/30.0
                repeats:YES block:^(NSTimer*) {
                    if (idle_callback_) idle_callback_();
                }];
        }
    }

    void set_resize_callback(ResizeCallback cb) override {
        resize_callback_ = std::move(cb);
    }

    void run_event_loop() override {
        @autoreleasepool {
            [NSApplication sharedApplication];
            auto dispatcher_alive = std::make_shared<std::atomic<bool>>(true);
            register_cocoa_dispatcher_liveness(dispatcher_alive);
            auto dispatcher_token =
                pulp::events::MainThreadDispatcher::register_backend(
                    make_cocoa_main_thread_backend(dispatcher_alive));
            // See header for initially_hidden.
            if (options_initially_hidden_) {
                [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            } else {
                [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
                install_app_menu([window_ title]);
                show();
                [NSApp activateIgnoringOtherApps:YES];
            }
            [NSApp run];
            dispatcher_alive->store(false, std::memory_order_release);
            pulp::events::MainThreadDispatcher::unregister_backend(dispatcher_token);
        }
    }

private:
    View& root_;
    FrameClock frame_clock_;
    NSWindow* window_ = nil;
    PulpView* view_ = nil;
    PulpWindowDelegate* delegate_ = nil;
    NSTimer* idle_timer_ = nil;
    std::function<void()> close_callback_;
    std::function<void()> idle_callback_;
    ResizeCallback resize_callback_;
    bool options_initially_hidden_ = false;
};

// ── MacGpuWindowHost (Dawn/Skia Graphite) ────────────────────────────────────

#ifdef PULP_HAS_SKIA

class MacGpuWindowHost : public WindowHost {
public:
    MacGpuWindowHost(View& root, const WindowOptions& options)
        : root_(root) {
        @autoreleasepool {
            root_.set_frame_clock(&frame_clock_);

            // arm the dirty tracker for the first frame and
            // optionally enable debug-printing of the per-frame union rect.
            tracker_.invalidate_all();
            tracker_.set_viewport(options.width, options.height);
            if (const char* env = std::getenv("PULP_PARTIAL_RENDERING_DEBUG")) {
                partial_rendering_debug_ = (env[0] == '1');
            }
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
            [window_ setReleasedWhenClosed:NO];
            [window_ setTitle:[NSString stringWithUTF8String:options.title.c_str()]];

            // Apply multi-window type configuration.
            configure_window_type(window_, options);

            if (options.min_width > 0 || options.min_height > 0)
                [window_ setContentMinSize:NSMakeSize(options.min_width, options.min_height)];

            options_initially_hidden_ = options.initially_hidden;

            // Create CAMetalLayer-backed view
            metal_view_ = [[PulpMetalView alloc] initWithFrame:frame];
            metal_view_.rootView = &root_;
            metal_view_.frameClock = &frame_clock_;
            metal_view_.repaintBlock = ^{
                needs_repaint_.store(true, std::memory_order_relaxed);
            };
            [window_ setContentView:metal_view_];

            delegate_ = [[PulpWindowDelegate alloc] init];
            // Role drives the close policy (see
            // windowShouldClose:). The GPU host is normally the primary main
            // canvas window; secondary windows opt in via
            // WindowOptions::secondary_window.
            delegate_.isSecondaryWindow = options.secondary_window ? YES : NO;
            delegate_.onResize = ^(float w, float h) {
                handle_resize(w, h);
            };
            [window_ setDelegate:delegate_];

            // Initialize GPU render stack
            init_gpu(options.width, options.height);
        }
    }

    ~MacGpuWindowHost() override {
        if (key_monitor_) { [NSEvent removeMonitor:key_monitor_]; key_monitor_ = nil; }
        app_key_handler_ = nullptr;
        if (render_dispatch_alive_)
            render_dispatch_alive_->store(false, std::memory_order_release);
        stop_display_link();
        render_dispatch_queued_.store(false, std::memory_order_release);

        // Detach the NSWindow + delegate FIRST, mirroring
        // ~MacWindowHost. The GPU host is the MAIN canvas window; if it is
        // destroyed while still visible (e.g. app teardown, host swap) AppKit
        // can otherwise keep the window alive holding a delegate whose onClose
        // block closes over this now-freed host (`close_callback_`), so a later
        // close: invokes a dangling callback (UAF / pointer-auth crash). Clear
        // the callback + delegate before close so windowShouldClose:/onClose
        // can't re-enter during teardown; releasedWhenClosed=NO so our ARC
        // strong ref controls the final dealloc.
        delegate_.onClose = nil;
        delegate_.onResize = nil;
        [window_ setDelegate:nil];
        [window_ setReleasedWhenClosed:NO];
        [window_ close];

        skia_surface_.reset();
        gpu_surface_.reset();
        // Cancel any queued deferred-click blocks before the
        // captured View tree / WidgetBridge / ScriptEngine can be freed.
        // PulpMetalView inherits -prepareForTeardown from PulpView.
        [metal_view_ prepareForTeardown];
        metal_view_.repaintBlock = nil;
        metal_view_.pointTransform = nil;
        metal_view_.rootView = nullptr;
        root_.set_window_host(nullptr);
        root_.set_frame_clock(nullptr);
    }

    void show() override {
        // Render one frame BEFORE ordering the window on screen so it appears
        // already showing content — no black flash in the gap between
        // makeKeyAndOrderFront and the first display-link tick. The GPU/Skia
        // surfaces are created in the constructor and the root view is laid out,
        // so the CAMetalLayer can acquire and present an off-screen drawable
        // here. If begin_frame can't get a drawable off-screen, render_frame()
        // no-ops (logged) and we just order front as before — no worse than the
        // old behavior. Skip for an initially-hidden window (nothing to show).
        if (!options_initially_hidden_ && gpu_surface_ && skia_surface_) {
            needs_repaint_.store(true, std::memory_order_relaxed);
            tracker_.invalidate_all();
            render_frame();
        }
        [window_ makeKeyAndOrderFront:nil];
        // A SECONDARY GPU window is created via WindowHost::create and never
        // enters run_event_loop() (only the primary window does), so it would
        // otherwise have NO CVDisplayLink. Without one, needs_repaint_ set by
        // repaint(), a DesignFrameView frame swap (request_repaint), or
        // set_active_notes is never consumed — the surface only redraws on the
        // one show() frame + direct input, so a piano⇄typing toggle resizes the
        // window but never repaints the new frame. Start a per-window display
        // link here, guarded so the primary (which already started its link in
        // run_event_loop()) is untouched.
        if (!display_link_) start_display_link();
        // Make the Metal view the first responder so the window receives key
        // events on its FIRST show. Only run_event_loop() (the PRIMARY window)
        // did this; a SECONDARY window (e.g. the MusicalTypingKeyboard popout)
        // showed via show() and never became first responder, so typed keys were
        // dropped until some later interaction (a click/toggle) focused it — the
        // "first ⌘K open doesn't register QWERTY until a piano/typing toggle" bug.
        // makeFirstResponder is idempotent, so re-showing is harmless.
        [window_ makeFirstResponder:metal_view_];
    }
    void hide() override { [window_ orderOut:nil]; }
    bool is_visible() const override { return [window_ isVisible]; }

    void position_beside(WindowHost* other) override {
        if (!other) return;
        auto* other_nswin = (__bridge NSWindow*)(other->native_window_handle());
        if (!other_nswin || !window_) return;
        auto other_frame = [other_nswin frame];
        auto screen_frame = [[other_nswin screen] visibleFrame];
        auto my_size = [window_ frame].size;

        // Align top of inspector with top of other window (macOS uses bottom-left origin)
        CGFloat target_y = other_frame.origin.y + other_frame.size.height - my_size.height;
        // Clamp vertically to screen bounds
        CGFloat screen_bottom = screen_frame.origin.y;
        CGFloat screen_top = screen_frame.origin.y + screen_frame.size.height;
        if (target_y + my_size.height > screen_top) target_y = screen_top - my_size.height;
        if (target_y < screen_bottom) target_y = screen_bottom;

        CGFloat right_x = other_frame.origin.x + other_frame.size.width + 8;
        if (right_x + my_size.width <= screen_frame.origin.x + screen_frame.size.width) {
            [window_ setFrameOrigin:NSMakePoint(right_x, target_y)];
        } else {
            CGFloat left_x = other_frame.origin.x - my_size.width - 8;
            [window_ setFrameOrigin:NSMakePoint(std::max(left_x, screen_frame.origin.x), target_y)];
        }
    }

    void* native_window_handle() const override { return (__bridge void*) window_; }
    void* native_content_view_handle() const override { return (__bridge void*) metal_view_; }
    ContentSize get_content_size() const override {
        return {
            static_cast<uint32_t>(std::max(0.0f, width_)),
            static_cast<uint32_t>(std::max(0.0f, height_)),
        };
    }
    void* dawn_device_handle() const override {
        return gpu_surface_ ? gpu_surface_->dawn_device_handle() : nullptr;
    }
    void* dawn_queue_handle() const override {
        return gpu_surface_ ? gpu_surface_->dawn_queue_handle() : nullptr;
    }
    void* dawn_instance_handle() const override {
        return gpu_surface_ ? gpu_surface_->dawn_instance_handle() : nullptr;
    }
    render::GpuSurface* gpu_surface() const override { return gpu_surface_.get(); }

    // Whole-recording GPU render time for the last presented frame, forwarded
    // from the owning SkiaSurface (Graphite GpuStats elapsed time). Guards null
    // so a host whose surface failed to create reports the honest no-op.
    double gpu_render_time_ms() const override {
        return skia_surface_ ? skia_surface_->gpu_render_time_ms() : 0.0;
    }
    bool gpu_render_timing_available() const override {
        return skia_surface_ ? skia_surface_->gpu_render_timing_available() : false;
    }

    bool attach_native_child_view(void* child_view,
                                  float x,
                                  float y,
                                  float width,
                                  float height) override {
        return attach_child_view_to_host(metal_view_, child_view, x, y, width, height);
    }
    bool set_native_child_view_bounds(void* child_view,
                                      float x,
                                      float y,
                                      float width,
                                      float height) override {
        return set_child_view_bounds_in_host(metal_view_, child_view, x, y, width, height);
    }
    void detach_native_child_view(void* child_view) override {
        detach_child_view_from_host(metal_view_, child_view);
    }
    bool set_native_child_view_clip(void* child_view,
                                    bool has_clip,
                                    float x,
                                    float y,
                                    float width,
                                    float height) override {
        return clip_child_view_in_host(metal_view_, child_view, has_clip, x, y,
                                       width, height);
    }

    void repaint() override {
        needs_repaint_ = true;
        // Until widgets pass per-rect bounds through request_repaint(),
        // every host-level repaint() invalidates the whole viewport.
        tracker_.invalidate_all();
        ++request_repaint_dirty_frames_;
    }

    std::vector<uint8_t> capture_png() override {
        if (gpu_surface_ && skia_surface_) {
            needs_repaint_.store(true, std::memory_order_relaxed);
            render_frame();

            auto live = pulp::view::mac_capture::capture_window_screencapture_png(window_);
            if (!live.empty()) return live;

            auto content = pulp::view::mac_capture::capture_window_content_png(window_, metal_view_);
            if (!content.empty()) return content;

            std::vector<uint8_t> pixels;
            uint32_t pixel_w = 0;
            uint32_t pixel_h = 0;
            if (render_frame(&pixels, &pixel_w, &pixel_h)) {
                auto encoded = pulp::view::mac_capture::encode_rgba_to_png(pixels.data(),
                                                  pixel_w,
                                                  pixel_h,
                                                  static_cast<size_t>(pixel_w) * 4u);
                if (!encoded.empty()) return encoded;
            }
        }

        auto live = pulp::view::mac_capture::capture_window_screencapture_png(window_);
        if (!live.empty()) return live;

        return pulp::view::mac_capture::capture_window_content_png(window_, metal_view_);
    }

    // Deterministic GPU back-buffer readback for hidden /
    // headless test windows. Bypasses screencapture (which fails on hidden
    // windows) and the content-view cache, going straight through the
    // existing render_frame(...) path that already powers the
    // capture_png() fallback. Returns {} on failure (no exceptions).
    std::vector<uint8_t> capture_back_buffer_png() override {
        if (!gpu_surface_ || !skia_surface_) return {};

        needs_repaint_.store(true, std::memory_order_relaxed);

        std::vector<uint8_t> pixels;
        uint32_t pixel_w = 0;
        uint32_t pixel_h = 0;
        if (!render_frame(&pixels, &pixel_w, &pixel_h)) return {};

        return pulp::view::mac_capture::encode_rgba_to_png(pixels.data(),
                                  pixel_w,
                                  pixel_h,
                                  static_cast<size_t>(pixel_w) * 4u);
    }

    void invalidate_input_state() override {
        [metal_view_ clearInteractionState];
    }

    void request_close() override {
        if (options_initially_hidden_ && window_) {
            request_hidden_cocoa_window_close(window_);
            return;
        }
        request_app_close(window_);
    }

    void set_close_callback(std::function<void()> cb) override {
        close_callback_ = std::move(cb);
        delegate_.onClose = ^{ if (close_callback_) close_callback_(); };
    }

    // App-level key monitor: route keyDown/keyUp to `handler` before the responder
    // chain, regardless of which window is key (so a popout keyboard keeps getting
    // keystrokes after the user clicks a control in another window). Returning true
    // from the handler CONSUMES the event (return nil), so it doesn't ALSO reach the
    // focused window's keyDown — no double-trigger.
    void set_app_key_monitor(std::function<bool(const KeyEvent&)> handler) override {
        if (key_monitor_) { [NSEvent removeMonitor:key_monitor_]; key_monitor_ = nil; }
        app_key_handler_ = std::move(handler);
        if (!app_key_handler_) return;
        key_monitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:
                            (NSEventMaskKeyDown | NSEventMaskKeyUp)
                        handler:^NSEvent*(NSEvent* event) {
            if (!app_key_handler_) return event;
            pulp::view::KeyEvent ke;
            ke.key = key_code_from_ns(event.keyCode);
            ke.modifiers = modifiers_from_ns_flags(event.modifierFlags);
            ke.is_down = (event.type == NSEventTypeKeyDown);
            ke.is_repeat = (event.type == NSEventTypeKeyDown) ? event.isARepeat : NO;
            return app_key_handler_(ke) ? nil : event;
        }];
    }

    void set_resize_callback(ResizeCallback cb) override {
        resize_callback_ = std::move(cb);
    }

    // Wire the host's idle callback into the
    // CVDisplayLink dispatch so JS rAF / setTimeout / async-result
    // queues are pumped each vsync. Without this override the GPU
    // host inherited the WindowHost base no-op, which dropped
    // standalone's `scripted_ui->poll()` on the floor: every
    // requestAnimationFrame() call would queue a callback, set
    // needs_repaint_=true via request_repaint, render one frame —
    // and then never fire the JS callback because nothing called
    // poll_async_results to drain pending_frame_ids_. The CPU host
    // has set_idle_callback wired to a 30Hz NSTimer (line ~1429);
    // GPU is now wired vsync-aligned via the display link instead
    // of a separate timer to keep pump cadence locked to the same
    // frame the renderer is about to emit.
    void set_idle_callback(std::function<void()> cb) override {
        idle_callback_ = std::move(cb);
        has_idle_callback_.store(static_cast<bool>(idle_callback_),
                                  std::memory_order_release);
    }

    void run_event_loop() override {
        @autoreleasepool {
            [NSApplication sharedApplication];
            auto dispatcher_alive = std::make_shared<std::atomic<bool>>(true);
            register_cocoa_dispatcher_liveness(dispatcher_alive);
            auto dispatcher_token =
                pulp::events::MainThreadDispatcher::register_backend(
                    make_cocoa_main_thread_backend(dispatcher_alive));
            // When initially_hidden is set,
            // skip Dock icon, focus stealing, and the show() call. Window
            // is created and the run loop drives the bridge per-vsync as
            // usual; just don't put glass on the user's screen. Used by
            // live-host smoke tests that need to exercise the per-vsync
            // pump without flashing a window during CI / local validation.
            if (options_initially_hidden_) {
                [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            } else {
                [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
                install_app_menu([window_ title]);
            }

            // Start display-linked render loop
            start_display_link();

            if (!options_initially_hidden_) {
                show();
                [window_ makeFirstResponder:metal_view_];
                [NSApp activateIgnoringOtherApps:YES];
            }
            [NSApp run];
            dispatcher_alive->store(false, std::memory_order_release);
            pulp::events::MainThreadDispatcher::unregister_backend(dispatcher_token);
        }
    }

    // ── Platform feature overrides ──────────────────────────────────────

    void set_mouse_relative_mode(bool enabled) override {
        [metal_view_ setRelativeMouseMode:enabled ? YES : NO];
        if (enabled) {
            CGAssociateMouseAndMouseCursorPosition(false);
            if (!s_cursor_hidden) {
                [NSCursor hide];
                s_cursor_hidden = true;
            }
        } else {
            CGAssociateMouseAndMouseCursorPosition(true);
            if (s_cursor_hidden) {
                [NSCursor unhide];
                s_cursor_hidden = false;
            }
        }
    }

    float dpi_scale() const override {
        if (window_) return static_cast<float>([window_ backingScaleFactor]);
        return 1.0f;
    }

    Size max_dimensions() const override {
        NSScreen* screen = [NSScreen mainScreen];
        NSRect frame = [screen visibleFrame];
        return {static_cast<float>(frame.size.width), static_cast<float>(frame.size.height)};
    }

    void set_always_on_top(bool on_top) override {
        if (window_)
            [window_ setLevel:on_top ? NSFloatingWindowLevel : NSNormalWindowLevel];
    }

    void set_fixed_aspect_ratio(float ratio) override {
        {
            FILE* f = fopen("/tmp/pulp-aspect.log", "a");
            if (f) {
                fprintf(f, "GPU::set_fixed_aspect_ratio(%.3f) win=%p delegate=%p wDeleg=%p\n",
                        ratio, (void*)window_, (void*)delegate_,
                        window_ ? (void*)[window_ delegate] : nullptr);
                fclose(f);
            }
        }
        if (!window_) return;
        if (ratio > 0) {
            [window_ setContentAspectRatio:NSMakeSize(ratio, 1.0)];
            if (delegate_) delegate_.aspectRatio = ratio;
        } else {
            // Clear: reset to "no aspect lock" by setting resize
            // increments equal to (0, 0) — macOS treats that as free.
            [window_ setResizeIncrements:NSMakeSize(1, 1)];
            if (delegate_) delegate_.aspectRatio = 0;
        }
        {
            FILE* f = fopen("/tmp/pulp-aspect.log", "a");
            if (f) {
                fprintf(f, "GPU::set_fixed_aspect_ratio AFTER: delegate.aspectRatio=%.3f\n",
                        delegate_ ? (double)delegate_.aspectRatio : -1.0);
                fclose(f);
            }
        }
    }

    void request_content_size(float w, float h) override {
        if (!window_ || w <= 0.0f || h <= 0.0f) return;
        // Keep the TOP-LEFT corner fixed while the size changes (the toolbar +
        // 🎹/⌨ toggles must not jump). NSWindow's origin is bottom-left, so
        // capture the current top edge, resize the content, then re-anchor the
        // origin so that top edge stays put — the window grows/shrinks downward.
        NSRect frame = [window_ frame];
        const CGFloat top = frame.origin.y + frame.size.height;
        [window_ setContentSize:NSMakeSize(w, h)];
        NSRect resized = [window_ frame];
        [window_ setFrameOrigin:NSMakePoint(resized.origin.x, top - resized.size.height)];
    }

    void set_design_viewport(float design_w, float design_h) override {
        design_viewport_w_ = design_w;
        design_viewport_h_ = design_h;
        if (metal_view_) {
            if (design_w > 0.0f && design_h > 0.0f) {
                __block MacGpuWindowHost* host = this;
                metal_view_.pointTransform = ^pulp::view::Point(pulp::view::Point pt) {
                    return host->map_window_to_root(pt);
                };
            } else {
                metal_view_.pointTransform = nil;
            }
        }
        needs_repaint_ = true;
    }

    // Compute the active design->window transform. Delegates to the
    // header-only free function so unit tests can hit the same math
    // without instantiating an NSWindow.
    bool design_transform(float& sx, float& sy, float& tx, float& ty) const {
        return WindowHost::compute_design_viewport_transform(
            width_, height_, design_viewport_w_, design_viewport_h_,
            sx, sy, tx, ty);
    }

    // Map a window-space point (e.g. mouse) into root view coordinates.
    Point map_window_to_root(Point pt) const {
        float sx, sy, tx, ty;
        if (!design_transform(sx, sy, tx, ty)) return pt;
        if (sx <= 0.0f || sy <= 0.0f) return pt;
        return { (pt.x - tx) / sx, (pt.y - ty) / sy };
    }

    void set_client_decoration(bool enabled) override {
        if (!window_) return;
        if (enabled) {
            [window_ setTitlebarAppearsTransparent:YES];
            [window_ setTitleVisibility:NSWindowTitleHidden];
            [window_ setStyleMask:[window_ styleMask] | NSWindowStyleMaskFullSizeContentView];
        } else {
            [window_ setTitlebarAppearsTransparent:NO];
            [window_ setTitleVisibility:NSWindowTitleVisible];
            [window_ setStyleMask:[window_ styleMask] & ~NSWindowStyleMaskFullSizeContentView];
        }
    }

    std::vector<MonitorInfo> get_monitors() const override {
        std::vector<MonitorInfo> monitors;
        for (NSScreen* screen in [NSScreen screens]) {
            NSRect frame = [screen frame];
            MonitorInfo info;
            info.bounds = {static_cast<float>(frame.origin.x),
                           static_cast<float>(frame.origin.y),
                           static_cast<float>(frame.size.width),
                           static_cast<float>(frame.size.height)};
            info.dpi_scale = static_cast<float>([screen backingScaleFactor]);
            info.name = std::string([[screen localizedName] UTF8String]);
            monitors.push_back(info);
        }
        return monitors;
    }

private:
    View& root_;
    FrameClock frame_clock_;
    NSWindow* window_ = nil;
    PulpMetalView* metal_view_ = nil;
    PulpWindowDelegate* delegate_ = nil;
    std::function<void()> close_callback_;
    id key_monitor_ = nil;                                       // NSEvent app key monitor
    std::function<bool(const pulp::view::KeyEvent&)> app_key_handler_;
    bool options_initially_hidden_ = false;

    std::unique_ptr<render::GpuSurface> gpu_surface_;
    std::unique_ptr<render::SkiaSurface> skia_surface_;
    CVDisplayLinkRef display_link_ = nullptr;
    std::atomic<bool> needs_repaint_{true};
    std::atomic<bool> continuous_frames_{false};
    std::atomic<bool> render_dispatch_queued_{false};
    std::shared_ptr<std::atomic<bool>> render_dispatch_alive_ =
        std::make_shared<std::atomic<bool>>(true);
    // Tracks per-frame invalidations
    // pushed by repaint() and the animation / FrameClock pump. The
    // render gate still uses the existing needs_repaint_/animation
    // flags so observable behaviour does not change yet:
    // the tracker is plumbed-but-non-authoritative. Debug-printed once
    // per painted frame when PULP_PARTIAL_RENDERING_DEBUG=1 in the
    // environment so we can audit "the dirty rect we would have
    // clipped to" against the unconditional repaint that still ships.
    render::DirtyTracker tracker_;
    bool partial_rendering_debug_ = false;
    uint64_t pump_dirty_frames_ = 0;
    uint64_t request_repaint_dirty_frames_ = 0;
    int frame_fail_count_ = 0;
    int frame_ok_count_ = 0;
    float width_ = 0, height_ = 0;
    // ── Design viewport (see WindowHost::set_design_viewport) ──────────
    // When set (> 0), root_ is laid out at design size and paint applies
    // an aspect-correct scale + letterbox translate to fit the window.
    // Mouse coords are inverse-mapped before hit_test (via the metal
    // view's pointTransform block). Used by content authored at a fixed
    // size (e.g. native-react renderers from Figma / Stitch / v0 /
    // Pencil) that needs proportional resize without re-layout.
    float design_viewport_w_ = 0.0f;
    float design_viewport_h_ = 0.0f;
    ResizeCallback resize_callback_;
    // Idle callback wiring. CVDisplayLink reads
    // has_idle_callback_ on the display thread; idle_callback_ is
    // invoked on main only.
    std::function<void()> idle_callback_;
    std::atomic<bool> has_idle_callback_{false};

    // The continuous-frame predicate is the shared
    // pulp::view::needs_continuous_frames() (continuous_frames.hpp).
    static void advance_widget_animations(View* view, float dt) {
        if (!view) return;
        if (auto* k = dynamic_cast<Knob*>(view)) k->advance_animations(dt);
        else if (auto* t = dynamic_cast<Toggle*>(view)) t->advance_animations(dt);
        else if (auto* f = dynamic_cast<Fader*>(view)) f->advance_animations(dt);
        else if (auto* sv = dynamic_cast<ScrollView*>(view)) sv->advance_animations(dt);
        else if (auto* tip = dynamic_cast<Tooltip*>(view)) tip->advance_animations(dt);

        // Also drive CSS animations on every View in the
        // tree. tick_animations honors animation_play_state_ ("paused"
        // → no advance) and is a no-op for Views with no active CSS
        // animations, so the recursive walk is cheap.
        view->tick_animations(dt);

        for (size_t i = 0; i < view->child_count(); ++i)
            advance_widget_animations(view->child_at(i), dt);
    }

    void init_gpu(float width, float height) {
        width_ = width;
        height_ = height;

        gpu_surface_ = render::GpuSurface::create_dawn();
        if (!gpu_surface_) return;

        // Configure GpuSurface at PHYSICAL pixel dimensions to match
        // the CAMetalLayer's drawableSize (which is logical * scale)
        CGFloat scale = metal_view_.metalLayer.contentsScale;
        uint32_t phys_w = static_cast<uint32_t>(width * scale);
        uint32_t phys_h = static_cast<uint32_t>(height * scale);

        render::GpuSurface::Config gpu_config{};
        gpu_config.width = phys_w;
        gpu_config.height = phys_h;
        gpu_config.native_surface_handle = (__bridge void*)metal_view_.metalLayer;

        if (!gpu_surface_->initialize(gpu_config)) {
            gpu_surface_.reset();
            return;
        }

        // SkiaSurface uses logical dimensions + scale factor.
        // The scale transform maps logical coordinates (800x550) to
        // physical pixels (1600x1100) in the GPU texture.
        render::SkiaSurface::Config skia_config{};
        skia_config.width = static_cast<uint32_t>(width);
        skia_config.height = static_cast<uint32_t>(height);
        skia_config.scale_factor = static_cast<float>(scale);

        skia_surface_ = render::SkiaSurface::create(*gpu_surface_, skia_config);
    }

    void handle_resize(float width, float height) {
        width_ = width;
        height_ = height;

        CGFloat scale = metal_view_.metalLayer.contentsScale;
        uint32_t phys_w = static_cast<uint32_t>(width * scale);
        uint32_t phys_h = static_cast<uint32_t>(height * scale);

        if (gpu_surface_) {
            gpu_surface_->resize(phys_w, phys_h);
        }
        if (skia_surface_) {
            skia_surface_->resize(static_cast<uint32_t>(width),
                                   static_cast<uint32_t>(height),
                                   static_cast<float>(scale));
        }
        // Relayout the root view synchronously so hit testing
        // and any user resize callback both see the new geometry. paint_scene
        // also calls set_bounds + layout_children, but that runs at the next
        // vsync — too late for input handlers fired during the resize drag.
        root_.set_bounds({0, 0, width, height});
        root_.layout_children();
        if (resize_callback_) {
            auto alive_token = render_dispatch_alive_;
            resize_callback_(
                static_cast<uint32_t>(std::max(0.0f, width)),
                static_cast<uint32_t>(std::max(0.0f, height)));
            if (!alive_token || !alive_token->load(std::memory_order_acquire))
                return;
        }
        needs_repaint_ = true;
        tracker_.set_viewport(width, height);
        tracker_.invalidate_all();
    }

    void paint_scene(canvas::Canvas& canvas) {
        // When a design viewport is set, the root
        // is pinned at design size and paint applies an aspect-correct
        // scale + letterbox translate to fit the current window. The
        // letterbox fill covers the entire window first so the
        // letterbox bars (visible only when the OS aspect-lock briefly
        // diverges during user drag) are the same color as the design
        // background. Overlays paint in window space (outside the
        // transform) so dropdowns/inspector hit-test against the
        // un-transformed window coords.
        float sx, sy, tx, ty;
        const bool has_viewport = design_transform(sx, sy, tx, ty);

        if (has_viewport) {
            root_.set_bounds({0, 0, design_viewport_w_, design_viewport_h_});
        } else {
            root_.set_bounds({0, 0, width_, height_});
        }
        root_.layout_children();  // emits the "layout" span

        // Paint pass: background fill + view-tree paint into the canvas. Runs
        // after layout, before the GPU submit/present in render_frame.
        PULP_TRACE_SCOPE_NAMED("canvas", "paint");
        canvas.set_fill_color(canvas::Color::rgba8(30, 30, 46));
        canvas.fill_rect(0, 0, width_, height_);

        if (has_viewport) {
            // paint_overlays MUST run inside the
            // design-viewport transform. View::OverlayRequest callbacks
            // are documented to draw in root coordinates (ComboBox
            // dropdowns, claimed overlays, inspector layer). The mouse
            // input path inverse-maps window→root via pointTransform
            // before hit_test, so overlay click routing assumes overlays
            // are positioned in root space. Painting them outside the
            // transform would put them at root-coords-but-in-window-space
            // — visually misaligned and non-clickable in any window size
            // that's not exactly design size.
            const int saved = canvas.save_count();
            canvas.save();
            canvas.translate(tx, ty);
            canvas.scale(sx, sy);
            root_.paint_all(canvas);
            pulp::view::View::paint_overlays(canvas, &root_);
            canvas.restore_to_count(saved);
        } else {
            root_.paint_all(canvas);
            pulp::view::View::paint_overlays(canvas, &root_);
        }

        // Inspector overlay is painted automatically via View::paint_overlays()
    }

    bool render_frame(std::vector<uint8_t>* capture_pixels = nullptr,
                      uint32_t* capture_width = nullptr,
                      uint32_t* capture_height = nullptr) {
        // Top-level frame span for the live host frame handler: one per rendered
        // frame, tagged with the FrameClock's monotonic frame index. This is the
        // real per-frame drive point (layout → paint → submit → present below).
        PULP_TRACE_SCOPE_NAMED_ARGS("render", "frame", "frame_index",
                                    frame_clock_.frame());

        if (!gpu_surface_ || !skia_surface_) return false;

        // Emit the per-frame dirty-rect decision the host would use for
        // clipping. The actual paint path is unchanged; this is a wiring
        // trace only. Gated on env so production CI logs stay quiet.
        if (partial_rendering_debug_ && tracker_.is_dirty()) {
            auto b = tracker_.bounds();
            fprintf(stderr,
                    "[partial-render] frame=%llu full=%d rects=%zu union=(%.0f,%.0f,%.0fx%.0f) "
                    "(req_repaint=%llu pump=%llu)\n",
                    (unsigned long long)tracker_.frame_count(),
                    tracker_.needs_full_repaint() ? 1 : 0,
                    tracker_.dirty_rects().size(),
                    b.x, b.y, b.w, b.h,
                    (unsigned long long)request_repaint_dirty_frames_,
                    (unsigned long long)pump_dirty_frames_);
        }

        if (!gpu_surface_->begin_frame()) {
            frame_fail_count_++;
            if (frame_fail_count_ <= 3)
                fprintf(stderr, "[gpu-host] begin_frame failed (%d)\n", frame_fail_count_);
            return false;
        }

        auto* canvas = skia_surface_->begin_frame();
        if (!canvas) {
            fprintf(stderr, "[gpu-host] skia begin_frame returned null\n");
            gpu_surface_->end_frame();
            return false;
        }

        if (frame_ok_count_++ == 0) {
            CGFloat scale = metal_view_.metalLayer.contentsScale;
            fprintf(stderr, "[gpu-host] first frame: logical=%.0fx%.0f gpu=%ux%u scale=%.1f\n",
                width_, height_, gpu_surface_->width(), gpu_surface_->height(), scale);
        }

        paint_scene(*canvas);

        continuous_frames_.store(
            pulp::view::needs_continuous_frames(&root_) || frame_clock_.has_active_subscribers(),
            std::memory_order_relaxed);

        skia_surface_->end_frame();   // submit Graphite recording

        bool captured = true;
        if (capture_pixels && capture_width && capture_height) {
            captured = skia_surface_->read_current_rgba(*capture_pixels, *capture_width, *capture_height);
        }

        gpu_surface_->end_frame();    // present to Metal surface

        needs_repaint_.store(continuous_frames_.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        // Clear the tracker AFTER present so the next frame starts clean.
        // The current render gate still uses needs_repaint_ / animation state.
        tracker_.clear();
        return captured;
    }

    // CVDisplayLink callback — fires on the display's vsync
    static CVReturn display_link_callback(
        CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*,
        CVOptionFlags, CVOptionFlags*, void* context)
    {
        auto* self = static_cast<MacGpuWindowHost*>(context);
        // Guard now also fires when an idle
        // callback is installed, so JS rAF / setTimeout / async-result
        // queues get a vsync-paced pump even when no native widget is
        // animating and no prior request_repaint set needs_repaint_.
        // The idle pump (`scripted_ui->poll()` → `bridge.poll_async_results()`)
        // drains pending_frame_ids_ via __flushFrames__ and re-arms
        // needs_repaint_ for any frame that requested another paint.
        const bool has_idle = self->has_idle_callback_.load(std::memory_order_acquire);
        if (self->needs_repaint_.load(std::memory_order_relaxed) ||
            self->continuous_frames_.load(std::memory_order_relaxed) ||
            has_idle) {
            bool expected = false;
            if (!self->render_dispatch_queued_.compare_exchange_strong(expected, true,
                                                                       std::memory_order_acq_rel,
                                                                       std::memory_order_relaxed)) {
                return kCVReturnSuccess;
            }
            auto alive_token = self->render_dispatch_alive_;
            // Dispatch rendering to the main thread (required for Cocoa + Metal)
            dispatch_async(dispatch_get_main_queue(), ^{
                @autoreleasepool {
                    if (!alive_token || !alive_token->load(std::memory_order_acquire))
                        return;

                    // Pump idle FIRST so JS rAF callbacks fire (and any
                    // request_repaint they trigger arms needs_repaint_)
                    // before we evaluate whether to render this frame.
                    if (self->idle_callback_) {
                        self->idle_callback_();
                        if (!alive_token || !alive_token->load(std::memory_order_acquire))
                            return;
                    }

                    bool animate = pulp::view::needs_continuous_frames(&self->root_);
                    bool tick_subscribers = self->frame_clock_.has_active_subscribers();
                    if (!self->needs_repaint_.load(std::memory_order_relaxed) && !animate && !tick_subscribers) {
                        self->continuous_frames_.store(false, std::memory_order_relaxed);
                        self->render_dispatch_queued_.store(false, std::memory_order_release);
                        return;
                    }
                    self->frame_clock_.tick(1.0f / 60.0f);
                    self->root_.advance_gesture_recognizers();
                    advance_widget_animations(&self->root_, 1.0f / 60.0f);
                    if (animate || tick_subscribers) {
                        self->needs_repaint_.store(true, std::memory_order_relaxed);
                        // Note: animation pump: animation /
                        // frame-clock pumps mutate view state without
                        // going through request_repaint(), so for the
                        // tracker they count as a full-surface dirty
                        // until animating widget paths are audited and
                        // converted to per-rect repaints.
                        self->tracker_.invalidate_all();
                        ++self->pump_dirty_frames_;
                    }
                    self->render_frame();
                    self->render_dispatch_queued_.store(false, std::memory_order_release);
                }
            });
        }
        return kCVReturnSuccess;
    }

    void start_display_link() {
        CVDisplayLinkCreateWithActiveCGDisplays(&display_link_);
        CVDisplayLinkSetOutputCallback(display_link_, display_link_callback, this);

        // Match the display the window is on
        CGDirectDisplayID display_id =
            (CGDirectDisplayID)[[window_.screen deviceDescription][@"NSScreenNumber"] unsignedIntValue];
        CVDisplayLinkSetCurrentCGDisplay(display_link_, display_id);
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

#endif // PULP_HAS_SKIA

// ── Factory ──────────────────────────────────────────────────────────────────

std::unique_ptr<WindowHost> WindowHost::create(View& root, const WindowOptions& options) {
#ifdef PULP_HAS_SKIA
    if (options.use_gpu) {
        auto host = std::make_unique<MacGpuWindowHost>(root, options);
        root.set_window_host(host.get());
        return host;
    }
#endif
    auto host = std::make_unique<MacWindowHost>(root, options);
    root.set_window_host(host.get());
    return host;
}

} // namespace pulp::view

#endif // TARGET_OS_OSX
