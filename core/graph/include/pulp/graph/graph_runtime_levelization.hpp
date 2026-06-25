#pragma once

#include <pulp/graph/graph_runtime_plan.hpp>

#include <cstdint>
#include <vector>

namespace pulp::graph {

// Off-RT levelization of a GraphRuntimePlan for static multicore scheduling.
//
// A node's LEVEL is the length of its longest dependency chain: 0 for a node
// with no feedforward dependencies, otherwise one more than the maximum level of
// any node it depends on through a non-feedback connection (audio, event, or
// automation — every edge that requires the source to run before the dest).
// Feedback connections are excluded (they read the previous block, like the
// topological sort), so they never raise a level or create a cycle.
//
// Nodes sharing a level are mutually independent — no dependency path connects
// them — so a parallel executor can run them concurrently, synchronizing only at
// level boundaries. This is the static schedule masterwork §6.1 precomputes at
// plan time; the serial executor ignores it (its processing order already
// respects dependencies), and a future parallel executor consumes it behind the
// canonical seam.
struct GraphRuntimeLevelization {
    // Per dense node index (matches GraphRuntimePlan::nodes order): the node's
    // level.
    std::vector<std::uint32_t> node_level;
    // Number of distinct levels (0 for an empty plan).
    std::uint32_t level_count = 0;
    // CSR grouping of node indices by level: the nodes at level L are
    // level_nodes[level_offsets[L] .. level_offsets[L + 1]). level_offsets has
    // level_count + 1 entries; level_nodes has one entry per node.
    std::vector<std::uint32_t> level_offsets;
    std::vector<std::uint32_t> level_nodes;
    // Per level (level_count entries): a static, deterministic work-weight =
    // sum of graph_runtime_node_cost_weight() over the level's nodes. A coarse
    // shape score (per-channel work proxy), NOT a CPU-cycle estimate; multiplied
    // by the runtime frame count it gives a "channel-samples" figure a parallel
    // executor compares against a fork/join break-even threshold to decide
    // whether dispatching the level across workers is worth the overhead.
    std::vector<std::uint64_t> level_work_weight;
    bool ok = false;
};

// Static per-node work weight feeding level_work_weight: the per-channel work a
// node does, approximated as max(input_ports, output_ports) for audio-processing
// kinds (Processor / Utility / Custom) and 0 for pure I/O (AudioInput /
// AudioOutput / MidiInput / MidiOutput), which neither justify a worker hop nor
// (for AudioOutput) ever run in parallel. Deterministic and allocation-free.
// NOTE: Gain and hosted-plugin nodes both map to Processor, so this cannot tell a
// cheap gain from an expensive plugin — per-node DSP-cost discrimination needs
// the runtime measured-load feed (masterwork §6.4), a later refinement.
std::uint64_t graph_runtime_node_cost_weight(GraphRuntimeNodeKind kind,
                                             std::uint32_t input_ports,
                                             std::uint32_t output_ports) noexcept;

// Compute the levelization for `plan`. Returns ok=false only on allocation
// failure or a malformed plan (processing order not covering every node); an
// empty plan yields ok=true with level_count=0.
GraphRuntimeLevelization build_graph_runtime_levelization(const GraphRuntimePlan& plan);

} // namespace pulp::graph
