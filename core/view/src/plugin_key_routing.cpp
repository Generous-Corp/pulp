#include <pulp/view/plugin_key_routing.hpp>

#include <pulp/view/overlay_dismissal.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {

View* plugin_key_focus(View& root) {
    View* fv = View::focused_input_;
    for (View* v = fv; v != nullptr; v = v->parent())
        if (v == &root)
            return fv;
    return nullptr;
}

bool is_plugin_navigation_key(KeyCode key, std::uint16_t modifiers) {
    constexpr std::uint16_t kChordModifiers = kModCtrl | kModAlt | kModMeta | kModCmd;
    if ((modifiers & kChordModifiers) != 0)
        return false;
    return key == KeyCode::left || key == KeyCode::right || key == KeyCode::up ||
           key == KeyCode::down || key == KeyCode::home || key == KeyCode::end_ ||
           key == KeyCode::enter || key == KeyCode::escape;
}

PluginKeyDisposition route_plugin_key(View& root, const PluginKeyOffer& offer) {
    const KeyEvent& ke = offer.key;

    // 1. An open overlay owns Escape, focus or no focus.
    if (ke.key == KeyCode::escape && ke.is_down) {
        if (route_escape_to_active_overlay(root, ke.modifiers, ke.is_repeat) !=
            OverlayEscapeResult::none) {
            return PluginKeyDisposition::consumed;
        }
    }

    // 2. The focused view — asked, never assumed.
    if (View* fv = plugin_key_focus(root); fv != nullptr) {
        const bool takes_text = fv->accepts_text_input();
        if (!takes_text && fv->accepts_navigation_input()) {
            // Outside the bounded set the widget is not even offered the key,
            // so it cannot consume transport by accident.
            if (!is_plugin_navigation_key(ke.key, ke.modifiers))
                return PluginKeyDisposition::forward_to_host;
            // The widget answers; a scripted document that claimed navigation
            // focus is offered the key by the seam afterwards (it needs the
            // seam's own per-press de-duplication, so it is not policy).
            return fv->on_key_event(ke) ? PluginKeyDisposition::consumed
                                        : PluginKeyDisposition::forward_to_host;
        }
        if (takes_text) {
            // A text field sees every key, because any printable character is
            // its content. Command handling (arrows, ⌘Z, ⌘A, Backspace) runs
            // first and its RETURN VALUE is the answer — a key it handled as a
            // command must not also be inserted as text.
            if (fv->on_key_event(ke))
                return PluginKeyDisposition::consumed;
            // It declined. A chord or a function key is not text, so there is
            // nothing left for the field to do with it and the host must get
            // it: swallowing here is what makes ⌃Space, ⌘Z-with-no-undo, and
            // F-key transport commands die whenever a type-in happens to be
            // open. Anything else is a printable candidate and goes to the
            // platform's text-insertion path.
            constexpr std::uint16_t kChordModifiers = kModCtrl | kModMeta | kModCmd;
            if ((ke.modifiers & kChordModifiers) != 0 || offer.is_function_key)
                return PluginKeyDisposition::forward_to_host;
            return PluginKeyDisposition::insert_as_text;
        }
        // Focused, but neither text nor navigation: it holds the slot without
        // claiming the keyboard. Offer the key and honor the answer.
        if (fv->on_key_event(ke))
            return PluginKeyDisposition::consumed;
    }

    // 3. The framework-level hook sees only what the focused view declined.
    if (!offer.global_hook_already_offered && root.on_global_key && root.on_global_key(ke))
        return PluginKeyDisposition::consumed;

    // 4. Nobody claimed it. The host owns the keyboard by default.
    return PluginKeyDisposition::forward_to_host;
}

} // namespace pulp::view
