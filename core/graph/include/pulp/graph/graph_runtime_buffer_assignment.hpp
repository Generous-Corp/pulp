#pragma once

#include <pulp/graph/graph_runtime_plan.hpp>

#include <cstdint>
#include <vector>

namespace pulp::graph {

// Off-RT buffer assignment for the canonical graph executor's routing path.
//
// A "slot" is one mono scratch channel buffer (sized max_frames by the pool
// that backs it). This v1 assignment is correct-but-not-minimal: every node
// gets a contiguous input region (one slot per input port) and output region
// (one slot per output port), all distinct. Slot reuse via liveness
// ref-counting is a later optimization that only shrinks slot_count; the
// executor's gather/scatter contract is unchanged by it.
//
// The executor reads inter-node routing straight from the plan's connection
// table (inbound_connection_indices + connections), so the assignment only
// needs to publish where each node's port regions live and how many slots the
// pool must hold.

struct GraphRuntimeNodeSlots {
    // Base scratch-slot index of this node's input region; the input port p
    // occupies slot (input_base + p). Meaningful only when input_ports > 0.
    std::uint32_t input_base = 0;
    // Base scratch-slot index of this node's output region; the output port p
    // occupies slot (output_base + p). Meaningful only when output_ports > 0.
    std::uint32_t output_base = 0;
};

struct GraphRuntimeBufferAssignment {
    std::uint32_t slot_count = 0;
    // Indexed by dense node index (matches GraphRuntimePlan::nodes order).
    std::vector<GraphRuntimeNodeSlots> nodes;
    // True if the plan has any feedback connection. The serial routing executor
    // gathers only feedforward edges (topological order guarantees their source
    // slots are filled first); feedback needs previous-block slot capture, which
    // is a later phase. Routing rejects such plans loudly rather than silently
    // diverging from the host graph.
    bool has_feedback = false;
    bool ok = false;
};

// Compute the slot layout for `plan`. Returns ok=false (and slot_count=0) only
// on allocation failure; an empty plan yields ok=true with slot_count=0.
GraphRuntimeBufferAssignment build_graph_runtime_buffer_assignment(
    const GraphRuntimePlan& plan);

} // namespace pulp::graph
