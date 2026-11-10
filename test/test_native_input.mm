// test_native_input.mm — translation from AppKit events to pulp::native input
// events, and the key-routing rule. Synthetic NSEvents and CGEvents drive the
// table tests; no real input device is involved. Compiled with ARC.

#include <catch2/catch_test_macros.hpp>

#include <pulp/native/input.hpp>

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::native;

@interface PulpNativeInputTestFlippedView : NSView
@end
@implementation PulpNativeInputTestFlippedView
- (BOOL)isFlipped {
    return YES;
}
@end

namespace {

struct Fixture {
    NSWindow* window = nil;
    NSView* view = nil;

    Fixture() {
        [NSApplication sharedApplication];
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 300)
                                             styleMask:NSWindowStyleMaskBorderless
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        window.releasedWhenClosed = NO;
        // A 200 x 100 surface whose top-left sits at window (50, 120) in
        // AppKit's bottom-left window coordinates.
        view = [[PulpNativeInputTestFlippedView alloc] initWithFrame:NSMakeRect(50, 80, 200, 100)];
        [window.contentView addSubview:view];
    }
    ~Fixture() {
        [window close];
    }

    // Window coordinates for a point given in the view's top-left space.
    NSPoint window_point(double x, double y) const {
        return NSMakePoint(50 + x, 80 + 100 - y);
    }

    NSEvent* mouse(NSEventType type, double x, double y, NSEventModifierFlags flags = 0,
                   NSInteger clicks = 1) const {
        return [NSEvent mouseEventWithType:type
                                  location:window_point(x, y)
                             modifierFlags:flags
                                 timestamp:12.5
                              windowNumber:window.windowNumber
                                   context:nil
                               eventNumber:0
                                clickCount:clicks
                                  pressure:1.0];
    }
};

NSEvent* key_event(NSEventType type, unsigned short code, NSString* chars,
                   NSEventModifierFlags flags = 0, BOOL repeat = NO) {
    return [NSEvent keyEventWithType:type
                            location:NSZeroPoint
                       modifierFlags:flags
                           timestamp:3.0
                        windowNumber:0
                             context:nil
                          characters:chars
         charactersIgnoringModifiers:chars
                           isARepeat:repeat
                             keyCode:code];
}

NSEvent* flags_event(unsigned short code, NSEventModifierFlags flags) {
    return [NSEvent keyEventWithType:NSEventTypeFlagsChanged
                            location:NSZeroPoint
                       modifierFlags:flags
                           timestamp:4.0
                        windowNumber:0
                             context:nil
                          characters:@""
         charactersIgnoringModifiers:@""
                           isARepeat:NO
                             keyCode:code];
}

} // namespace

TEST_CASE("pointer events map kind, button and modifiers", "[native-surface][input]") {
    Fixture f;
    struct Row {
        NSEventType type;
        PointerKind kind;
        PointerButton button;
    };
    const std::vector<Row> rows = {
        {NSEventTypeLeftMouseDown, PointerKind::down, PointerButton::left},
        {NSEventTypeLeftMouseUp, PointerKind::up, PointerButton::left},
        {NSEventTypeRightMouseDown, PointerKind::down, PointerButton::right},
        {NSEventTypeRightMouseUp, PointerKind::up, PointerButton::right},
        {NSEventTypeLeftMouseDragged, PointerKind::drag, PointerButton::left},
        {NSEventTypeRightMouseDragged, PointerKind::drag, PointerButton::right},
        {NSEventTypeMouseMoved, PointerKind::move, PointerButton::none},
    };
    for (const auto& row : rows) {
        auto e =
            translate_pointer((__bridge void*)f.mouse(row.type, 10, 20), (__bridge void*)f.view);
        REQUIRE(e);
        CHECK(e->kind == row.kind);
        CHECK(e->button == row.button);
        CHECK(e->x == 10.0);
        CHECK(e->y == 20.0);
        CHECK(e->timestamp == 12.5);
    }
    REQUIRE_FALSE(translate_pointer((__bridge void*)key_event(NSEventTypeKeyDown, kVK_ANSI_A, @"a"),
                                    (__bridge void*)f.view));
}

TEST_CASE("a control-click is a left press with Control, not a right press",
          "[native-surface][input]") {
    Fixture f;
    auto e = translate_pointer(
        (__bridge void*)f.mouse(NSEventTypeLeftMouseDown, 5, 5, NSEventModifierFlagControl),
        (__bridge void*)f.view);
    REQUIRE(e);
    REQUIRE(e->button == PointerButton::left);
    REQUIRE(e->button_number == 0);
    REQUIRE(e->modifiers == modifier_control);
}

TEST_CASE("every modifier is reported on every pointer event", "[native-surface][input]") {
    Fixture f;
    const NSEventModifierFlags all = NSEventModifierFlagShift | NSEventModifierFlagControl |
                                     NSEventModifierFlagOption | NSEventModifierFlagCommand |
                                     NSEventModifierFlagCapsLock;
    for (auto type : {NSEventTypeLeftMouseDown, NSEventTypeMouseMoved, NSEventTypeLeftMouseDragged,
                      NSEventTypeLeftMouseUp}) {
        auto e =
            translate_pointer((__bridge void*)f.mouse(type, 1, 1, all), (__bridge void*)f.view);
        REQUIRE(e);
        REQUIRE(e->modifiers == (modifier_shift | modifier_control | modifier_option |
                                 modifier_command | modifier_caps_lock));
    }
}

TEST_CASE("positions are top-left logical points and are not clamped", "[native-surface][input]") {
    Fixture f;
    auto corner = [&](double x, double y) {
        return translate_pointer((__bridge void*)f.mouse(NSEventTypeLeftMouseDown, x, y),
                                 (__bridge void*)f.view);
    };
    auto origin = corner(0, 0);
    REQUIRE(origin->x == 0.0);
    REQUIRE(origin->y == 0.0);
    auto far = corner(200, 100);
    REQUIRE(far->x == 200.0);
    REQUIRE(far->y == 100.0);
    // A drag that left the surface keeps its true position.
    auto outside = translate_pointer((__bridge void*)f.mouse(NSEventTypeLeftMouseDragged, -30, 140),
                                     (__bridge void*)f.view);
    REQUIRE(outside->x == -30.0);
    REQUIRE(outside->y == 140.0);
}

TEST_CASE("click count is reported on presses and releases", "[native-surface][input]") {
    Fixture f;
    auto e = translate_pointer((__bridge void*)f.mouse(NSEventTypeLeftMouseDown, 1, 1, 0, 2),
                               (__bridge void*)f.view);
    REQUIRE(e->click_count == 2);
}

TEST_CASE("further buttons keep their number", "[native-surface][input]") {
    for (const auto& [number, expected] : std::vector<std::pair<int, PointerButton>>{
             {2, PointerButton::middle}, {3, PointerButton::other}, {4, PointerButton::other}}) {
        CGEventRef cg = CGEventCreateMouseEvent(nullptr, kCGEventOtherMouseDown, CGPointMake(5, 5),
                                                static_cast<CGMouseButton>(number));
        NSEvent* event = [NSEvent eventWithCGEvent:cg];
        CFRelease(cg);
        auto e = translate_pointer((__bridge void*)event, nullptr);
        REQUIRE(e);
        REQUIRE(e->kind == PointerKind::down);
        REQUIRE(e->button == expected);
        REQUIRE(e->button_number == number);
    }
}

TEST_CASE("wheel deltas pass through once, with their precision", "[native-surface][input]") {
    // A detent wheel: line units.
    CGEventRef lines = CGEventCreateScrollWheelEvent2(nullptr, kCGScrollEventUnitLine, 2, 3, -1, 0);
    NSEvent* line_event = [NSEvent eventWithCGEvent:lines];
    CFRelease(lines);
    auto detent = translate_wheel((__bridge void*)line_event, nullptr);
    REQUIRE(detent);
    REQUIRE_FALSE(detent->precise);
    // The content delta is AppKit's scrollingDelta, unchanged: the user's
    // natural-scrolling choice is already in it, so negating it again on an
    // inverted device would scroll the wrong way.
    REQUIRE(detent->dy == line_event.scrollingDeltaY);
    REQUIRE(detent->dx == line_event.scrollingDeltaX);
    REQUIRE(detent->raw_dy == line_event.deltaY);
    REQUIRE(detent->direction_inverted ==
            static_cast<bool>(line_event.isDirectionInvertedFromDevice));
    REQUIRE(detent->dy != 0.0);

    // A trackpad: pixel units, precise.
    CGEventRef pixels =
        CGEventCreateScrollWheelEvent2(nullptr, kCGScrollEventUnitPixel, 1, 80, 0, 0);
    NSEvent* pixel_event = [NSEvent eventWithCGEvent:pixels];
    CFRelease(pixels);
    auto pad = translate_wheel((__bridge void*)pixel_event, nullptr);
    REQUIRE(pad);
    REQUIRE(pad->precise);
    REQUIRE(pad->dy == pixel_event.scrollingDeltaY);
    REQUIRE(std::abs(pad->dy) == 80.0);

    REQUIRE_FALSE(
        translate_wheel((__bridge void*)key_event(NSEventTypeKeyDown, kVK_Space, @" "), nullptr));
}

TEST_CASE("every physical key in the table translates to its key", "[native-surface][input]") {
    std::set<Key> seen;
    std::set<std::string> names;
    int mapped = 0;
    for (std::uint16_t code = 0; code < 128; ++code) {
        const Key key = key_for_native_code(code);
        if (key == Key::unknown)
            continue;
        ++mapped;
        REQUIRE(seen.insert(key).second); // one code per key
        auto down = translate_key((__bridge void*)key_event(NSEventTypeKeyDown, code, @""));
        REQUIRE(down);
        REQUIRE(down->kind == KeyKind::down);
        REQUIRE(down->key == key);
        REQUIRE(down->native_code == code);
        auto up = translate_key((__bridge void*)key_event(NSEventTypeKeyUp, code, @""));
        REQUIRE(up->kind == KeyKind::up);
        names.insert(std::string(key_name(key)));
    }
    REQUIRE(mapped == 120);
    REQUIRE(names.size() == 120);
    // Keys a test of each family would expect to find.
    REQUIRE(key_for_native_code(kVK_F13) == Key::f13);
    REQUIRE(key_for_native_code(kVK_F20) == Key::f20);
    REQUIRE(key_for_native_code(kVK_ANSI_KeypadEnter) == Key::keypad_enter);
    REQUIRE(key_for_native_code(kVK_Return) == Key::enter);
    REQUIRE(key_for_native_code(kVK_ANSI_Keypad7) == Key::keypad_7);
    REQUIRE(key_for_native_code(kVK_ANSI_7) == Key::digit_7);
    REQUIRE(key_for_native_code(kVK_RightShift) == Key::shift_right);
    REQUIRE(key_for_native_code(kVK_Help) == Key::help);
    REQUIRE(key_for_native_code(kVK_VolumeUp) == Key::volume_up);
    REQUIRE(key_for_native_code(0x7F) == Key::unknown);
    REQUIRE(key_name(Key::keypad_enter) == "keypad_enter");
    REQUIRE(key_name(Key::f24) == "f24");
}

TEST_CASE("key events carry characters, repeat and modifiers separately from identity",
          "[native-surface][input]") {
    auto e = translate_key((__bridge void*)key_event(NSEventTypeKeyDown, kVK_ANSI_E, @"é",
                                                     NSEventModifierFlagOption, YES));
    REQUIRE(e);
    REQUIRE(e->key == Key::e);
    REQUIRE(e->characters == "é");
    REQUIRE(e->repeat);
    REQUIRE(e->modifiers == modifier_option);
    auto up = translate_key((__bridge void*)key_event(NSEventTypeKeyUp, kVK_ANSI_E, @"é"));
    REQUIRE_FALSE(up->repeat);
    auto ret = translate_key((__bridge void*)key_event(NSEventTypeKeyDown, kVK_Return, @"\r"));
    REQUIRE(ret->key == Key::enter);
}

TEST_CASE("a modifier on its own is a down and an up of that key, with its side",
          "[native-surface][input]") {
    struct Row {
        unsigned short code;
        NSEventModifierFlags flags;
        Key key;
    };
    // Device bits from IOKit's NX_DEVICE*KEYMASK.
    const std::vector<Row> rows = {
        {kVK_Shift, NSEventModifierFlagShift | 0x2, Key::shift_left},
        {kVK_RightShift, NSEventModifierFlagShift | 0x4, Key::shift_right},
        {kVK_Control, NSEventModifierFlagControl | 0x1, Key::control_left},
        {kVK_RightControl, NSEventModifierFlagControl | 0x2000, Key::control_right},
        {kVK_Option, NSEventModifierFlagOption | 0x20, Key::option_left},
        {kVK_RightOption, NSEventModifierFlagOption | 0x40, Key::option_right},
        {kVK_Command, NSEventModifierFlagCommand | 0x8, Key::command_left},
        {kVK_RightCommand, NSEventModifierFlagCommand | 0x10, Key::command_right},
        {kVK_CapsLock, NSEventModifierFlagCapsLock, Key::caps_lock},
    };
    for (const auto& row : rows) {
        auto down = translate_flags_changed((__bridge void*)flags_event(row.code, row.flags));
        REQUIRE(down);
        CHECK(down->key == row.key);
        CHECK(down->kind == KeyKind::down);
        auto up = translate_flags_changed((__bridge void*)flags_event(row.code, 0));
        REQUIRE(up);
        CHECK(up->key == row.key);
        CHECK(up->kind == KeyKind::up);
    }
    // Right Shift released while left Shift is still held: the right key is up.
    auto right_up = translate_flags_changed(
        (__bridge void*)flags_event(kVK_RightShift, NSEventModifierFlagShift | 0x2));
    REQUIRE(right_up->kind == KeyKind::up);
    REQUIRE(right_up->modifiers & modifier_shift);
}

TEST_CASE("keys stay with the host unless the client holds the keyboard",
          "[native-surface][input]") {
    KeyEvent space;
    space.key = Key::space;
    space.characters = " ";
    KeyEvent save;
    save.key = Key::s;
    save.modifiers = modifier_command;
    KeyEvent letter;
    letter.key = Key::a;
    for (const auto& e : {space, save, letter}) {
        REQUIRE(route_key(e, false) == KeyRoute::host);
        REQUIRE(route_key(e, true) == KeyRoute::client_first);
        REQUIRE(route_key(e, KeyboardFocus::none) == KeyRoute::host);
        // Keys without text entry are offered to the client first too; what
        // it declines goes back to the host (the surface decides that part).
        REQUIRE(route_key(e, KeyboardFocus::keys) == KeyRoute::client_first);
        REQUIRE(route_key(e, KeyboardFocus::text) == KeyRoute::client_first);
    }
}

TEST_CASE("each named cursor maps to its own system cursor", "[native-surface][input]") {
    [NSApplication sharedApplication];
    NSCursor* arrow = (__bridge NSCursor*)ns_cursor_for(Cursor::arrow);
    REQUIRE(arrow == [NSCursor arrowCursor]);
    std::set<void*> distinct;
    for (auto c : {Cursor::pointing_hand, Cursor::open_hand, Cursor::closed_hand, Cursor::ibeam,
                   Cursor::crosshair, Cursor::resize_horizontal, Cursor::resize_vertical,
                   Cursor::not_allowed}) {
        REQUIRE(cursor_is_native(c));
        void* ns = ns_cursor_for(c);
        REQUIRE(ns != (__bridge void*)arrow);
        distinct.insert(ns);
    }
    REQUIRE(distinct.size() == 8);
    // The system has no public busy cursor: it falls back to the arrow, and
    // says so.
    REQUIRE_FALSE(cursor_is_native(Cursor::busy));
    REQUIRE(ns_cursor_for(Cursor::busy) == (__bridge void*)arrow);
    if (@available(macOS 15.0, *)) {
        REQUIRE(cursor_is_native(Cursor::resize_diagonal_nwse));
        REQUIRE(ns_cursor_for(Cursor::resize_diagonal_nwse) != (__bridge void*)arrow);
        REQUIRE(ns_cursor_for(Cursor::resize_diagonal_nesw) !=
                ns_cursor_for(Cursor::resize_diagonal_nwse));
    }
}
