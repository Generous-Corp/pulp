// SPDX-License-Identifier: MIT
//
// Isolated-slot query for the web (WAM / WebCLAP) graph profile.
//
// Worker isolation spawns a child process and talks to it over a shared
// mapping, none of which exists in a wasm module, so the web source closure
// does not compile isolated_plugin_slot.cpp. The graph still asks every
// PluginSlot whether it is an isolated proxy; on the web no such proxy can be
// constructed, so the answer is always false.

#include <pulp/host/isolated_plugin_slot.hpp>

namespace pulp::host {

bool is_isolated_plugin_slot(const PluginSlot&) noexcept { return false; }

} // namespace pulp::host
