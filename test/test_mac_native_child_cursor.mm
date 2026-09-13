// macOS hover cursor when an ATTACHED NATIVE CHILD view owns the pointer.
//
// A Pulp host can embed a foreign NSView — a WKWebView, a hosted plug-in
// editor, any platform control — as a subview via attach_native_child_view.
// That child is NOT in the Pulp View tree, and it owns its own cursor: WebKit
// sets an I-beam over text and a pointing hand over a link, exactly as it does
// in Safari.
//
// A macOS tracking area is not occluded by subviews, so the Pulp host view
// still receives -mouseMoved: while the pointer sits over that child. If the
// host answers by hit-testing its own root and publishing the result, it sets
// the arrow on every button-less move, wiping whatever cursor the child just
// chose. The visible symptom is precise and counter-intuitive: the cursor only
// appears to change once a button goes DOWN, because the drag path publishes
// the captured cursor instead and no -mouseMoved: arrives while dragging.
//
// These cases assert the specific property — after a button-less move over an
// attached native child, the cursor the child set is still current — and pair
// it with a control on the same instrument: a move over a region the Pulp tree
// DOES own must still publish that region's cursor.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/view.hpp>

#include <memory>

// Runtime-looked-up interfaces for the Obj-C classes the mac hosts declare.
@interface PulpView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@end

@interface PulpPluginView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@end

// A foreign child that owns its own cursor, standing in for a WKWebView.
@interface PulpTestForeignChildView : NSView
@end

@implementation PulpTestForeignChildView
- (void)resetCursorRects {
    [self addCursorRect:self.bounds cursor:[NSCursor IBeamCursor]];
}
@end

namespace {

class StubView : public pulp::view::View {
public:
    void paint(pulp::canvas::Canvas&) override {}
};

// A 200x100 root whose LEFT half is a Pulp widget claiming the crosshair. The
// RIGHT half is left to the native child that the cases attach over it.
struct SplitRoot {
    StubView root;
    StubView* left = nullptr;

    SplitRoot() {
        root.set_bounds({0, 0, 200, 100});
        auto l = std::make_unique<StubView>();
        l->set_bounds({0, 0, 100, 100});
        l->set_cursor(pulp::view::View::CursorStyle::crosshair);
        left = l.get();
        root.add_child(std::move(l));
    }
};

// nil when the class is not registered in this binary, in which case callers
// skip rather than false-fail.
id make_host_view(NSString* class_name) {
    Class cls = NSClassFromString(class_name);
    if (cls == nil) return nil;
    return [[[cls alloc] initWithFrame:NSMakeRect(0, 0, 200, 100)] autorelease];
}

// A button-less mouse-moved event at a point in the view's own (top-down)
// coordinates. pressedMouseButtons is 0 for a synthesized move, which is the
// whole point: this is a HOVER, not a drag.
NSEvent* hover_move_at(NSView* view, CGFloat local_x, CGFloat local_y) {
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

// Can this process observe +[NSCursor currentCursor] at all? A run that cannot
// see the cursor must skip rather than report a passing measurement of nothing.
bool cursor_state_is_observable() {
    [[NSCursor arrowCursor] set];
    if ([NSCursor currentCursor] != [NSCursor arrowCursor]) return false;
    [[NSCursor IBeamCursor] set];
    return [NSCursor currentCursor] == [NSCursor IBeamCursor];
}

// Attach a foreign child over the root's right half, the way
// attach_native_child_view parents a WKWebView into the host's content view.
NSView* attach_foreign_child(NSView* host) {
    NSView* child = [[[PulpTestForeignChildView alloc]
        initWithFrame:NSMakeRect(100, 0, 100, 100)] autorelease];
    [host addSubview:child];
    return child;
}

// Drive one host class through the defect and its control. Returns false when
// the class is absent from this binary.
bool run_native_child_cursor_case(NSString* class_name) {
    id host = make_host_view(class_name);
    if (host == nil) return false;

    SplitRoot scene;
    [host setRootView:&scene.root];
    NSView* host_view = (NSView*) host;
    attach_foreign_child(host_view);

    // CONTROL: a button-less move over the region the Pulp tree owns must
    // publish that region's cursor. If this fails the rig is broken and the
    // negative finding below would mean nothing.
    [[NSCursor arrowCursor] set];
    [host_view mouseMoved:hover_move_at(host_view, 50, 50)];
    REQUIRE([NSCursor currentCursor] == [NSCursor crosshairCursor]);

    // The child has just chosen its own cursor, as WebKit does over text.
    [[NSCursor IBeamCursor] set];
    REQUIRE([NSCursor currentCursor] == [NSCursor IBeamCursor]);

    // THE PROPERTY: a button-less move whose position is owned by the attached
    // native child must leave that child's cursor alone.
    [host_view mouseMoved:hover_move_at(host_view, 150, 50)];
    REQUIRE([NSCursor currentCursor] == [NSCursor IBeamCursor]);

    // AppKit's own cursor pass must not stomp it either.
    [host_view cursorUpdate:hover_move_at(host_view, 150, 50)];
    REQUIRE([NSCursor currentCursor] == [NSCursor IBeamCursor]);

    [host setRootView:nullptr];
    return true;
}

}  // namespace

TEST_CASE("standalone host leaves an attached native child's cursor alone",
          "[view][cursor][hover][macos]") {
    @autoreleasepool {
        if (!cursor_state_is_observable()) {
            SKIP("this process cannot observe +[NSCursor currentCursor]");
        }
        if (!run_native_child_cursor_case(@"PulpView")) {
            SKIP("PulpView is not registered in this binary");
        }
    }
}

TEST_CASE("plugin host leaves an attached native child's cursor alone",
          "[view][cursor][hover][macos]") {
    @autoreleasepool {
        if (!cursor_state_is_observable()) {
            SKIP("this process cannot observe +[NSCursor currentCursor]");
        }
        if (!run_native_child_cursor_case(@"PulpPluginView")) {
            SKIP("PulpPluginView is not registered in this binary");
        }
    }
}
