#pragma once

// The editor-host contract's key event for an AppKit key event. Objective-C++
// only: shared by the macOS hosts that offer keys to a contract editor (the AU
// v2 contract view and the standalone contract window).

#import <AppKit/AppKit.h>

#include <pulp/format/editor_host.hpp>

#include <cstdint>

namespace pulp::format::detail {

inline EditorKeyEvent appkit_contract_key_event(unsigned short key_code, NSString* characters,
                                                NSEventModifierFlags flags, bool is_down,
                                                bool is_repeat) {
    EditorKeyEvent event;
    event.is_down = is_down;
    event.is_repeat = is_repeat;
    switch (key_code) {
    case 49:
        event.key = EditorKey::space;
        break;
    case 36:
    case 76:
        event.key = EditorKey::enter;
        break;
    case 48:
        event.key = EditorKey::tab;
        break;
    case 51:
        event.key = EditorKey::backspace;
        break;
    case 53:
        event.key = EditorKey::escape;
        break;
    case 117:
        event.key = EditorKey::delete_forward;
        break;
    case 123:
        event.key = EditorKey::left;
        break;
    case 124:
        event.key = EditorKey::right;
        break;
    case 125:
        event.key = EditorKey::down;
        break;
    case 126:
        event.key = EditorKey::up;
        break;
    case 115:
        event.key = EditorKey::home;
        break;
    case 119:
        event.key = EditorKey::end;
        break;
    case 116:
        event.key = EditorKey::page_up;
        break;
    case 121:
        event.key = EditorKey::page_down;
        break;
    default:
        break;
    }
    if (characters.length > 0) {
        const unichar first = [characters characterAtIndex:0];
        // Function keys arrive as private-use code points; they carry no text.
        if (first < 0xF700 || first > 0xF8FF)
            event.character = static_cast<char32_t>(first);
    }
    if (event.key == EditorKey::space)
        event.character = U' ';
    std::uint8_t mods = 0;
    if ((flags & NSEventModifierFlagShift) != 0)
        mods |= editor_modifier_shift;
    if ((flags & NSEventModifierFlagControl) != 0)
        mods |= editor_modifier_control;
    if ((flags & NSEventModifierFlagOption) != 0)
        mods |= editor_modifier_alt;
    if ((flags & NSEventModifierFlagCommand) != 0)
        mods |= editor_modifier_command;
    event.modifiers = mods;
    return event;
}

} // namespace pulp::format::detail
