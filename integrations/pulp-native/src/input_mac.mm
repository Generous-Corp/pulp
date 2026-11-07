// Translation from AppKit events to pulp::native input events. Compiled with
// ARC.

#include <pulp/native/input.hpp>

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h> // HIToolbox virtual key codes only

#include <array>
#include <cstddef>
#include <unordered_map>

#if !__has_feature(objc_arc)
#error "input_mac.mm must be compiled with -fobjc-arc"
#endif

namespace pulp::native {

namespace {

constexpr const char* kKeyNames[] = {
    "unknown",
    "a",
    "b",
    "c",
    "d",
    "e",
    "f",
    "g",
    "h",
    "i",
    "j",
    "k",
    "l",
    "m",
    "n",
    "o",
    "p",
    "q",
    "r",
    "s",
    "t",
    "u",
    "v",
    "w",
    "x",
    "y",
    "z",
    "digit_0",
    "digit_1",
    "digit_2",
    "digit_3",
    "digit_4",
    "digit_5",
    "digit_6",
    "digit_7",
    "digit_8",
    "digit_9",
    "space",
    "enter",
    "tab",
    "backspace",
    "escape",
    "delete_forward",
    "left",
    "right",
    "up",
    "down",
    "home",
    "end",
    "page_up",
    "page_down",
    "help",
    "f1",
    "f2",
    "f3",
    "f4",
    "f5",
    "f6",
    "f7",
    "f8",
    "f9",
    "f10",
    "f11",
    "f12",
    "f13",
    "f14",
    "f15",
    "f16",
    "f17",
    "f18",
    "f19",
    "f20",
    "f21",
    "f22",
    "f23",
    "f24",
    "keypad_0",
    "keypad_1",
    "keypad_2",
    "keypad_3",
    "keypad_4",
    "keypad_5",
    "keypad_6",
    "keypad_7",
    "keypad_8",
    "keypad_9",
    "keypad_decimal",
    "keypad_plus",
    "keypad_minus",
    "keypad_multiply",
    "keypad_divide",
    "keypad_equals",
    "keypad_enter",
    "keypad_clear",
    "keypad_comma",
    "minus",
    "equal",
    "left_bracket",
    "right_bracket",
    "backslash",
    "semicolon",
    "quote",
    "grave",
    "comma",
    "period",
    "slash",
    "section",
    "shift_left",
    "shift_right",
    "control_left",
    "control_right",
    "option_left",
    "option_right",
    "command_left",
    "command_right",
    "caps_lock",
    "function",
    "context_menu",
    "volume_up",
    "volume_down",
    "mute",
    "jis_yen",
    "jis_underscore",
    "jis_eisu",
    "jis_kana",
};

const std::unordered_map<std::uint16_t, Key>& native_key_table() {
    static const std::unordered_map<std::uint16_t, Key> table = {
        {kVK_ANSI_A, Key::a},
        {kVK_ANSI_B, Key::b},
        {kVK_ANSI_C, Key::c},
        {kVK_ANSI_D, Key::d},
        {kVK_ANSI_E, Key::e},
        {kVK_ANSI_F, Key::f},
        {kVK_ANSI_G, Key::g},
        {kVK_ANSI_H, Key::h},
        {kVK_ANSI_I, Key::i},
        {kVK_ANSI_J, Key::j},
        {kVK_ANSI_K, Key::k},
        {kVK_ANSI_L, Key::l},
        {kVK_ANSI_M, Key::m},
        {kVK_ANSI_N, Key::n},
        {kVK_ANSI_O, Key::o},
        {kVK_ANSI_P, Key::p},
        {kVK_ANSI_Q, Key::q},
        {kVK_ANSI_R, Key::r},
        {kVK_ANSI_S, Key::s},
        {kVK_ANSI_T, Key::t},
        {kVK_ANSI_U, Key::u},
        {kVK_ANSI_V, Key::v},
        {kVK_ANSI_W, Key::w},
        {kVK_ANSI_X, Key::x},
        {kVK_ANSI_Y, Key::y},
        {kVK_ANSI_Z, Key::z},
        {kVK_ANSI_0, Key::digit_0},
        {kVK_ANSI_Keypad0, Key::keypad_0},
        {kVK_ANSI_1, Key::digit_1},
        {kVK_ANSI_Keypad1, Key::keypad_1},
        {kVK_ANSI_2, Key::digit_2},
        {kVK_ANSI_Keypad2, Key::keypad_2},
        {kVK_ANSI_3, Key::digit_3},
        {kVK_ANSI_Keypad3, Key::keypad_3},
        {kVK_ANSI_4, Key::digit_4},
        {kVK_ANSI_Keypad4, Key::keypad_4},
        {kVK_ANSI_5, Key::digit_5},
        {kVK_ANSI_Keypad5, Key::keypad_5},
        {kVK_ANSI_6, Key::digit_6},
        {kVK_ANSI_Keypad6, Key::keypad_6},
        {kVK_ANSI_7, Key::digit_7},
        {kVK_ANSI_Keypad7, Key::keypad_7},
        {kVK_ANSI_8, Key::digit_8},
        {kVK_ANSI_Keypad8, Key::keypad_8},
        {kVK_ANSI_9, Key::digit_9},
        {kVK_ANSI_Keypad9, Key::keypad_9},
        {kVK_Space, Key::space},
        {kVK_Return, Key::enter},
        {kVK_Tab, Key::tab},
        {kVK_Delete, Key::backspace},
        {kVK_Escape, Key::escape},
        {kVK_ForwardDelete, Key::delete_forward},
        {kVK_LeftArrow, Key::left},
        {kVK_RightArrow, Key::right},
        {kVK_UpArrow, Key::up},
        {kVK_DownArrow, Key::down},
        {kVK_Home, Key::home},
        {kVK_End, Key::end},
        {kVK_PageUp, Key::page_up},
        {kVK_PageDown, Key::page_down},
        {kVK_Help, Key::help},
        {kVK_ANSI_KeypadDecimal, Key::keypad_decimal},
        {kVK_ANSI_KeypadPlus, Key::keypad_plus},
        {kVK_ANSI_KeypadMinus, Key::keypad_minus},
        {kVK_ANSI_KeypadMultiply, Key::keypad_multiply},
        {kVK_ANSI_KeypadDivide, Key::keypad_divide},
        {kVK_ANSI_KeypadEquals, Key::keypad_equals},
        {kVK_ANSI_KeypadEnter, Key::keypad_enter},
        {kVK_ANSI_KeypadClear, Key::keypad_clear},
        {kVK_JIS_KeypadComma, Key::keypad_comma},
        {kVK_ANSI_Minus, Key::minus},
        {kVK_ANSI_Equal, Key::equal},
        {kVK_ANSI_LeftBracket, Key::left_bracket},
        {kVK_ANSI_RightBracket, Key::right_bracket},
        {kVK_ANSI_Backslash, Key::backslash},
        {kVK_ANSI_Semicolon, Key::semicolon},
        {kVK_ANSI_Quote, Key::quote},
        {kVK_ANSI_Grave, Key::grave},
        {kVK_ANSI_Comma, Key::comma},
        {kVK_ANSI_Period, Key::period},
        {kVK_ANSI_Slash, Key::slash},
        {kVK_ISO_Section, Key::section},
        {kVK_Shift, Key::shift_left},
        {kVK_RightShift, Key::shift_right},
        {kVK_Control, Key::control_left},
        {kVK_RightControl, Key::control_right},
        {kVK_Option, Key::option_left},
        {kVK_RightOption, Key::option_right},
        {kVK_Command, Key::command_left},
        {kVK_RightCommand, Key::command_right},
        {kVK_CapsLock, Key::caps_lock},
        {kVK_Function, Key::function},
        {kVK_ContextualMenu, Key::context_menu},
        {kVK_VolumeUp, Key::volume_up},
        {kVK_VolumeDown, Key::volume_down},
        {kVK_Mute, Key::mute},
        {kVK_JIS_Yen, Key::jis_yen},
        {kVK_JIS_Underscore, Key::jis_underscore},
        {kVK_JIS_Eisu, Key::jis_eisu},
        {kVK_JIS_Kana, Key::jis_kana},
        {kVK_F1, Key::f1},
        {kVK_F2, Key::f2},
        {kVK_F3, Key::f3},
        {kVK_F4, Key::f4},
        {kVK_F5, Key::f5},
        {kVK_F6, Key::f6},
        {kVK_F7, Key::f7},
        {kVK_F8, Key::f8},
        {kVK_F9, Key::f9},
        {kVK_F10, Key::f10},
        {kVK_F11, Key::f11},
        {kVK_F12, Key::f12},
        {kVK_F13, Key::f13},
        {kVK_F14, Key::f14},
        {kVK_F15, Key::f15},
        {kVK_F16, Key::f16},
        {kVK_F17, Key::f17},
        {kVK_F18, Key::f18},
        {kVK_F19, Key::f19},
        {kVK_F20, Key::f20},
    };
    return table;
}

// Device-dependent modifier bits from IOKit's NX_DEVICE*KEYMASK, which tell
// the left and right modifier keys apart in a flags-changed event.
constexpr std::uint64_t kDeviceLeftControl = 0x00000001;
constexpr std::uint64_t kDeviceLeftShift = 0x00000002;
constexpr std::uint64_t kDeviceRightShift = 0x00000004;
constexpr std::uint64_t kDeviceLeftCommand = 0x00000008;
constexpr std::uint64_t kDeviceRightCommand = 0x00000010;
constexpr std::uint64_t kDeviceLeftOption = 0x00000020;
constexpr std::uint64_t kDeviceRightOption = 0x00000040;
constexpr std::uint64_t kDeviceRightControl = 0x00002000;

ScrollPhase phase_from(NSEventPhase phase) {
    if (phase & NSEventPhaseBegan)
        return ScrollPhase::began;
    if (phase & NSEventPhaseChanged)
        return ScrollPhase::changed;
    if (phase & NSEventPhaseEnded)
        return ScrollPhase::ended;
    if (phase & NSEventPhaseCancelled)
        return ScrollPhase::cancelled;
    if (phase & NSEventPhaseMayBegin)
        return ScrollPhase::may_begin;
    return ScrollPhase::none;
}

NSPoint local_point(NSEvent* event, NSView* view) {
    NSPoint p = event.locationInWindow;
    if (view && event.window)
        return [view convertPoint:p fromView:nil];
    if (view) {
        // An event built without a window: its location is already in the
        // view's window base coordinates.
        return [view convertPoint:p fromView:nil];
    }
    return p;
}

double top_left_y(NSView* view, NSPoint p) {
    if (!view || view.isFlipped)
        return p.y;
    return view.bounds.size.height - p.y;
}

std::string utf8(NSString* s) {
    if (!s || s.length == 0)
        return {};
    const char* c = s.UTF8String;
    return c ? std::string(c) : std::string();
}

} // namespace

KeyRoute route_key(const KeyEvent&, bool client_holds_text_focus) noexcept {
    return client_holds_text_focus ? KeyRoute::client_first : KeyRoute::host;
}

Key key_for_native_code(std::uint16_t code) noexcept {
    const auto& table = native_key_table();
    const auto it = table.find(code);
    return it == table.end() ? Key::unknown : it->second;
}

std::string_view key_name(Key key) noexcept {
    const auto index = static_cast<std::size_t>(key);
    if (index >= std::size(kKeyNames))
        return "unknown";
    return kKeyNames[index];
}

std::uint32_t modifiers_from_ns_flags(std::uint64_t flags) noexcept {
    std::uint32_t m = 0;
    if (flags & NSEventModifierFlagShift)
        m |= modifier_shift;
    if (flags & NSEventModifierFlagControl)
        m |= modifier_control;
    if (flags & NSEventModifierFlagOption)
        m |= modifier_option;
    if (flags & NSEventModifierFlagCommand)
        m |= modifier_command;
    if (flags & NSEventModifierFlagCapsLock)
        m |= modifier_caps_lock;
    if (flags & NSEventModifierFlagFunction)
        m |= modifier_function;
    if (flags & NSEventModifierFlagNumericPad)
        m |= modifier_numeric_pad;
    return m;
}

std::optional<PointerEvent> translate_pointer(void* ns_event, void* ns_view) {
    NSEvent* event = (__bridge NSEvent*)ns_event;
    NSView* view = (__bridge NSView*)ns_view;
    if (!event)
        return std::nullopt;
    PointerEvent out;
    switch (event.type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeRightMouseDown:
    case NSEventTypeOtherMouseDown:
        out.kind = PointerKind::down;
        break;
    case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseUp:
    case NSEventTypeOtherMouseUp:
        out.kind = PointerKind::up;
        break;
    case NSEventTypeMouseMoved:
        out.kind = PointerKind::move;
        break;
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged:
        out.kind = PointerKind::drag;
        break;
    case NSEventTypeMouseEntered:
        out.kind = PointerKind::enter;
        break;
    case NSEventTypeMouseExited:
        out.kind = PointerKind::leave;
        break;
    default:
        return std::nullopt;
    }
    const NSPoint p = local_point(event, view);
    out.x = p.x;
    out.y = top_left_y(view, p);
    out.modifiers = modifiers_from_ns_flags(event.modifierFlags);
    out.timestamp = event.timestamp;
    out.buttons_held = static_cast<std::uint32_t>([NSEvent pressedMouseButtons]);
    const bool has_button = out.kind == PointerKind::down || out.kind == PointerKind::up ||
                            out.kind == PointerKind::drag;
    if (has_button) {
        // The event type names the left and right buttons; only an "other"
        // event needs its button number (synthetic events report 0 for all).
        switch (event.type) {
        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseUp:
        case NSEventTypeLeftMouseDragged:
            out.button_number = 0;
            break;
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseUp:
        case NSEventTypeRightMouseDragged:
            out.button_number = 1;
            break;
        default:
            out.button_number = static_cast<int>(event.buttonNumber);
            break;
        }
        switch (out.button_number) {
        case 0:
            out.button = PointerButton::left;
            break;
        case 1:
            out.button = PointerButton::right;
            break;
        case 2:
            out.button = PointerButton::middle;
            break;
        default:
            out.button = PointerButton::other;
            break;
        }
        if (out.kind != PointerKind::drag)
            out.click_count = static_cast<int>(event.clickCount);
    }
    return out;
}

std::optional<WheelEvent> translate_wheel(void* ns_event, void* ns_view) {
    NSEvent* event = (__bridge NSEvent*)ns_event;
    NSView* view = (__bridge NSView*)ns_view;
    if (!event || event.type != NSEventTypeScrollWheel)
        return std::nullopt;
    WheelEvent out;
    const NSPoint p = local_point(event, view);
    out.x = p.x;
    out.y = top_left_y(view, p);
    // scrollingDelta* already carries the user's natural-scrolling choice;
    // passing it through unchanged applies that choice exactly once.
    out.dx = event.scrollingDeltaX;
    out.dy = event.scrollingDeltaY;
    out.precise = event.hasPreciseScrollingDeltas;
    out.raw_dx = event.deltaX;
    out.raw_dy = event.deltaY;
    out.direction_inverted = event.isDirectionInvertedFromDevice;
    out.phase = phase_from(event.phase);
    out.momentum_phase = phase_from(event.momentumPhase);
    out.modifiers = modifiers_from_ns_flags(event.modifierFlags);
    out.timestamp = event.timestamp;
    return out;
}

std::optional<KeyEvent> translate_key(void* ns_event) {
    NSEvent* event = (__bridge NSEvent*)ns_event;
    if (!event || (event.type != NSEventTypeKeyDown && event.type != NSEventTypeKeyUp))
        return std::nullopt;
    KeyEvent out;
    out.kind = event.type == NSEventTypeKeyDown ? KeyKind::down : KeyKind::up;
    out.native_code = event.keyCode;
    out.key = key_for_native_code(event.keyCode);
    out.repeat = event.type == NSEventTypeKeyDown && event.isARepeat;
    out.modifiers = modifiers_from_ns_flags(event.modifierFlags);
    out.characters = utf8(event.characters);
    out.timestamp = event.timestamp;
    return out;
}

std::optional<KeyEvent> translate_flags_changed(void* ns_event) {
    NSEvent* event = (__bridge NSEvent*)ns_event;
    if (!event || event.type != NSEventTypeFlagsChanged)
        return std::nullopt;
    const std::uint64_t flags = event.modifierFlags;
    KeyEvent out;
    out.native_code = event.keyCode;
    out.key = key_for_native_code(event.keyCode);
    out.modifiers = modifiers_from_ns_flags(flags);
    out.timestamp = event.timestamp;
    bool down = false;
    switch (out.key) {
    case Key::shift_left:
        down = flags & kDeviceLeftShift;
        break;
    case Key::shift_right:
        down = flags & kDeviceRightShift;
        break;
    case Key::control_left:
        down = flags & kDeviceLeftControl;
        break;
    case Key::control_right:
        down = flags & kDeviceRightControl;
        break;
    case Key::option_left:
        down = flags & kDeviceLeftOption;
        break;
    case Key::option_right:
        down = flags & kDeviceRightOption;
        break;
    case Key::command_left:
        down = flags & kDeviceLeftCommand;
        break;
    case Key::command_right:
        down = flags & kDeviceRightCommand;
        break;
    case Key::caps_lock:
        down = flags & NSEventModifierFlagCapsLock;
        break;
    case Key::function:
        down = flags & NSEventModifierFlagFunction;
        break;
    default:
        return std::nullopt;
    }
    out.kind = down ? KeyKind::down : KeyKind::up;
    return out;
}

void* ns_cursor_for(Cursor cursor) {
    NSCursor* c = nil;
    switch (cursor) {
    case Cursor::arrow:
        c = [NSCursor arrowCursor];
        break;
    case Cursor::pointing_hand:
        c = [NSCursor pointingHandCursor];
        break;
    case Cursor::open_hand:
        c = [NSCursor openHandCursor];
        break;
    case Cursor::closed_hand:
        c = [NSCursor closedHandCursor];
        break;
    case Cursor::ibeam:
        c = [NSCursor IBeamCursor];
        break;
    case Cursor::crosshair:
        c = [NSCursor crosshairCursor];
        break;
    case Cursor::resize_horizontal:
        c = [NSCursor resizeLeftRightCursor];
        break;
    case Cursor::resize_vertical:
        c = [NSCursor resizeUpDownCursor];
        break;
    case Cursor::not_allowed:
        c = [NSCursor operationNotAllowedCursor];
        break;
    case Cursor::resize_diagonal_nesw:
    case Cursor::resize_diagonal_nwse:
    case Cursor::busy:
        // No public NSCursor before macOS 15 (diagonal frame resize) or at
        // all (busy); see cursor_is_native.
        c = nil;
        break;
    }
    if (@available(macOS 15.0, *)) {
        if (cursor == Cursor::resize_diagonal_nesw)
            c = [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionTopRight
                                           inDirections:NSCursorFrameResizeDirectionsAll];
        else if (cursor == Cursor::resize_diagonal_nwse)
            c = [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionTopLeft
                                           inDirections:NSCursorFrameResizeDirectionsAll];
    }
    return (__bridge void*)(c ? c : [NSCursor arrowCursor]);
}

bool cursor_is_native(Cursor cursor) noexcept {
    switch (cursor) {
    case Cursor::busy:
        return false;
    case Cursor::resize_diagonal_nesw:
    case Cursor::resize_diagonal_nwse:
        if (@available(macOS 15.0, *))
            return true;
        return false;
    default:
        return true;
    }
}

} // namespace pulp::native
