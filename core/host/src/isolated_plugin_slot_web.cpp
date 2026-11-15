// SPDX-License-Identifier: MIT
//
// Isolated-slot queries for the web (WAM / WebCLAP) graph profile.
//
// Worker isolation spawns a child process and talks to it over a shared
// mapping, none of which exists in a wasm module, so the web source closure
// does not compile isolated_plugin_slot.cpp. The graph still asks every
// PluginSlot whether it is an isolated proxy; on the web no such proxy can be
// constructed, so every answer is false.
//
// The declaration is repeated rather than included: this TU needs nothing from
// pulp/host/isolated_plugin_slot.hpp beyond the signature, and including it
// would pull the whole PluginSlot header graph into one more translation unit.
// It must match the declaration in that header exactly or the web link fails.

namespace pulp::host {

class PluginSlot;

bool is_isolated_plugin_slot(const PluginSlot&) noexcept;
bool same_isolated_plugin_binary(const PluginSlot&, const PluginSlot&) noexcept;

bool is_isolated_plugin_slot(const PluginSlot&) noexcept {
    return false;
}

bool same_isolated_plugin_binary(const PluginSlot&, const PluginSlot&) noexcept {
    return false;
}

} // namespace pulp::host
