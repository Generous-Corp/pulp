#include <pulp/view/window_host.hpp>
#include <pulp/view/window_manager.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/modal.hpp>

#include "window_host_mac_capture.h"
#include "window_host_mac_internal.hpp"
#include <pulp/view/widget_bridge.hpp>

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/canvas/cg_canvas.hpp>
#import <Cocoa/Cocoa.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>   // pulp #2502 — shared_ptr liveness token for deferred clicks

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

// ── App menu helper ──────────────────────────────────────────────────────────

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
    NSWindow* target = [NSApp keyWindow];
    if (target == nil) target = [NSApp mainWindow];
    if (target != nil) {
        [target performClose:sender];
    } else {
        [NSApp stop:nil];
    }
}

@end

// request_app_close, find_topmost_modal, configure_window_type, the
// child-view geometry helpers, and the coordinate / event-translation
// helpers (modifiers_from_ns_flags, to_local, view_is_in_tree,
// key_code_from_ns) were extracted to window_host_mac_geometry.mm in
// the R2-5 refactor — see window_host_mac_internal.hpp. Used here via
// the `pulp::view::mac_geometry` namespace.
using namespace pulp::view::mac_geometry;

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

@interface PulpView : NSView <NSTextInputClient>
@property (nonatomic, assign) pulp::view::View* rootView;
@property (nonatomic, assign) pulp::view::FrameClock* frameClock;
@property (nonatomic, strong) NSTimer* animationTimer;
@property (nonatomic, strong) NSTrackingArea* trackingArea;
// Inverse design-viewport transform applied to every window-space input
// point before hit_test. Set by WindowHost::set_design_viewport; nil
// when no design viewport is in effect (identity).
@property (nonatomic, copy) pulp::view::Point (^pointTransform)(pulp::view::Point);
// pulp #2502 — the host destructor MUST call this before the View tree /
// WidgetBridge / ScriptEngine the deferred-click blocks were built from can
// be freed. It invalidates every still-queued `mouseUp:` deferred-click block
// so none of them can run a `std::function` (`on_click` / `on_global_click`)
// whose closure references freed bridge/engine state.
- (void)prepareForTeardown;
@end

@implementation PulpView {
    pulp::view::View* _dragTarget;
    pulp::view::View* _focusedView;
    // pulp #2502 — liveness token for `mouseUp:`'s deferred-click blocks.
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
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        // pulp #1321: ensure the content view tracks the window's content rect
        // so AppKit resizes our frame when the user drags the window edge.
        // Without this, the view's bounds stay at the original frame and Yoga
        // never reflows on resize.
        self.autoresizesSubviews = YES;
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _deferredClickAlive = std::make_shared<std::atomic<bool>>(true);
    }
    return self;
}

// pulp #2502 — invalidate every queued deferred-click block. After this
// returns, any deferred-click block that later drains is a guaranteed no-op,
// so it is safe for the owning View tree / WidgetBridge / ScriptEngine to be
// destroyed. Invoked from the host destructors (`~MacWindowHost`,
// `~MacGpuWindowHost`) which run synchronously in the same teardown step —
// between a test's bridge going out of scope and the next run-loop pump — so
// flipping the token here reliably defuses every block before it can fault.
- (void)prepareForTeardown {
    self.rootView = nullptr;
    _dragTarget = nullptr;
    if (_deferredClickAlive)
        _deferredClickAlive->store(false);
}

- (BOOL)isFlipped { return NO; }
// pulp #1382 — drawRect fills the entire bounds with rgba8(30,30,46)
// before painting the view tree, so PulpView is opaque by definition.
// Override isOpaque=YES so AppKit doesn't composite NSWindow.backgroundColor
// (white in light mode) under us on hover/focus repaints. Without this,
// the Spectr filterbank flashes WHITE on mouse-over because AppKit clears
// the dirty region with the window bg before invoking drawRect.
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { (void)e; return YES; }

// pulp #2088 — the Obj-C `_focusedView` ivar is a parallel pointer to
// `pulp::view::View::focused_input_`. The static is auto-cleared by ~View()
// (pulp #1708) when the focused widget is destroyed (e.g. React unmount
// of a clicked widget); the ivar is not. The #1818 fix re-syncs at the
// TOP of mouseDown — but mouseDown's own work (overlay dispatch, ComboBox
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
    // pulp #59/#63/#64/#65 — when a design viewport is in effect, inverse-
    // transform window-space coords into root-space before hit_test sees
    // them. Identity when no viewport is set.
    if (self.pointTransform) pt = self.pointTransform(pt);
    return pt;
}

- (void)scrollWheel:(NSEvent*)event {
    if (!self.rootView) return;
    auto pt = [self localPoint:event];
    auto* target = self.rootView->hit_test(pt);
    if (!target) return;

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

    // Walk up from target to find nearest ScrollView ancestor
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
        if (auto* sv = dynamic_cast<pulp::view::ScrollView*>(v)) {
            sv->on_mouse_event(me);
            sv->layout_children();
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

            // pulp #1818 / #2088 — the Obj-C `_focusedView` ivar is a parallel
            // pointer to `pulp::view::View::focused_input_`. The static is
            // auto-cleared by ~View() (pulp #1708) when the focused widget
            // is destroyed (e.g., a React unmount of a clicked widget); the
            // ivar is not. The original #1818 fix re-synced ONLY at this top
            // of mouseDown — but mouseDown's own work (overlay dispatch, ComboBox
            // routing, on_mouse_event → React unmount) can destroy the focused
            // view MID-FUNCTION, so subsequent _focusedView derefs PAC-faulted
            // (#2088: "-[PulpView mouseDown:] + 1172"). The complete fix routes
            // every deref below through -[liveFocusedView], which performs the
            // re-sync at each access site. This top-level re-sync is now
            // redundant with the per-call accessor, but kept as defense-in-depth
            // for any future deref that forgets to use the accessor.
            if (_focusedView && _focusedView != pulp::view::View::focused_input_) {
                _focusedView = pulp::view::View::focused_input_;
            }

        // Inspector intercept — consume clicks when inspector is active
        {
            auto mods = modifiers_from_ns_flags(event.modifierFlags);
            pulp::view::MouseEvent me;
            me.position = {pt.x, pt.y};
            me.modifiers = mods;
            me.is_down = true;
            if (pulp::view::View::call_inspector_mouse_hook(me)) {
                [self setNeedsDisplay:YES];
                return;
            }
        }

        // Check if click is inside an active ComboBox dropdown overlay.
        // The dropdown renders as a paint overlay with no view backing, so
        // normal hit_test finds the view behind the dropdown. Route the click
        // to the ComboBox instead so it can process the dropdown item selection.
        if (pulp::view::ComboBox::active_popup_) {
            auto* combo = pulp::view::ComboBox::active_popup_;
            float abs_x = 0, abs_y = 0;
            pulp::view::View* v = combo;
            while (v) { abs_x += v->bounds().x; abs_y += v->bounds().y; v = v->parent(); }
            float base_h = std::min(combo->local_bounds().height, 28.0f);
            float dd_top = abs_y + base_h + 2;
            float dd_w = combo->dropdown_width_hint();
            float dd_h = static_cast<float>(combo->items().size()) * 24.0f;
            if (pt.x >= abs_x && pt.x <= abs_x + dd_w &&
                pt.y >= dd_top && pt.y <= dd_top + dd_h) {
                _dragTarget = combo;
                auto local = to_local(pt, combo, self.rootView);
                pulp::view::MouseEvent me;
                me.position = local;
                me.window_position = pt;
                me.button = pulp::view::MouseButton::left;
                me.is_down = true;
                me.click_count = 1;
                combo->on_mouse_event(me);
                [self setNeedsDisplay:YES];
                return;
            }
        }

        // pulp #1148 — generalized overlay-click routing for React popovers.
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
                // pulp #1313 (Codex P1 on #1297) — only dispatch when
                // hit_test returns a real view. If hit_test returns
                // nullptr, the overlay (or some ancestor in its
                // subtree) failed the visible / enabled / hit_testable
                // / pointer_events check; force-dispatching to the
                // overlay anyway would bypass those guards. Fall
                // through to the standard hit_test below instead.
                auto local_to_overlay = to_local(pt, overlay, self.rootView);
                if (auto* sub = overlay->hit_test(local_to_overlay)) {
                    // pulp #1314 (Codex P2 on #1297) — when the
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
                    me.is_down = true;
                    me.click_count = static_cast<int>(event.clickCount);
                    _dragTarget->on_mouse_event(me);
                    _dragTarget->on_mouse_down(local);
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
                // pulp #1361 — go through dismiss_active_overlay() (not the
                // bare release_overlay()) so React state can flip
                // setOpen(false) via on_overlay_dismissed. Falls through to
                // the standard hit_test below so the click also activates
                // whatever view is underneath, matching existing WebView
                // behavior where outside-click closes-and-clicks-through.
                pulp::view::View::dismiss_active_overlay();
            }
        }

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
            me.is_down = true;
            me.click_count = static_cast<int>(event.clickCount);
            _dragTarget->on_mouse_event(me);
            _dragTarget->on_mouse_down(local);

            // Bubble pointerdown through ancestors that subscribed via
            // registerPointer. on_mouse_event is the W3C bubbling
            // channel; on_mouse_down stays deepest-wins. Without this,
            // a wrap-div around a canvas child (Spectr's FilterBank
            // band-drawer is this exact pattern) never sees the down
            // event because the canvas child wins hit_test. Recompute
            // each ancestor's local-coord position by re-toLocal'ing.
            for (auto* bubble = _dragTarget->parent(); bubble; bubble = bubble->parent()) {
                if (!bubble->on_pointer_event) continue;
                pulp::view::MouseEvent bme = me;
                bme.position = to_local(pt, bubble, self.rootView);
                bubble->on_pointer_event(bme);
            }
        }
            [self setNeedsDisplay:YES];
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
                if (pulp::view::View::call_inspector_mouse_hook(me)) {
                    [self setNeedsDisplay:YES];
                    return;
                }
            }
            // pulp #992 — _dragTarget is captured in mouseDown but the View
            // it points to may be unmounted (and freed) before the next
            // drag event arrives, e.g. when a click triggers a React state
            // change that destroys the widget. Re-validate against the
            // live tree before any deref.
            if (!_dragTarget || !self.rootView) return;
            if (!view_is_in_tree(_dragTarget, self.rootView)) {
                _dragTarget = nullptr;
                return;
            }
            auto pt = [self localPoint:event];
            auto local = to_local(pt, _dragTarget, self.rootView);
            _dragTarget->on_mouse_drag(local);
            if (_dragTarget->on_drag) _dragTarget->on_drag(local);

            // pulp jsx-instrument-import 2026-05-17 — bubble on_drag up
            // to ancestors with on_drag set. Mirrors what mouseDown
            // already does for on_pointer_event (window_host_mac.mm:569).
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
                if (pulp::view::View::call_inspector_mouse_hook(me)) {
                    [self setNeedsDisplay:YES];
                    return;
                }
            }
            if (_dragTarget) {
                // pulp #992 — _dragTarget may point at a freed View if
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
                auto pt = [self localPoint:event];
                auto local = to_local(pt, _dragTarget, self.rootView);
                auto released_target = self.rootView ? self.rootView->hit_test(pt) : nullptr;
                // pulp #1067 — DOM-style click bubbling. `hit_test` returns
                // the deepest hit-testable view under the cursor, but the
                // `onClick` handler (registered via `registerClick(id)`) may
                // live on an ancestor. The classic reproducer: @pulp/react
                // turns `<button onClick=...>Clear</button>` into a
                // `<View onClick=...>` parent with a `<Label>Clear</Label>`
                // child (Spectr's dom-adapter wraps string children in
                // synthetic Labels). Clicking the visible "Clear" text
                // hits the Label, which has no `on_click`, so #1006/#1008's
                // capture-by-_dragTarget path silently dropped the click.
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
                up_me.is_down = false;
                up_me.click_count = static_cast<int>(event.clickCount);
                _dragTarget->on_mouse_event(up_me);
                for (auto* bubble = _dragTarget->parent(); bubble; bubble = bubble->parent()) {
                    if (!bubble->on_pointer_event) continue;
                    pulp::view::MouseEvent bme = up_me;
                    bme.position = to_local(pt, bubble, self.rootView);
                    bubble->on_pointer_event(bme);
                }
                if (released_target == _dragTarget && (click_handler || global_click)) {
                    // pulp #2502 — `click_handler` / `global_click` are
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
                    // gone. `-prepareForTeardown` — called from the host
                    // destructor before the bridge/engine can be freed, and
                    // again from -dealloc — flips the token to false. The
                    // teardown runs synchronously with no run-loop pump
                    // between the bridge's destruction and the flip, so every
                    // still-queued block is defused. A block that drains after
                    // teardown sees `false` and no-ops WITHOUT touching `self`
                    // or invoking the captured handlers. `self` is captured
                    // unretained (this file is compiled MRC); that is safe
                    // because every `self` deref is gated behind the
                    // token-is-alive check.
                    std::shared_ptr<std::atomic<bool>> aliveToken = _deferredClickAlive;
                    dispatch_async(dispatch_get_main_queue(), ^{
                        // Defused after teardown: do not touch `self` or the
                        // captured `std::function`s — their backing
                        // WidgetBridge / ScriptEngine may already be freed.
                        if (!aliveToken || !aliveToken->load())
                            return;
                        @try {
                            try {
                                // Token still alive ⇒ the host (and therefore
                                // this view) has not been torn down. The extra
                                // rootView guard rejects a detached view tree.
                                if (self.rootView) {
                                    if (click_handler) click_handler();
                                    if (global_click) global_click(clicked_id, modifiers);
                                    [self setNeedsDisplay:YES];
                                }
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
            auto* target = self.rootView->hit_test(pt);
            if (target && target->on_context_menu) {
                auto local = to_local(pt, target, self.rootView);
                target->on_context_menu(local);
            }
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

// pulp #2128 follow-up — NSResponder routes Cmd-modified key chords
// through performKeyEquivalent:, NOT keyDown:. Without this override,
// every Cmd+,/Cmd+S/Cmd+? chord is consumed by the responder chain and
// never reaches View::on_global_key OR WidgetBridge::dispatch_global_key,
// so `registerShortcut(...)` callbacks and `window.addEventListener
// ('keydown', ...)` listeners are silently dead for Cmd-modified chords.
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    if (!self.rootView) return NO;
    auto key  = key_code_from_ns(event.keyCode);
    auto mods = modifiers_from_ns_flags(event.modifierFlags);
    pulp::view::KeyEvent gke;
    gke.key = key;
    gke.modifiers = mods;
    gke.is_down = true;
    gke.is_repeat = event.isARepeat;
    if (self.rootView->on_global_key) self.rootView->on_global_key(gke);
    // Fan out to every live WidgetBridge so `@pulp/react` apps (Spectr,
    // etc.) and the design-tool both receive Cmd-modified chords without
    // each having to wire its own `on_global_key` lambda.
    pulp::view::WidgetBridge::dispatch_global_key(
        static_cast<int>(key), mods, /*is_down=*/true);
    // Return NO so the responder chain still considers standard menu
    // shortcuts (Cmd+W close, Cmd+Q quit, etc.) — the bridge fan-out is
    // additive, not consuming.
    return NO;
}

- (void)keyDown:(NSEvent*)event {
    @try {
        try {
            auto key = key_code_from_ns(event.keyCode);
            auto mods = modifiers_from_ns_flags(event.modifierFlags);

            // pulp #1818 — re-sync the Obj-C `_focusedView` ivar from the
            // auto-clearing `View::focused_input_` static. Same rationale
            // as mouseDown: if the focused View was unmounted between the
            // last input event and this one (~View clears the static),
            // the ivar still dangles and any deref below (Tab nav at
            // L749/751 or the final dispatch at L808) would PAC-fault on
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
            // pulp #2088 — re-sync via liveFocusedView so a destroyed prior
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

        // pulp #2128 follow-up — fan out to every live WidgetBridge so
        // `@pulp/react` apps (Spectr) receive bare-key shortcuts like
        // 'S' or Escape that React effects bind via
        // `window.addEventListener('keydown', ...)`. The bridge's own
        // registered-shortcut path suppresses bare keys when a text-input
        // widget owns focus (see WidgetBridge::forward_key_event), so
        // typing `?` into a search box doesn't trigger a `?` shortcut.
        // This is additive — focused-view delivery still runs below.
        pulp::view::WidgetBridge::dispatch_global_key(
            static_cast<int>(key), mods, /*is_down=*/true);

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
            // pulp #68 — host-level ESC fallback for an open ComboBox dropdown
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
            // pulp #1361 — generic active_overlay_ ESC dismissal. ModalOverlay,
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

            // pulp #2088 — interpretKeyEvents above can dispatch IME / app
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

- (void)insertText:(id)string replacementRange:(NSRange)range {
    (void)range;
    // pulp #1708 — read from View::focused_input_ rather than the raw
    // _focusedView ivar. The static is auto-cleared by ~View() when the
    // focused widget is destroyed (e.g., React unmount of an open modal),
    // so we never dispatch text-input on freed memory.
    auto* fv = pulp::view::View::focused_input_;
    if (fv) {
        NSString* str = [string isKindOfClass:[NSAttributedString class]]
            ? [(NSAttributedString*)string string] : (NSString*)string;
        pulp::view::TextInputEvent te;
        te.text = [str UTF8String];
        fv->on_text_input(te);
        [self setNeedsDisplay:YES];
    }
}

- (pulp::view::TextEditor*)focusedTextEditor {
    // pulp #1708 — use the auto-clearing static slot. Without this, the
    // raw _focusedView ivar dangles after the focused widget is freed
    // (React unmount of an open modal) and dynamic_cast on freed memory
    // segfaults inside libc++abi during -[NSTextInputContext
    // hasMarkedTextWithCompletionHandler:] on the next keypress.
    auto* fv = pulp::view::View::focused_input_;
    auto* te = dynamic_cast<pulp::view::TextEditor*>(fv);
    return te;
}

- (BOOL)hasMarkedText {
    auto* te = [self focusedTextEditor];
    return te ? te->has_marked_text() : NO;
}

- (NSRange)markedRange {
    auto* te = [self focusedTextEditor];
    if (!te || !te->has_marked_text()) return NSMakeRange(NSNotFound, 0);
    auto [start, len] = te->marked_range();
    return NSMakeRange(static_cast<NSUInteger>(start), static_cast<NSUInteger>(len));
}

- (NSRange)selectedRange {
    auto* te = [self focusedTextEditor];
    if (!te) return NSMakeRange(0, 0);
    return NSMakeRange(static_cast<NSUInteger>(te->caret_pos()), 0);
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)rep {
    (void)rep;
    auto* te = [self focusedTextEditor];
    if (!te) return;
    NSString* str = [string isKindOfClass:[NSAttributedString class]]
        ? [(NSAttributedString*)string string] : (NSString*)string;
    te->set_marked_text([str UTF8String],
                        static_cast<int>(sel.location),
                        static_cast<int>(sel.length));
    [self setNeedsDisplay:YES];
}

- (void)unmarkText {
    auto* te = [self focusedTextEditor];
    if (te) te->unmark_text();
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText { return @[]; }

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)a {
    (void)a;
    auto* te = [self focusedTextEditor];
    if (!te) return nil;
    auto& text = te->text();
    if (r.location >= text.size()) return nil;
    auto len = std::min(r.length, text.size() - r.location);
    auto sub = text.substr(r.location, len);
    return [[NSAttributedString alloc] initWithString:[NSString stringWithUTF8String:sub.c_str()]];
}

- (NSUInteger)characterIndexForPoint:(NSPoint)p { (void)p; return 0; }

- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)a {
    (void)r; (void)a;
    // IME candidate window positioning. `TextEditor::caret_rect()` is
    // the post-paint, layout-aware caret geometry — single source of
    // truth for both the visible caret and the IME hook so the
    // candidate window aligns with the rendered position even when
    // the text is wrapped or proportional-width.
    auto* te = [self focusedTextEditor];
    if (!te) return NSZeroRect;

    pulp::view::Rect caret = te->caret_rect();
    float local_x = caret.x;
    float local_y = caret.y;
    float caret_w = caret.width > 0.f ? caret.width : 1.0f;
    float caret_h = caret.height > 0.f ? caret.height : te->font_size();

    float rx = local_x, ry = local_y;
    for (auto* v = static_cast<pulp::view::View*>(te); v; v = v->parent()) {
        rx += v->bounds().x;
        ry += v->bounds().y;
    }

    float viewHeight = static_cast<float>(self.bounds.size.height);
    NSRect viewRect = NSMakeRect(rx, viewHeight - ry - caret_h, caret_w, caret_h);
    NSRect windowRect = [self convertRect:viewRect toView:nil];
    return [self.window convertRectToScreen:windowRect];
}

- (void)doCommandBySelector:(SEL)sel { (void)sel; }

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
                pulp::view::View::call_inspector_mouse_hook(me);
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
            // WYSIWYG P2d — the inspector overlay may override the cursor for
            // its move/resize affordances (it owns mouse-move before normal
            // hit-testing). A returned style >= 0 wins over the hit view's
            // own cursor(); -1 defers to the normal path below.
            int inspector_cursor = -1;
            {
                pulp::view::MouseEvent cme;
                cme.position = {pt.x, pt.y};
                cme.is_down = false;
                inspector_cursor = pulp::view::View::call_inspector_cursor_hook(cme);
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
                        // macOS doesn't have a diagonal NW-SE cursor — use crosshair
                        [[NSCursor crosshairCursor] set]; break;
                    case pulp::view::View::CursorStyle::top_right_resize:
                    case pulp::view::View::CursorStyle::bottom_left_resize:
                        // macOS doesn't have a diagonal NE-SW cursor — use crosshair
                        [[NSCursor crosshairCursor] set]; break;
                    case pulp::view::View::CursorStyle::multi_directional_resize:
                        [[NSCursor openHandCursor] set]; break;
                    // pulp #1550 Tier-4 macOS partial 2026-05-12 — 5
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

// ── Trackpad gestures (P4: pinch/rotate) ───────────────────────────

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
            if (self.frameClock) {
                self.frameClock->tick(1.0f / 60.0f);
            }
            // Advance widget animations
            [self advanceWidgetAnimations:self.rootView dt:1.0f/60.0f];
            [self setNeedsDisplay:YES];

            // Stop timer when no animations are active
            if (self.frameClock && !self.frameClock->has_active_subscribers()) {
                if (![self hasActiveAnimations:self.rootView]) {
                    [self.animationTimer invalidate];
                    self.animationTimer = nil;
                }
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
    // pulp #1321: AppKit resizes us during a window resize. Push the new
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
    // pulp #2502 — belt-and-suspenders: even if a host teardown path forgot
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

        // pulp #1382 — declare the layer opaque and seed its background to
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

        self.layer = layer;
        _metalLayer = layer;
    }
    return self;
}

// pulp #1382 — `wantsUpdateLayer = YES` tells AppKit to use the
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
/// pulp-internal #70 — real-time aspect-lock snap during user drag.
/// macOS `setContentAspectRatio:` is unreliable in practice (it does
/// not engage during edge-drag on every monitor configuration, and is
/// bypassed entirely by the green zoom button / programmatic resize).
/// Implementing `windowWillResize:toSize:` constrains every proposed
/// resize before AppKit commits it, so the live drag is locked to the
/// design aspect regardless of drag handle or zoom path. When 0, no
/// constraint is applied.
@property (nonatomic, assign) CGFloat aspectRatio;
@end

@implementation PulpWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
    if (self.onClose) self.onClose();
    [NSApp stop:nil];
    [sender orderOut:nil];
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
    // DEBUG: write to /tmp/pulp-aspect.log unconditionally
    {
        FILE* f = fopen("/tmp/pulp-aspect.log", "a");
        if (f) {
            fprintf(f, "ENTRY aspectRatio=%.3f frame=%.0fx%.0f\n",
                    self.aspectRatio, frameSize.width, frameSize.height);
            fclose(f);
        }
    }
    // No-op when aspect-lock isn't requested.
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

    // DEBUG: write to /tmp/pulp-aspect.log
    FILE* f = fopen("/tmp/pulp-aspect.log", "a");
    if (f) {
        fprintf(f, "willResize frame=%.0fx%.0f content=%.0fx%.0f cur=%.0fx%.0f dw=%.0f dh=%.0f ar=%.3f",
                frameSize.width, frameSize.height,
                targetW, targetH,
                currentContent.width, currentContent.height,
                dw, dh, self.aspectRatio);
    }

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
    if (f) {
        fprintf(f, " -> content=%.0fx%.0f frame=%.0fx%.0f\n",
                targetW, targetH, newFrame.size.width, newFrame.size.height);
        fclose(f);
    }
    return newFrame.size;
}

@end

// configure_window_type and the child-view geometry helpers
// (child_view_frame_in_host, attach_child_view_to_host,
// set_child_view_bounds_in_host, detach_child_view_from_host) were
// extracted to window_host_mac_geometry.mm in the R2-5 refactor — see
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

            // pulp #1382 — NSWindow's default backgroundColor is
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

            // Apply multi-window type configuration (Phase 6)
            configure_window_type(window_, options);

            if (options.min_width > 0 || options.min_height > 0)
                [window_ setContentMinSize:NSMakeSize(options.min_width, options.min_height)];

            options_initially_hidden_ = options.initially_hidden;

            view_ = [[PulpView alloc] initWithFrame:frame];
            view_.rootView = &root_;
            view_.frameClock = &frame_clock_;
            [window_ setContentView:view_];

            delegate_ = [[PulpWindowDelegate alloc] init];
            delegate_.onResize = ^(float w, float h) {
                // pulp #1321: belt-and-suspenders relayout — PulpView's
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
        // pulp #2502 — cancel any queued deferred-click blocks before the
        // root View tree / WidgetBridge / ScriptEngine they captured can be
        // freed. This destructor runs synchronously, with no run-loop pump
        // between here and the caller's earlier-destroyed bridge/engine, so
        // cancelling now reliably catches every still-queued block.
        [view_ prepareForTeardown];
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
    std::vector<uint8_t> capture_png() override {
        auto live = pulp::view::mac_capture::capture_window_screencapture_png(window_);
        return !live.empty() ? live : pulp::view::mac_capture::capture_window_content_png(window_, view_);
    }

    // issue #2001 — host-managed pixels only. CG-backed host has no GPU
    // back-buffer, so the deterministic surface is the rasterized content
    // view. Skips screencapture so hidden windows still produce bytes.
    std::vector<uint8_t> capture_back_buffer_png() override {
        return pulp::view::mac_capture::capture_window_content_png(window_, view_);
    }

    void invalidate_input_state() override { [view_ clearInteractionState]; }
    void request_close() override { request_app_close(window_); }

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
            // pulp-internal #71 follow-up — see header for initially_hidden.
            if (options_initially_hidden_) {
                [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            } else {
                [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
                install_app_menu([window_ title]);
                show();
                [NSApp activateIgnoringOtherApps:YES];
            }
            [NSApp run];
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
            [window_ setTitle:[NSString stringWithUTF8String:options.title.c_str()]];

            // Apply multi-window type configuration (Phase 6)
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
            delegate_.onResize = ^(float w, float h) {
                handle_resize(w, h);
            };
            [window_ setDelegate:delegate_];

            // Initialize GPU render stack
            init_gpu(options.width, options.height);
        }
    }

    ~MacGpuWindowHost() override {
        stop_display_link();
        skia_surface_.reset();
        gpu_surface_.reset();
        // pulp #2502 — cancel any queued deferred-click blocks before the
        // captured View tree / WidgetBridge / ScriptEngine can be freed.
        // PulpMetalView inherits -prepareForTeardown from PulpView.
        [metal_view_ prepareForTeardown];
        metal_view_.rootView = nullptr;
        root_.set_window_host(nullptr);
        root_.set_frame_clock(nullptr);
    }

    void show() override { [window_ makeKeyAndOrderFront:nil]; }
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

    void repaint() override {
        needs_repaint_ = true;
        // until widgets pass per-rect bounds through
        // request_repaint(), every host-level repaint() invalidates
        // the whole viewport. the follow-up partial-rendering slice adds the Rect overload.
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

    // issue #2001 — deterministic GPU back-buffer readback for hidden /
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
        request_app_close(window_);
    }

    void set_close_callback(std::function<void()> cb) override {
        close_callback_ = std::move(cb);
        delegate_.onClose = ^{ if (close_callback_) close_callback_(); };
    }

    void set_resize_callback(ResizeCallback cb) override {
        resize_callback_ = std::move(cb);
    }

    // pulp #1387 gap #3 — wire the host's idle callback into the
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
            // pulp-internal #71 follow-up — when initially_hidden is set,
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
        }
    }

    // ── Platform feature overrides ──────────────────────────────────────

    void set_mouse_relative_mode(bool enabled) override {
        if (enabled) {
            CGAssociateMouseAndMouseCursorPosition(false);
            [NSCursor hide];
        } else {
            CGAssociateMouseAndMouseCursorPosition(true);
            [NSCursor unhide];
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
    bool options_initially_hidden_ = false;

    std::unique_ptr<render::GpuSurface> gpu_surface_;
    std::unique_ptr<render::SkiaSurface> skia_surface_;
    CVDisplayLinkRef display_link_ = nullptr;
    std::atomic<bool> needs_repaint_{true};
    std::atomic<bool> continuous_frames_{false};
    std::atomic<bool> render_dispatch_queued_{false};
    // partial-rendering POC. Tracks per-frame invalidations
    // pushed by repaint() and the animation / FrameClock pump. The
    // render gate still uses the existing needs_repaint_/animation
    // flags so observable behaviour does not change in this slice —
    // the tracker is plumbed-but-non-authoritative. Debug-printed once
    // per painted frame when PULP_PARTIAL_RENDERING_DEBUG=1 in the
    // environment so we can audit "the dirty rect we WOULD have
    // clipped to" against the unconditional repaint that still ships.
    // the follow-up partial-rendering slice promotes the tracker to the authoritative gate + adds
    // Skia clipIRect.
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
    // pulp #1387 gap #3 — idle callback wiring. CVDisplayLink reads
    // has_idle_callback_ on the display thread; idle_callback_ is
    // invoked on main only.
    std::function<void()> idle_callback_;
    std::atomic<bool> has_idle_callback_{false};

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

        // pulp #1734 (Codex P1): CSS animations on a generic View were
        // not keeping the CVDisplayLink loop alive. tick_animations()
        // is called every frame in advance_widget_animations, but
        // without a continuous-frame request the loop stalls after
        // needs_repaint_ clears once. Check for any unpaused active
        // CSS animation; matches the gate inside tick_animations()
        // (early-out when animation_play_state_ == "paused").
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

        // pulp #1668 — also drive CSS animations on every View in the
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
        // pulp #1321: relayout the root view synchronously so hit testing
        // and any user resize callback both see the new geometry. paint_scene
        // also calls set_bounds + layout_children, but that runs at the next
        // vsync — too late for input handlers fired during the resize drag.
        root_.set_bounds({0, 0, width, height});
        root_.layout_children();
        if (resize_callback_) {
            resize_callback_(
                static_cast<uint32_t>(std::max(0.0f, width)),
                static_cast<uint32_t>(std::max(0.0f, height)));
        }
        needs_repaint_ = true;
        tracker_.set_viewport(width, height);
        tracker_.invalidate_all();
    }

    void paint_scene(canvas::Canvas& canvas) {
        // pulp #59/#63/#64/#65 — when a design viewport is set, the root
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
        root_.layout_children();

        canvas.set_fill_color(canvas::Color::rgba8(30, 30, 46));
        canvas.fill_rect(0, 0, width_, height_);

        if (has_viewport) {
            // pulp PR #1984 Codex P1 — paint_overlays MUST run inside the
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
        if (!gpu_surface_ || !skia_surface_) return false;

        // emit the per-frame dirty-rect decision the host
        // WOULD use to clip in the follow-up partial-rendering slice. The actual paint path is
        // unchanged; this is a wiring trace only. Gated on env so
        // production CI logs stay quiet.
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
            view_needs_continuous_frames(&root_) || frame_clock_.has_active_subscribers(),
            std::memory_order_relaxed);

        bool captured = true;
        if (capture_pixels && capture_width && capture_height) {
            captured = skia_surface_->read_current_rgba(*capture_pixels, *capture_width, *capture_height);
        }

        skia_surface_->end_frame();   // submit Graphite recording
        gpu_surface_->end_frame();    // present to Metal surface

        needs_repaint_.store(continuous_frames_.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        // clear the tracker AFTER present so the next frame
        // starts clean. the follow-up partial-rendering slice will gate begin_frame() on
        // tracker_.is_dirty() before paying for any of the above work.
        tracker_.clear();
        return captured;
    }

    // CVDisplayLink callback — fires on the display's vsync
    static CVReturn display_link_callback(
        CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*,
        CVOptionFlags, CVOptionFlags*, void* context)
    {
        auto* self = static_cast<MacGpuWindowHost*>(context);
        // pulp #1387 gap #3 — guard now also fires when an idle
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
            // Dispatch rendering to the main thread (required for Cocoa + Metal)
            dispatch_async(dispatch_get_main_queue(), ^{
                @autoreleasepool {
                    // Pump idle FIRST so JS rAF callbacks fire (and any
                    // request_repaint they trigger arms needs_repaint_)
                    // before we evaluate whether to render this frame.
                    if (self->idle_callback_) {
                        self->idle_callback_();
                    }

                    bool animate = view_needs_continuous_frames(&self->root_);
                    bool tick_subscribers = self->frame_clock_.has_active_subscribers();
                    if (!self->needs_repaint_.load(std::memory_order_relaxed) && !animate && !tick_subscribers) {
                        self->continuous_frames_.store(false, std::memory_order_relaxed);
                        self->render_dispatch_queued_.store(false, std::memory_order_release);
                        return;
                    }
                    self->frame_clock_.tick(1.0f / 60.0f);
                    advance_widget_animations(&self->root_, 1.0f / 60.0f);
                    if (animate || tick_subscribers) {
                        self->needs_repaint_.store(true, std::memory_order_relaxed);
                        // Note: animation pump: animation /
                        // frame-clock pumps mutate view state without
                        // going through request_repaint(), so for the
                        // tracker they count as a full-surface dirty
                        // until the follow-up partial-rendering slice audits and converts the
                        // animating widget paths to per-rect repaints.
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
