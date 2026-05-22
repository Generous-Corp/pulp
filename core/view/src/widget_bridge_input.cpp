// widget_bridge_input.cpp — keyboard input forwarding for WidgetBridge.
//
// Extracted from widget_bridge.cpp in the 2026-05 refactor batch (A1).
// This TU owns:
//   * `keycode_to_w3c_key` — KeyCode enum → W3C UIEvent.key string
//     translation (so JSX handlers reading `e.key === 'Escape'` work).
//   * `WidgetBridge::forward_key_event` — global shortcut dispatch +
//     text-input focus-guard + `__dispatch__` keydown emission.
//
// Compat-surface mapping: `core/view/src/widget_bridge_input.cpp` is
// mapped to the `react` prefix in tools/scripts/compat_path_map.json,
// not to `*` like the legacy widget_bridge.cpp — so edits here only
// require updates to React-input compat artifacts, not to canvas2d /
// css / yoga / etc.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/script_event_dispatch.hpp>
#include <string>

namespace pulp::view {

namespace {

// Map a KeyCode enum value to its W3C UIEvent.key string. JSX handlers
// in @pulp/react consumers read `e.key === 'Escape'` / `'ArrowLeft'` /
// etc. — the previous implementation sent the raw int, so every such
// comparison failed silently (e.g. Spectr's `e.key === 'Escape'` modal
// close, history undo arrows, etc. were dead).
std::string keycode_to_w3c_key(int key_code, bool shift_held) {
    using K = KeyCode;
    auto kc = static_cast<K>(key_code);
    switch (kc) {
        case K::escape:     return "Escape";
        case K::enter:      return "Enter";
        case K::tab:        return "Tab";
        case K::backspace:  return "Backspace";
        case K::delete_:    return "Delete";
        case K::space:      return " ";
        case K::left:       return "ArrowLeft";
        case K::right:      return "ArrowRight";
        case K::up:         return "ArrowUp";
        case K::down:       return "ArrowDown";
        case K::home:       return "Home";
        case K::end_:       return "End";
        case K::page_up:    return "PageUp";
        case K::page_down:  return "PageDown";
        case K::f1:         return "F1";
        case K::f2:         return "F2";
        case K::f3:         return "F3";
        case K::f4:         return "F4";
        case K::f5:         return "F5";
        case K::f6:         return "F6";
        case K::f7:         return "F7";
        case K::f8:         return "F8";
        case K::f9:         return "F9";
        case K::f10:        return "F10";
        case K::f11:        return "F11";
        case K::f12:        return "F12";
        default: break;
    }
    // Printable chars: letters lower-case unless shift, digits as-is.
    if (key_code >= 'a' && key_code <= 'z') {
        char c = shift_held ? static_cast<char>(key_code - 32) : static_cast<char>(key_code);
        return std::string(1, c);
    }
    if (key_code >= '0' && key_code <= '9') {
        return std::string(1, static_cast<char>(key_code));
    }
    return "Unidentified";
}

bool shortcut_key_matches(KeyCode registered_key, int incoming_key_code) noexcept {
    if (registered_key == static_cast<KeyCode>(incoming_key_code)) return true;
    if (incoming_key_code >= 'A' && incoming_key_code <= 'Z') {
        return registered_key == static_cast<KeyCode>(incoming_key_code + ('a' - 'A'));
    }
    return false;
}

struct ScriptGlobalKeyDispatcherRegistrar {
    ScriptGlobalKeyDispatcherRegistrar() {
        script_events::set_global_key_dispatcher(&WidgetBridge::dispatch_global_key);
    }
};

const ScriptGlobalKeyDispatcherRegistrar script_global_key_dispatcher_registrar;

} // namespace

void WidgetBridge::forward_key_event(int key_code, uint16_t modifiers, bool is_down) {
    if (!is_down) return;

    // Check registered shortcuts first. Bare-key shortcuts (no Ctrl/Alt/
    // Meta/Cmd — Shift alone counts as bare since it just selects the
    // upper-case glyph) are suppressed while a text-accepting widget has
    // keyboard focus, so a user typing `?` into a search box doesn't
    // trigger a global "open cheatsheet" handler. Modifier chords always
    // fire — Cmd+S, Cmd+,, etc. are always-global by design.
    //
    // The focus slot `View::focused_input_` is claimed by ANY focusable
    // widget (Knob, Button, ListBox, TextEditor, ...) via the macOS host
    // focus path. We narrow that to text-accepting widgets via the virtual
    // `View::accepts_text_input()` — defaults false; only TextEditor (and
    // any future text-input widget) returns true. Otherwise focusing a
    // knob would silently kill every single-key shortcut until focus
    // moved off it.
    constexpr uint16_t kGlobalModifierMask =
        kModCtrl | kModAlt | kModMeta | kModCmd;
    const bool has_global_modifier = (modifiers & kGlobalModifierMask) != 0;
    const bool text_input_focused  =
        (View::focused_input_ != nullptr &&
         View::focused_input_->accepts_text_input());

    for (auto& s : shortcuts_) {
        if (shortcut_key_matches(s.key, key_code) && s.modifiers == modifiers) {
            if (!has_global_modifier && text_input_focused) {
                // Let the key fall through to the focused text input.
                break;
            }
            engine_.evaluate(s.callback + "()");
            return;
        }
    }

    // W3C UIEvent.key string; modifier booleans match KeyboardEvent
    // (ctrlKey/shiftKey/altKey/metaKey). `keyCode` retained for legacy.
    bool shift_held = (modifiers & kModShift) != 0;
    std::string key_str = keycode_to_w3c_key(key_code, shift_held);
    // JSON-escape backslash + quote (single-char ascii here is safe but
    // keep the guard for safety against future printable additions).
    std::string key_json;
    key_json.reserve(key_str.size() + 2);
    for (char c : key_str) {
        if (c == '\\' || c == '\'') key_json += '\\';
        key_json += c;
    }

    engine_.evaluate("__dispatch__('__global__', 'keydown', {"
        "key:'" + key_json + "',"
        "keyCode:" + std::to_string(key_code) + ","
        "ctrlKey:" + ((modifiers & kModCtrl) ? "true" : "false") + ","
        "shiftKey:" + ((modifiers & kModShift) ? "true" : "false") + ","
        "altKey:" + ((modifiers & kModAlt) ? "true" : "false") + ","
        "metaKey:" + (((modifiers & kModMeta) || (modifiers & kModCmd)) ? "true" : "false") + ","
        "mods:" + std::to_string(modifiers) + "})");
}

} // namespace pulp::view
