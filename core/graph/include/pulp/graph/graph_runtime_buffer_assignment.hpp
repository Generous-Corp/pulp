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
// ref-counting is a later optimization that only shrinks slot_count; consumers
// read only `slots.input_base + p` / `slots.output_base + p` and slot_count, so
// the data model below is reuse-ready without churn.
//
// 4e precondition: reuse means a producer's output slot and a later consumer's
// input slot may ALIAS. The executor's gather currently zeroes each input slot
// before summing, which assumes input slots are private scratch — keep input
// regions private under reuse, or revisit the gather, when liveness lands.
//
// The executor reads inter-node routing straight from the plan's connection
// table (inbound_connection_indices + connections), so the assignment only
// needs to publish where each node's port regions live and how many slots the
// pool must hold.

inline constexpr std::uint32_t kGraphRuntimeNoSlot = 0xFFFFFFFFu;

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
    // Indexed by connection index (matches GraphRuntimePlan::connections). For a
    // feedback connection, the dedicated previous-block storage slot (one mono
    // slot per feedback edge, appended after the node port regions and persisted
    // across blocks); kGraphRuntimeNoSlot for every non-feedback connection.
    // The executor gathers feedback as dst += prev_slot (last block) and copies
    // the source's output slot into prev_slot after the block — a one-block
    // delay matching host::SignalGraph's feedback_prev.
    std::vector<std::uint32_t> feedback_prev_slot;
    // True if the plan has any feedback connection.
    bool has_feedback = false;
    bool ok = false;
};

// Compute the slot layout for `plan`. Returns ok=false (and slot_count=0) only
// on allocation failure; an empty plan yields ok=true with slot_count=0.
GraphRuntimeBufferAssignment build_graph_runtime_buffer_assignment(
    const GraphRuntimePlan& plan);

} // namespace pulp::graph
