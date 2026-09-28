// The macOS cursor while a RIGHT or MIDDLE button is held, and on its release.
//
// AppKit sends no -mouseMoved: while any button is down; a held right button
// produces -rightMouseDragged: and a held middle button -otherMouseDragged:.
// A host that answers only -mouseMoved: therefore freezes the cursor at
// whatever it was when the button went down. The visible symptom: right-click
// a surface that shows a crosshair, keep the button down, slide onto the
// context menu that press just opened -- the crosshair stays over the menu.
//
// Each case drives the shipping view class with synthesized AppKit events and
// reads +[NSCursor currentCursor]. The control is a button-less -mouseMoved:
// over the crosshair region: it exercises every stage except the held-button
// entry point, so if it fails the rig is broken and the property means nothing.
//
// The held-button path resolves the cursor only. The region under the pointer
// counts the scripted pointer moves it receives, and must receive none: a held
// non-primary button reaches the editor exactly as it did before.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/input_events.hpp>
#include <pulp/view/view.hpp>

#include <memory>

@interface PulpView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@end

@interface PulpPluginView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@end

namespace {

using CursorStyle = pulp::view::View::CursorStyle;

class StubView : public pulp::view::View {
public:
    void paint(pulp::canvas::Canvas&) override {}
};

// Left half shows a crosshair (the surface the menu was opened on); right half
// shows a pointer (the menu row) and counts the scripted moves it receives.
struct HeldButtonScene {
    StubView root;
    StubView* row = nullptr;
    int row_moves = 0;

    HeldButtonScene() {
        root.set_bounds({0, 0, 200, 100});
        auto surface = std::make_unique<StubView>();
        surface->set_bounds({0, 0, 100, 100});
        surface->set_cursor(CursorStyle::crosshair);
        root.add_child(std::move(surface));
        auto menu_row = std::make_unique<StubView>();
        menu_row->set_bounds({100, 0, 100, 100});
        menu_row->set_cursor(CursorStyle::pointer);
        menu_row->on_dom_pointer_move_event = [this](const pulp::view::MouseEvent&, bool) {
            ++row_moves;
        };
        row = menu_row.get();
        root.add_child(std::move(menu_row));
    }
};

NSEvent* event_at(NSView* view, NSEventType type, CGFloat x, CGFloat y) {
    return [NSEvent mouseEventWithType:type
                              location:NSMakePoint(x, view.bounds.size.height - y)
                         modifierFlags:0
                             timestamp:0
                          windowNumber:0
                               context:nil
                           eventNumber:0
                            clickCount:(type == NSEventTypeMouseMoved ? 0 : 1)
                              pressure:0];
}

bool cursor_state_is_observable() {
    [[NSCursor arrowCursor] set];
    if ([NSCursor currentCursor] != [NSCursor arrowCursor]) return false;
    [[NSCursor IBeamCursor] set];
    return [NSCursor currentCursor] == [NSCursor IBeamCursor];
}

// Returns false when the class is not registered in this binary.
bool run_held_button_case(NSString* class_name) {
    Class cls = NSClassFromString(class_name);
    if (cls == nil) return false;
    NSView* host = [[[cls alloc] initWithFrame:NSMakeRect(0, 0, 200, 100)] autorelease];
    HeldButtonScene scene;
    [(id)host setRootView:&scene.root];

    struct Entry {
        const char* name;
        NSEventType type;
        SEL selector;
    };
    const Entry entries[] = {
        {"rightMouseDragged", NSEventTypeRightMouseDragged, @selector(rightMouseDragged:)},
        {"rightMouseUp", NSEventTypeRightMouseUp, @selector(rightMouseUp:)},
        {"otherMouseDragged", NSEventTypeOtherMouseDragged, @selector(otherMouseDragged:)},
        {"otherMouseUp", NSEventTypeOtherMouseUp, @selector(otherMouseUp:)},
    };
    for (const auto& entry : entries) {
        INFO([class_name UTF8String] << " " << entry.name);
        // CONTROL: a button-less move over the surface publishes its crosshair.
        [[NSCursor arrowCursor] set];
        [host mouseMoved:event_at(host, NSEventTypeMouseMoved, 50, 50)];
        REQUIRE([NSCursor currentCursor] == [NSCursor crosshairCursor]);

        // THE PROPERTY: with the button held (or on its release) over the row,
        // the row's cursor replaces the crosshair.
        const int moves_before = scene.row_moves;
        [host performSelector:entry.selector
                   withObject:event_at(host, entry.type, 150, 50)];
        CHECK([NSCursor currentCursor] == [NSCursor pointingHandCursor]);
        // Cursor only: nothing reached the row's scripted move handler.
        CHECK(scene.row_moves == moves_before);
    }

    [(id)host setRootView:nullptr];
    return true;
}

}  // namespace

TEST_CASE("standalone host re-resolves the cursor while a non-primary button is held",
          "[view][cursor][macos]") {
    @autoreleasepool {
        if (!cursor_state_is_observable())
            SKIP("this process cannot observe +[NSCursor currentCursor]");
        if (!run_held_button_case(@"PulpView"))
            SKIP("PulpView is not registered in this binary");
    }
}

TEST_CASE("plugin host re-resolves the cursor while a non-primary button is held",
          "[view][cursor][macos]") {
    @autoreleasepool {
        if (!cursor_state_is_observable())
            SKIP("this process cannot observe +[NSCursor currentCursor]");
        if (!run_held_button_case(@"PulpPluginView"))
            SKIP("PulpPluginView is not registered in this binary");
    }
}

TEST_CASE("GPU plugin host re-resolves the cursor while a non-primary button is held",
          "[view][cursor][macos]") {
    @autoreleasepool {
        if (!cursor_state_is_observable())
            SKIP("this process cannot observe +[NSCursor currentCursor]");
        if (!run_held_button_case(@"PulpGpuPluginView"))
            SKIP("PulpGpuPluginView is not registered in this binary");
    }
}
