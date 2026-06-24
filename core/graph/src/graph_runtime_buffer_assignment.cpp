#include <pulp/graph/graph_runtime_buffer_assignment.hpp>

namespace pulp::graph {

GraphRuntimeBufferAssignment build_graph_runtime_buffer_assignment(
    const GraphRuntimePlan& plan) {
    GraphRuntimeBufferAssignment assignment;
    try {
        assignment.nodes.resize(plan.nodes.size());
        assignment.feedback_prev_slot.assign(plan.connections.size(), kGraphRuntimeNoSlot);
    } catch (...) {
        assignment.nodes.clear();
        assignment.feedback_prev_slot.clear();
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

    // Append one persistent previous-block storage slot per feedback edge.
    for (std::size_t i = 0; i < plan.connections.size(); ++i) {
        if (plan.connections[i].feedback) {
            assignment.feedback_prev_slot[i] = cursor++;
            assignment.has_feedback = true;
        }
    }

    assignment.slot_count = cursor;
    assignment.ok = true;
    return assignment;
}

} // namespace pulp::graph
