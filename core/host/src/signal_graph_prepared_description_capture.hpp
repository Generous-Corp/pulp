// SPDX-License-Identifier: MIT
//
// Prepared-description capture for a compiled prepared-topology candidate.
//
// PreparedTopologyEdit::prepare() compiles a private candidate graph off-side.
// When the edit carries an authored prepared description, the record exported
// with that snapshot must describe the routed plan that was actually compiled,
// not the authored request. capture_prepared_description() rebuilds the record
// from the serial routed snapshot and its bound buffer pool, cross-checks every
// node, edge, port and buffer slot against the authored candidate, and refuses
// (returns nullptr) on any mismatch or out-of-contract bound.
#pragma once

#include <pulp/host/signal_graph_runtime.hpp>

#include <cstdint>
#include <memory>

namespace pulp::host::detail {

// Portable kind name for a node type a prepared description can carry, or
// nullptr for every other type.
const char* prepared_kind(NodeType type) noexcept;

// Builds the prepared description for `candidate` from `seed` (the authored
// record bound before preparation) and the candidate's compiled serial routed
// path. The caller has already checked that the candidate uses the dynamic
// serial canonical-executor domain and that `serial_pool` is bound to
// `serial_snapshot`. `execution_selection_generation` is the candidate's
// current selection generation, `execution_generation` the compiled
// snapshot's, and `owner_generation` the owner authoring generation the edit
// was based on. Returns nullptr when the compiled plan does not match the
// authored candidate or exceeds an effective bound. Allocation failure
// propagates as std::bad_alloc.
std::shared_ptr<PreparedDescriptionData>
capture_prepared_description(const SignalGraph& candidate, const PreparedDescriptionData& seed,
                             const format::GraphRuntimeSnapshot& serial_snapshot,
                             const format::GraphRuntimeBufferPool& serial_pool, double sample_rate,
                             int max_block_size, std::uint64_t execution_selection_generation,
                             std::uint64_t execution_generation, std::uint64_t owner_generation);

} // namespace pulp::host::detail
