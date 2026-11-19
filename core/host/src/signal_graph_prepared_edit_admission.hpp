// SPDX-License-Identifier: MIT
//
// Admission gates for a prepared-topology candidate.
//
// PreparedTopologyEdit::prepare() runs admission_result_() under the owner's
// mutation lock before compiling the candidate off-side. It refuses a stale
// base, an unsupported spectral or MIDI-output transaction, a removed baseline
// node, a candidate that fails preflight, and any processor, plugin or custom
// instance that would need re-preparation, then prepares the instances minted
// inside the edit. The prepared-owned plugin wrapper stays private to
// signal_graph_prepared_topology_edit.cpp; this header exposes only the type check
// admission needs.
#pragma once

namespace pulp::host {
class PluginSlot;
} // namespace pulp::host

namespace pulp::host::detail {

// True when `slot` is the wrapper a prepared edit installs around a built-in
// plugin it admitted, so admission may prepare it as a new graph-owned slot.
bool is_prepared_owned_built_in_slot(const PluginSlot* slot) noexcept;

} // namespace pulp::host::detail
