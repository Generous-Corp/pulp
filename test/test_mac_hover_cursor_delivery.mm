// The cursor a macOS window host applies while the pointer HOVERS — no button
// held, no click, delivered through the host's own -mouseMoved:.
//
// The property under test is the one the user reports missing: move the
// pointer over a region and the cursor becomes that region's cursor, with no
// mouse-down anywhere in the sequence.
//
// What makes this able to SEE the defect is the scene. A view whose cursor is
// assigned once, up front, is resolvable by hit-test alone, so hovering it
// changes the cursor whether or not hover is delivered into the document — a
// scene like that passes on the broken build and is exactly the proxy that
// produced the earlier false green. The shipping UI is scripted: its cursor is
// chosen by a `pointermove` handler, which the bridge installs as
// `View::on_dom_pointer_move_event` (core/view/src/widget_bridge/event_api.cpp).
// So the two regions here claim their cursor ONLY from that callback — the
// real seam, not a stand-in for it.
//
// The scene also carries a STATIC region as the positive control. Hovering it
// exercises every stage the property arm needs — NSEvent construction, the
// window's coordinate flip, host routing, hit_test, cursor resolution,
// set_ns_cursor_for_style, and +[NSCursor currentCursor] observation — and
// nothing else. It passes on the broken build too. If it ever fails, the
// harness is broken and the property result below means nothing.
//
// Every assertion runs with zero pressed mouse buttons.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/input_events.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <memory>

@interface PulpView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
- (void)refreshHoverCursor;
@end

namespace {

using pulp::view::MouseButton;
using pulp::view::MouseEvent;
using pulp::view::MousePhase;
using CursorStyle = pulp::view::View::CursorStyle;

// A region that behaves like a scripted one: it owns no cursor until a
// pointer-MOVE is delivered into the document, and then claims its own. This
// is the same callback slot the JS bridge fills for a `pointermove` listener.
class ScriptedCursorView : public pulp::view::View {
public:
    explicit ScriptedCursorView(CursorStyle style) : style_(style) {
        on_dom_pointer_move_event = [this](const MouseEvent& e, bool) {
            ++moves;
            last = e;
            set_cursor(style_);
        };
    }
    void paint(pulp::canvas::Canvas&) override {}

    int moves = 0;
    MouseEvent last{};

private:
    CursorStyle style_;
};

class StaticCursorView : public pulp::view::View {
public:
    explicit StaticCursorView(CursorStyle style) { set_cursor(style); }
    void paint(pulp::canvas::Canvas&) override {}
};

// Three equal columns: a static control, then two scripted regions.
struct Scene {
    pulp::view::View root;
    StaticCursorView* control = nullptr;
    ScriptedCursorView* left = nullptr;
    ScriptedCursorView* right = nullptr;

    Scene(float w, float h) {
        // Sized through flex, not set_bounds: the host runs a layout pass over
        // the root, and a child positioned only by set_bounds is collapsed to
        // zero by it — a scene that hit-tests to the root everywhere, which
        // reads as "the cursor never changed".
        root.set_bounds({0, 0, w, h});
        root.flex().direction = pulp::view::FlexDirection::row;

        auto c = std::make_unique<StaticCursorView>(CursorStyle::crosshair);
        c->flex().flex_grow = 1;
        control = c.get();
        root.add_child(std::move(c));

        auto l = std::make_unique<ScriptedCursorView>(CursorStyle::pointer);
        l->flex().flex_grow = 1;
        left = l.get();
        root.add_child(std::move(l));

        auto r = std::make_unique<ScriptedCursorView>(CursorStyle::text);
        r->flex().flex_grow = 1;
        right = r.get();
        root.add_child(std::move(r));

        root.layout_children();
    }
};

// Can this process observe +[NSCursor currentCursor]? A run that cannot must
// skip rather than report a passing measurement of nothing.
bool cursor_state_is_observable() {
    [[NSCursor arrowCursor] set];
    if ([NSCursor currentCursor] != [NSCursor arrowCursor]) return false;
    [[NSCursor IBeamCursor] set];
    return [NSCursor currentCursor] == [NSCursor IBeamCursor];
}

// Pointer motion with NO button down. `local_y` is top-down view coordinates;
// NSWindow base coordinates are bottom-up.
NSEvent* hover_move(NSWindow* window, CGFloat local_x, CGFloat local_y) {
    const CGFloat flipped_y = window.contentView.bounds.size.height - local_y;
    return [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                              location:NSMakePoint(local_x, flipped_y)
                         modifierFlags:0
                             timestamp:0
                          windowNumber:window.windowNumber
                               context:nil
                           eventNumber:0
                            clickCount:0
                              pressure:0];
}

// The NSWindow hosting THIS root. Matched on the root pointer, not just on
// "is a Pulp host view": a window from an earlier case in the same binary can
// still be in -[NSApp windows], and driving that one would measure the wrong
// scene. Nil when this binary has no native host (non-Apple factory, or the
// class was renamed out of the binary).
NSWindow* host_window(pulp::view::View* root) {
    for (NSWindow* w in [NSApp windows]) {
        if (![w.contentView respondsToSelector:@selector(refreshHoverCursor)]) continue;
        if (((PulpView*)w.contentView).rootView == root) return w;
    }
    return nil;
}

}  // namespace

TEST_CASE("hovering a scripted region applies the cursor its move handler set",
          "[mac][cursor][hover]") {
    if (!cursor_state_is_observable())
        SKIP("+[NSCursor currentCursor] is not observable in this session");
    [NSApplication sharedApplication];

    Scene scene(300, 300);
    pulp::view::WindowOptions options;
    options.width = 300;
    options.height = 300;
    options.title = "hover-cursor";
    // Never ordered front: this asserts event routing, not presentation.
    options.initially_hidden = true;

    auto host = pulp::view::WindowHost::create(scene.root, options);
    if (!host) SKIP("no native window host in this binary");
    NSWindow* window = host_window(&scene.root);
    if (window == nil) SKIP("no Pulp host view is registered in this binary");
    [window makeFirstResponder:window.contentView];
    id view = (id)window.contentView;

    // Column centres in top-down view coordinates.
    const CGFloat kControlX = 50, kLeftX = 150, kRightX = 250, kMidY = 150;

    // ── Control ──────────────────────────────────────────────────────────
    // Same instrument, same window, same -mouseMoved:, over a region whose
    // cursor needs no delivery into the document. Everything except the move
    // channel. This MUST change the cursor; if it does not, nothing below is
    // a measurement of the product.
    [[NSCursor arrowCursor] set];
    REQUIRE([NSEvent pressedMouseButtons] == 0);
    [view mouseMoved:hover_move(window, kControlX, kMidY)];
    REQUIRE([NSCursor currentCursor] == [NSCursor crosshairCursor]);

    // ── The property ─────────────────────────────────────────────────────
    // Hover a region that decides its cursor in a pointer-move handler. No
    // button is pressed at any point in this test.
    [[NSCursor arrowCursor] set];
    REQUIRE([NSEvent pressedMouseButtons] == 0);
    [view mouseMoved:hover_move(window, kLeftX, kMidY)];

    // The move reached the scripted seam at all…
    REQUIRE(scene.left->moves == 1);
    // …and the cursor the handler chose is the one now on screen.
    REQUIRE([NSCursor currentCursor] == [NSCursor pointingHandCursor]);

    // A hover is not a drag. A handler that branches on the held button — the
    // grab/grabbing idiom — must take the hover branch here.
    CHECK(scene.left->last.is_down == false);
    CHECK(scene.left->last.button == MouseButton::none);
    CHECK(scene.left->last.phase == MousePhase::hover);

    // Moving on to the other scripted region changes it again, still with no
    // button, so the first result cannot be a cursor that merely stuck.
    [[NSCursor arrowCursor] set];
    REQUIRE([NSEvent pressedMouseButtons] == 0);
    [view mouseMoved:hover_move(window, kRightX, kMidY)];
    REQUIRE(scene.right->moves == 1);
    REQUIRE([NSCursor currentCursor] == [NSCursor IBeamCursor]);

    // No press ever ran: the modern press/release channel is untouched, so
    // this cannot be a mouse-down result in disguise.
    CHECK(scene.left->last.click_count == 0);
}

// The same delivery, one layer up: pushed through -[NSWindow sendEvent:],
// which drops NSEventTypeMouseMoved unless the window accepts mouse-moved
// events. This is a narrower claim than the case above — it covers the
// sendEvent: route only (real pointer motion over a tracking area carrying
// NSTrackingMouseMoved reaches the owner regardless of that flag) — but a
// window that drops the event there is unreachable for synthesized motion and
// for any point no tracking area covers.
TEST_CASE("mouse-moved events survive the window's sendEvent: gate",
          "[mac][cursor][hover]") {
    if (!cursor_state_is_observable())
        SKIP("+[NSCursor currentCursor] is not observable in this session");
    [NSApplication sharedApplication];

    Scene scene(300, 300);
    pulp::view::WindowOptions options;
    options.width = 300;
    options.height = 300;
    options.title = "hover-cursor-gate";
    options.initially_hidden = true;

    auto host = pulp::view::WindowHost::create(scene.root, options);
    if (!host) SKIP("no native window host in this binary");
    NSWindow* window = host_window(&scene.root);
    if (window == nil) SKIP("no Pulp host view is registered in this binary");
    [window makeFirstResponder:window.contentView];

    // Control: the view accepts the same event when messaged directly, so a
    // failure below is the window gate and not the scene.
    [[NSCursor arrowCursor] set];
    [(id)window.contentView mouseMoved:hover_move(window, 50, 150)];
    REQUIRE([NSCursor currentCursor] == [NSCursor crosshairCursor]);

    [[NSCursor arrowCursor] set];
    REQUIRE([NSEvent pressedMouseButtons] == 0);
    [window sendEvent:hover_move(window, 50, 150)];
    REQUIRE([NSCursor currentCursor] == [NSCursor crosshairCursor]);
}
