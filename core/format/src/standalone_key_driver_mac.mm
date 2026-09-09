#include <pulp/format/detail/standalone_key_driver.hpp>

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <Cocoa/Cocoa.h>
#include <Carbon/Carbon.h>  // kVK_* virtual keycodes

namespace pulp::format::detail {
namespace {

/// Inverse of mac_geometry::key_code_from_ns: a portable KeyCode back to the
/// virtual keycode AppKit would report for that physical key. The host reads
/// `event.keyCode`, so a synthesized event carrying the wrong number is a
/// different key press than the one the caller asked for.
unsigned short vk_from_key_code(view::KeyCode key) {
    using KC = view::KeyCode;
    switch (key) {
        case KC::a: return kVK_ANSI_A;   case KC::b: return kVK_ANSI_B;
        case KC::c: return kVK_ANSI_C;   case KC::d: return kVK_ANSI_D;
        case KC::e: return kVK_ANSI_E;   case KC::f: return kVK_ANSI_F;
        case KC::g: return kVK_ANSI_G;   case KC::h: return kVK_ANSI_H;
        case KC::i: return kVK_ANSI_I;   case KC::j: return kVK_ANSI_J;
        case KC::k: return kVK_ANSI_K;   case KC::l: return kVK_ANSI_L;
        case KC::m: return kVK_ANSI_M;   case KC::n: return kVK_ANSI_N;
        case KC::o: return kVK_ANSI_O;   case KC::p: return kVK_ANSI_P;
        case KC::q: return kVK_ANSI_Q;   case KC::r: return kVK_ANSI_R;
        case KC::s: return kVK_ANSI_S;   case KC::t: return kVK_ANSI_T;
        case KC::u: return kVK_ANSI_U;   case KC::v: return kVK_ANSI_V;
        case KC::w: return kVK_ANSI_W;   case KC::x: return kVK_ANSI_X;
        case KC::y: return kVK_ANSI_Y;   case KC::z: return kVK_ANSI_Z;
        case KC::num0: return kVK_ANSI_0; case KC::num1: return kVK_ANSI_1;
        case KC::num2: return kVK_ANSI_2; case KC::num3: return kVK_ANSI_3;
        case KC::num4: return kVK_ANSI_4; case KC::num5: return kVK_ANSI_5;
        case KC::num6: return kVK_ANSI_6; case KC::num7: return kVK_ANSI_7;
        case KC::num8: return kVK_ANSI_8; case KC::num9: return kVK_ANSI_9;
        case KC::semicolon:  return kVK_ANSI_Semicolon;
        case KC::apostrophe: return kVK_ANSI_Quote;
        case KC::left:  return kVK_LeftArrow;  case KC::right: return kVK_RightArrow;
        case KC::down:  return kVK_DownArrow;  case KC::up:    return kVK_UpArrow;
        case KC::home:  return kVK_Home;       case KC::end_:  return kVK_End;
        case KC::page_up: return kVK_PageUp;   case KC::page_down: return kVK_PageDown;
        case KC::enter: return kVK_Return;     case KC::escape: return kVK_Escape;
        case KC::tab:   return kVK_Tab;        case KC::backspace: return kVK_Delete;
        case KC::delete_: return kVK_ForwardDelete;
        case KC::space: return kVK_Space;
        case KC::f1: return kVK_F1;  case KC::f2: return kVK_F2;
        case KC::f3: return kVK_F3;  case KC::f4: return kVK_F4;
        case KC::f5: return kVK_F5;  case KC::f6: return kVK_F6;
        case KC::f7: return kVK_F7;  case KC::f8: return kVK_F8;
        case KC::f9: return kVK_F9;  case KC::f10: return kVK_F10;
        case KC::f11: return kVK_F11; case KC::f12: return kVK_F12;
        default: return 0;
    }
}

/// The `characters` string AppKit would attach. interpretKeyEvents: and any
/// text-input consumer read this rather than the keycode, so an arrow key
/// needs its private-use function-key character, not an empty string.
NSString* characters_for_key_code(view::KeyCode key) {
    using KC = view::KeyCode;
    switch (key) {
        case KC::up:    return [NSString stringWithFormat:@"%C", (unichar)NSUpArrowFunctionKey];
        case KC::down:  return [NSString stringWithFormat:@"%C", (unichar)NSDownArrowFunctionKey];
        case KC::left:  return [NSString stringWithFormat:@"%C", (unichar)NSLeftArrowFunctionKey];
        case KC::right: return [NSString stringWithFormat:@"%C", (unichar)NSRightArrowFunctionKey];
        case KC::home:  return [NSString stringWithFormat:@"%C", (unichar)NSHomeFunctionKey];
        case KC::end_:  return [NSString stringWithFormat:@"%C", (unichar)NSEndFunctionKey];
        case KC::page_up:   return [NSString stringWithFormat:@"%C", (unichar)NSPageUpFunctionKey];
        case KC::page_down: return [NSString stringWithFormat:@"%C", (unichar)NSPageDownFunctionKey];
        case KC::enter:     return @"\r";
        case KC::escape:    return @"\x1b";
        case KC::tab:       return @"\t";
        case KC::backspace: return @"\x08";
        case KC::delete_:   return @"\x7f";
        case KC::space:     return @" ";
        default: break;
    }
    const int raw = static_cast<int>(key);
    if ((raw >= 'a' && raw <= 'z') || (raw >= '0' && raw <= '9')
        || raw == ';' || raw == '\'')
        return [NSString stringWithFormat:@"%c", static_cast<char>(raw)];
    return @"";
}

NSEventModifierFlags ns_flags_from_modifiers(uint16_t mods) {
    NSEventModifierFlags flags = 0;
    if (mods & view::kModShift) flags |= NSEventModifierFlagShift;
    if (mods & view::kModCtrl)  flags |= NSEventModifierFlagControl;
    if (mods & view::kModAlt)   flags |= NSEventModifierFlagOption;
    if (mods & (view::kModCmd | view::kModMeta)) flags |= NSEventModifierFlagCommand;
    return flags;
}

/// True for the arrow keys, which AppKit additionally flags as numeric-pad.
bool is_arrow_key(view::KeyCode key) {
    using KC = view::KeyCode;
    return key == KC::up || key == KC::down || key == KC::left || key == KC::right;
}

bool is_function_key(view::KeyCode key) {
    using KC = view::KeyCode;
    switch (key) {
        case KC::up: case KC::down: case KC::left: case KC::right:
        case KC::home: case KC::end_: case KC::page_up: case KC::page_down:
            return true;
        default:
            return static_cast<int>(key) >= static_cast<int>(KC::f1)
                && static_cast<int>(key) <= static_cast<int>(KC::f12);
    }
}

NSEvent* make_key_event(NSView* view, const KeySequenceStep& step,
                        NSEventType type) {
    // Match the flags a real keyboard produces. An arrow / page / home / end
    // key always carries the function-key flag, and the arrows additionally
    // carry numeric-pad; a consumer that filters on either would otherwise
    // drop a synthesized press that a physical one reaches it with.
    NSEventModifierFlags flags = ns_flags_from_modifiers(step.modifiers);
    if (is_function_key(step.key)) flags |= NSEventModifierFlagFunction;
    if (is_arrow_key(step.key)) flags |= NSEventModifierFlagNumericPad;
    NSString* chars = characters_for_key_code(step.key);
    return [NSEvent keyEventWithType:type
                            location:NSZeroPoint
                       modifierFlags:flags
                           timestamp:NSProcessInfo.processInfo.systemUptime
                        windowNumber:view.window ? view.window.windowNumber : 0
                             context:nil
                          characters:chars
         charactersIgnoringModifiers:chars
                           isARepeat:NO
                             keyCode:vk_from_key_code(step.key)];
}

}  // namespace

bool synthetic_key_delivery_supported() { return true; }

bool deliver_synthetic_key(void* native_content_view, const KeySequenceStep& step) {
    if (!native_content_view) return false;
    if (step.key == view::KeyCode::unknown) return false;

    @autoreleasepool {
        NSView* view = (__bridge NSView*)native_content_view;
        if (![view isKindOfClass:[NSView class]]) return false;

        NSEvent* down = make_key_event(view, step, NSEventTypeKeyDown);
        if (!down) return false;

        // Reproduce NSWindow's own key-down ordering. AppKit offers EVERY key
        // down to performKeyEquivalent: first and only sends keyDown: when that
        // returns NO. Calling keyDown: alone would skip the offer entirely and
        // hide any defect in how the two entry points divide the work — which
        // is the class of defect this driver exists to expose.
        //
        // The event is handed to the responder methods directly rather than
        // queued with -[NSApplication postEvent:atStart:]: a queued synthetic
        // event is silently discarded when the window is not key, which is the
        // normal state for an unattended/headless run. The methods invoked are
        // the same ones AppKit calls, so the host's real path still executes.
        if (![view performKeyEquivalent:down])
            [view keyDown:down];

        if (NSEvent* up = make_key_event(view, step, NSEventTypeKeyUp))
            [view keyUp:up];
    }
    return true;
}

}  // namespace pulp::format::detail

#else   // TARGET_OS_OSX

namespace pulp::format::detail {
bool synthetic_key_delivery_supported() { return false; }
bool deliver_synthetic_key(void*, const KeySequenceStep&) { return false; }
}  // namespace pulp::format::detail

#endif  // TARGET_OS_OSX
