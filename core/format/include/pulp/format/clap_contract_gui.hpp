#pragma once

// The CLAP GUI of a processor that supplies an editor factory
// (`Processor::editor_factory()`, see pulp/format/editor_host.hpp).
//
// The per-plug-in CLAP entry returns this `clap_plugin_gui` whenever the
// initialized processor's factory is non-null, and keeps its own GUI (the
// `create_view()` path, compiled only with PULP_CLAP_GUI) otherwise. It needs
// no view layer, so a view-free plug-in (`pulp_add_plugin(... UI <package>)`,
// which links pulp-format-clap-core) has a working editor.
//
// Each create() opens one ContractEditorSession (contract_editor_session.hpp)
// over the adapter's store. Edits go through its StateStoreParamPort into the
// store, whose gesture and value changes the adapter already reports to the
// host as CLAP param gesture and value events. The calls map as:
//
//   is_api_supported / get_preferred_api -> the platform's embedded window API
//   create / destroy        -> a new session / detach and drop it
//   set_parent              -> Editor::attach(NativeParent)
//   show / hide             -> Editor::visibility_changed()
//   set_scale               -> Editor::set_size() with the new scale
//   get_size / set_size     -> Editor::size() / Editor::set_size()
//   can_resize / get_resize_hints / adjust_size -> the editor's size hints and
//                              adjust_size(), clamped to the hints
//   plug-in destroy         -> the session goes before the processor; an
//                              editor still attached is invalidated
//
// Idle: when the host offers timer-support, a timer registered at set_parent()
// drives Editor::idle() through this plug-in's timer-support extension;
// otherwise the session's own main-thread tick does. CLAP has no key callback,
// so keys reach the editor only through its own native view, and Space reaches
// the host the way the host's window routes it.

#include <clap/clap.h>
#include <clap/ext/gui.h>
#include <clap/ext/timer-support.h>

namespace pulp::format::clap_adapter {

/// The contract GUI extension for `plugin`, or null when its processor is not
/// initialized, has no editor factory, or editors are blocked by the
/// environment (headless, CI, tests).
const clap_plugin_gui_t* contract_gui_extension(const clap_plugin_t* plugin);

/// The timer-support extension that drives a contract editor's idle tick, or
/// null when the contract GUI does not apply to `plugin`.
const clap_plugin_timer_support_t* contract_timer_extension(const clap_plugin_t* plugin);

} // namespace pulp::format::clap_adapter
