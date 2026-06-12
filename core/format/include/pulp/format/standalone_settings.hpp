#pragma once

/// @file standalone_settings.hpp
/// Standalone settings-chrome navigation for plugin editors.
///
/// The standalone host wraps a plugin editor and an Audio/MIDI Settings panel
/// in a tab-bar-less TabPanel (editor = tab 0, "Settings" = tab 1). A plugin
/// editor can add its OWN control — e.g. a gear button — that opens the
/// Settings panel, but ONLY when it is actually hosted in the standalone:
/// inside a DAW the host owns audio + MIDI routing, so there is no settings
/// tab. These helpers give editors a one-call, structure-agnostic way to both
/// detect the chrome and open settings, without reaching into its internals.
/// They are the editor-side mirror of the Settings panel's "Done" button
/// (which switches back to the editor tab).

#include <pulp/view/view.hpp>
#include <pulp/view/ui_components.hpp>  // TabPanel

#include <string_view>

namespace pulp::format {

/// The nearest TabPanel ancestor of `from`, or nullptr. The standalone settings
/// chrome is the only place a Pulp plugin editor is nested under a TabPanel, so
/// this doubles as "am I in the settings chrome?".
inline view::TabPanel* enclosing_settings_chrome(view::View* from) {
    for (view::View* v = (from != nullptr ? from->parent() : nullptr);
         v != nullptr; v = v->parent()) {
        if (auto* tp = dynamic_cast<view::TabPanel*>(v)) return tp;
    }
    return nullptr;
}

/// True when `from` is hosted in the standalone settings chrome (a TabPanel
/// ancestor carrying the Settings tab alongside the editor). Editors gate their
/// gear / "Settings" control on this so it never appears when running as a
/// plugin in a DAW.
inline bool standalone_settings_available(view::View* from) {
    auto* tp = enclosing_settings_chrome(from);
    return tp != nullptr && tp->tab_count() >= 2;
}

/// Switch the enclosing standalone chrome to its Settings tab (Audio/MIDI). A
/// no-op if `from` is not inside such a chrome.
inline void open_standalone_settings(view::View* from) {
    if (auto* tp = enclosing_settings_chrome(from)) {
        if (tp->tab_count() >= 2) tp->set_active_tab(std::string_view("Settings"));
    }
}

}  // namespace pulp::format
