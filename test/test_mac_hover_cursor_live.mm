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
// interior points are sampled; the DAW-hosted AU/VST3 case, which is a
// different window and tracking-area situation and is not exercised at all;
// and one known host property that is measured but not judged here: the
// cursor pass (-cursorUpdate:) resolves from the hit view's cursor slot
// without first delivering a hover sample, so on a surface that picks its
// cursor from its own hover handler a pass that runs at a point the pointer
// has not yet MOVED over publishes the slot's previous value until the next
// -mouseMoved: lands. The filter-surface cases below therefore sample the pass
// only at the point the last move landed on.
//
// These cases take over the system pointer and need an idle machine; a human
// or another job moving the mouse corrupts them in both directions, so the
// measurement aborts as VOID rather than emitting a verdict when it detects
// the pointer is not where it put it.

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/hover_cursor.hpp>
#include <pulp/view/pointer_dispatch.hpp>
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
    if (c == [NSCursor openHandCursor]) return "openHand";
    if (c == [NSCursor closedHandCursor]) return "closedHand";
    if (c == [NSCursor resizeLeftRightCursor]) return "resizeLeftRight";
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


// One region covering the whole view, asking for an open hand while nothing is
// pressed and switching to a closed hand from its own press handler. This is
// what makes the two paths distinguishable: the host resolves a hover from the
// hit-tested view BEFORE any handler runs, and resolves a press from the
// captured view AFTER, so a correct host must show two different cursors here
// at one single point.
class GrabView : public StubView {
public:
    int presses = 0;
    void on_mouse_down(pulp::view::Point) override {
        presses++;
        set_cursor(pulp::view::View::CursorStyle::grabbing);
    }
};

struct GrabRoot {
    StubView root;
    GrabView* grabber = nullptr;
    float w_ = 0, h_ = 0;

    explicit GrabRoot(float w, float h) : w_(w), h_(h) {
        auto g = std::make_unique<GrabView>();
        g->set_cursor(pulp::view::View::CursorStyle::grab);
        grabber = g.get();
        root.add_child(std::move(g));
        apply();
    }

    void apply() {
        root.set_bounds({0, 0, w_, h_});
        grabber->set_bounds({0, 0, w_, h_});
    }
};

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
// The hover and the press readings below are only comparable if they name the
// same Pulp point, so both go through this one conversion. Pulp's root space is
// top-left origin; the NSView's is bottom-left.
static NSPoint pulp_point_in_window(NSView* v, CGFloat lx, CGFloat ly) {
    return [v convertPoint:NSMakePoint(lx, v.bounds.size.height - ly) toView:nil];
}

// Synthetic posted moves do not drive AppKit's cursor pass (see the file
// header), so this invokes the entry point directly and then reads the applied
// cursor -- still a screen-level readback, not the resolver's return value.
static NSCursor* applied_cursor_for_cursor_pass(NSWindow* w, NSView* v,
                                                CGFloat lx, CGFloat ly) {
    [[NSCursor arrowCursor] set];  // so a no-op host is visibly a no-op
    NSPoint win = pulp_point_in_window(v, lx, ly);
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

// ---------------------------------------------------------------------------
// Hover versus press.
//
// Everything above measures the cursor a person sees with NO button held. That
// is one of two paths, and they are not the same code: the hover pass answers
// from the view under the pointer as hit-testing found it, while a press
// answers from the view the press was captured on, after its handler ran. A
// control whose handler switches its own cursor -- the ordinary open-hand to
// closed-hand of a drag -- therefore shows one cursor on hover and a different
// one on press, at one single point.
//
// That divergence is the thing worth measuring, because it is how "the cursor
// changes when I click but not when I hover" happens: the press path publishes
// unconditionally at every pointer phase, so it keeps working while the hover
// pass is broken, and the defect hides behind a control that still feels alive
// under the finger.
//
// Both readings here are +[NSCursor currentCursor] after invoking the shipping
// host's own entry point, and each carries the button count AppKit's view saw,
// so a passing case has named which path it exercised rather than assuming it.

static NSCursor* applied_cursor_for_press(NSWindow* w, NSView* v,
                                          CGFloat lx, CGFloat ly) {
    [[NSCursor arrowCursor] set];  // so a host that publishes nothing is visible
    NSPoint win = pulp_point_in_window(v, lx, ly);
    const auto make = [&](NSEventType type, NSInteger clicks) {
        return [NSEvent mouseEventWithType:type
                                  location:win
                             modifierFlags:0
                                 timestamp:[[NSProcessInfo processInfo] systemUptime]
                              windowNumber:[w windowNumber]
                                   context:nil
                               eventNumber:0
                                clickCount:clicks
                                  pressure:1.0];
    };
    [(id)v mouseDown:make(NSEventTypeLeftMouseDown, 1)];
    NSCursor* applied = [NSCursor currentCursor];
    // Leave the host un-captured; a view left mid-drag poisons any later case.
    [(id)v mouseUp:make(NSEventTypeLeftMouseUp, 1)];
    return applied;
}

TEST_CASE("the press fixture switches its own cursor, before AppKit is involved",
          "[mac][cursor][press][live][issue-8121]") {
    // Fixture control. If the scene does not actually declare two cursors, both
    // readings in the case below collapse to one value and that reads exactly
    // like a host that ignores the press -- the false FAIL this guards against.
    GrabRoot scene(kW, kH);
    const pulp::view::Point p{kW * 0.5f, kH * 0.5f};

    REQUIRE(pulp::view::hover_cursor_at(scene.root, p)
            == pulp::view::View::CursorStyle::grab);
    REQUIRE(pulp::view::deliver_mouse_down(scene.root, scene.root.hit_test(p), p, 0));
    REQUIRE(scene.grabber->presses == 1);
    REQUIRE(scene.grabber->cursor() == pulp::view::View::CursorStyle::grabbing);
}

TEST_CASE("hover and press put different cursors on screen at one point",
          "[mac][cursor][press][hover][live][issue-8121]") {
    ensure_app();
    if (!live_window_session_available())
        SKIP("no window-server session with Accessibility trust and an observable cursor");
    Class cls = NSClassFromString(@"PulpView");
    if (cls == nil) SKIP("PulpView is not registered in this binary");

    GrabRoot scene(kW, kH);
    CursorProofPulpView* view =
        [[CursorProofPulpView alloc] initWithFrame:NSMakeRect(0, 0, kW, kH)];
    view.rootView = &scene.root;
    NSWindow* w = present(view);
    scene.apply();

    // Scaffolding controls: without these the cursor readings say nothing.
    REQUIRE(view.rootView == &scene.root);
    REQUIRE(view.rootView->hit_test({kW * 0.5f, kH * 0.5f}) == scene.grabber);

    g_button_events = 0;
    NSCursor* hovered = applied_cursor_for_cursor_pass(w, view, kW * 0.5, kH * 0.5);
    const NSUInteger buttons_during_hover = g_button_events;

    g_button_events = 0;
    NSCursor* pressed = applied_cursor_for_press(w, view, kW * 0.5, kH * 0.5);
    const NSUInteger buttons_during_press = g_button_events;
    dismiss(w, view);

    INFO("hover: " << cursor_name(hovered) << "  press: " << cursor_name(pressed));
    // Which path each reading came from, asserted from the view's own vantage
    // rather than from the fact that the test meant to drive one of them.
    CHECK(buttons_during_hover == 0);
    CHECK(buttons_during_press > 0);

    CHECK(hovered == [NSCursor openHandCursor]);
    CHECK(pressed == [NSCursor closedHandCursor]);
    CHECK(hovered != pressed);
    CHECK(scene.grabber->presses == 1);
}

TEST_CASE("a target that does not switch shows the same cursor on both paths",
          "[mac][cursor][press][hover][live][negative-control][issue-8121]") {
    // Negative control for the case above. "Hover and press differ" is only
    // evidence about the fixture if the press path is otherwise capable of
    // agreeing with hover -- a press path that published its own unrelated
    // value, or nothing at all, would also produce a difference.
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

    REQUIRE(view.rootView->hit_test({kW * 0.25f, kH * 0.5f}) == scene.left);

    NSCursor* hovered = applied_cursor_for_cursor_pass(w, view, kW * 0.25, kH * 0.5);
    NSCursor* pressed = applied_cursor_for_press(w, view, kW * 0.25, kH * 0.5);
    dismiss(w, view);

    INFO("hover: " << cursor_name(hovered) << "  press: " << cursor_name(pressed));
    CHECK(hovered == [NSCursor pointingHandCursor]);
    CHECK(pressed == [NSCursor pointingHandCursor]);
    CHECK(hovered == pressed);
}

// ---------------------------------------------------------------------------
// A surface that decides its own cursor from where the pointer is INSIDE it.
//
// The fixtures above give each region its own view with a fixed style, so the
// hover pass can answer from hit_test alone. Real editing surfaces are not
// built that way: a spectrum plot with an inline minimap is ONE view whose
// hover handler flips its cursor between crosshair (the plot), an open hand
// (the movable viewport window), a left-right resize (the window's trim
// handles) and back, and whose press handler flips the open hand to a closed
// one. Every one of those cursors lives in the same View slot, so a hover
// path that reads the slot without first delivering the hover sample shows
// whatever the LAST handler wrote -- which is how a trim cursor comes to
// appear only after a click, or a closed hand to linger after release.
//
// So these cases enter through the host's real -mouseMoved:, which delivers
// the hover sample (simulate_hover -> on_hover_move) and THEN resolves, rather
// than through -cursorUpdate:, which resolves from the slot alone. The readback
// is still the applied +[NSCursor currentCursor]. The sentinel set before each
// reading is the I-beam, which none of the expected cursors resolve to, so a
// host that publishes nothing is visible as "IBeam" rather than hiding behind
// an arrow that a `default` region would legitimately produce.

namespace {

constexpr float kMinimapTop = 220.0f;   // plot above, minimap strip below
constexpr float kWindowLeft = 120.0f;   // viewport window inside the minimap
constexpr float kWindowRight = 280.0f;
constexpr float kTrimWidth = 8.0f;      // resize handles inside each window edge

// The points the cases sample, one per region, all well inside their region so
// the assertion is about the mapping and not about the boundary.
constexpr float kPlotX = 200, kPlotY = 100;
constexpr float kWindowX = 200, kWindowY = 260;
constexpr float kLeftTrimX = kWindowLeft + kTrimWidth * 0.5f;
constexpr float kRightTrimX = kWindowRight - kTrimWidth * 0.5f;
constexpr float kTrackX = 40;

class FilterSurface : public StubView {
public:
    int hovers = 0, presses = 0, drags = 0, releases = 0;

    using CS = pulp::view::View::CursorStyle;

    static CS region_cursor(pulp::view::Point local) {
        if (local.y < kMinimapTop) return CS::crosshair;
        if (local.x >= kWindowLeft && local.x < kWindowLeft + kTrimWidth) return CS::horizontal_resize;
        if (local.x >= kWindowRight - kTrimWidth && local.x < kWindowRight) return CS::horizontal_resize;
        if (local.x >= kWindowLeft && local.x < kWindowRight) return CS::grab;
        return CS::default_;
    }

    void on_hover_move(pulp::view::Point local) override {
        hovers++;
        if (!dragging_) set_cursor(region_cursor(local));
    }
    void on_mouse_down(pulp::view::Point local) override {
        presses++;
        dragging_ = true;
        // Only the movable window grabs; a trim keeps its resize cursor and the
        // plot keeps its crosshair for the duration of the press.
        if (region_cursor(local) == CS::grab) set_cursor(CS::grabbing);
    }
    void on_mouse_drag(pulp::view::Point) override { drags++; }
    void on_mouse_up(pulp::view::Point local) override {
        releases++;
        dragging_ = false;
        set_cursor(region_cursor(local));
    }

private:
    bool dragging_ = false;
};

struct FilterRoot {
    StubView root;
    FilterSurface* surface = nullptr;
    float w_ = 0, h_ = 0;

    explicit FilterRoot(float w, float h) : w_(w), h_(h) {
        auto s = std::make_unique<FilterSurface>();
        s->set_cursor(FilterSurface::CS::crosshair);
        surface = s.get();
        root.add_child(std::move(s));
        apply();
    }
    void apply() {
        root.set_bounds({0, 0, w_, h_});
        surface->set_bounds({0, 0, w_, h_});
    }
};

NSEvent* synth_mouse_event(NSWindow* w, NSView* v, NSEventType type,
                           CGFloat lx, CGFloat ly, NSInteger clicks, float pressure) {
    return [NSEvent mouseEventWithType:type
                              location:pulp_point_in_window(v, lx, ly)
                         modifierFlags:0
                             timestamp:[[NSProcessInfo processInfo] systemUptime]
                          windowNumber:[w windowNumber]
                               context:nil
                           eventNumber:0
                            clickCount:clicks
                              pressure:pressure];
}

// Reset to a sentinel no region resolves to, drive ONE entry point of the
// shipping host, and read back what AppKit was handed.
NSCursor* applied_after(NSView* v, SEL entry, NSEvent* e) {
    [[NSCursor IBeamCursor] set];
    [(id)v performSelector:entry withObject:e];
    // A drag sample is held for the next presented frame; deliver it now so
    // the reading is of the drag's publish, not of the press that preceded it.
    if (entry == @selector(mouseDragged:)
        && [v respondsToSelector:@selector(flushCoalescedPointerInput)])
        [(id)v performSelector:@selector(flushCoalescedPointerInput)];
    return [NSCursor currentCursor];
}

NSCursor* applied_after_hover_move(NSWindow* w, NSView* v, CGFloat lx, CGFloat ly) {
    return applied_after(v, @selector(mouseMoved:),
                         synth_mouse_event(w, v, NSEventTypeMouseMoved, lx, ly, 0, 0));
}

}  // namespace

TEST_CASE("the filter-surface fixture resolves each region's cursor before AppKit is involved",
          "[mac][cursor][hover][press][fixture][issue-8121]") {
    // Fixture control: if the surface does not actually change its own slot
    // per region, every live reading below collapses to crosshair and the
    // cases read like a host that ignores hover -- the false FAIL this guards.
    using CS = pulp::view::View::CursorStyle;
    FilterRoot scene(kW, kH);
    auto hover = [&](float x, float y) {
        scene.root.simulate_hover({x, y});
        return pulp::view::hover_cursor_at(scene.root, {x, y});
    };
    REQUIRE(scene.root.hit_test({kPlotX, kPlotY}) == scene.surface);
    CHECK(hover(kPlotX, kPlotY) == CS::crosshair);
    CHECK(hover(kWindowX, kWindowY) == CS::grab);
    CHECK(hover(kLeftTrimX, kWindowY) == CS::horizontal_resize);
    CHECK(hover(kRightTrimX, kWindowY) == CS::horizontal_resize);
    CHECK(hover(kTrackX, kWindowY) == CS::default_);
    CHECK(hover(kPlotX, kPlotY) == CS::crosshair);

    // Hover alone must never produce the closed hand.
    CHECK(hover(kWindowX, kWindowY) == CS::grab);
    CHECK(scene.surface->presses == 0);

    const pulp::view::Point p{kWindowX, kWindowY};
    REQUIRE(pulp::view::deliver_mouse_down(scene.root, scene.root.hit_test(p), p, 0));
    CHECK(scene.surface->cursor() == CS::grabbing);
    pulp::view::MouseUpHost up_host;
    pulp::view::deliver_mouse_up(scene.root, scene.surface, p, 0, 1, up_host);
    CHECK(scene.surface->cursor() == CS::grab);
    CHECK(scene.surface->presses == 1);
    CHECK(scene.surface->releases == 1);
}

TEST_CASE("hover alone puts crosshair, open hand and left-right resize on screen",
          "[mac][cursor][hover][live][issue-8121]") {
    ensure_app();
    if (!live_window_session_available())
        SKIP("no window-server session with Accessibility trust and an observable cursor");
    Class cls = NSClassFromString(@"PulpView");
    if (cls == nil) SKIP("PulpView is not registered in this binary");

    FilterRoot scene(kW, kH);
    CursorProofPulpView* view =
        [[CursorProofPulpView alloc] initWithFrame:NSMakeRect(0, 0, kW, kH)];
    view.rootView = &scene.root;
    NSWindow* w = present(view);
    scene.apply();

    REQUIRE(view.rootView->hit_test({kPlotX, kPlotY}) == scene.surface);
    g_button_events = 0;
    const int hovers_before = scene.surface->hovers;

    NSCursor* plot = applied_after_hover_move(w, view, kPlotX, kPlotY);
    NSCursor* window = applied_after_hover_move(w, view, kWindowX, kWindowY);
    NSCursor* left_trim = applied_after_hover_move(w, view, kLeftTrimX, kWindowY);
    // Once a move has landed, AppKit's own cursor pass at that same point must
    // agree with it -- the two hover entry points read one slot. (The pass
    // reads the slot WITHOUT delivering a hover sample, so this agreement holds
    // only at the point the last move landed on; see the file header.)
    NSCursor* left_trim_pass = applied_cursor_for_cursor_pass(w, view, kLeftTrimX, kWindowY);
    NSCursor* right_trim = applied_after_hover_move(w, view, kRightTrimX, kWindowY);
    NSCursor* track = applied_after_hover_move(w, view, kTrackX, kWindowY);
    NSCursor* plot_again = applied_after_hover_move(w, view, kPlotX, kPlotY);
    dismiss(w, view);

    INFO("plot: " << cursor_name(plot) << "  window: " << cursor_name(window)
         << "  left trim: " << cursor_name(left_trim)
         << "  right trim: " << cursor_name(right_trim)
         << "  track: " << cursor_name(track)
         << "  plot again: " << cursor_name(plot_again)
         << "  left trim via cursor pass: " << cursor_name(left_trim_pass));

    // The hover sample reached the surface, and no button was ever involved --
    // asserted from the view's own vantage, not from what the test meant to do.
    CHECK(scene.surface->hovers > hovers_before);
    CHECK(g_button_events == 0);
    CHECK(scene.surface->presses == 0);

    CHECK(plot == [NSCursor crosshairCursor]);
    CHECK(window == [NSCursor openHandCursor]);
    CHECK(window != [NSCursor closedHandCursor]);
    CHECK(left_trim == [NSCursor resizeLeftRightCursor]);
    CHECK(right_trim == [NSCursor resizeLeftRightCursor]);
    CHECK(track == [NSCursor arrowCursor]);
    CHECK(plot_again == [NSCursor crosshairCursor]);
    CHECK(left_trim_pass == [NSCursor resizeLeftRightCursor]);
}

TEST_CASE("the closed hand appears only while a button is held over the viewport",
          "[mac][cursor][hover][press][live][issue-8121]") {
    ensure_app();
    if (!live_window_session_available())
        SKIP("no window-server session with Accessibility trust and an observable cursor");
    Class cls = NSClassFromString(@"PulpView");
    if (cls == nil) SKIP("PulpView is not registered in this binary");

    FilterRoot scene(kW, kH);
    CursorProofPulpView* view =
        [[CursorProofPulpView alloc] initWithFrame:NSMakeRect(0, 0, kW, kH)];
    view.rootView = &scene.root;
    NSWindow* w = present(view);
    scene.apply();
    REQUIRE(view.rootView->hit_test({kWindowX, kWindowY}) == scene.surface);

    // Hover over the window with nothing pressed.
    g_button_events = 0;
    NSCursor* hovered = applied_after_hover_move(w, view, kWindowX, kWindowY);
    const NSUInteger buttons_during_hover = g_button_events;

    // Press. The host publishes the captured target's post-handler
    // style; AppKit sends no cursor pass while this view owns the drag.
    g_button_events = 0;
    NSCursor* pressed = applied_after(
        view, @selector(mouseDown:),
        synth_mouse_event(w, view, NSEventTypeLeftMouseDown, kWindowX, kWindowY, 1, 1.0f));
    const NSUInteger buttons_during_press = g_button_events;

    // Drag, still held. The closed hand must survive the drag publish.
    g_button_events = 0;
    NSCursor* dragged = applied_after(
        view, @selector(mouseDragged:),
        synth_mouse_event(w, view, NSEventTypeLeftMouseDragged, kWindowX + 30, kWindowY, 1, 1.0f));
    const NSUInteger buttons_during_drag = g_button_events;

    // Release over the window. The open hand must come back without
    // waiting for the pointer to move.
    g_button_events = 0;
    NSCursor* released = applied_after(
        view, @selector(mouseUp:),
        synth_mouse_event(w, view, NSEventTypeLeftMouseUp, kWindowX + 30, kWindowY, 1, 0.0f));
    const NSUInteger buttons_during_release = g_button_events;

    // A later hover back over the plot is a crosshair again -- the
    // drag left nothing behind.
    g_button_events = 0;
    NSCursor* after = applied_after_hover_move(w, view, kPlotX, kPlotY);
    const NSUInteger buttons_after = g_button_events;
    dismiss(w, view);

    INFO("hover: " << cursor_name(hovered) << "  press: " << cursor_name(pressed)
         << "  drag: " << cursor_name(dragged) << "  release: " << cursor_name(released)
         << "  hover after: " << cursor_name(after));

    CHECK(buttons_during_hover == 0);
    CHECK(buttons_during_press > 0);
    CHECK(buttons_during_drag > 0);
    CHECK(buttons_during_release > 0);
    CHECK(buttons_after == 0);
    CHECK(scene.surface->presses == 1);
    CHECK(scene.surface->drags == 1);
    CHECK(scene.surface->releases == 1);

    CHECK(hovered == [NSCursor openHandCursor]);
    CHECK(pressed == [NSCursor closedHandCursor]);
    CHECK(dragged == [NSCursor closedHandCursor]);
    CHECK(released == [NSCursor openHandCursor]);
    CHECK(after == [NSCursor crosshairCursor]);
    CHECK(hovered != pressed);
}
