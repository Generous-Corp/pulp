#pragma once

/// @file
/// The answer a Vellum editor gives to a key its format host offers through
/// `format::Editor::offer_key()`.
///
/// A host offers a key before acting on it itself. For VST3 that is
/// `IPlugView::onKeyDown`, which REAPER (with "send all keyboard input to
/// plug-in" off, its default) calls before its own accelerator table; the
/// NSView key path never sees that press. Space is the one key that matters
/// there: the host wants it for transport, and a focused text field wants it
/// as text. Every other key keeps the native view's own key path, because a
/// host that also delivers it to the NSView would otherwise insert it twice.
///
/// The policy is Vellum's `route_plugin_key`, the same question the macOS
/// NSView seam asks, so an open overlay, a focused non-text widget and a
/// focused text field behave identically across formats.

#include <pulp/format/editor_host.hpp>

#include <vellum/view/input_events.hpp>
#include <vellum/view/plugin_key_routing.hpp>
#include <vellum/view/text_editor.hpp>
#include <vellum/view/view.hpp>

namespace pulp::vellum {

/// True for the one key a Vellum editor answers through `offer_key()`: a
/// plain or shifted Space press. Command and Control chords are host or
/// system shortcuts (Spotlight, for one) and never text.
inline bool offers_editor_key(const format::EditorKeyEvent& event) {
    if (event.key != format::EditorKey::space && event.character != U' ')
        return false;
    return (event.modifiers &
            (format::editor_modifier_command | format::editor_modifier_control)) == 0;
}

/// Route a host-offered key through `root`. Only a Space press is ever
/// consumed; a key release, a chord and every other key are forwarded.
/// `offer_global_hook` is false when the host's keys have already been
/// through the native key-equivalent pass over the editor's view, which gave
/// the root's global hook its one look at the press
/// (`format::editor_keys_after_native_pass_extension_id`).
inline format::KeyDisposition offer_editor_key(::vellum::view::View& root,
                                               const format::EditorKeyEvent& event,
                                               bool offer_global_hook = true) {
    if (!event.is_down || !offers_editor_key(event))
        return format::KeyDisposition::forward;

    namespace view = ::vellum::view;
    view::PluginKeyOffer offer;
    offer.key.key = view::KeyCode::space;
    offer.key.modifiers = static_cast<std::uint16_t>(
        (event.modifiers & format::editor_modifier_shift) != 0 ? view::kModShift : 0);
    offer.key.is_down = true;
    offer.key.is_repeat = event.is_repeat;
    // Where the host's offer is the press's only delivery point, this is the
    // one place the root's global hook is consulted for it.
    offer.offer_global_hook = offer_global_hook;

    switch (view::route_plugin_key(root, offer)) {
    case view::PluginKeyDisposition::consumed:
        root.request_repaint();
        return format::KeyDisposition::consumed;
    case view::PluginKeyDisposition::insert_as_text: {
        // The host hands over a key, not composed text, and there is no input
        // method on this path: insert the one character Space stands for.
        auto* field = dynamic_cast<view::TextEditor*>(view::plugin_key_focus(root));
        if (field == nullptr)
            return format::KeyDisposition::forward;
        view::TextInputEvent text;
        text.text = " ";
        field->on_text_input(text);
        root.request_repaint();
        return format::KeyDisposition::consumed;
    }
    case view::PluginKeyDisposition::forward_to_host:
        break;
    }
    return format::KeyDisposition::forward;
}

} // namespace pulp::vellum
