#include <pulp/graph/graph_runtime_buffer_assignment.hpp>

namespace pulp::graph {

GraphRuntimeBufferAssignment build_graph_runtime_buffer_assignment(
    const GraphRuntimePlan& plan) {
    GraphRuntimeBufferAssignment assignment;
    try {
        assignment.nodes.resize(plan.nodes.size());
    } catch (...) {
        assignment.nodes.clear();
        assignment.slot_count = 0;
        assignment.ok = false;
        return assignment;
    }

    // Lay out each node's input region then output region contiguously. The
    // running cursor is the next free slot, so the final value is the total
    // slot count the backing pool must provide.
    std::uint32_t cursor = 0;
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto& node = plan.nodes[i];
        auto& slots = assignment.nodes[i];
        slots.input_base = cursor;
        cursor += node.input_ports;
        slots.output_base = cursor;
        cursor += node.output_ports;
    }

    assignment.slot_count = cursor;
    for (const auto& conn : plan.connections) {
        if (conn.feedback) {
            assignment.has_feedback = true;
            break;
        }
    }
    assignment.ok = true;
    return assignment;
}

} // namespace pulp::graph
