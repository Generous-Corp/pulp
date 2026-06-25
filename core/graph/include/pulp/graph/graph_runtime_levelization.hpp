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
    bool ok = false;
};

// Compute the levelization for `plan`. Returns ok=false only on allocation
// failure or a malformed plan (processing order not covering every node); an
// empty plan yields ok=true with level_count=0.
GraphRuntimeLevelization build_graph_runtime_levelization(const GraphRuntimePlan& plan);

} // namespace pulp::graph
