// macOS hover-cursor updates for a STATIONARY pointer.
//
// AppKit decides which cursor to show on pointer EDGES: it sends -cursorUpdate:
// when the pointer crosses into a tracking area and re-asks as the pointer
// moves. It never re-asks because the content under a still pointer changed. So
// a layout pass that slides a different widget under the pointer — a panel
// opening, a list reflowing, a script mutating style — leaves the previous
// cursor on screen until the user jiggles the mouse or clicks.
//
// The hosts close that gap by remembering the pointer position and re-resolving
// from their frame path via -refreshHoverCursor. These cases drive that method
// directly and assert the cursor AppKit is told to display changes with NO
// synthesized mouse move and NO click in between.
//
// They also pin the other half: PulpView must implement -cursorUpdate: (and ask
// for NSTrackingCursorUpdate) so AppKit's own cursor pass adopts the view's
// answer instead of resetting to the arrow after each mouse-moved.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/view.hpp>

#include <memory>

// Runtime-looked-up interfaces for the Obj-C classes the mac hosts declare.
@interface PulpView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
- (void)refreshHoverCursor;
@end

@interface PulpPluginView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
- (void)refreshHoverCursor;
@end

namespace {

class StubView : public pulp::view::View {
public:
    void paint(pulp::canvas::Canvas&) override {}
};

// Two side-by-side children with different cursor styles, so a bounds swap can
// change which one owns a fixed point.
struct SplitRoot {
    StubView root;
    StubView* left = nullptr;
    StubView* right = nullptr;

    SplitRoot() {
        root.set_bounds({0, 0, 100, 100});
        auto l = std::make_unique<StubView>();
        l->set_bounds({0, 0, 50, 100});
        l->set_cursor(pulp::view::View::CursorStyle::pointer);
        left = l.get();
        root.add_child(std::move(l));

        auto r = std::make_unique<StubView>();
        r->set_bounds({50, 0, 50, 100});
        r->set_cursor(pulp::view::View::CursorStyle::text);
        right = r.get();
        root.add_child(std::move(r));
    }

    // Slide `right` under the whole surface without moving anything else.
    void swap_under_pointer() {
        left->set_bounds({-50, 0, 50, 100});
        right->set_bounds({0, 0, 100, 100});
    }
};

// nil when the class is not registered in this binary, in which case callers
// skip rather than false-fail.
id make_host_view(NSString* class_name) {
    Class cls = NSClassFromString(class_name);
    if (cls == nil) return nil;
    return [[[cls alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)] autorelease];
}

// A mouse-moved event at a point in the view's own (top-down) coordinates.
// Used only to seed the pointer position; every assertion below happens with
// no further event.
NSEvent* mouse_moved_at(NSView* view, CGFloat local_x, CGFloat local_y) {
    const CGFloat flipped_y = view.bounds.size.height - local_y;
    return [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                              location:NSMakePoint(local_x, flipped_y)
                         modifierFlags:0
                             timestamp:0
                          windowNumber:0
                               context:nil
                           eventNumber:0
                            clickCount:0
                              pressure:0];
}

// Can this process observe +[NSCursor currentCursor] at all? A headless test
// run under some session types cannot, and a test that cannot see the cursor
// must skip rather than report a passing measurement of nothing.
bool cursor_state_is_observable() {
    [[NSCursor arrowCursor] set];
    if ([NSCursor currentCursor] != [NSCursor arrowCursor]) return false;
    [[NSCursor IBeamCursor] set];
    return [NSCursor currentCursor] == [NSCursor IBeamCursor];
}

}  // namespace

TEST_CASE("PulpView implements the AppKit cursor-update callback", "[mac][cursor]") {
    SplitRoot scene;
    PulpView* view = (PulpView*)make_host_view(@"PulpView");
    if (view == nil) SKIP("PulpView is not registered in this binary");
    view.rootView = &scene.root;

    // Structural precondition only. Without -cursorUpdate: AppKit has no way to
    // ask the view what to show and its own cursor pass resets to the arrow
    // after each mouse-moved, so the selector has to exist — but its presence
    // says nothing about whether a hover ever produces a new cursor VALUE, and
    // reading it as though it did is how a hover defect once passed review. The
    // property itself is asserted in test_mac_hover_cursor_delivery.mm, which
    // drives a real -mouseMoved: with no button held and reads back
    // +[NSCursor currentCursor].
    REQUIRE([view respondsToSelector:@selector(cursorUpdate:)]);
    REQUIRE([view respondsToSelector:@selector(refreshHoverCursor)]);
}

TEST_CASE("a stationary pointer over PulpView gets the new region's cursor",
          "[mac][cursor]") {
    if (!cursor_state_is_observable())
        SKIP("+[NSCursor currentCursor] is not observable in this session");

    SplitRoot scene;
    PulpView* view = (PulpView*)make_host_view(@"PulpView");
    if (view == nil) SKIP("PulpView is not registered in this binary");
    view.rootView = &scene.root;

    // Seed the pointer over the left child. This is setup, not the property
    // under test.
    [view mouseMoved:mouse_moved_at(view, 25, 50)];
    REQUIRE([NSCursor currentCursor] == [NSCursor pointingHandCursor]);

    // Content moves under the pointer. The pointer does not move: no mouse
    // event of any kind is delivered from here on.
    scene.swap_under_pointer();

    // Prove the next assertion comes from the refresh rather than from cursor
    // state left behind by the seed event.
    [[NSCursor arrowCursor] set];
    REQUIRE([NSCursor currentCursor] == [NSCursor arrowCursor]);

    [view refreshHoverCursor];

    REQUIRE([NSCursor currentCursor] == [NSCursor IBeamCursor]);
}

TEST_CASE("an unchanged hovered region does not re-push the cursor every frame",
          "[mac][cursor]") {
    if (!cursor_state_is_observable())
        SKIP("+[NSCursor currentCursor] is not observable in this session");

    SplitRoot scene;
    PulpView* view = (PulpView*)make_host_view(@"PulpView");
    if (view == nil) SKIP("PulpView is not registered in this binary");
    view.rootView = &scene.root;

    [view mouseMoved:mouse_moved_at(view, 25, 50)];
    REQUIRE([NSCursor currentCursor] == [NSCursor pointingHandCursor]);

    // The refresh runs once per rendered frame, so it must be a no-op when the
    // answer has not changed: it must not fight an NSCursor another part of the
    // app pushed.
    [[NSCursor crosshairCursor] set];
    [view refreshHoverCursor];
    REQUIRE([NSCursor currentCursor] == [NSCursor crosshairCursor]);
}

TEST_CASE("a stationary pointer over the plug-in editor view gets the new cursor",
          "[mac][cursor]") {
    if (!cursor_state_is_observable())
        SKIP("+[NSCursor currentCursor] is not observable in this session");

    SplitRoot scene;
    PulpPluginView* view = (PulpPluginView*)make_host_view(@"PulpPluginView");
    if (view == nil) SKIP("PulpPluginView is not registered in this binary");
    view.rootView = &scene.root;

    [view mouseMoved:mouse_moved_at(view, 25, 50)];
    REQUIRE([NSCursor currentCursor] == [NSCursor pointingHandCursor]);

    scene.swap_under_pointer();

    [[NSCursor arrowCursor] set];
    REQUIRE([NSCursor currentCursor] == [NSCursor arrowCursor]);

    [view refreshHoverCursor];

    REQUIRE([NSCursor currentCursor] == [NSCursor IBeamCursor]);
}
