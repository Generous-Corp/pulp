// Does hovering with NO button held change the cursor a person actually sees?
//
// The portable cases elsewhere drive -refreshHoverCursor on a view that was
// never put in a window and assert what the resolver computed, or merely that
// the view responds to a selector. That proves the hit-test picks a style; it
// cannot prove AppKit displays it. A host can resolve "text" correctly and
// still show an arrow. Everything a person complains about lives in that gap,
// so every case here reads back the APPLIED cursor (+[NSCursor currentCursor])
// from a real NSWindow, never the resolver's return value.
//
// METHOD, and the limit that shapes it. The obvious harness -- post a
// CGEvent mouse-move and watch the cursor change -- cannot work here, and
// silently returns the wrong answer if you assume it does. AppKit drives a
// hover cursor through its cursor pass (-cursorUpdate: on an
// NSTrackingCursorUpdate area), and that pass is NOT triggered by
// synthetically posted moves: measured with both kCGSessionEventTap and
// kCGHIDEventTap, and with the pointer associated and dissociated, -mouseMoved:
// arrives every time while -cursorUpdate: is never called. A host whose cursor
// logic lives in -cursorUpdate: therefore reads as "cursor never changes" under
// synthetic motion no matter how correct it is. That is a false FAIL, and it is
// exactly as useless as the false PASS this file exists to prevent.
//
// So the end-to-end claim is split into two halves that ARE machine-checkable,
// and neither is sufficient alone:
//   * AppKit is asked to run the pass -- a tracking area carrying
//     NSTrackingCursorUpdate is installed on the live view in the key window;
//   * when the pass runs, the host applies the RIGHT cursor to the screen --
//     the pass entry point is invoked at a chosen point and the resulting
//     +[NSCursor currentCursor] is read back, per region, with zero button
//     events.
//
// CONTROLS. A fixture case asserts the scene declares two different cursors
// before AppKit is involved at all, because a collapsed fixture makes every
// reading below "arrow" and looks precisely like a host bug. A negative control
// runs the identical measurement against a host whose cursor pass answers
// nothing and requires it to report "did not change"; a positive control runs a
// host that honors the contract and requires a change. A measurement that
// cannot fail is not evidence.
//
// WHAT THIS STILL DOES NOT COVER: that AppKit calls -cursorUpdate: for a real
// physical hover (no automation available here drives it, so that link is
// argued from the tracking area, not observed); the region BOUNDARY, since only
// two interior points are sampled; and the DAW-hosted AU/VST3 case, which is a
// different window and tracking-area situation and is not exercised at all.
//
// These cases take over the system pointer and need an idle machine; a human
// or another job moving the mouse corrupts them in both directions, so the
// measurement aborts as VOID rather than emitting a verdict when it detects
// the pointer is not where it put it.

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/view.hpp>

#include <memory>
#include <cmath>
#include <csignal>
#include <cstdlib>

@interface PulpView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
- (void)refreshHoverCursor;
@end

namespace {

class StubView : public pulp::view::View {
public:
    void paint(pulp::canvas::Canvas&) override {}
};

// Left half asks for a pointing hand, right half for a text bar, so a pointer
// crossing the midline must visibly change the cursor.
struct SplitRoot {
    StubView root;
    StubView* left = nullptr;
    StubView* right = nullptr;
    float w_ = 0, h_ = 0;

    explicit SplitRoot(float w, float h) : w_(w), h_(h) {
        auto l = std::make_unique<StubView>();
        l->set_cursor(pulp::view::View::CursorStyle::pointer);
        left = l.get();
        root.add_child(std::move(l));

        auto r = std::make_unique<StubView>();
        r->set_cursor(pulp::view::View::CursorStyle::text);
        right = r.get();
        root.add_child(std::move(r));
        apply();
    }

    // The host resizes the root to the NSView's size, and that layout pass
    // repositions children -- so the split has to be (re)stated after the
    // window has laid the view out, not only at construction.
    void apply() {
        root.set_bounds({0, 0, w_, h_});
        left->set_bounds({0, 0, w_ / 2, h_});
        right->set_bounds({w_ / 2, 0, w_ / 2, h_});
    }
};

const char* cursor_name(NSCursor* c) {
    if (c == nil) return "nil";
    if (c == [NSCursor arrowCursor]) return "arrow";
    if (c == [NSCursor IBeamCursor]) return "IBeam";
    if (c == [NSCursor pointingHandCursor]) return "pointingHand";
    if (c == [NSCursor crosshairCursor]) return "crosshair";
    return "other";
}

// Run AppKit for real: dequeue and dispatch events, then let the run loop turn.
// Reading the cursor without this measures the value just written rather than
// the value AppKit settled on, which is the difference between a false pass and
// a measurement.
void pump(double seconds) {
    NSDate* end = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([end timeIntervalSinceNow] > 0) {
        NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                         untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
                                            inMode:NSDefaultRunLoopMode
                                           dequeue:YES];
        if (ev) [NSApp sendEvent:ev];
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
    }
}

void reassociate_pointer() { CGAssociateMouseAndMouseCursorPosition(true); }

[[noreturn]] void reassociate_and_die(int sig) {
    reassociate_pointer();
    _exit(128 + sig);
}

// Detaching the cursor from the HID mouse makes synthetic positioning
// authoritative; without it the physical mouse keeps re-asserting its own
// location and samples land somewhere other than the point under test.
//
// A decoupled pointer that is never restored leaves the machine with a mouse
// that moves nothing, which costs the user a logout. Restoration is therefore
// wired to every exit path this process has -- scope exit, a thrown REQUIRE,
// normal termination, and a fatal signal -- not to the destructor alone.
struct PointerControl {
    PointerControl() {
        static const bool once = [] {
            std::atexit(reassociate_pointer);
            for (int sig : {SIGINT, SIGTERM, SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGQUIT})
                std::signal(sig, reassociate_and_die);
            return true;
        }();
        (void)once;
        CGAssociateMouseAndMouseCursorPosition(false);
    }
    ~PointerControl() { reassociate_pointer(); }
};

// A human touching the trackpad mid-measurement overrides the synthetic
// position, so the cursor is sampled at a point nobody chose. That corrupts the
// verdict in BOTH directions: a synthetic move that never lands reads as "the
// cursor never changed", and a real pointer resting somewhere live reads as a
// change this harness did not cause. Neither is visible in the cursor value, so
// contention is caught positionally and voids the run.
constexpr CGFloat kContendedSlopPx = 2.0;

bool pointer_is_where_we_put_it(CGPoint want) {
    CGEventRef e = CGEventCreate(NULL);
    CGPoint got = CGEventGetLocation(e);
    CFRelease(e);
    return std::fabs(got.x - want.x) <= kContendedSlopPx &&
           std::fabs(got.y - want.y) <= kContendedSlopPx;
}

// Can this process drive and observe a real window at all? A run with no window
// server, no Accessibility trust, or no observable cursor must skip rather than
// report a passing measurement of nothing.
bool live_window_session_available() {
    if (NSApp == nil) return false;
    if (!AXIsProcessTrusted()) return false;
    if ([NSScreen screens].count == 0) return false;
    [[NSCursor arrowCursor] set];
    if ([NSCursor currentCursor] != [NSCursor arrowCursor]) return false;
    [[NSCursor IBeamCursor] set];
    return [NSCursor currentCursor] == [NSCursor IBeamCursor];
}

void ensure_app() {
    if (NSApp == nil) {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    }
}

constexpr CGFloat kW = 400;
constexpr CGFloat kH = 300;

// A live window with `view` as its content, made key so a tracking area
// qualified NSTrackingActiveInKeyWindow is armed.
NSWindow* present(NSView* view) {
    NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(160, 160, kW, kH)
                                              styleMask:NSWindowStyleMaskTitled
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    [w setContentView:view];
    [w setAcceptsMouseMovedEvents:YES];
    [w makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    pump(0.6);
    return w;
}

// Tear the window down in the order the host expects. PulpView keeps a frame
// timer that dereferences state owned by the test scene, so the view has to be
// told to stand down BEFORE the scene goes out of scope; skipping this crashes
// once a second case runs in the same process.
void dismiss(NSWindow* w, NSView* v) {
    if ([v respondsToSelector:@selector(prepareForTeardown)])
        [v performSelector:@selector(prepareForTeardown)];
    [w orderOut:nil];
    [w setContentView:[[NSView alloc] initWithFrame:NSZeroRect]];
    [w close];
    pump(0.15);
}

// Move the pointer to a content-local point and let AppKit settle. The warp
// positions, the posted moved-event is what a tracking area reacts to. No
// button is pressed here or anywhere in this file outside the drag case.
// Sets *contended when a human moved the pointer during the sample, in which
// case the returned cursor means nothing and callers must not judge on it.
NSCursor* hover_at(NSWindow* w, NSView* v, CGFloat lx, CGFloat ly, bool* contended) {
    const CGFloat screen_h = [[NSScreen screens][0] frame].size.height;
    NSPoint in_win = [v convertPoint:NSMakePoint(lx, ly) toView:nil];
    NSRect on_screen = [w convertRectToScreen:NSMakeRect(in_win.x, in_win.y, 1, 1)];
    CGPoint cg = CGPointMake(on_screen.origin.x, screen_h - on_screen.origin.y);

    CGWarpMouseCursorPosition(cg);
    CGEventRef mv = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, cg, kCGMouseButtonLeft);
    CGEventPost(kCGSessionEventTap, mv);
    CFRelease(mv);
    pump(0.35);
    if (!pointer_is_where_we_put_it(cg)) { *contended = true; return nil; }
    NSCursor* c = [NSCursor currentCursor];
    // A move landing mid-read corrupts the sample just as thoroughly.
    if (!pointer_is_where_we_put_it(cg)) { *contended = true; return nil; }
    return c;
}

constexpr const char* kVoidMessage =
    "VOID: human input detected during the measurement (the pointer left the "
    "sampled point). No verdict is emitted. Re-run on an idle machine.";

// Counts every button event that reaches the view, so "no button was involved"
// is asserted from the view's own vantage rather than assumed from the fact
// that the test did not post one.
NSUInteger g_button_events = 0;

// Counts AppKit's cursor-pass callbacks so a failure distinguishes 'AppKit never
// asked' from 'the host answered with the wrong cursor'.
NSUInteger g_cursor_update_calls = 0;
NSUInteger g_mouse_moved_calls = 0;

}  // namespace

// The shipping view class, instrumented only to count button events.
@interface CursorProofPulpView : PulpView
@end
@implementation CursorProofPulpView
- (void)cursorUpdate:(NSEvent*)e { g_cursor_update_calls++; [super cursorUpdate:e]; }
- (void)mouseMoved:(NSEvent*)e { g_mouse_moved_calls++; [super mouseMoved:e]; }
- (void)mouseDown:(NSEvent*)e { g_button_events++; [super mouseDown:e]; }
- (void)mouseUp:(NSEvent*)e { g_button_events++; [super mouseUp:e]; }
- (void)mouseDragged:(NSEvent*)e { g_button_events++; [super mouseDragged:e]; }
@end

// A host that installs the tracking area but does nothing in its cursor pass.
// This is the NEGATIVE CONTROL for the measurement below: it must be reported
// as "did not change". A harness that cannot fail here cannot fail at all.
@interface InertCursorPassView : NSView
@property (nonatomic, strong) NSTrackingArea* ta;
@end
@implementation InertCursorPassView
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (self.ta) [self removeTrackingArea:self.ta];
    self.ta = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                      NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect |
                      NSTrackingCursorUpdate)
               owner:self
            userInfo:nil];
    [self addTrackingArea:self.ta];
}
- (void)cursorUpdate:(NSEvent*)e { (void)e; /* answers nothing */ }
- (void)drawRect:(NSRect)r { [[NSColor whiteColor] set]; NSRectFill(r); }
@end

// The AppKit cursor contract honored correctly: a tracking area that requests
// NSTrackingCursorUpdate, and a -cursorUpdate: that sets the cursor during
// AppKit's own cursor pass rather than fighting it from -mouseMoved:. This is
// the POSITIVE control. Without it, a failure in the live-window case is
// ambiguous: it could mean the host is broken, or it could mean this
// measurement is incapable of ever observing a cursor change in-process. This
// view is known-good by construction, so a PASS here proves the instrument
// works and makes the other case's failure a statement about the host.
@interface CorrectHoverView : NSView
@property (nonatomic, strong) NSTrackingArea* ta;
@property (nonatomic, assign) pulp::view::View* rootView;
@end
@implementation CorrectHoverView
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (self.ta) [self removeTrackingArea:self.ta];
    self.ta = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                      NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect |
                      NSTrackingCursorUpdate)
               owner:self
            userInfo:nil];
    [self addTrackingArea:self.ta];
}
- (NSCursor*)cursorForPointerLocation {
    NSPoint p = [self convertPoint:[self.window mouseLocationOutsideOfEventStream]
                          fromView:nil];
    pulp::view::Point pt{static_cast<float>(p.x),
                         static_cast<float>(self.bounds.size.height - p.y)};
    if (!self.rootView) return [NSCursor arrowCursor];
    if (auto* t = self.rootView->hit_test(pt)) {
        switch (t->cursor()) {
            case pulp::view::View::CursorStyle::pointer: return [NSCursor pointingHandCursor];
            case pulp::view::View::CursorStyle::text: return [NSCursor IBeamCursor];
            default: break;
        }
    }
    return [NSCursor arrowCursor];
}
- (void)cursorUpdate:(NSEvent*)e { (void)e; [[self cursorForPointerLocation] set]; }
- (void)mouseMoved:(NSEvent*)e { (void)e; [[self cursorForPointerLocation] set]; }
- (void)drawRect:(NSRect)r { [[NSColor whiteColor] set]; NSRectFill(r); }
@end

TEST_CASE("the hover measurement observes a cursor change when the host is correct",
          "[mac][cursor][hover][live][positive-control][issue-8121]") {
    ensure_app();
    if (!live_window_session_available())
        SKIP("no window-server session with Accessibility trust and an observable cursor");

    SplitRoot scene(kW, kH);
    CorrectHoverView* view = [[CorrectHoverView alloc] initWithFrame:NSMakeRect(0, 0, kW, kH)];
    view.rootView = &scene.root;
    NSWindow* w = present(view);
    scene.apply();

    PointerControl pointer;
    bool contended = false;
    NSCursor* left = hover_at(w, view, kW * 0.25, kH * 0.5, &contended);
    NSCursor* right = contended ? nil : hover_at(w, view, kW * 0.75, kH * 0.5, &contended);
    dismiss(w, view);
    if (contended) FAIL(kVoidMessage);

    INFO("correct control, left: " << cursor_name(left) << " right: " << cursor_name(right));

    // A failure here does NOT mean the host is broken -- it means this
    // measurement cannot see a hover cursor change at all, and every other
    // verdict in this file is void.
    CHECK(left != right);
    CHECK(left == [NSCursor pointingHandCursor]);
    CHECK(right == [NSCursor IBeamCursor]);
}

TEST_CASE("the split fixture declares two different cursors before AppKit is involved",
          "[mac][cursor][hover][fixture][issue-8121]") {
    SplitRoot scene(kW, kH);
    auto* l = scene.root.hit_test({kW * 0.25f, kH * 0.5f});
    auto* r = scene.root.hit_test({kW * 0.75f, kH * 0.5f});
    REQUIRE(l != nullptr);
    REQUIRE(r != nullptr);
    INFO("left style " << static_cast<int>(l->cursor())
         << " right style " << static_cast<int>(r->cursor()));
    CHECK(l->cursor() == pulp::view::View::CursorStyle::pointer);
    CHECK(r->cursor() == pulp::view::View::CursorStyle::text);
}

// Synthesizes the callback AppKit makes during its cursor pass, at a chosen
// content-local point, and returns the cursor the host actually applied.
// Synthetic posted moves do not drive AppKit's cursor pass (see the file
// header), so this invokes the entry point directly and then reads the applied
// cursor -- still a screen-level readback, not the resolver's return value.
static NSCursor* applied_cursor_for_cursor_pass(NSWindow* w, NSView* v,
                                                CGFloat lx, CGFloat ly) {
    [[NSCursor arrowCursor] set];  // so a no-op host is visibly a no-op
    NSPoint win = [v convertPoint:NSMakePoint(lx, v.bounds.size.height - ly) toView:nil];
    // NSEventTypeCursorUpdate is not constructible via mouseEventWithType:
    // (AppKit rejects it); the handler reads only locationInWindow.
    NSEvent* e = [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                                    location:win
                               modifierFlags:0
                                   timestamp:[[NSProcessInfo processInfo] systemUptime]
                                windowNumber:[w windowNumber]
                                     context:nil
                                 eventNumber:0
                                  clickCount:0
                                    pressure:0];
    [(id)v cursorUpdate:e];
    return [NSCursor currentCursor];
}

TEST_CASE("the host applies the right cursor when AppKit runs its cursor pass",
          "[mac][cursor][hover][live][issue-8121]") {
    ensure_app();
    if (!live_window_session_available())
        SKIP("no window-server session with Accessibility trust and an observable cursor");
    Class cls = NSClassFromString(@"PulpView");
    if (cls == nil) SKIP("PulpView is not registered in this binary");

    SplitRoot scene(kW, kH);
    CursorProofPulpView* view =
        [[CursorProofPulpView alloc] initWithFrame:NSMakeRect(0, 0, kW, kH)];
    view.rootView = &scene.root;
    NSWindow* w = present(view);
    scene.apply();
    g_button_events = 0;

    // Scaffolding controls: if these fail, the cursor readings below say
    // nothing about the host.
    REQUIRE(view.rootView == &scene.root);
    REQUIRE(view.rootView->hit_test({kW * 0.25f, kH * 0.5f}) != nullptr);
    REQUIRE(view.rootView->hit_test({kW * 0.25f, kH * 0.5f})->cursor()
            == pulp::view::View::CursorStyle::pointer);

    NSCursor* left = applied_cursor_for_cursor_pass(w, view, kW * 0.25, kH * 0.5);
    NSCursor* right = applied_cursor_for_cursor_pass(w, view, kW * 0.75, kH * 0.5);
    dismiss(w, view);

    INFO("left: " << cursor_name(left) << "  right: " << cursor_name(right));
    CHECK(left != right);
    CHECK(left == [NSCursor pointingHandCursor]);
    CHECK(right == [NSCursor IBeamCursor]);
    CHECK(g_button_events == 0);
}

TEST_CASE("the live window carries a tracking area that asks AppKit for the cursor pass",
          "[mac][cursor][hover][live][issue-8121]") {
    ensure_app();
    if (!live_window_session_available())
        SKIP("no window-server session with Accessibility trust and an observable cursor");
    Class cls = NSClassFromString(@"PulpView");
    if (cls == nil) SKIP("PulpView is not registered in this binary");

    SplitRoot scene(kW, kH);
    CursorProofPulpView* view =
        [[CursorProofPulpView alloc] initWithFrame:NSMakeRect(0, 0, kW, kH)];
    view.rootView = &scene.root;
    g_cursor_update_calls = 0;
    g_mouse_moved_calls = 0;
    NSWindow* w = present(view);
    scene.apply();

    NSArray<NSTrackingArea*>* areas = [view trackingAreas];
    BOOL asks_for_cursor_update = NO;
    for (NSTrackingArea* a in areas)
        if (a.options & NSTrackingCursorUpdate) asks_for_cursor_update = YES;
    const BOOL in_key_window = [w isKeyWindow];
    dismiss(w, view);

    // Reported, not asserted: AppKit typically runs its cursor pass while the
    // window is being activated, which is the only direct observation available
    // here that the pass reaches this view at all. It is not asserted because
    // the count depends on where the pointer happens to be sitting.
    INFO("tracking areas " << [areas count] << ", key window " << (int)in_key_window
         << ", cursor-pass callbacks during activation " << g_cursor_update_calls
         << ", moves " << g_mouse_moved_calls);
    // Together with the case above, this is what makes the end-to-end claim:
    // AppKit is asked to run the pass, and the host answers it correctly.
    CHECK([areas count] > 0);
    CHECK(asks_for_cursor_update);
    CHECK(in_key_window);
}


TEST_CASE("the cursor-pass measurement reports a host with an inert cursor pass",
          "[mac][cursor][hover][live][negative-control][issue-8121]") {
    ensure_app();
    if (!live_window_session_available())
        SKIP("no window-server session with Accessibility trust and an observable cursor");

    InertCursorPassView* view =
        [[InertCursorPassView alloc] initWithFrame:NSMakeRect(0, 0, kW, kH)];
    NSWindow* w = present(view);

    NSCursor* left = applied_cursor_for_cursor_pass(w, view, kW * 0.25, kH * 0.5);
    NSCursor* right = applied_cursor_for_cursor_pass(w, view, kW * 0.75, kH * 0.5);
    dismiss(w, view);

    INFO("inert host, left: " << cursor_name(left) << "  right: " << cursor_name(right));
    // The defect this whole file exists to catch: the cursor does not change.
    CHECK(left == right);
    CHECK(left == [NSCursor arrowCursor]);
}
