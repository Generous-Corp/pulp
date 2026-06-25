#include <pulp/graph/graph_runtime_levelization.hpp>

#include <algorithm>
#include <vector>

namespace pulp::graph {

GraphRuntimeLevelization build_graph_runtime_levelization(const GraphRuntimePlan& plan) {
    GraphRuntimeLevelization result;
    const std::size_t node_count = plan.nodes.size();

    // A malformed plan whose processing order does not cover every node can't be
    // levelled deterministically (a source might be visited after its consumer).
    if (plan.processing_order_indices.size() != node_count) {
        return result;  // ok = false
    }

    try {
        result.node_level.assign(node_count, 0);

        // Walk in topological order so every non-feedback source's level is final
        // before any consumer reads it. level[dest] = max(level[dest],
        // level[source] + 1) over each non-feedback inbound edge.
        std::uint32_t max_level = 0;
        for (const auto node_index : plan.processing_order_indices) {
            if (node_index >= node_count) return result;  // ok = false
            const auto& node = plan.nodes[node_index];
            std::uint32_t level = 0;
            for (std::uint32_t c = 0; c < node.inbound_connection_count; ++c) {
                const auto ci =
                    plan.inbound_connection_indices[node.first_inbound_connection + c];
                const auto& conn = plan.connections[ci];
                if (conn.feedback) continue;  // reads previous block; no ordering
                level = std::max(level, result.node_level[conn.source_index] + 1);
            }
            result.node_level[node_index] = level;
            max_level = std::max(max_level, level);
        }

        result.level_count = node_count == 0 ? 0 : max_level + 1;

        // CSR grouping by level: counting sort over node_level.
        result.level_offsets.assign(result.level_count + 1, 0);
        result.level_nodes.assign(node_count, 0);
        for (std::size_t n = 0; n < node_count; ++n) {
            ++result.level_offsets[result.node_level[n] + 1];
        }
        for (std::uint32_t l = 0; l < result.level_count; ++l) {
            result.level_offsets[l + 1] += result.level_offsets[l];
        }
        // Place nodes within each level in PROCESSING (topological) order, not
        // node-index order, by walking processing_order_indices. Same-level nodes
        // are independent so a parallel executor may run them in any order, but a
        // serial fallback for a level (e.g. one containing an AudioOutput node
        // that accumulates into the shared bus) then iterates this slice in the
        // exact order the serial walk would — preserving bit-identical, non-
        // associative float accumulation.
        std::vector<std::uint32_t> cursor(result.level_offsets.begin(),
                                          result.level_offsets.end());
        for (const auto n : plan.processing_order_indices) {
            const std::uint32_t level = result.node_level[n];
            result.level_nodes[cursor[level]++] = n;
        }
    } catch (...) {
        result = {};
        return result;  // ok = false
    }

    result.ok = true;
    return result;
}

} // namespace pulp::graph
