#include <pulp/view/window_host.hpp>
#include <pulp/view/window_manager.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/modal.hpp>

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/canvas/cg_canvas.hpp>
#import <Cocoa/Cocoa.h>
#include <algorithm>
#include <atomic>
#include <iostream>

#ifdef PULP_HAS_SKIA
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

static void request_app_close(NSWindow* window) {
    NSWindow* target = window != nil ? window : [NSApp keyWindow];
    if (target == nil) target = [NSApp mainWindow];
    if (target != nil) {
        [target performClose:nil];
    } else {
        [NSApp stop:nil];
    }
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

static std::vector<uint8_t> nsdata_to_bytes(NSData* data) {
    if (!data || data.length == 0) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(data.length));
    memcpy(bytes.data(), data.bytes, static_cast<size_t>(data.length));
    return bytes;
}

static std::vector<uint8_t> bitmap_rep_to_png(NSBitmapImageRep* rep) {
    if (!rep) return {};
    NSData* data = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return nsdata_to_bytes(data);
}

static std::vector<uint8_t> encode_rgba_to_png(const uint8_t* pixels,
                                               uint32_t pixel_w,
                                               uint32_t pixel_h,
                                               size_t row_bytes) {
    if (!pixels || pixel_w == 0 || pixel_h == 0) return {};

    CGColorSpaceRef color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!color_space) return {};

    CGBitmapInfo bitmap_info =
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
        static_cast<CGBitmapInfo>(kCGBitmapByteOrderDefault);

    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr, pixels, row_bytes * pixel_h, nullptr);
    if (!provider) {
        CGColorSpaceRelease(color_space);
        return {};
    }

    CGImageRef image = CGImageCreate(
        pixel_w, pixel_h,
        8, 32,
        row_bytes,
        color_space,
        bitmap_info,
        provider,
        nullptr,
        false,
        kCGRenderingIntentDefault);

    CGDataProviderRelease(provider);
    CGColorSpaceRelease(color_space);
    if (!image) return {};

    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:image];
    CGImageRelease(image);
    return bitmap_rep_to_png(rep);
}

static std::vector<uint8_t> capture_view_cache_png(NSView* view) {
    if (!view) return {};
    NSRect bounds = [view bounds];
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return {};
    NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:bounds];
    if (!rep) return {};
    [view cacheDisplayInRect:bounds toBitmapImageRep:rep];
    return bitmap_rep_to_png(rep);
}

static std::vector<uint8_t> capture_window_content_png(NSWindow* window, NSView* contentView) {
    if (!window || !contentView) return {};

    [window displayIfNeeded];
    [contentView displayIfNeeded];
    return capture_view_cache_png(contentView);
}

static pulp::view::ModalOverlay* find_topmost_modal(pulp::view::View* root) {
    if (!root || !root->visible()) return nullptr;

    for (size_t i = root->child_count(); i > 0; --i) {
        if (auto* modal = find_topmost_modal(root->child_at(i - 1)))
            return modal;
    }

    return dynamic_cast<pulp::view::ModalOverlay*>(root);
}

static std::vector<uint8_t> capture_window_screencapture_png(NSWindow* window) {
    if (!window) return {};

    NSString* temp_name = [NSString stringWithFormat:@"pulp-window-capture-%@.png", NSUUID.UUID.UUIDString];
    NSString* temp_path = [NSTemporaryDirectory() stringByAppendingPathComponent:temp_name];
    NSString* window_arg = [NSString stringWithFormat:@"-l%u", static_cast<unsigned int>(window.windowNumber)];

    NSTask* task = [[NSTask alloc] init];
    task.launchPath = @"/usr/sbin/screencapture";
    task.arguments = @[ @"-x", @"-o", window_arg, temp_path ];

    @try {
        [task launch];
        [task waitUntilExit];
    } @catch (NSException*) {
        [[NSFileManager defaultManager] removeItemAtPath:temp_path error:nil];
        return {};
    }

    if (task.terminationStatus != 0) {
        [[NSFileManager defaultManager] removeItemAtPath:temp_path error:nil];
        return {};
    }

    NSData* data = [NSData dataWithContentsOfFile:temp_path];
    [[NSFileManager defaultManager] removeItemAtPath:temp_path error:nil];
    return nsdata_to_bytes(data);
}

// ── PulpView: CoreGraphics NSView (CPU rendering path) ───────────────────────

@interface PulpView : NSView <NSTextInputClient>
@property (nonatomic, assign) pulp::view::View* rootView;
@property (nonatomic, assign) pulp::view::FrameClock* frameClock;
@property (nonatomic, strong) NSTimer* animationTimer;
@property (nonatomic, strong) NSTrackingArea* trackingArea;
@end

@implementation PulpView {
    pulp::view::View* _dragTarget;
    pulp::view::View* _focusedView;
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
    }
    return self;
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

- (void)clearInteractionState {
    _dragTarget = nullptr;
    if (_focusedView) _focusedView->release_input_focus();
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
    return {static_cast<float>(p.x), viewHeight - static_cast<float>(p.y)};
}

- (void)scrollWheel:(NSEvent*)event {
    if (!self.rootView) return;
    auto pt = [self localPoint:event];
    auto* target = self.rootView->hit_test(pt);
    if (!target) return;

    pulp::view::MouseEvent me;
    me.position = pt;
    me.is_wheel = true;
    me.scroll_delta_x = static_cast<float>(event.scrollingDeltaX);
    me.scroll_delta_y = static_cast<float>(-event.scrollingDeltaY);

    // Walk up from target to find nearest ScrollView ancestor
    auto* v = target;
    while (v) {
        if (auto* sv = dynamic_cast<pulp::view::ScrollView*>(v)) {
            sv->on_mouse_event(me);
            sv->layout_children();  // re-layout after scroll position change
            [self setNeedsDisplay:YES];
            return;
        }
        v = v->parent();
    }
    target->on_mouse_event(me);
    [self setNeedsDisplay:YES];
}

static uint16_t modifiersFromNSFlags(NSEventModifierFlags flags) {
    uint16_t m = 0;
    if (flags & NSEventModifierFlagShift)   m |= pulp::view::kModShift;
    if (flags & NSEventModifierFlagControl) m |= pulp::view::kModCtrl;
    if (flags & NSEventModifierFlagOption)  m |= pulp::view::kModAlt;
    if (flags & NSEventModifierFlagCommand) m |= pulp::view::kModCmd;
    return m;
}

static pulp::view::Point toLocal(pulp::view::Point pos, pulp::view::View* target, pulp::view::View* root) {
    auto local = pos;
    for (auto* v = target; v && v != root; v = v->parent()) {
        local.x -= v->bounds().x;
        local.y -= v->bounds().y;
    }
    return local;
}

// pulp #992 — confirm a cached View* is still attached to the live tree
// before any dereference. Walks `root` and compares pointers only, so it's
// safe to call with `needle` pointing into freed memory. Returns false if
// `needle` was unparented or destroyed between the cached capture (e.g. at
// mouseDown) and the next event (e.g. mouseUp). Does NOT detect the ABA
// case where memory was freed and reused at the same address — see #992
// follow-up issue if profiling shows that's hit; for now a tree walk
// catches the React-unmount-during-click pattern that actually crashes.
static bool view_is_in_tree(pulp::view::View* needle, pulp::view::View* root) {
    if (!needle || !root) return false;
    if (needle == root) return true;
    for (size_t i = 0; i < root->child_count(); ++i) {
        if (view_is_in_tree(needle, root->child_at(i))) return true;
    }
    return false;
}

static pulp::view::KeyCode keyCodeFromNS(unsigned short code) {
    using KC = pulp::view::KeyCode;
    switch (code) {
        case 0: return KC::a; case 1: return KC::s; case 2: return KC::d;
        case 3: return KC::f; case 4: return KC::h; case 5: return KC::g;
        case 6: return KC::z; case 7: return KC::x; case 8: return KC::c;
        case 9: return KC::v; case 11: return KC::b; case 12: return KC::q;
        case 13: return KC::w; case 14: return KC::e; case 15: return KC::r;
        case 16: return KC::y; case 17: return KC::t; case 31: return KC::o;
        case 32: return KC::u; case 34: return KC::i; case 35: return KC::p;
        case 37: return KC::l; case 38: return KC::j; case 40: return KC::k;
        case 45: return KC::n; case 46: return KC::m;
        case 36: return KC::enter; case 53: return KC::escape;
        case 48: return KC::tab; case 51: return KC::backspace;
        case 117: return KC::delete_;
        case 123: return KC::left; case 124: return KC::right;
        case 125: return KC::down; case 126: return KC::up;
        case 115: return KC::home; case 119: return KC::end_;
        case 49: return KC::space;
        default: return KC::unknown;
    }
}

- (void)mouseDown:(NSEvent*)event {
    @try {
        try {
            if (!self.rootView) return;
            if (self.window.firstResponder != self) {
                [self.window makeFirstResponder:self];
            }
            auto pt = [self localPoint:event];

        // Inspector intercept — consume clicks when inspector is active
        {
            auto mods = modifiersFromNSFlags(event.modifierFlags);
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
                auto local = toLocal(pt, combo, self.rootView);
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
                auto local_to_overlay = toLocal(pt, overlay, self.rootView);
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
                    auto local = toLocal(pt, _dragTarget, self.rootView);

                    if (_dragTarget->focusable()) {
                        if (_focusedView && _focusedView != _dragTarget)
                            _focusedView->on_focus_changed(false);
                        _focusedView = _dragTarget;
                        _focusedView->on_focus_changed(true);
                        _focusedView->claim_input_focus();
                    } else if (_focusedView) {
                        _focusedView->on_focus_changed(false);
                        _focusedView->release_input_focus();
                        _focusedView = nullptr;
                    }

                    pulp::view::MouseEvent me;
                    me.position = local;
                    me.window_position = pt;
                    me.button = pulp::view::MouseButton::left;
                    me.modifiers = modifiersFromNSFlags(event.modifierFlags);
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
            auto local = toLocal(pt, _dragTarget, self.rootView);

            if (_dragTarget->focusable()) {
                if (_focusedView && _focusedView != _dragTarget)
                    _focusedView->on_focus_changed(false);
                _focusedView = _dragTarget;
                _focusedView->on_focus_changed(true);
                _focusedView->claim_input_focus();
            } else if (_focusedView) {
                _focusedView->on_focus_changed(false);
                _focusedView->release_input_focus();
                _focusedView = nullptr;
            }

            pulp::view::MouseEvent me;
            me.position = local;
            me.window_position = pt;
            me.button = pulp::view::MouseButton::left;
            me.modifiers = modifiersFromNSFlags(event.modifierFlags);
            me.is_down = true;
            me.click_count = static_cast<int>(event.clickCount);
            _dragTarget->on_mouse_event(me);
            _dragTarget->on_mouse_down(local);
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
            auto local = toLocal(pt, _dragTarget, self.rootView);
            _dragTarget->on_mouse_drag(local);
            if (_dragTarget->on_drag) _dragTarget->on_drag(local);
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
                auto local = toLocal(pt, _dragTarget, self.rootView);
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
                auto modifiers = modifiersFromNSFlags(event.modifierFlags);
                _dragTarget->on_mouse_up(local);
                if (released_target == _dragTarget && (click_handler || global_click)) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        @try {
                            try {
                                if (click_handler) click_handler();
                                if (global_click) global_click(clicked_id, modifiers);
                                [self setNeedsDisplay:YES];
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
                auto local = toLocal(pt, target, self.rootView);
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

- (void)keyDown:(NSEvent*)event {
    @try {
        try {
            auto key = keyCodeFromNS(event.keyCode);
            auto mods = modifiersFromNSFlags(event.modifierFlags);

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
            auto* old = _focusedView;
            pulp::view::View* next = nullptr;
            if (mods & pulp::view::kModShift)
                next = pulp::view::View::focus_prev(*self.rootView, _focusedView);
            else
                next = pulp::view::View::focus_next(*self.rootView, _focusedView);
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

            if (_focusedView) {
                pulp::view::KeyEvent ke;
                ke.key = key;
                ke.modifiers = mods;
                ke.is_down = true;
                ke.is_repeat = event.isARepeat;
                _focusedView->on_key_event(ke);
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
    // Return the caret rect in screen coordinates for IME candidate window positioning
    auto* te = [self focusedTextEditor];
    if (!te) return NSZeroRect;

    // Approximate caret position using monospace character width
    float char_w = te->font_size() * 0.6f;
    float caret_x = 6.0f + te->caret_pos() * char_w;
    float caret_y = 0;

    // Walk up parents to get root-relative position
    float rx = caret_x, ry = caret_y;
    for (auto* v = static_cast<pulp::view::View*>(te); v; v = v->parent()) {
        rx += v->bounds().x;
        ry += v->bounds().y;
    }

    // Convert to NSView coordinates (bottom-up)
    float viewHeight = static_cast<float>(self.bounds.size.height);
    NSRect viewRect = NSMakeRect(rx, viewHeight - ry - te->font_size(), char_w, te->font_size());
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
            if (target) {
                auto style = target->cursor();
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
        pulp::view::View::paint_overlays(canvas);

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

@end

// ── Multi-window type configuration (Phase 6) ──────────────────────────────

static void configure_window_type(NSWindow* window, const pulp::view::WindowOptions& options) {
    if (!options.window_type) return;

    using pulp::view::WindowType;
    WindowType type = *options.window_type;

    switch (type) {
        case WindowType::palette:
            // Floating palette: stays above main windows, no taskbar entry
            [window setLevel:NSFloatingWindowLevel];
            [window setHidesOnDeactivate:YES];
            [window setCollectionBehavior:
                NSWindowCollectionBehaviorTransient |
                NSWindowCollectionBehaviorFullScreenAuxiliary];
            break;

        case WindowType::inspector:
            // Inspector: floating, resizable, auxiliary panel
            [window setLevel:NSFloatingWindowLevel];
            [window setHidesOnDeactivate:YES];
            [window setCollectionBehavior:
                NSWindowCollectionBehaviorFullScreenAuxiliary];
            break;

        case WindowType::popup:
            // Popup: above everything, auto-dismiss expected by caller
            [window setLevel:NSPopUpMenuWindowLevel];
            [window setHidesOnDeactivate:YES];
            [window setCollectionBehavior:NSWindowCollectionBehaviorTransient];
            break;

        case WindowType::dialog:
            // Dialog: modal window level, centered
            [window setLevel:NSModalPanelWindowLevel];
            [window center];
            break;

        case WindowType::main:
            // Main: default behavior, no special configuration
            break;
    }

    // Set parent-child relationship if a parent handle was provided
    if (options.parent_native_handle) {
        NSWindow* parent = (__bridge NSWindow*)options.parent_native_handle;
        [parent addChildWindow:window ordered:NSWindowAbove];
    }
}

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

    NSView* child = (__bridge NSView*) child_view_handle;
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

    NSView* child = (__bridge NSView*) child_view_handle;
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

    NSView* child = (__bridge NSView*) child_view_handle;
    if (child && child.superview == container) {
        [child removeFromSuperview];
    }
}

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
        auto live = capture_window_screencapture_png(window_);
        return !live.empty() ? live : capture_window_content_png(window_, view_);
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
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            install_app_menu([window_ title]);
            show();
            [NSApp activateIgnoringOtherApps:YES];
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
};

// ── MacGpuWindowHost (Dawn/Skia Graphite) ────────────────────────────────────

#ifdef PULP_HAS_SKIA

class MacGpuWindowHost : public WindowHost {
public:
    MacGpuWindowHost(View& root, const WindowOptions& options)
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
            [window_ setTitle:[NSString stringWithUTF8String:options.title.c_str()]];

            // Apply multi-window type configuration (Phase 6)
            configure_window_type(window_, options);

            if (options.min_width > 0 || options.min_height > 0)
                [window_ setContentMinSize:NSMakeSize(options.min_width, options.min_height)];

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
    }

    std::vector<uint8_t> capture_png() override {
        if (gpu_surface_ && skia_surface_) {
            needs_repaint_.store(true, std::memory_order_relaxed);
            render_frame();

            auto live = capture_window_screencapture_png(window_);
            if (!live.empty()) return live;

            auto content = capture_window_content_png(window_, metal_view_);
            if (!content.empty()) return content;

            std::vector<uint8_t> pixels;
            uint32_t pixel_w = 0;
            uint32_t pixel_h = 0;
            if (render_frame(&pixels, &pixel_w, &pixel_h)) {
                auto encoded = encode_rgba_to_png(pixels.data(),
                                                  pixel_w,
                                                  pixel_h,
                                                  static_cast<size_t>(pixel_w) * 4u);
                if (!encoded.empty()) return encoded;
            }
        }

        auto live = capture_window_screencapture_png(window_);
        if (!live.empty()) return live;

        return capture_window_content_png(window_, metal_view_);
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
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            install_app_menu([window_ title]);

            // Start display-linked render loop
            start_display_link();

            show();
            [window_ makeFirstResponder:metal_view_];
            [NSApp activateIgnoringOtherApps:YES];
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
        if (window_ && ratio > 0)
            [window_ setContentAspectRatio:NSMakeSize(ratio, 1.0)];
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

    std::unique_ptr<render::GpuSurface> gpu_surface_;
    std::unique_ptr<render::SkiaSurface> skia_surface_;
    CVDisplayLinkRef display_link_ = nullptr;
    std::atomic<bool> needs_repaint_{true};
    std::atomic<bool> continuous_frames_{false};
    std::atomic<bool> render_dispatch_queued_{false};
    int frame_fail_count_ = 0;
    int frame_ok_count_ = 0;
    float width_ = 0, height_ = 0;
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
    }

    void paint_scene(canvas::Canvas& canvas) {
        root_.set_bounds({0, 0, width_, height_});
        root_.layout_children();

        canvas.set_fill_color(canvas::Color::rgba8(30, 30, 46));
        canvas.fill_rect(0, 0, width_, height_);

        root_.paint_all(canvas);
        pulp::view::View::paint_overlays(canvas);

        // Inspector overlay is painted automatically via View::paint_overlays()
    }

    bool render_frame(std::vector<uint8_t>* capture_pixels = nullptr,
                      uint32_t* capture_width = nullptr,
                      uint32_t* capture_height = nullptr) {
        if (!gpu_surface_ || !skia_surface_) return false;

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
