#pragma once

/// @file editor_keys.hpp
/// The editor-host contract's key and focus members for an RmlUi editor:
/// `offer_key` and `focus_changed` as one call each, so an editor does not
/// decide for itself whether a text field wants a key. Needs `pulp::format-core`
/// for `editor_host.hpp`.
///
/// Only the key and the character the host composed reach the field through
/// `offer_key`. Option-composed characters, dead keys and input-method text
/// arrive through the platform's text input, not as an offered key; an editor
/// whose host offers them must route that text to the surface itself.

#include <pulp/format/editor_host.hpp>
#include <pulp/rmlui_native/rmlui_surface.hpp>

namespace pulp::rmlui_native {

/// The pulp-native key for a contract key, or `Key::unknown`.
inline pulp::native::Key native_key_for(pulp::format::EditorKey key) noexcept {
    using pulp::format::EditorKey;
    using pulp::native::Key;
    switch (key) {
    case EditorKey::space:
        return Key::space;
    case EditorKey::enter:
        return Key::enter;
    case EditorKey::tab:
        return Key::tab;
    case EditorKey::backspace:
        return Key::backspace;
    case EditorKey::escape:
        return Key::escape;
    case EditorKey::delete_forward:
        return Key::delete_forward;
    case EditorKey::left:
        return Key::left;
    case EditorKey::right:
        return Key::right;
    case EditorKey::up:
        return Key::up;
    case EditorKey::down:
        return Key::down;
    case EditorKey::home:
        return Key::home;
    case EditorKey::end:
        return Key::end;
    case EditorKey::page_up:
        return Key::page_up;
    case EditorKey::page_down:
        return Key::page_down;
    case EditorKey::none:
        break;
    }
    return Key::unknown;
}

/// `event` as a pulp-native key event.
inline pulp::native::KeyEvent native_key_event_for(const pulp::format::EditorKeyEvent& event) {
    using namespace pulp::format;
    pulp::native::KeyEvent out;
    out.kind = event.is_down ? pulp::native::KeyKind::down : pulp::native::KeyKind::up;
    out.key = native_key_for(event.key);
    out.repeat = event.is_repeat;
    if (event.modifiers & editor_modifier_shift)
        out.modifiers |= pulp::native::modifier_shift;
    if (event.modifiers & editor_modifier_control)
        out.modifiers |= pulp::native::modifier_control;
    if (event.modifiers & editor_modifier_alt)
        out.modifiers |= pulp::native::modifier_option;
    if (event.modifiers & editor_modifier_command)
        out.modifiers |= pulp::native::modifier_command;
    // A lone UTF-16 surrogate is not a scalar value; it would encode as
    // invalid UTF-8, so it types nothing.
    if (event.character != 0 && !(event.character >= 0xD800 && event.character <= 0xDFFF)) {
        const char32_t c = event.character;
        if (c < 0x80) {
            out.characters.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.characters.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.characters.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.characters.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.characters.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.characters.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.characters.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.characters.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.characters.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.characters.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

/// The body of `Editor::offer_key` for an RmlUi editor: `consumed` only while
/// a text field has focus and takes the key, `forward` otherwise.
inline pulp::format::KeyDisposition offer_editor_key(RmlUiSurface& surface,
                                                     const pulp::format::EditorKeyEvent& event) {
    return surface.offer_key(native_key_event_for(event)) ? pulp::format::KeyDisposition::consumed
                                                          : pulp::format::KeyDisposition::forward;
}

/// The body of `Editor::focus_changed` for an RmlUi editor.
inline void forward_editor_focus(RmlUiSurface& surface, bool focused) {
    surface.set_window_focused(focused);
}

} // namespace pulp::rmlui_native
