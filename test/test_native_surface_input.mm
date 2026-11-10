// test_native_surface_input.mm — input reaching a MetalSurface's view: pointer
// and wheel delivery, the host-first key rule, text and marked text, focus,
// cursors and file drops. Events are synthetic and sent to the view the way
// AppKit sends them. Each case skips, saying so, without a Metal device.
// Compiled with ARC.

#include <catch2/catch_test_macros.hpp>

#include <pulp/native/metal_surface.hpp>

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <Metal/Metal.h>

#include <memory>
#include <string>
#include <vector>

using namespace pulp::native;

/// A host view under the surface that records what is passed on to it.
@interface PulpSurfaceInputTestHost : NSView
@property(nonatomic) int keyDowns;
@property(nonatomic) int keyUps;
@property(nonatomic) int wheels;
@property(nonatomic) int flags;
@end
@implementation PulpSurfaceInputTestHost
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (void)keyDown:(NSEvent*)event {
    self.keyDowns += 1;
}
- (void)keyUp:(NSEvent*)event {
    self.keyUps += 1;
}
- (void)scrollWheel:(NSEvent*)event {
    self.wheels += 1;
}
- (void)flagsChanged:(NSEvent*)event {
    self.flags += 1;
}
@end

/// Just enough of NSDraggingInfo for the surface's drop methods.
@interface PulpSurfaceInputTestDrag : NSObject
@property(nonatomic) NSPoint location;
@property(nonatomic, strong) NSPasteboard* board;
@end
@implementation PulpSurfaceInputTestDrag
- (NSPoint)draggingLocation {
    return self.location;
}
- (NSPasteboard*)draggingPasteboard {
    return self.board;
}
@end

namespace {

struct Fixture {
    NSWindow* window = nil;
    PulpSurfaceInputTestHost* host = nil;
    std::unique_ptr<MetalSurface> surface;
    NSView* view = nil;

    std::vector<PointerEvent> pointers;
    std::vector<WheelEvent> wheels;
    std::vector<KeyEvent> keys;
    std::vector<std::string> texts;
    std::vector<std::string> marked;
    std::vector<FocusEvent> focus;
    std::vector<DropEvent> drops;
    bool consume_keys = true;
    bool accept_drops = true;

    bool ready() const {
        return surface != nullptr;
    }

    Fixture() {
        if (!MTLCreateSystemDefaultDevice())
            return;
        [NSApplication sharedApplication];
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 300, 200)
                                             styleMask:NSWindowStyleMaskBorderless
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        window.releasedWhenClosed = NO;
        host = [[PulpSurfaceInputTestHost alloc] initWithFrame:NSMakeRect(0, 0, 300, 200)];
        window.contentView = host;
        surface = MetalSurface::create();
        if (!surface)
            return;
        surface->attach((__bridge void*)host);
        view = (__bridge NSView*)surface->view();
        [window makeFirstResponder:host];
        InputHandler h;
        h.pointer = [this](const PointerEvent& e) { pointers.push_back(e); };
        h.wheel = [this](const WheelEvent& e) { wheels.push_back(e); };
        h.key = [this](const KeyEvent& e) {
            keys.push_back(e);
            return consume_keys;
        };
        h.text = [this](const std::string& t) { texts.push_back(t); };
        h.marked_text = [this](const std::string& t, std::size_t) { marked.push_back(t); };
        h.focus = [this](const FocusEvent& e) { focus.push_back(e); };
        h.drop = [this](const DropEvent& e) {
            drops.push_back(e);
            return accept_drops;
        };
        surface->set_input_handler(h);
    }
    ~Fixture() {
        if (surface)
            surface->detach();
        [window close];
    }

    NSEvent* mouse(NSEventType type, double x, double y) const {
        return [NSEvent mouseEventWithType:type
                                  location:NSMakePoint(x, 200 - y)
                             modifierFlags:0
                                 timestamp:1.0
                              windowNumber:window.windowNumber
                                   context:nil
                               eventNumber:0
                                clickCount:1
                                  pressure:1.0];
    }
    // Enter and exit events have their own constructor; mouseEventWithType
    // throws for them.
    NSEvent* crossing(NSEventType type, double x, double y) const {
        return [NSEvent enterExitEventWithType:type
                                      location:NSMakePoint(x, 200 - y)
                                 modifierFlags:0
                                     timestamp:1.0
                                  windowNumber:window.windowNumber
                                       context:nil
                                   eventNumber:0
                                trackingNumber:0
                                      userData:nullptr];
    }
    NSEvent* key(NSEventType type, unsigned short code, NSString* chars,
                 NSEventModifierFlags flags = 0) const {
        return [NSEvent keyEventWithType:type
                                location:NSZeroPoint
                           modifierFlags:flags
                               timestamp:1.0
                            windowNumber:window.windowNumber
                                 context:nil
                              characters:chars
             charactersIgnoringModifiers:chars
                               isARepeat:NO
                                 keyCode:code];
    }
};

#define REQUIRE_READY(f)                                                                           \
    do {                                                                                           \
        if (!(f).ready())                                                                          \
            SKIP("no Metal device on this host; surface input was not exercised");                 \
    } while (0)

} // namespace

TEST_CASE("pointer events reach the client in surface coordinates",
          "[native-surface][input][surface]") {
    Fixture f;
    REQUIRE_READY(f);
    [f.view mouseDown:f.mouse(NSEventTypeLeftMouseDown, 10, 20)];
    [f.view mouseDragged:f.mouse(NSEventTypeLeftMouseDragged, 350, -15)];
    [f.view mouseUp:f.mouse(NSEventTypeLeftMouseUp, 350, -15)];
    [f.view rightMouseDown:f.mouse(NSEventTypeRightMouseDown, 5, 5)];
    [f.view mouseMoved:f.mouse(NSEventTypeMouseMoved, 7, 8)];
    REQUIRE(f.pointers.size() == 5);
    REQUIRE(f.pointers[0].kind == PointerKind::down);
    REQUIRE(f.pointers[0].x == 10.0);
    REQUIRE(f.pointers[0].y == 20.0);
    // A drag that left the surface keeps its outside position.
    REQUIRE(f.pointers[1].kind == PointerKind::drag);
    REQUIRE(f.pointers[1].x == 350.0);
    REQUIRE(f.pointers[1].y == -15.0);
    REQUIRE(f.pointers[2].kind == PointerKind::up);
    REQUIRE(f.pointers[3].button == PointerButton::right);
    REQUIRE(f.pointers[4].kind == PointerKind::move);
}

TEST_CASE("the first click into an inactive window is a press, and the surface tracks "
          "hover",
          "[native-surface][input][surface]") {
    Fixture f;
    REQUIRE_READY(f);
    REQUIRE([f.view acceptsFirstMouse:f.mouse(NSEventTypeLeftMouseDown, 1, 1)]);
    [f.view updateTrackingAreas];
    REQUIRE(f.view.trackingAreas.count == 1);
    NSTrackingAreaOptions o = f.view.trackingAreas.firstObject.options;
    REQUIRE((o & NSTrackingMouseEnteredAndExited) != 0);
    REQUIRE((o & NSTrackingMouseMoved) != 0);
    REQUIRE((o & NSTrackingInVisibleRect) != 0);
    // Updating again replaces the area rather than stacking a second one.
    [f.view updateTrackingAreas];
    REQUIRE(f.view.trackingAreas.count == 1);
}

TEST_CASE("without text focus the host keeps every key", "[native-surface][input][surface]") {
    Fixture f;
    REQUIRE_READY(f);
    REQUIRE_FALSE([f.view acceptsFirstResponder]);
    [f.view keyDown:f.key(NSEventTypeKeyDown, kVK_Space, @" ")];
    [f.view keyUp:f.key(NSEventTypeKeyUp, kVK_Space, @" ")];
    REQUIRE(f.keys.empty());
    REQUIRE(f.host.keyDowns == 1);
    REQUIRE(f.host.keyUps == 1);
    REQUIRE_FALSE([f.view performKeyEquivalent:f.key(NSEventTypeKeyDown, kVK_ANSI_S, @"s",
                                                     NSEventModifierFlagCommand)]);
    REQUIRE(f.keys.empty());
}

TEST_CASE("with text focus keys go to the client first, and unconsumed ones become text",
          "[native-surface][input][surface]") {
    Fixture f;
    REQUIRE_READY(f);
    REQUIRE(f.surface->set_text_focus(true));
    REQUIRE(f.window.firstResponder == f.view);
    REQUIRE([f.view acceptsFirstResponder]);

    [f.view keyDown:f.key(NSEventTypeKeyDown, kVK_Space, @" ")];
    REQUIRE(f.keys.size() == 1);
    REQUIRE(f.keys[0].key == Key::space);
    REQUIRE(f.host.keyDowns == 0);
    REQUIRE([f.view performKeyEquivalent:f.key(NSEventTypeKeyDown, kVK_ANSI_S, @"s",
                                               NSEventModifierFlagCommand)]);
    REQUIRE(f.keys.size() == 2);

    // A key the client declines does not go to the host while text focus is
    // held: it becomes text input.
    f.consume_keys = false;
    [f.view keyDown:f.key(NSEventTypeKeyDown, kVK_ANSI_A, @"a")];
    REQUIRE(f.host.keyDowns == 0);
    REQUIRE(f.keys.size() == 3);
}

TEST_CASE("holding the keyboard for keys hands every key the client declines back to the host",
          "[native-surface][input][surface]") {
    Fixture f;
    REQUIRE_READY(f);
    REQUIRE(f.surface->set_keyboard_focus(KeyboardFocus::keys));
    REQUIRE(f.window.firstResponder == f.view);
    REQUIRE(f.surface->keyboard_focus() == KeyboardFocus::keys);
    REQUIRE_FALSE(f.surface->text_focus());

    // A key the client consumes stays with the client.
    [f.view keyDown:f.key(NSEventTypeKeyDown, kVK_ANSI_K, @"k")];
    REQUIRE(f.keys.size() == 1);
    REQUIRE(f.host.keyDowns == 0);

    // A key it declines goes on to the next responder, the host: Space still
    // reaches the DAW's transport, and nothing becomes text.
    f.consume_keys = false;
    [f.view keyDown:f.key(NSEventTypeKeyDown, kVK_Space, @" ")];
    [f.view keyUp:f.key(NSEventTypeKeyUp, kVK_Space, @" ")];
    REQUIRE(f.keys.size() == 3);
    REQUIRE(f.keys[1].key == Key::space);
    REQUIRE(f.host.keyDowns == 1);
    REQUIRE(f.host.keyUps == 1);
    REQUIRE(f.texts.empty());
    REQUIRE(f.marked.empty());

    // So does a declined Command chord.
    REQUIRE_FALSE([f.view performKeyEquivalent:f.key(NSEventTypeKeyDown, kVK_ANSI_S, @"s",
                                                     NSEventModifierFlagCommand)]);
    REQUIRE(f.keys.size() == 4);
}

TEST_CASE("text entry keeps declined keys from the host until the client switches to keys",
          "[native-surface][input][surface]") {
    Fixture f;
    REQUIRE_READY(f);
    f.consume_keys = false;
    REQUIRE(f.surface->set_keyboard_focus(KeyboardFocus::text));
    REQUIRE(f.surface->text_focus());
    [f.view keyDown:f.key(NSEventTypeKeyDown, kVK_Space, @" ")];
    REQUIRE(f.host.keyDowns == 0);

    // An unfinished composition does not survive leaving text entry.
    id<NSTextInputClient> client = (id<NSTextInputClient>)f.view;
    [client setMarkedText:@"ka"
            selectedRange:NSMakeRange(2, 0)
         replacementRange:NSMakeRange(NSNotFound, 0)];
    REQUIRE(f.marked == std::vector<std::string>{"ka"});

    REQUIRE(f.surface->set_keyboard_focus(KeyboardFocus::keys));
    REQUIRE(f.window.firstResponder == f.view);
    REQUIRE_FALSE(f.surface->text_focus());
    REQUIRE_FALSE([client hasMarkedText]);
    REQUIRE(f.marked == std::vector<std::string>{"ka", ""});
    [f.view keyDown:f.key(NSEventTypeKeyDown, kVK_Space, @" ")];
    REQUIRE(f.host.keyDowns == 1);

    // Giving the keyboard back ends either hold and restores the host.
    REQUIRE(f.surface->set_text_focus(false));
    REQUIRE(f.surface->keyboard_focus() == KeyboardFocus::none);
    REQUIRE(f.window.firstResponder == f.host);
    REQUIRE_FALSE([f.view acceptsFirstResponder]);
}

TEST_CASE("committed and marked text arrive separately from key identity",
          "[native-surface][input][surface]") {
    Fixture f;
    REQUIRE_READY(f);
    id<NSTextInputClient> client = (id<NSTextInputClient>)f.view;
    REQUIRE([f.view conformsToProtocol:@protocol(NSTextInputClient)]);
    [client setMarkedText:@"ka"
            selectedRange:NSMakeRange(2, 0)
         replacementRange:NSMakeRange(NSNotFound, 0)];
    REQUIRE([client hasMarkedText]);
    REQUIRE([client markedRange].length == 2);
    [client insertText:@"か" replacementRange:NSMakeRange(NSNotFound, 0)];
    REQUIRE_FALSE([client hasMarkedText]);
    [client insertText:@"aé" replacementRange:NSMakeRange(NSNotFound, 0)];
    REQUIRE(f.texts == std::vector<std::string>{"か", "aé"});
    // Marked "ka", then cleared when the composition committed.
    REQUIRE(f.marked == std::vector<std::string>{"ka", ""});
    REQUIRE(f.keys.empty());
}

TEST_CASE("focus is reported for the responder, the window and the application",
          "[native-surface][input][surface]") {
    Fixture f;
    REQUIRE_READY(f);
    REQUIRE(f.surface->set_text_focus(true));
    REQUIRE(f.focus.size() == 1);
    REQUIRE(f.focus.back().focused);
    REQUIRE(f.focus.back().reason == FocusReason::responder);

    [[NSNotificationCenter defaultCenter] postNotificationName:NSWindowDidResignKeyNotification
                                                        object:f.window];
    REQUIRE(f.focus.back().reason == FocusReason::window);
    REQUIRE_FALSE(f.focus.back().focused);
    [[NSNotificationCenter defaultCenter]
        postNotificationName:NSApplicationDidResignActiveNotification
                      object:NSApp];
    REQUIRE(f.focus.back().reason == FocusReason::application);
    REQUIRE_FALSE(f.focus.back().focused);

    // A host that takes the keyboard ends the hold.
    [f.window makeFirstResponder:f.host];
    REQUIRE_FALSE(f.surface->text_focus());
    REQUIRE(f.focus.back().reason == FocusReason::responder);
    REQUIRE_FALSE(f.focus.back().focused);
    REQUIRE_FALSE([f.view acceptsFirstResponder]);
}

TEST_CASE("giving text focus back restores the host's own responder",
          "[native-surface][input][surface]") {
    Fixture f;
    REQUIRE_READY(f);
    REQUIRE(f.window.firstResponder == f.host);
    REQUIRE(f.surface->set_text_focus(true));
    REQUIRE(f.surface->set_text_focus(false));
    REQUIRE(f.window.firstResponder == f.host);
}

TEST_CASE("an unhandled wheel goes on to the host", "[native-surface][input][surface]") {
    Fixture f;
    REQUIRE_READY(f);
    CGEventRef cg = CGEventCreateScrollWheelEvent2(nullptr, kCGScrollEventUnitLine, 1, 2, 0, 0);
    NSEvent* event = [NSEvent eventWithCGEvent:cg];
    CFRelease(cg);
    [f.view scrollWheel:event];
    REQUIRE(f.wheels.size() == 1);
    REQUIRE(f.host.wheels == 0);
    InputHandler none;
    f.surface->set_input_handler(none);
    [f.view scrollWheel:event];
    REQUIRE(f.host.wheels == 1);
}

TEST_CASE("modifier keys reach the client and still reach the host",
          "[native-surface][input][surface]") {
    Fixture f;
    REQUIRE_READY(f);
    NSEvent* shift = [NSEvent keyEventWithType:NSEventTypeFlagsChanged
                                      location:NSZeroPoint
                                 modifierFlags:NSEventModifierFlagShift | 0x2
                                     timestamp:1.0
                                  windowNumber:f.window.windowNumber
                                       context:nil
                                    characters:@""
                   charactersIgnoringModifiers:@""
                                     isARepeat:NO
                                       keyCode:kVK_Shift];
    [f.view flagsChanged:shift];
    REQUIRE(f.keys.size() == 1);
    REQUIRE(f.keys[0].key == Key::shift_left);
    REQUIRE(f.host.flags == 1);
}

TEST_CASE("the chosen cursor shows over the surface and the arrow returns on leave",
          "[native-surface][input][surface]") {
    Fixture f;
    REQUIRE_READY(f);
    f.surface->set_cursor(Cursor::ibeam);
    [f.view mouseEntered:f.crossing(NSEventTypeMouseEntered, 5, 5)];
    REQUIRE([NSCursor currentCursor] == [NSCursor IBeamCursor]);
    f.surface->set_cursor(Cursor::crosshair);
    REQUIRE([NSCursor currentCursor] == [NSCursor crosshairCursor]);
    [f.view mouseExited:f.crossing(NSEventTypeMouseExited, 400, 5)];
    REQUIRE([NSCursor currentCursor] == [NSCursor arrowCursor]);
    REQUIRE(f.pointers.size() == 2);
    REQUIRE(f.pointers[0].kind == PointerKind::enter);
    REQUIRE(f.pointers[1].kind == PointerKind::leave);
}

TEST_CASE("a file drag is offered on enter and every move, and drops once",
          "[native-surface][input][surface]") {
    Fixture f;
    REQUIRE_READY(f);
    REQUIRE([f.view.registeredDraggedTypes containsObject:NSPasteboardTypeFileURL]);
    NSPasteboard* board = [NSPasteboard pasteboardWithUniqueName];
    [board clearContents];
    [board writeObjects:@[ [NSURL fileURLWithPath:@"/tmp/pulp-surface-drop.wav"] ]];
    PulpSurfaceInputTestDrag* drag = [PulpSurfaceInputTestDrag new];
    drag.board = board;
    drag.location = NSMakePoint(30, 200 - 40);
    id<NSDraggingDestination> dest = (id<NSDraggingDestination>)f.view;
    id<NSDraggingInfo> info = (id<NSDraggingInfo>)drag;

    REQUIRE([dest draggingEntered:info] == NSDragOperationCopy);
    REQUIRE(f.drops.back().phase == DropPhase::enter);
    REQUIRE(f.drops.back().x == 30.0);
    REQUIRE(f.drops.back().y == 40.0);
    REQUIRE(f.drops.back().paths == std::vector<std::string>{"/tmp/pulp-surface-drop.wav"});

    // The client turns the drag down over another region: no drop.
    f.accept_drops = false;
    REQUIRE([dest draggingUpdated:info] == NSDragOperationNone);
    REQUIRE_FALSE([dest performDragOperation:info]);
    f.accept_drops = true;
    REQUIRE([dest draggingUpdated:info] == NSDragOperationCopy);
    REQUIRE([dest performDragOperation:info]);
    REQUIRE(f.drops.back().phase == DropPhase::drop);
    int dropped = 0;
    for (const auto& d : f.drops)
        dropped += d.phase == DropPhase::drop ? 1 : 0;
    REQUIRE(dropped == 1);
    [board releaseGlobally];
}

TEST_CASE("input stops at the surface once it is invalidated", "[native-surface][input][surface]") {
    Fixture f;
    REQUIRE_READY(f);
    NSView* view = f.view;
    f.surface->invalidate();
    [view mouseDown:f.mouse(NSEventTypeLeftMouseDown, 1, 1)];
    [view keyDown:f.key(NSEventTypeKeyDown, kVK_Space, @" ")];
    REQUIRE(f.pointers.empty());
    REQUIRE(f.keys.empty());
    REQUIRE_FALSE([view acceptsFirstResponder]);
}
