#pragma once

#include <pulp/graph/graph_runtime_plan.hpp>

#include <cstdint>
#include <vector>

namespace pulp::graph {

// Off-RT buffer assignment for the canonical graph executor's routing path.
//
// A "slot" is one mono scratch channel buffer (sized max_frames by the pool
// that backs it). Each node gets a contiguous input region (one slot per input
// port) and output region (one slot per output port); a node's own two regions
// are always disjoint so a binding can read inputs while writing outputs.
//
// Scratch is REUSED across non-overlapping lifetimes (liveness allocation): a
// region is recycled only by strictly-later nodes in topological order, after
// its last reader this block, so it never aliases a value still in use. Reuse is
// size-bucketed to keep regions contiguous, so the executor keeps addressing
// slots as `slots.input_base + p` / `slots.output_base + p` — reuse only shrinks
// slot_count, the consumption contract is unchanged. A feedback source's output
// region is pinned live to end-of-block (the feedback capture reads it), and the
// per-edge feedback previous-block slots are never recycled (cross-block state).
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
    // Indexed by connection index (matches GraphRuntimePlan::connections). For a
    // feedforward audio connection, the number of samples its source output must
    // be delayed so it time-aligns with the destination's most-latent input —
    // plug-in delay compensation. Derived by propagating per-node
    // latency_samples through the topology (input_latency = max upstream
    // output_latency; output_latency = input_latency + this node's
    // latency_samples) and taking dst.input_latency - src.output_latency. 0 for
    // connections that need no delay and for every feedback/event connection.
    // The backing delay ring (sized delay + max_frames, with a persisted write
    // position) lives in the GraphRuntimeBufferPool, which knows max_frames; the
    // assignment carries only the per-connection sample counts.
    std::vector<std::uint32_t> connection_delay_samples;
    // True if any connection needs a non-zero delay.
    bool has_delay = false;
    bool ok = false;
};

// Compute the slot layout for `plan`. Returns ok=false (and slot_count=0) only
// on allocation failure; an empty plan yields ok=true with slot_count=0.
GraphRuntimeBufferAssignment build_graph_runtime_buffer_assignment(
    const GraphRuntimePlan& plan);

} // namespace pulp::graph
